#include "nsig_extract.h"

#include "hermes/AST/Context.h"
#include "hermes/AST/ESTree.h"
#include "hermes/AST/RecursiveVisitor.h"
#include "hermes/Parser/JSParser.h"
#include "llvh/Support/MemoryBuffer.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Faithful native port of LuanRT/YouTube.js
//   src/utils/javascript/{helpers,matchers,JsAnalyzer,JsExtractor}.ts
// over the Hermes ESTree AST. See nsig_extract.h.

namespace epotoken::nsig_extract {

namespace es = hermes::ESTree;
using es::Node;

namespace {

constexpr const char* INDENT = "  ";

// ---------------------------------------------------------------------------
// Node type helpers (Hermes uses distinct classes; meriyah used string types)
// ---------------------------------------------------------------------------

template <class T> T* as(Node* n) { return llvh::dyn_cast_or_null<T>(n); }
template <class T> bool is(Node* n) { return n && llvh::isa<T>(n); }

std::string labelStr(es::NodeLabel l) { return l ? l->str().str() : std::string{}; }

std::string idName(Node* n) {
    auto* id = as<es::IdentifierNode>(n);
    return id ? labelStr(id->_name) : std::string{};
}

// meriyah "Literal" spans several Hermes literal kinds.
bool isLiteral(Node* n) {
    return is<es::StringLiteralNode>(n) || is<es::NumericLiteralNode>(n) ||
           is<es::BooleanLiteralNode>(n) || is<es::NullLiteralNode>(n) ||
           is<es::RegExpLiteralNode>(n);
}
bool isStringLiteralValue(Node* n, const char* v) {
    auto* s = as<es::StringLiteralNode>(n);
    return s && s->_value->str() == v;
}

bool isFunctionLike(Node* n) {
    return is<es::FunctionExpressionNode>(n) || is<es::ArrowFunctionExpressionNode>(n) ||
           is<es::FunctionDeclarationNode>(n);
}

// ---------------------------------------------------------------------------
// Insertion-order-preserving string set (mirrors JS Set semantics used for
// dependency/closure ordering).
// ---------------------------------------------------------------------------
struct OrderedSet {
    std::vector<std::string> items;
    std::unordered_set<std::string> seen;
    bool add(const std::string& s) {
        if (seen.insert(s).second) { items.push_back(s); return true; }
        return false;
    }
    bool has(const std::string& s) const { return seen.count(s) != 0; }
    size_t size() const { return items.size(); }
    auto begin() const { return items.begin(); }
    auto end() const { return items.end(); }
};

// ---------------------------------------------------------------------------
// jsBuiltIns (helpers.ts)
// ---------------------------------------------------------------------------
const std::unordered_set<std::string>& jsBuiltIns() {
    static const std::unordered_set<std::string> s = {
        "AbortController","AbortSignal","Array","ArrayBuffer","AsyncContext","Atomics","AudioContext","BigInt","BigInt64Array","BigUint64Array",
        "Blob","Boolean","BroadcastChannel","Buffer","CanvasRenderingContext2D","clearImmediate","clearInterval","clearTimeout","confirm",
        "console","Crypto","CustomEvent","DataView","Date","decodeURI","decodeURIComponent","document","Element","encodeURI",
        "encodeURIComponent","Error","escape","eval","Event","EventTarget","fetch","File","FileReader","Float32Array","Float64Array",
        "FormData","function","global","globalThis","hasOwnProperty","Headers","History","HTMLElement","HTMLCollection","IDBKeyRange",
        "Infinity","Int16Array","Int32Array","Int8Array","Intl","IntersectionObserver","isFinite","isNaN","isPrototypeOf","JSON",
        "location","log","Map","Math","MediaRecorder","MediaSource","MediaStream","MemberExpression","MutationObserver","NaN",
        "navigator","Node","NodeList","Number","Object","OfflineAudioContext","parse","parseFloat","parseInt","Performance",
        "process","Promise","prompt","prototype","Proxy","ReadableStream","Reflect","RegExp","requestAnimationFrame","requestIdleCallback",
        "Request","Response","ResizeObserver","Screen","setImmediate","setInterval","setTimeout","SharedArrayBuffer","SharedWorker",
        "SourceBuffer","split","String","stringify","structuredClone","SubtleCrypto","Symbol","TextDecoder","TextEncoder","this",
        "toString","TransformStream","Uint16Array","Uint32Array","Uint8Array","Uint8ClampedArray","undefined","unescape","URL",
        "URLSearchParams","valueOf","WeakMap","WeakSet","WebAssembly","WebGLRenderingContext","window","Worker","WritableStream",
        "XMLHttpRequest","alert","arguments","atob","btoa","cancelAnimationFrame","cancelIdleCallback","queueMicrotask"
    };
    return s;
}

// ---------------------------------------------------------------------------
// Generic AST walker with enter/leave + parent (mirrors helpers.ts walkAst).
// enter returns: 0=continue, 1=skip children, 2=stop.
// ---------------------------------------------------------------------------
struct AstWalker {
    std::function<int(Node*, Node*)> enter;
    std::function<void(Node*)> leave;
    Node* curParent = nullptr;
    bool stopped = false;

    bool incRecursionDepth(Node*) { return !stopped; }
    void decRecursionDepth() {}
    void visit(Node* n) {
        if (stopped || !n) return;
        Node* parent = curParent;
        int r = enter ? enter(n, parent) : 0;
        if (r == 2) { stopped = true; return; }
        if (r != 1) {
            curParent = n;
            es::visitESTreeChildren(*this, n);
            curParent = parent;
            if (stopped) return;
        }
        if (leave) leave(n);
    }
};

void walkAst(Node* root, std::function<int(Node*, Node*)> enter,
             std::function<void(Node*)> leave = nullptr) {
    AstWalker w;
    w.enter = std::move(enter);
    w.leave = std::move(leave);
    es::visitESTreeNode(w, root, static_cast<Node*>(nullptr));
}

// Iterate a NodeList into a vector of pointers.
std::vector<Node*> listVec(es::NodeList& l) {
    std::vector<Node*> v;
    for (Node& n : l) v.push_back(&n);
    return v;
}
size_t listLen(es::NodeList& l) {
    size_t c = 0; for (Node& n : l) { (void)n; ++c; } return c;
}
Node* listAt(es::NodeList& l, size_t i) {
    size_t c = 0; for (Node& n : l) { if (c++ == i) return &n; } return nullptr;
}

} // namespace

// ===========================================================================
// Analyzer
// ===========================================================================
class Analyzer {
public:
    const std::string& source;
    const char* base;   // buffer start for offset math
    es::ProgramNode* program;

    std::string iifeParamName;

    struct VarMeta {
        std::string name;
        Node* node = nullptr;
        std::set<std::string> dependencies;      // sibling order irrelevant (see note)
        std::set<std::string> dependents;
        // aliasExpr ("g.r.") -> member metas bound to this class's prototype
        std::vector<std::pair<std::string, std::vector<VarMeta*>>> prototypeAliases;
        bool predeclared = false;

        std::vector<VarMeta*>* aliasBucket(const std::string& key) {
            for (auto& p : prototypeAliases) if (p.first == key) return &p.second;
            prototypeAliases.emplace_back(key, std::vector<VarMeta*>{});
            return &prototypeAliases.back().second;
        }
    };

    // Stable storage: pointers into these stay valid.
    std::unordered_map<std::string, VarMeta*> declaredVariables;
    std::vector<std::unique_ptr<VarMeta>> pool;
    std::unordered_map<std::string, std::set<std::string>> dependentsTracker;

    struct Extraction {
        std::string friendlyName;
        std::function<Node*(Node*)> match;   // returns matched node or nullptr
        bool collectDependencies = true;

        Node* node = nullptr;
        VarMeta* metadata = nullptr;
        Node* matchContext = nullptr;
        bool ready = false;
    };
    std::vector<Extraction> extractions;

    std::pair<std::string, VarMeta*> pendingAlias{"", nullptr};
    bool hasPendingAlias = false;

    Analyzer(const std::string& src, const char* b, es::ProgramNode* p)
        : source(src), base(b), program(p) {}

    VarMeta* newMeta() { pool.push_back(std::make_unique<VarMeta>()); return pool.back().get(); }

    std::string nodeSrc(Node* n) {
        if (!n) return {};
        auto r = n->getSourceRange();
        size_t s = r.Start.getPointer() - base;
        size_t e = r.End.getPointer() - base;
        if (e < s || e > source.size()) return {};
        return source.substr(s, e - s);
    }

    static void rtrim(std::string& s) {
        while (!s.empty() && (s.back()==' '||s.back()=='\n'||s.back()=='\t'||s.back()=='\r')) s.pop_back();
    }
    static void trim(std::string& s) {
        size_t i = 0; while (i < s.size() && (s[i]==' '||s[i]=='\n'||s[i]=='\t'||s[i]=='\r')) i++;
        s.erase(0, i); rtrim(s);
    }

    // memberToString (helpers.ts)
    std::string memberToString(Node* me) {
        auto* m0 = as<es::MemberExpressionNode>(me);
        if (!m0) return {};
        std::vector<std::string> segs;
        Node* cur = me;
        while (auto* m = as<es::MemberExpressionNode>(cur)) {
            Node* prop = m->_property;
            if (!prop) return {};
            if (m->_computed) {
                std::string ps = nodeSrc(prop); trim(ps);
                if (ps.empty()) return {};
                segs.push_back("[" + ps + "]");
            } else {
                auto* pid = as<es::IdentifierNode>(prop);
                if (!pid) return {};
                segs.push_back("." + labelStr(pid->_name));
            }
            cur = m->_object;
        }
        std::string basev;
        if (auto* id = as<es::IdentifierNode>(cur)) basev = labelStr(id->_name);
        else if (is<es::ThisExpressionNode>(cur)) basev = "this";
        if (basev.empty()) return {};
        std::string out = basev;
        for (auto it = segs.rbegin(); it != segs.rend(); ++it) out += *it;
        return out;
    }

    // memberBaseName (helpers.ts)
    std::string memberBaseName(Node* me) {
        auto* m = as<es::MemberExpressionNode>(me);
        if (!m) return {};
        Node* target = m->_object;
        while (auto* t = as<es::MemberExpressionNode>(target)) {
            std::string pn = memberToString(t);
            if (!pn.empty()) return pn;
            target = t->_object;
        }
        if (auto* id = as<es::IdentifierNode>(target)) return labelStr(id->_name);
        if (is<es::ThisExpressionNode>(target)) return "this";
        return {};
    }

    static std::string replaceAll(std::string s, const std::string& a, const std::string& b) {
        size_t pos = 0;
        while ((pos = s.find(a, pos)) != std::string::npos) { s.replace(pos, a.size(), b); pos += b.size(); }
        return s;
    }

    bool needsDependencyAnalysis(Node* n) {
        return isFunctionLike(n) || is<es::ArrayExpressionNode>(n) || is<es::LogicalExpressionNode>(n) ||
               is<es::CallExpressionNode>(n) || is<es::NewExpressionNode>(n) || is<es::MemberExpressionNode>(n) ||
               is<es::BinaryExpressionNode>(n) || is<es::ConditionalExpressionNode>(n) ||
               is<es::ObjectExpressionNode>(n) || is<es::SequenceExpressionNode>(n) ||
               is<es::ClassExpressionNode>(n) || is<es::IdentifierNode>(n);
    }

    // findDependencies (JsAnalyzer.ts) — scope-aware free-variable collection.
    std::set<std::string> findDependencies(Node* rootNode, const std::string& identifierName) {
        std::set<std::string> dependencies;
        if (!rootNode) return dependencies;

        struct Scope { std::unordered_set<std::string> names; bool isFunction; };
        std::vector<Scope> scopeStack;
        scopeStack.push_back({{}, false});

        auto currentScope = [&]() -> Scope& { return scopeStack.back(); };
        auto isInScope = [&](const std::string& nm) {
            for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it)
                if (it->names.count(nm)) return true;
            return false;
        };
        std::function<void(Node*, std::unordered_set<std::string>&)> collectBinding =
            [&](Node* pat, std::unordered_set<std::string>& target) {
                if (!pat) return;
                if (auto* id = as<es::IdentifierNode>(pat)) { target.insert(labelStr(id->_name)); return; }
                if (auto* op = as<es::ObjectPatternNode>(pat)) {
                    for (Node& pr : op->_properties) {
                        if (auto* re = as<es::RestElementNode>(&pr)) collectBinding(re->_argument, target);
                        else if (auto* p = as<es::PropertyNode>(&pr)) collectBinding(p->_value, target);
                    }
                    return;
                }
                if (auto* ap = as<es::ArrayPatternNode>(pat)) {
                    for (Node& el : ap->_elements) collectBinding(&el, target);
                    return;
                }
                if (auto* re = as<es::RestElementNode>(pat)) { collectBinding(re->_argument, target); return; }
                if (auto* asg = as<es::AssignmentPatternNode>(pat)) { collectBinding(asg->_left, target); return; }
            };

        std::string rootIdentifierName;
        // 'id' in rootNode && rootNode.id is Identifier
        if (auto* fd = as<es::FunctionDeclarationNode>(rootNode)) { if (fd->_id) rootIdentifierName = idName(fd->_id); }
        else if (auto* fe = as<es::FunctionExpressionNode>(rootNode)) { if (fe->_id) rootIdentifierName = idName(fe->_id); }
        else if (auto* cd = as<es::ClassDeclarationNode>(rootNode)) { if (cd->_id) rootIdentifierName = idName(cd->_id); }
        else if (auto* ce = as<es::ClassExpressionNode>(rootNode)) { if (ce->_id) rootIdentifierName = idName(ce->_id); }

        auto enter = [&](Node* n, Node* parent) -> int {
            if (auto* fe = as<es::FunctionExpressionNode>(n)) {
                std::string fnName = fe->_id ? idName(fe->_id) : std::string{};
                Scope fs{{}, true};
                if (!fnName.empty()) fs.names.insert(fnName);
                for (Node& p : fe->_params) collectBinding(&p, fs.names);
                scopeStack.push_back(std::move(fs));
                return 0;
            }
            if (auto* ar = as<es::ArrowFunctionExpressionNode>(n)) {
                Scope fs{{}, true};
                for (Node& p : ar->_params) collectBinding(&p, fs.names);
                scopeStack.push_back(std::move(fs));
                return 0;
            }
            if (auto* fd = as<es::FunctionDeclarationNode>(n)) {
                std::string fnName = fd->_id ? idName(fd->_id) : std::string{};
                if (!fnName.empty()) currentScope().names.insert(fnName);
                Scope fs{{}, true};
                for (Node& p : fd->_params) collectBinding(&p, fs.names);
                scopeStack.push_back(std::move(fs));
                return 0;
            }
            if (is<es::BlockStatementNode>(n)) { scopeStack.push_back({{}, false}); return 0; }
            if (auto* cc = as<es::CatchClauseNode>(n)) {
                Scope s{{}, false};
                if (cc->_param) collectBinding(cc->_param, s.names);
                scopeStack.push_back(std::move(s));
                return 0;
            }
            if (auto* vd = as<es::VariableDeclarationNode>(n)) {
                bool isVar = labelStr(vd->_kind) == "var";
                Scope* target = &currentScope();
                if (isVar) {
                    for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it)
                        if (it->isFunction) { target = &*it; break; }
                }
                for (Node& d : vd->_declarations)
                    if (auto* dcl = as<es::VariableDeclaratorNode>(&d))
                        collectBinding(dcl->_id, target->names);
                return 0;
            }
            if (auto* cd = as<es::ClassDeclarationNode>(n)) {
                if (cd->_id) currentScope().names.insert(idName(cd->_id));
                return 0;
            }
            if (auto* ls = as<es::LabeledStatementNode>(n)) {
                if (auto* lid = as<es::IdentifierNode>(ls->_label)) currentScope().names.insert(labelStr(lid->_name));
                return 0;
            }
            if (auto* id = as<es::IdentifierNode>(n)) {
                std::string nm = labelStr(id->_name);
                if (!rootIdentifierName.empty() && nm == rootIdentifierName) return 0;
                // property key / member property handling
                if (auto* pr = as<es::PropertyNode>(parent)) {
                    if (pr->_key == n && !pr->_computed) return 0;
                }
                if (auto* md = as<es::MethodDefinitionNode>(parent)) {
                    if (md->_key == n && !md->_computed) return 0;
                }
                if (auto* me = as<es::MemberExpressionNode>(parent)) {
                    if (me->_property == n && !me->_computed) {
                        if (is<es::ThisExpressionNode>(me->_object)) return 0;
                        std::string full = memberToString(me);
                        if (full.empty()) return 0;
                        auto dvIt = declaredVariables.find(full);
                        if (dvIt != declaredVariables.end()) {
                            dvIt->second->dependents.insert(identifierName);
                            dependencies.insert(full);
                        } else if (auto* obj = as<es::IdentifierNode>(me->_object)) {
                            std::string baseName = labelStr(obj->_name);
                            auto bIt = declaredVariables.find(baseName);
                            bool declaredBase = bIt != declaredVariables.end();
                            if ((declaredBase || baseName == iifeParamName) &&
                                !isInScope(baseName) && !jsBuiltIns().count(baseName)) {
                                if (declaredBase) bIt->second->dependents.insert(identifierName);
                                dependencies.insert(full);
                                dependentsTracker[full].insert(identifierName);
                            }
                        }
                        return 0;
                    }
                }
                if (is<es::MetaPropertyNode>(parent)) return 0;
                if (isInScope(nm) || jsBuiltIns().count(nm)) return 0;
                dependencies.insert(nm);
                auto dvIt = declaredVariables.find(nm);
                if (dvIt != declaredVariables.end()) dvIt->second->dependents.insert(identifierName);
                else dependentsTracker[nm].insert(identifierName);
                return 0;
            }
            if (is<es::ForStatementNode>(n) || is<es::ForInStatementNode>(n) || is<es::ForOfStatementNode>(n)) {
                scopeStack.push_back({{}, false});
                return 0;
            }
            return 0;
        };
        auto leave = [&](Node* n) {
            if (isFunctionLike(n) || is<es::BlockStatementNode>(n) || is<es::CatchClauseNode>(n) ||
                is<es::ForStatementNode>(n) || is<es::ForInStatementNode>(n) || is<es::ForOfStatementNode>(n)) {
                if (scopeStack.size() > 1) scopeStack.pop_back();
            }
        };
        walkAst(rootNode, enter, leave);
        return dependencies;
    }

    bool areDependenciesResolved(const std::set<std::string>& deps, std::unordered_set<std::string>& seen) {
        if (deps.empty()) return true;
        for (const auto& dep : deps) {
            if (dep.empty()) continue;
            if (jsBuiltIns().count(dep)) continue;
            if (dep == iifeParamName) continue;
            if (seen.count(dep)) continue;
            auto it = declaredVariables.find(dep);
            if (it == declaredVariables.end()) return false;
            seen.insert(dep);
            if (!areDependenciesResolved(it->second->dependencies, seen)) return false;
        }
        return true;
    }

    void refreshExtraction(Extraction& st) {
        if (!st.node) { st.ready = false; return; }
        if (!st.collectDependencies) { st.ready = true; return; }
        if (!st.metadata) { st.ready = false; return; }
        std::unordered_set<std::string> seen;
        st.ready = areDependenciesResolved(st.metadata->dependencies, seen);
    }

    bool shouldStop() {
        bool hasTarget = false;
        for (auto& st : extractions) {
            hasTarget = true;
            if (!st.node) return false;
            if (!st.ready) return false;
        }
        return hasTarget;
    }

    bool onMatch(Node* node, VarMeta* metadata) {
        bool matched = false;
        for (auto& st : extractions) {
            if (!st.node) {
                if (is<es::VariableDeclaratorNode>(node) && !as<es::VariableDeclaratorNode>(node)->_init) continue;
                Node* result = st.match(node);
                if (!result) continue;
                st.node = node;
                matched = true;
                if (metadata) {
                    st.metadata = metadata;
                    st.matchContext = result;
                }
                refreshExtraction(st);
            } else if (st.node != node) {
                refreshExtraction(st);
                if (shouldStop()) return true;
            }
        }
        if (!matched) return false;
        return shouldStop();
    }

    // analyzeAst (JsAnalyzer.ts)
    void analyze() {
        es::BlockStatementNode* iifeBody = nullptr;
        for (Node& stmt : program->_body) {
            if (auto* es_ = as<es::ExpressionStatementNode>(&stmt)) {
                if (auto* call = as<es::CallExpressionNode>(es_->_expression)) {
                    if (auto* fn = as<es::FunctionExpressionNode>(call->_callee)) {
                        Node* firstParam = listLen(fn->_params) > 0 ? listAt(fn->_params, 0) : nullptr;
                        if (iifeParamName.empty()) {
                            if (auto* pid = as<es::IdentifierNode>(firstParam)) iifeParamName = labelStr(pid->_name);
                        }
                        if (auto* b = as<es::BlockStatementNode>(fn->_body)) { iifeBody = b; break; }
                    }
                }
            }
        }
        if (!iifeBody) return;

        for (Node& cur : iifeBody->_body) {
            if (auto* exprStmt = as<es::ExpressionStatementNode>(&cur)) {
                auto* assign = as<es::AssignmentExpressionNode>(exprStmt->_expression);
                if (!assign) continue;
                Node* left = assign->_left;
                Node* right = assign->_right;

                // detect  a.b = X.prototype
                if (auto* rm = as<es::MemberExpressionNode>(right)) {
                    if (!rm->_computed) {
                        if (auto* rp = as<es::IdentifierNode>(rm->_property)) {
                            if (labelStr(rp->_name) == "prototype") {
                                std::string protoSrc = memberToString(rm);
                                std::string aliasTarget = is<es::IdentifierNode>(left) ? idName(left) : memberToString(left);
                                if (!protoSrc.empty()) {
                                    std::string ownerKey = replaceAll(protoSrc, ".prototype", "");
                                    auto ownIt = declaredVariables.find(ownerKey);
                                    if (!aliasTarget.empty() && ownIt != declaredVariables.end()) {
                                        std::string aliasExpr = aliasTarget + ".";
                                        pendingAlias = {aliasExpr, ownIt->second};
                                        hasPendingAlias = true;
                                        ownIt->second->aliasBucket(aliasExpr);
                                    }
                                }
                            }
                        }
                    }
                }

                if (is<es::IdentifierNode>(left)) {
                    std::string ln = idName(left);
                    auto it = declaredVariables.find(ln);
                    if (it == declaredVariables.end()) continue;
                    VarMeta* existing = it->second;
                    // existingVariable.node.init = right (patch predeclared declarator)
                    if (auto* decl = as<es::VariableDeclaratorNode>(existing->node)) decl->_init = right;
                    if (needsDependencyAnalysis(right))
                        existing->dependencies = findDependencies(right, ln);
                    if (onMatch(existing->node, existing)) return;
                } else if (auto* lm = as<es::MemberExpressionNode>(left)) {
                    std::string memberName = memberToString(lm);
                    std::string activeAlias = hasPendingAlias ? pendingAlias.first : std::string{};

                    if (!activeAlias.empty() &&
                        (!memberName.empty() &&
                         (memberName.find(activeAlias) != std::string::npos ||
                          memberName == activeAlias.substr(0, activeAlias.size()-1)))) {
                        VarMeta* owner = pendingAlias.second;
                        if (owner) {
                            VarMeta* m = newMeta();
                            m->name = memberName;
                            m->node = &cur;
                            auto dtIt = dependentsTracker.find(memberName);
                            if (dtIt != dependentsTracker.end()) m->dependents = dtIt->second;
                            m->predeclared = false;
                            m->dependencies = findDependencies(right, memberName);
                            owner->aliasBucket(activeAlias)->push_back(m);
                        }
                    } else {
                        hasPendingAlias = false;
                    }

                    if (memberName.empty() || declaredVariables.count(memberName)) continue;

                    VarMeta* m = newMeta();
                    m->name = memberName;
                    m->node = &cur;
                    auto dtIt = dependentsTracker.find(memberName);
                    if (dtIt != dependentsTracker.end()) m->dependents = dtIt->second;
                    m->predeclared = false;
                    m->dependencies = findDependencies(right, memberName);

                    std::string baseName = memberBaseName(lm);
                    if (!baseName.empty() && baseName != memberName && baseName.rfind("this.", 0) != 0)
                        m->dependencies.insert(replaceAll(baseName, ".prototype", ""));

                    dependentsTracker.erase(memberName);
                    declaredVariables[memberName] = m;
                    if (onMatch(&cur, m)) return;
                }
            } else if (auto* varDecl = as<es::VariableDeclarationNode>(&cur)) {
                hasPendingAlias = false;
                std::string kind = labelStr(varDecl->_kind);
                for (Node& d : varDecl->_declarations) {
                    auto* decl = as<es::VariableDeclaratorNode>(&d);
                    if (!decl) continue;
                    auto* did = as<es::IdentifierNode>(decl->_id);
                    if (!did) continue;
                    std::string nm = labelStr(did->_name);

                    VarMeta* m = newMeta();
                    m->name = nm;
                    m->node = &d;
                    auto dtIt = dependentsTracker.find(nm);
                    if (dtIt != dependentsTracker.end()) m->dependents = dtIt->second;

                    Node* init = decl->_init;
                    if (!init && kind == "var") m->predeclared = true;
                    else if (init && needsDependencyAnalysis(init)) m->dependencies = findDependencies(init, nm);

                    dependentsTracker.erase(nm);
                    declaredVariables[nm] = m;
                    if (onMatch(&d, m)) return;
                }
            }
        }
    }
};

// ===========================================================================
// Matchers (matchers.ts)
// ===========================================================================
namespace {

Node* nsigMatcher(Analyzer& a, Node* node) {
    auto* decl = as<es::VariableDeclaratorNode>(node);
    if (!decl) return nullptr;
    auto* fn = as<es::FunctionExpressionNode>(decl->_init);
    if (!fn) return nullptr;
    if (listLen(fn->_params) < 3) return nullptr;
    auto params = listVec(fn->_params);
    Node* url = params[0];
    Node* sigName = params[1];
    Node* sigValue = params[2];
    if (!is<es::IdentifierNode>(url) || !is<es::AssignmentPatternNode>(sigName) || !is<es::AssignmentPatternNode>(sigValue))
        return nullptr;
    std::string urlName = idName(url);

    auto* body = as<es::BlockStatementNode>(fn->_body);
    if (!body) return nullptr;

    bool hasUrlCtor = false, hasSetAlr = false;
    for (Node& st : body->_body) {
        auto* exprStmt = as<es::ExpressionStatementNode>(&st);
        if (!exprStmt) continue;
        Node* expr = exprStmt->_expression;
        if (auto* asg = as<es::AssignmentExpressionNode>(expr)) {
            if (labelStr(asg->_operator) == "=" && is<es::IdentifierNode>(asg->_left) &&
                idName(asg->_left) == urlName) {
                if (auto* ne = as<es::NewExpressionNode>(asg->_right))
                    if (is<es::MemberExpressionNode>(ne->_callee)) hasUrlCtor = true;
            }
        }
        if (auto* call = as<es::CallExpressionNode>(expr)) {
            if (is<es::MemberExpressionNode>(call->_callee)) {
                auto args = listVec(call->_arguments);
                if (args.size() == 2 && isStringLiteralValue(args[0], "alr") && isStringLiteralValue(args[1], "yes"))
                    hasSetAlr = true;
            }
        }
    }
    if (!hasUrlCtor || !hasSetAlr) return nullptr;
    return node;
}

// timestampMatcher — returns the matched Property node (the matchContext).
Node* timestampMatcher(Analyzer& a, Node* node) {
    auto* decl = as<es::VariableDeclaratorNode>(node);
    if (!decl) return nullptr;
    auto* fn = as<es::FunctionExpressionNode>(decl->_init);
    if (!fn || !fn->_body) return nullptr;

    Node* found = nullptr;
    walkAst(fn->_body, [&](Node* inner, Node*) -> int {
        if (auto* obj = as<es::ObjectExpressionNode>(inner)) {
            for (Node& pr : obj->_properties) {
                if (auto* p = as<es::PropertyNode>(&pr)) {
                    if (auto* k = as<es::IdentifierNode>(p->_key)) {
                        if (labelStr(k->_name) == "signatureTimestamp") { found = &pr; return 2; }
                    }
                }
            }
        }
        return 0;
    });
    return found;
}

} // namespace

// ===========================================================================
// Extractor (JsExtractor.ts) — emits the self-contained decipher script.
// ===========================================================================
class Extractor {
public:
    Analyzer& a;
    explicit Extractor(Analyzer& an) : a(an) {}

    using SideEffectMode = int; // 0 = strict, 1 = loose
    const SideEffectMode STRICT = 0;

    bool areSafeArgs(es::NodeList& args) {
        for (Node& arg : args) {
            if (is<es::SpreadElementNode>(&arg)) return false;
            if (!isSafeInitializer(&arg)) return false;
        }
        return true;
    }

    bool isSafeInitializer(Node* n) {
        if (!n) return true;
        if (is<es::ClassExpressionNode>(n)) return true;
        if (isLiteral(n)) return true; // string/number/boolean/null/regex
        if (auto* tl = as<es::TemplateLiteralNode>(n)) {
            for (Node& e : tl->_expressions) if (!isSafeInitializer(&e)) return false;
            return true;
        }
        if (auto* ar = as<es::ArrayExpressionNode>(n)) {
            for (Node& el : ar->_elements) {
                if (is<es::SpreadElementNode>(&el)) return false;
                if (!isSafeInitializer(&el)) return false;
            }
            return true;
        }
        if (auto* ob = as<es::ObjectExpressionNode>(n)) {
            for (Node& pr : ob->_properties) {
                auto* p = as<es::PropertyNode>(&pr);
                if (!p) return false;
                if (p->_computed) return false;
                if (labelStr(p->_kind) != "init") return false;
                Node* v = p->_value;
                if (!v) return false;
                if (!(isFunctionLike(v) || isLiteral(v))) return false;
            }
            return true;
        }
        if (auto* call = as<es::CallExpressionNode>(n)) {
            if (auto* cid = as<es::IdentifierNode>(call->_callee)) {
                if (jsBuiltIns().count(labelStr(cid->_name))) return areSafeArgs(call->_arguments);
                return false;
            } else if (auto* cm = as<es::MemberExpressionNode>(call->_callee)) {
                if (!isSafeInitializer(cm->_object)) return false;
                // strict: property must be a builtin, non-computed
                std::string propName = as<es::IdentifierNode>(cm->_property) ? idName(cm->_property) : std::string{};
                if (cm->_computed || !jsBuiltIns().count(propName)) return false;
                return areSafeArgs(call->_arguments);
            }
            return false;
        }
        if (auto* ne = as<es::NewExpressionNode>(n)) {
            if (auto* cid = as<es::IdentifierNode>(ne->_callee))
                if (jsBuiltIns().count(labelStr(cid->_name))) return areSafeArgs(ne->_arguments);
            return false;
        }
        if (auto* un = as<es::UnaryExpressionNode>(n)) return isSafeInitializer(un->_argument);
        if (isFunctionLike(n) && !is<es::FunctionDeclarationNode>(n)) return true; // FunctionExpression/Arrow
        if (is<es::IdentifierNode>(n)) return true;
        if (auto* me = as<es::MemberExpressionNode>(n)) {
            // strict: allow a.b = c.prototype
            if (!me->_computed) {
                if (auto* p = as<es::IdentifierNode>(me->_property))
                    if (labelStr(p->_name) == "prototype") return true;
            }
            return false;
        }
        if (auto* le = as<es::LogicalExpressionNode>(n)) return isSafeInitializer(le->_left) && isSafeInitializer(le->_right);
        if (auto* be = as<es::BinaryExpressionNode>(n)) return isSafeInitializer(be->_left) && isSafeInitializer(be->_right);
        if (is<es::ConditionalExpressionNode>(n)) return false; // strict
        if (is<es::SequenceExpressionNode>(n)) return false;    // strict
        if (auto* asg = as<es::AssignmentExpressionNode>(n)) {
            if (auto* lm = as<es::MemberExpressionNode>(asg->_left)) {
                if (!lm->_computed) {
                    if (auto* obj = as<es::IdentifierNode>(lm->_object)) {
                        auto it = a.declaredVariables.find(labelStr(obj->_name));
                        if (it != a.declaredVariables.end()) {
                            auto* decl = as<es::VariableDeclaratorNode>(it->second->node);
                            if (decl && decl->_init) return isSafeInitializer(asg->_right);
                        }
                    }
                }
            } else if (is<es::IdentifierNode>(asg->_left)) {
                if (a.declaredVariables.count(idName(asg->_left))) return isSafeInitializer(asg->_right);
            }
            return false;
        }
        return false;
    }

    std::string getInitializerFallback(Node* init) {
        if (is<es::ObjectExpressionNode>(init) || is<es::NewExpressionNode>(init) ||
            is<es::MemberExpressionNode>(init) || is<es::LogicalExpressionNode>(init)) return "{}";
        if (is<es::ArrayExpressionNode>(init)) return "[]";
        return "undefined";
    }

    // renderNode (JsExtractor.ts) with disallowSideEffectInitializers = strict.
    std::string renderNode(Node* node, bool preDeclared) {
        Node* assignmentTarget = nullptr;
        if (auto* asg = as<es::AssignmentExpressionNode>(node)) assignmentTarget = node;
        else if (auto* es_ = as<es::ExpressionStatementNode>(node)) {
            if (is<es::AssignmentExpressionNode>(es_->_expression)) assignmentTarget = es_->_expression;
        }
        auto* asgN = as<es::AssignmentExpressionNode>(assignmentTarget);

        Node* init = nullptr;
        if (asgN && labelStr(asgN->_operator) == "=") init = asgN->_right;
        else if (auto* decl = as<es::VariableDeclaratorNode>(node)) init = decl->_init;

        bool forceRemove = init && !isSafeInitializer(init);
        std::string initializerFallback = init ? getInitializerFallback(init) : "undefined";
        std::string initSource = initializerFallback;

        if (!forceRemove && init) {
            if (!preDeclared && is<es::IdentifierNode>(init) && !a.declaredVariables.count(idName(init))) {
                initSource = initializerFallback;
            } else {
                Node* left = asgN ? asgN->_left : nullptr;
                bool isPrototypeAlias = false;
                if (auto* im = as<es::MemberExpressionNode>(init)) {
                    if (!im->_computed)
                        if (auto* p = as<es::IdentifierNode>(im->_property))
                            if (labelStr(p->_name) == "prototype") isPrototypeAlias = true;
                }
                if (!isPrototypeAlias && as<es::MemberExpressionNode>(left) && init) {
                    auto* lm = as<es::MemberExpressionNode>(left);
                    if (as<es::IdentifierNode>(lm->_object) &&
                        !isFunctionLike(init) && !is<es::LogicalExpressionNode>(init) && !is<es::ClassExpressionNode>(init)) {
                        return std::string(INDENT) + "// Skipped " + a.memberToString(lm) + " assignment.";
                    }
                }
                std::string s = a.nodeSrc(init);
                Analyzer::trim(s);
                // strip trailing ';'
                while (!s.empty() && (s.back() == ';' || s.back()==' ' || s.back()=='\n' || s.back()=='\t' || s.back()=='\r')) s.pop_back();
                initSource = s.empty() ? "undefined /* [extract] empty init */" : s;
            }
        }
        if (!forceRemove && init && is<es::SequenceExpressionNode>(init) && (initSource.empty() || initSource[0] != '('))
            initSource = "(" + initSource + ")";

        std::string idNameStr = "unknown";
        if (auto* decl = as<es::VariableDeclaratorNode>(node)) {
            if (auto* did = as<es::IdentifierNode>(decl->_id)) idNameStr = labelStr(did->_name);
        } else if (asgN) {
            if (is<es::IdentifierNode>(asgN->_left)) idNameStr = idName(asgN->_left);
            else { std::string m = a.memberToString(asgN->_left); Analyzer::trim(m); if (!m.empty()) idNameStr = m; }
        }

        std::string assignmentExpression = idNameStr + " = " + initSource + ";";
        if (auto* decl = as<es::VariableDeclaratorNode>(node)) {
            if (decl->_init && !preDeclared) return std::string(INDENT) + "var " + assignmentExpression;
        }
        return std::string(INDENT) + assignmentExpression;
    }

    // parseFunctionArguments / generateWrapper / createWrapperFunction (helpers.ts)
    std::vector<std::string> parseFunctionArguments(std::vector<Node*> args) {
        std::vector<std::string> params;
        bool hasInput = false;
        for (Node* arg : args) {
            if (auto* id = as<es::IdentifierNode>(arg)) {
                std::string nm = labelStr(id->_name);
                if (a.declaredVariables.count(nm)) params.push_back(nm);
                else params.push_back(nm);
            } else if (is<es::StringLiteralNode>(arg) || is<es::NumericLiteralNode>(arg)) {
                std::string s = a.nodeSrc(arg); Analyzer::trim(s);
                params.push_back(s);
            } else if (is<es::UnaryExpressionNode>(arg)) {
                std::string s = a.nodeSrc(arg); Analyzer::trim(s);
                if (!s.empty()) params.push_back(s);
            } else if (auto* ap = as<es::AssignmentPatternNode>(arg)) {
                if (auto* l = as<es::IdentifierNode>(ap->_left)) params.push_back(labelStr(l->_name));
            } else {
                if (!hasInput) { params.push_back("input"); hasInput = true; }
            }
        }
        return params;
    }
    std::string generateWrapper(const std::string& fn, const std::string& target,
                                const std::vector<std::string>& args, bool useNew) {
        std::string a1;
        for (size_t i = 0; i < args.size(); ++i) { if (i) a1 += ", "; a1 += args[i]; }
        std::string I = INDENT;
        return I + "function " + fn + "(" + a1 + ") {\n" +
               I + I + "return " + (useNew ? "new " : "") + target + "(" + a1 + ");\n" +
               I + "}";
    }
    std::string createWrapperFunction(const std::string& name, Node* node) {
        if (auto* call = as<es::CallExpressionNode>(node)) {
            if (auto* cid = as<es::IdentifierNode>(call->_callee)) {
                if (a.declaredVariables.count(labelStr(cid->_name)))
                    return generateWrapper(name, labelStr(cid->_name), parseFunctionArguments(listVec(call->_arguments)), false);
            }
        } else if (auto* decl = as<es::VariableDeclaratorNode>(node)) {
            if (auto* fe = as<es::FunctionExpressionNode>(decl->_init)) {
                if (auto* did = as<es::IdentifierNode>(decl->_id))
                    return generateWrapper(name, labelStr(did->_name), parseFunctionArguments(listVec(fe->_params)), false);
            }
        } else if (auto* ne = as<es::NewExpressionNode>(node)) {
            if (auto* cm = as<es::MemberExpressionNode>(ne->_callee)) {
                if (is<es::IdentifierNode>(cm->_object)) {
                    std::string target = a.memberToString(cm);
                    if (!target.empty())
                        return generateWrapper(name, target, parseFunctionArguments(listVec(ne->_arguments)), true);
                }
            }
        }
        return {};
    }

    // buildScript — only nsigFunction is emitted; the timestamp value is read
    // out separately (rawValueOnly), so no rawValues block is embedded.
    struct BuildOut { std::string output; bool exportedNsig = false; };

    BuildOut buildScript(const std::string& nsigName) {
        std::vector<Analyzer::Extraction*> matches;
        for (auto& e : a.extractions) if (e.node) matches.push_back(&e);

        OrderedSet seen;
        for (auto* e : matches) if (e->metadata) seen.add(e->metadata->name);

        std::vector<std::string> snippets;
        OrderedSet predeclared;
        // exported: friendlyName -> matchContext
        std::vector<std::pair<std::string, Node*>> exported;

        auto registerPredeclared = [&](const std::string& nm) {
            if (nm.empty() || nm.find('.') != std::string::npos) return;
            predeclared.add(nm);
        };

        std::function<void(Analyzer::VarMeta*)> visit = [&](Analyzer::VarMeta* metadata) {
            if (!metadata) return;
            for (const auto& dep : metadata->dependencies) {
                if (seen.has(dep)) continue;
                seen.add(dep);
                auto it = a.declaredVariables.find(dep);
                if (it == a.declaredVariables.end()) continue;
                Analyzer::VarMeta* depMeta = it->second;
                bool shouldPre = depMeta->predeclared;
                if (shouldPre) registerPredeclared(dep);
                visit(depMeta);
                snippets.push_back(renderNode(depMeta->node, shouldPre));
                for (auto& aliasPair : depMeta->prototypeAliases) {
                    for (auto* member : aliasPair.second) {
                        visit(member);
                        snippets.push_back(renderNode(member->node, shouldPre));
                    }
                }
            }
        };

        for (auto* ex : matches) {
            const std::string& fname = ex->friendlyName;
            bool skipEmit = (fname == "signatureTimestampVar"); // rawValueOnly
            if (!ex->metadata) {
                // timestamp with collectDependencies=false still has matchContext
                if (ex->matchContext && !fname.empty()) exported.emplace_back(fname, ex->matchContext);
                continue;
            }
            if (!skipEmit) snippets.push_back(std::string(INDENT) + "//#region --- start [" + fname + "] ---");
            bool shouldPre = ex->metadata->predeclared && !skipEmit;
            if (shouldPre) registerPredeclared(ex->metadata->name);
            if (ex->collectDependencies && !skipEmit) visit(ex->metadata);
            if (ex->matchContext && !fname.empty()) exported.emplace_back(fname, ex->matchContext);
            if (!skipEmit) {
                snippets.push_back(renderNode(ex->metadata->node, shouldPre));
                snippets.push_back(std::string(INDENT) + "//#endregion --- end [" + fname + "] ---\n");
            }
        }

        // Assemble output.
        std::string I = INDENT;
        std::string out;
        auto line = [&](const std::string& s) { out += s; out += "\n"; };

        line("const __jsExtractorGlobal = typeof globalThis !== 'undefined' ? globalThis :");
        line(I + "typeof self !== 'undefined' ? self :");
        line(I + "typeof window !== 'undefined' ? window :");
        line(I + "typeof global !== 'undefined' ? global : {};\n");

        line("const exportedVars = (function(" + a.iifeParamName + ") {");
        line(I + "const window = typeof __jsExtractorGlobal.window !== 'undefined' ? __jsExtractorGlobal.window : Object.create(null);");
        line(I + "const document = typeof __jsExtractorGlobal.document !== 'undefined' ? __jsExtractorGlobal.document : {};");
        line(I + "const self = typeof __jsExtractorGlobal.self !== 'undefined' ? __jsExtractorGlobal.self : window;\n");

        if (predeclared.size() > 0) {
            std::string pd;
            size_t i = 0;
            for (const auto& v : predeclared) { if (i++) pd += ", "; pd += v; }
            line(I + "var " + pd + ";\n");
        }

        for (size_t i = 0; i < snippets.size(); ++i) { out += snippets[i]; if (i + 1 < snippets.size()) out += "\n"; }
        out += "\n";

        std::vector<std::string> exportedVars;
        BuildOut result;
        for (auto& [friendlyName, node] : exported) {
            Node* fnNode = nullptr;
            if (auto* id = as<es::IdentifierNode>(node)) {
                auto it = a.declaredVariables.find(labelStr(id->_name));
                if (it != a.declaredVariables.end()) {
                    auto* decl = as<es::VariableDeclaratorNode>(it->second->node);
                    if (decl && is<es::FunctionExpressionNode>(decl->_init)) fnNode = it->second->node;
                }
            } else if (is<es::CallExpressionNode>(node) || is<es::NewExpressionNode>(node) || is<es::VariableDeclaratorNode>(node)) {
                fnNode = node;
            }
            if (fnNode) {
                std::string wrapper = createWrapperFunction(friendlyName, fnNode);
                if (!wrapper.empty()) { out += wrapper + "\n\n"; exportedVars.push_back(friendlyName); }
            }
        }

        std::string ev;
        for (size_t i = 0; i < exportedVars.size(); ++i) { if (i) ev += ", "; ev += exportedVars[i]; }
        line(I + "return { " + ev + " };");
        line("})({});\n");

        result.output = out;
        for (auto& e : exportedVars) if (e == nsigName) result.exportedNsig = true;
        return result;
    }
};

// ===========================================================================
// Chunker: lexical splitter + scope-aware closure selector.
//
// Parsing the whole 2.5 MB base.js into one AST arena costs ~72 MB regardless
// of build type. To stay well under a tight RAM budget we instead select just
// the nsig dependency closure (~1.2 MB of source) and feed only that to the
// analyzer/extractor. Faithful C++ port of tools/nsig_bundle/src/{pruner,selector}.ts.
// ===========================================================================
namespace chunk {

inline bool isIdStart(unsigned c){ return (c>=97&&c<=122)||(c>=65&&c<=90)||c==95||c==36; }
inline bool isIdPart(unsigned c){ return isIdStart(c)||(c>=48&&c<=57); }
inline bool isWs(unsigned c){ return c==32||c==9||c==10||c==13||c==12||c==11; }
inline unsigned cc(const std::string& s, size_t i){ return i<s.size()?(unsigned char)s[i]:0; }

const std::unordered_set<std::string>& regexKw(){
    static const std::unordered_set<std::string> s={"return","typeof","instanceof","in","of","new","delete","void","do","else","throw","case","yield","await"};
    return s;
}

size_t skipString(const std::string& src, size_t i){
    size_t n=src.size(); unsigned q=cc(src,i); i++;
    if(q==96){ // template literal
        while(i<n){ unsigned c=cc(src,i);
            if(c==92){ i+=2; continue; }
            if(c==96) return i+1;
            if(c==36&&cc(src,i+1)==123){ i+=2; int depth=1;
                while(i<n&&depth>0){ unsigned d=cc(src,i);
                    if(d==123)depth++; else if(d==125)depth--;
                    else if(d==34||d==39||d==96){ i=skipString(src,i); continue; }
                    else if(d==47&&cc(src,i+1)==47){ while(i<n&&cc(src,i)!=10)i++; continue; }
                    else if(d==47&&cc(src,i+1)==42){ i+=2; while(i<n&&!(cc(src,i)==42&&cc(src,i+1)==47))i++; i+=2; continue; }
                    i++;
                }
                continue;
            }
            i++;
        }
        return i;
    }
    while(i<n){ unsigned c=cc(src,i); if(c==92){i+=2;continue;} if(c==q)return i+1; i++; }
    return i;
}
size_t skipRegex(const std::string& src, size_t i){
    size_t n=src.size(); i++; bool inClass=false;
    while(i<n){ unsigned c=cc(src,i);
        if(c==92){i+=2;continue;}
        if(c==91)inClass=true; else if(c==93)inClass=false;
        else if(c==47&&!inClass){i++;break;}
        else if(c==10)break;
        i++;
    }
    while(i<n&&isIdPart(cc(src,i)))i++;
    return i;
}
inline bool regexAllowedCh(unsigned prev){
    if(prev==0)return true;
    if(isIdPart(prev))return false;
    if(prev==41||prev==93)return false;
    return true;
}
size_t matchDelim(const std::string& src, size_t open, size_t limit){
    unsigned openCh=cc(src,open);
    unsigned closeCh=openCh==40?41:openCh==91?93:125;
    size_t n=std::min(src.size(),limit); size_t i=open+1; int depth=1; unsigned prevSig=openCh;
    while(i<n){ unsigned c=cc(src,i);
        if(isWs(c)){i++;continue;}
        if(c==47){ unsigned c2=cc(src,i+1);
            if(c2==47){i+=2;while(i<n&&cc(src,i)!=10)i++;continue;}
            if(c2==42){i+=2;while(i<n&&!(cc(src,i)==42&&cc(src,i+1)==47))i++;i+=2;continue;}
            if(regexAllowedCh(prevSig)){i=skipRegex(src,i);prevSig=47;continue;}
            prevSig=47;i++;continue;
        }
        if(c==34||c==39||c==96){i=skipString(src,i);prevSig=34;continue;}
        if(c==40||c==91||c==123){i=matchDelim(src,i,limit);prevSig=41;continue;}
        if(c==41||c==93||c==125){i++;if(c==closeCh){depth--;if(depth==0)return i;}prevSig=c;continue;}
        prevSig=c;i++;
    }
    return i;
}
size_t matchBrace(const std::string& src, size_t open){
    size_t n=src.size(); size_t i=open+1; int depth=1; unsigned prevSig=123;
    while(i<n){ unsigned c=cc(src,i);
        if(isWs(c)){i++;continue;}
        if(c==47){ unsigned c2=cc(src,i+1);
            if(c2==47){i+=2;while(i<n&&cc(src,i)!=10)i++;continue;}
            if(c2==42){i+=2;while(i<n&&!(cc(src,i)==42&&cc(src,i+1)==47))i++;i+=2;continue;}
            if(regexAllowedCh(prevSig)){i=skipRegex(src,i);prevSig=47;continue;}
            prevSig=47;i++;continue;
        }
        if(c==34||c==39||c==96){i=skipString(src,i);prevSig=34;continue;}
        if(c==123){depth++;i++;prevSig=123;continue;}
        if(c==125){depth--;i++;if(depth==0)return i-1;prevSig=125;continue;}
        prevSig=c;i++;
    }
    return std::string::npos;
}

struct Iife { std::string param; size_t bodyStart; size_t bodyEnd; bool ok=false; };
Iife findIife(const std::string& src){
    Iife r; size_t n=src.size(); size_t i=0;
    while(i<n){ unsigned c=cc(src,i);
        if(c==47){ unsigned c2=cc(src,i+1);
            if(c2==47){i+=2;while(i<n&&cc(src,i)!=10)i++;continue;}
            if(c2==42){i+=2;while(i<n&&!(cc(src,i)==42&&cc(src,i+1)==47))i++;i+=2;continue;}
        }
        if(c==34||c==39||c==96){i=skipString(src,i);continue;}
        if(c==40&&src.compare(i+1,8,"function")==0){
            size_t j=i+1+8; while(j<n&&isWs(cc(src,j)))j++;
            if(isIdStart(cc(src,j))){ while(j<n&&isIdPart(cc(src,j)))j++; while(j<n&&isWs(cc(src,j)))j++; }
            if(cc(src,j)!=40){i++;continue;}
            j++; while(j<n&&isWs(cc(src,j)))j++;
            std::string param;
            if(isIdStart(cc(src,j))){ size_t s=j; while(j<n&&isIdPart(cc(src,j)))j++; param=src.substr(s,j-s); }
            while(j<n&&cc(src,j)!=41)j++; j++; while(j<n&&isWs(cc(src,j)))j++;
            if(cc(src,j)!=123){i++;continue;}
            size_t bodyStart=j+1; size_t bodyEnd=matchBrace(src,j);
            if(bodyEnd==std::string::npos)return r;
            r.param=param; r.bodyStart=bodyStart; r.bodyEnd=bodyEnd; r.ok=true; return r;
        }
        i++;
    }
    return r;
}

struct Region { size_t start,end; std::vector<std::string> decls; std::vector<std::string> bases; std::string varKind; };

// The parseable/emittable text of a region. Per-declarator var regions store
// only `NAME=INIT`, so re-attach the `var`/`let`/`const` keyword.
std::string regionText(const std::string& src, const Region& r){
    std::string body=src.substr(r.start,r.end-r.start);
    // trim
    size_t a=0,b=body.size(); while(a<b&&isWs((unsigned char)body[a]))a++; while(b>a&&isWs((unsigned char)body[b-1]))b--;
    body=body.substr(a,b-a);
    if(!r.varKind.empty())return r.varKind+" "+body;
    return body;
}

std::string readWord(const std::string& src,size_t i,size_t end){
    if(i>=end||!isIdStart(cc(src,i)))return {};
    size_t s=i; while(i<end&&isIdPart(cc(src,i)))i++; return src.substr(s,i-s);
}
void collectVarNames(const std::string& src,size_t i,size_t end,std::vector<std::string>& out){
    while(i<end){
        while(i<end&&isWs(cc(src,i)))i++;
        std::string name=readWord(src,i,end); if(!name.empty())out.push_back(name);
        while(i<end){ unsigned c=cc(src,i);
            if(isWs(c)){i++;continue;}
            if(c==47&&cc(src,i+1)==47){i+=2;while(i<end&&cc(src,i)!=10)i++;continue;}
            if(c==47&&cc(src,i+1)==42){i+=2;while(i<end&&!(cc(src,i)==42&&cc(src,i+1)==47))i++;i+=2;continue;}
            if(c==34||c==39||c==96){i=skipString(src,i);continue;}
            if(c==40||c==91||c==123){i=matchDelim(src,i,end);continue;}
            if(c==47){i=skipRegex(src,i);continue;}
            if(c==44){i++;break;}
            i++;
        }
    }
}
long findTopEq(const std::string& src,size_t s,size_t end){
    size_t i=s; unsigned prevSig=0;
    while(i<end){ unsigned c=cc(src,i);
        if(isWs(c)){i++;continue;}
        if(c==47&&cc(src,i+1)==47){i+=2;while(i<end&&cc(src,i)!=10)i++;continue;}
        if(c==47&&cc(src,i+1)==42){i+=2;while(i<end&&!(cc(src,i)==42&&cc(src,i+1)==47))i++;i+=2;continue;}
        if(c==34||c==39||c==96){i=skipString(src,i);prevSig=34;continue;}
        if(c==40||c==91||c==123){i=matchDelim(src,i,end);prevSig=41;continue;}
        if(c==47){ if(regexAllowedCh(prevSig)){i=skipRegex(src,i);prevSig=47;continue;} prevSig=47;i++;continue; }
        if(c==61){ unsigned next=cc(src,i+1);
            if(next==61){i+=2;prevSig=61;continue;}
            if(prevSig==33||prevSig==60||prevSig==62){i++;continue;}
            return (long)i;
        }
        prevSig=c;i++;
    }
    return -1;
}
bool makeRegion(const std::string& src,size_t start,size_t end,Region& out){
    size_t s=start;
    while(s<end){ unsigned c=cc(src,s);
        if(isWs(c)){s++;continue;}
        if(c==47&&cc(src,s+1)==47){s+=2;while(s<end&&cc(src,s)!=10)s++;continue;}
        if(c==47&&cc(src,s+1)==42){s+=2;while(s<end&&!(cc(src,s)==42&&cc(src,s+1)==47))s++;s+=2;continue;}
        break;
    }
    if(s>=end)return false;
    out.start=start; out.end=end;
    std::string word=readWord(src,s,end);
    if(word=="var"||word=="let"||word=="const"){
        collectVarNames(src,s+word.size(),end,out.decls);
    } else if(word=="function"){
        size_t j=s+8; while(j<end&&isWs(cc(src,j)))j++;
        std::string name=readWord(src,j,end); if(!name.empty())out.decls.push_back(name);
    } else {
        long eq=findTopEq(src,s,end);
        if(eq>0){
            std::string lhs=src.substr(s,eq-s);
            // trim
            size_t a=0,b=lhs.size(); while(a<b&&isWs((unsigned char)lhs[a]))a++; while(b>a&&isWs((unsigned char)lhs[b-1]))b--;
            lhs=lhs.substr(a,b-a);
            bool okName=!lhs.empty()&&isIdStart((unsigned char)lhs[0]);
            for(char ch:lhs){ unsigned uc=(unsigned char)ch;
                if(!(isIdPart(uc)||uc=='.'||uc=='['||uc==']'||uc=='\''||uc=='"'||uc==' ')){okName=false;break;} }
            if(okName){
                std::string member; for(char ch:lhs) if(!isWs((unsigned char)ch)) member+=ch;
                out.decls.push_back(member);
                size_t dot=member.find_first_of(".[");
                std::string bse=dot==std::string::npos?member:member.substr(0,dot);
                if(!bse.empty()&&bse!=member)out.bases.push_back(bse);
            }
        }
    }
    return true;
}
// Splits a `var/let/const a=..,b=..` statement into one region per declarator so
// the selector pulls dependencies of only the referenced declarator (matching
// how JsAnalyzer treats each declared variable separately). This is the main
// over-inclusion fix: minified base.js has huge multi-declarator var statements.
void pushStatement(const std::string& src,size_t start,size_t end,std::vector<Region>& regions){
    size_t s=start;
    while(s<end){ unsigned c=cc(src,s);
        if(isWs(c)){s++;continue;}
        if(c==47&&cc(src,s+1)==47){s+=2;while(s<end&&cc(src,s)!=10)s++;continue;}
        if(c==47&&cc(src,s+1)==42){s+=2;while(s<end&&!(cc(src,s)==42&&cc(src,s+1)==47))s++;s+=2;continue;}
        break;
    }
    if(s>=end)return;
    std::string word=readWord(src,s,end);
    if(word=="var"||word=="let"||word=="const"){
        size_t i=s+word.size();
        while(i<end){
            while(i<end&&isWs(cc(src,i)))i++;
            if(i>=end)break;
            size_t dStart=i;
            std::string name=readWord(src,i,end);
            int prev=1; // just consumed the declarator name (Value)
            // advance to the depth-0 comma (or end), tracking regex-vs-division.
            while(i<end){ unsigned c=cc(src,i);
                if(isWs(c)){i++;continue;}
                if(c==47&&cc(src,i+1)==47){i+=2;while(i<end&&cc(src,i)!=10)i++;continue;}
                if(c==47&&cc(src,i+1)==42){i+=2;while(i<end&&!(cc(src,i)==42&&cc(src,i+1)==47))i++;i+=2;continue;}
                if(c==34||c==39||c==96){i=skipString(src,i);prev=1;continue;}
                if(c==40||c==91||c==123){i=matchDelim(src,i,end);prev=1;continue;}
                if(c==47){ if(prev==0){i=skipRegex(src,i);prev=1;} else {i++;prev=0;} continue; }
                if(isIdStart(c)){ size_t w=i;i++; while(i<end&&isIdPart(cc(src,i)))i++; prev=regexKw().count(src.substr(w,i-w))?0:1; continue; }
                if(c>=48&&c<=57){ i++; while(i<end){unsigned d=cc(src,i); if(isIdPart(d)||d==46)i++;else break;} prev=1; continue; }
                if(c==44)break; // depth-0 comma -> next declarator
                prev=0; i++;
            }
            size_t dEnd=i;
            if(!name.empty()){
                Region r; r.start=dStart; r.end=dEnd; r.decls.push_back(name); r.varKind=word;
                regions.push_back(std::move(r));
            }
            if(i<end&&cc(src,i)==44)i++;
        }
        return;
    }
    Region r; if(makeRegion(src,start,end,r))regions.push_back(std::move(r));
}

std::vector<Region> splitRegions(const std::string& src,size_t bodyStart,size_t bodyEnd){
    std::vector<Region> regions;
    size_t i=bodyStart,stmtStart=bodyStart; int depth=0; int prev=0; // 0=RegexOk,1=Value
    auto push=[&](size_t end){ pushStatement(src,stmtStart,end,regions); };
    while(i<bodyEnd){ unsigned c=cc(src,i);
        if(isWs(c)){i++;continue;}
        if(c==47){ unsigned c2=cc(src,i+1);
            if(c2==47){i+=2;while(i<bodyEnd&&cc(src,i)!=10)i++;continue;}
            if(c2==42){i+=2;while(i<bodyEnd&&!(cc(src,i)==42&&cc(src,i+1)==47))i++;i+=2;continue;}
            if(prev==0){i=skipRegex(src,i);prev=1;continue;}
            prev=0;i++;continue;
        }
        if(c==34||c==39||c==96){i=skipString(src,i);prev=1;continue;}
        if(isIdStart(c)){ size_t s=i;i++; while(i<bodyEnd&&isIdPart(cc(src,i)))i++;
            std::string word=src.substr(s,i-s); prev=regexKw().count(word)?0:1; continue; }
        if(c>=48&&c<=57){ i++; while(i<bodyEnd){unsigned d=cc(src,i); if(isIdPart(d)||d==46)i++;else break;} prev=1; continue; }
        if(c==40||c==91||c==123){depth++;i++;prev=0;continue;}
        if(c==41||c==93){if(depth>0)depth--;i++;prev=1;continue;}
        if(c==125){ if(depth>0)depth--; i++;
            if(depth==0){
                size_t k=i;
                while(k<bodyEnd){ unsigned d=cc(src,k);
                    if(isWs(d)){k++;continue;}
                    if(d==47&&cc(src,k+1)==47){k+=2;while(k<bodyEnd&&cc(src,k)!=10)k++;continue;}
                    if(d==47&&cc(src,k+1)==42){k+=2;while(k<bodyEnd&&!(cc(src,k)==42&&cc(src,k+1)==47))k++;k+=2;continue;}
                    break;
                }
                unsigned nx=k<bodyEnd?cc(src,k):0;
                bool cont = nx==59||nx==41||nx==93||nx==125||nx==44||nx==46||nx==40||nx==91||
                    nx==42||nx==43||nx==45||nx==37||nx==60||nx==62||nx==61||nx==33||
                    nx==38||nx==124||nx==94||nx==63||nx==58||nx==47;
                if(!cont){ push(i); stmtStart=i; }
            }
            prev=0; continue;
        }
        if(c==59){ if(depth==0){ push(i); i++; stmtStart=i; prev=0; continue; } i++; prev=0; continue; }
        prev=0; i++;
    }
    push(bodyEnd);
    return regions;
}

// --- selector (scope-aware free vars + closure BFS) ---

std::string memberChain(Node* node){
    std::vector<std::string> segs; Node* cur=node;
    while(auto* m=as<es::MemberExpressionNode>(cur)){
        if(m->_computed)return {};
        auto* p=as<es::IdentifierNode>(m->_property); if(!p)return {};
        segs.push_back(labelStr(p->_name)); cur=m->_object;
    }
    if(auto* id=as<es::IdentifierNode>(cur)){
        std::string out=labelStr(id->_name);
        for(auto it=segs.rbegin();it!=segs.rend();++it){ out+="."; out+=*it; }
        return out;
    }
    return {};
}

struct Refs { std::unordered_set<std::string> ids; std::unordered_set<std::string> members; };
void collectFreeVars(Node* root, Refs& refs){
    struct Scope{ std::unordered_set<std::string> names; bool isFunction; };
    std::vector<Scope> stack; stack.push_back({{},false});
    auto isInScope=[&](const std::string& nm){ for(auto it=stack.rbegin();it!=stack.rend();++it) if(it->names.count(nm))return true; return false; };
    std::function<void(Node*,std::unordered_set<std::string>&)> bind=[&](Node* pat,std::unordered_set<std::string>& t){
        if(!pat)return;
        if(auto* id=as<es::IdentifierNode>(pat)){t.insert(labelStr(id->_name));return;}
        if(auto* op=as<es::ObjectPatternNode>(pat)){ for(Node& pr:op->_properties){ if(auto* re=as<es::RestElementNode>(&pr))bind(re->_argument,t); else if(auto* p=as<es::PropertyNode>(&pr))bind(p->_value,t);} return; }
        if(auto* ap=as<es::ArrayPatternNode>(pat)){ for(Node& el:ap->_elements)bind(&el,t); return; }
        if(auto* re=as<es::RestElementNode>(pat)){bind(re->_argument,t);return;}
        if(auto* asg=as<es::AssignmentPatternNode>(pat)){bind(asg->_left,t);return;}
    };
    auto enter=[&](Node* n,Node* parent)->int{
        if(auto* fe=as<es::FunctionExpressionNode>(n)){ Scope fs{{},true}; if(fe->_id)fs.names.insert(idName(fe->_id)); for(Node& p:fe->_params)bind(&p,fs.names); stack.push_back(std::move(fs)); return 0; }
        if(auto* ar=as<es::ArrowFunctionExpressionNode>(n)){ Scope fs{{},true}; for(Node& p:ar->_params)bind(&p,fs.names); stack.push_back(std::move(fs)); return 0; }
        if(auto* fd=as<es::FunctionDeclarationNode>(n)){ if(fd->_id)stack.back().names.insert(idName(fd->_id)); Scope fs{{},true}; for(Node& p:fd->_params)bind(&p,fs.names); stack.push_back(std::move(fs)); return 0; }
        if(is<es::BlockStatementNode>(n)){ stack.push_back({{},false}); return 0; }
        if(auto* cc_=as<es::CatchClauseNode>(n)){ Scope s{{},false}; if(cc_->_param)bind(cc_->_param,s.names); stack.push_back(std::move(s)); return 0; }
        if(auto* vd=as<es::VariableDeclarationNode>(n)){ bool isVar=labelStr(vd->_kind)=="var"; Scope* tgt=&stack.back();
            if(isVar){ for(auto it=stack.rbegin();it!=stack.rend();++it) if(it->isFunction){tgt=&*it;break;} }
            for(Node& d:vd->_declarations) if(auto* dcl=as<es::VariableDeclaratorNode>(&d)) bind(dcl->_id,tgt->names); return 0; }
        if(auto* cd=as<es::ClassDeclarationNode>(n)){ if(cd->_id)stack.back().names.insert(idName(cd->_id)); return 0; }
        if(auto* ls=as<es::LabeledStatementNode>(n)){ if(auto* l=as<es::IdentifierNode>(ls->_label))stack.back().names.insert(labelStr(l->_name)); return 0; }
        if(auto* me=as<es::MemberExpressionNode>(n)){ std::string ch=memberChain(n); if(!ch.empty()){ std::string bse=ch.substr(0,ch.find('.')); if(!isInScope(bse)&&!jsBuiltIns().count(bse))refs.members.insert(ch);} return 0; }
        if(auto* id=as<es::IdentifierNode>(n)){
            if(auto* pr=as<es::PropertyNode>(parent)){ if(pr->_key==n&&!pr->_computed)return 0; }
            if(auto* md=as<es::MethodDefinitionNode>(parent)){ if(md->_key==n&&!md->_computed)return 0; }
            if(auto* m=as<es::MemberExpressionNode>(parent)){ if(m->_property==n&&!m->_computed)return 0; }
            if(is<es::MetaPropertyNode>(parent))return 0;
            std::string nm=labelStr(id->_name);
            if(isInScope(nm)||jsBuiltIns().count(nm))return 0;
            refs.ids.insert(nm); return 0;
        }
        if(is<es::ForStatementNode>(n)||is<es::ForInStatementNode>(n)||is<es::ForOfStatementNode>(n)){ stack.push_back({{},false}); return 0; }
        return 0;
    };
    auto leave=[&](Node* n){
        if(isFunctionLike(n)||is<es::BlockStatementNode>(n)||is<es::CatchClauseNode>(n)||
           is<es::ForStatementNode>(n)||is<es::ForInStatementNode>(n)||is<es::ForOfStatementNode>(n)){
            if(stack.size()>1)stack.pop_back();
        }
    };
    walkAst(root, enter, leave);
}

// Detect `[var ] Alias = Base.prototype`. Alias may be a member (e.g. g.r).
bool matchAlias(const std::string& text, std::string& alias, std::string& base){
    size_t i=0,n=text.size();
    while(i<n&&isWs((unsigned char)text[i]))i++;
    if(text.compare(i,4,"var ")==0)i+=4;
    while(i<n&&isWs((unsigned char)text[i]))i++;
    size_t as0=i; while(i<n){ unsigned c=(unsigned char)text[i]; if(isIdPart(c)||c=='.')i++; else break; }
    if(i==as0)return false; std::string a=text.substr(as0,i-as0);
    while(i<n&&isWs((unsigned char)text[i]))i++;
    if(i>=n||text[i]!='=')return false; i++;
    while(i<n&&isWs((unsigned char)text[i]))i++;
    size_t bs0=i; while(i<n){ unsigned c=(unsigned char)text[i]; if(isIdPart(c)||c=='.')i++; else break; }
    std::string chain=text.substr(bs0,i-bs0);
    while(i<n&&isWs((unsigned char)text[i]))i++;
    if(chain.size()<11||chain.compare(chain.size()-10,10,".prototype")!=0)return false;
    if(i!=n)return false;
    alias=a; base=chain.substr(0,chain.size()-10);
    return true;
}

// True when the extractor's strict isSafeInitializer would *definitely* reject
// `init` (independent of declared-variable state), i.e. the value gets stubbed.
bool isDefinitelyStripped(Node* init){
    if(!init)return false;
    if(auto* call=as<es::CallExpressionNode>(init)){
        if(auto* id=as<es::IdentifierNode>(call->_callee)) return !jsBuiltIns().count(labelStr(id->_name));
        if(auto* m=as<es::MemberExpressionNode>(call->_callee)){
            std::string prop=(!m->_computed&&as<es::IdentifierNode>(m->_property))?idName(m->_property):std::string{};
            return m->_computed || !jsBuiltIns().count(prop); // non-builtin method -> stripped
        }
        return false;
    }
    if(auto* ne=as<es::NewExpressionNode>(init)){
        if(auto* id=as<es::IdentifierNode>(ne->_callee)) return !jsBuiltIns().count(labelStr(id->_name));
        if(is<es::MemberExpressionNode>(ne->_callee)) return true;
        return false;
    }
    if(auto* me=as<es::MemberExpressionNode>(init)){
        if(!me->_computed) if(auto* p=as<es::IdentifierNode>(me->_property)) if(labelStr(p->_name)=="prototype") return false;
        return true;
    }
    return false;
}
// Matches JsExtractor.getInitializerFallback.
const char* fallbackText(Node* init){
    if(is<es::ObjectExpressionNode>(init)||is<es::NewExpressionNode>(init)||
       is<es::MemberExpressionNode>(init)||is<es::LogicalExpressionNode>(init)) return "{}";
    if(is<es::ArrayExpressionNode>(init)) return "[]";
    return "undefined";
}

// Parses a region in isolation. Fills `refs` with its free vars. If the region
// is a single `var X = <stripped-init>` declaration, returns true and sets
// `stub` to the equivalent stubbed source (so the reduced program never parses
// the stripped initializer, and the caller skips following its dependencies).
bool processRegion(const std::string& regionSrc, Refs& refs, std::string& stub){
    auto ctx=std::make_shared<hermes::Context>();
    ctx->getSourceErrorManager().setDiagHandler(
        [](const llvh::SMDiagnostic&, void*){}, nullptr);
    auto buf=llvh::MemoryBuffer::getMemBuffer(regionSrc,"r.js");
    auto id=ctx->getSourceErrorManager().addNewSourceBuffer(std::move(buf));
    hermes::parser::JSParser parser(*ctx,id,hermes::parser::FullParse);
    auto parsed=parser.parse();
    if(!parsed)return false;

    // A single top-level statement whose value the extractor strips can be
    // replaced by a stub so the reduced program never parses the stripped init.
    es::NodeList& body=(*parsed)->_body;
    size_t bn=0; Node* only=nullptr; for(Node& s:body){ only=&s; if(++bn>1)break; }
    if(bn==1){
        // `var X = <stripped>` -> `var X = <fallback>;`
        if(auto* vd=as<es::VariableDeclarationNode>(only)){
            size_t dn=0; Node* d0=nullptr; for(Node& d:vd->_declarations){ d0=&d; if(++dn>1)break; }
            if(dn==1){
                if(auto* decl=as<es::VariableDeclaratorNode>(d0)){
                    if(auto* did=as<es::IdentifierNode>(decl->_id)){
                        if(decl->_init && isDefinitelyStripped(decl->_init)){
                            stub="var "+labelStr(did->_name)+" = "+fallbackText(decl->_init)+";";
                            return true;
                        }
                    }
                }
            }
        }
        // `A.b = <stripped>` -> omitted (the extractor emits only a skip comment).
        if(auto* es_=as<es::ExpressionStatementNode>(only)){
            if(auto* asg=as<es::AssignmentExpressionNode>(es_->_expression)){
                if(labelStr(asg->_operator)=="=" && is<es::MemberExpressionNode>(asg->_left) &&
                   isDefinitelyStripped(asg->_right)){
                    stub=""; // skip entirely
                    return true;
                }
            }
        }
    }
    collectFreeVars(*parsed, refs);
    return false;
}

std::string trimCopy(const std::string& s){
    size_t a=0,b=s.size(); while(a<b&&isWs((unsigned char)s[a]))a++; while(b>a&&isWs((unsigned char)s[b-1]))b--;
    return s.substr(a,b-a);
}

// True if `fn` is the nsig decipher function (mirrors matchers.ts nsigMatcher's
// FunctionExpression checks), accepting both the `var X=function` and predeclared
// `X=function` forms.
bool functionIsNsig(es::FunctionExpressionNode* fn){
    if(!fn)return false;
    if(listLen(fn->_params)<3)return false;
    auto params=listVec(fn->_params);
    if(!is<es::IdentifierNode>(params[0])||!is<es::AssignmentPatternNode>(params[1])||!is<es::AssignmentPatternNode>(params[2]))return false;
    std::string urlName=idName(params[0]);
    auto* body=as<es::BlockStatementNode>(fn->_body); if(!body)return false;
    bool hasUrlCtor=false,hasSetAlr=false;
    for(Node& st:body->_body){
        auto* es_=as<es::ExpressionStatementNode>(&st); if(!es_)continue;
        Node* expr=es_->_expression;
        if(auto* asg=as<es::AssignmentExpressionNode>(expr)){
            if(labelStr(asg->_operator)=="="&&is<es::IdentifierNode>(asg->_left)&&idName(asg->_left)==urlName)
                if(auto* ne=as<es::NewExpressionNode>(asg->_right)) if(is<es::MemberExpressionNode>(ne->_callee))hasUrlCtor=true;
        }
        if(auto* call=as<es::CallExpressionNode>(expr)){
            if(is<es::MemberExpressionNode>(call->_callee)){
                auto args=listVec(call->_arguments);
                if(args.size()==2&&isStringLiteralValue(args[0],"alr")&&isStringLiteralValue(args[1],"yes"))hasSetAlr=true;
            }
        }
    }
    return hasUrlCtor&&hasSetAlr;
}

// Returns the FunctionExpression init of a top-level `var X=fn` / `X=fn` region.
es::FunctionExpressionNode* topLevelFunctionInit(es::ProgramNode* prog){
    for(Node& s:prog->_body){
        if(auto* vd=as<es::VariableDeclarationNode>(&s))
            for(Node& d:vd->_declarations)
                if(auto* decl=as<es::VariableDeclaratorNode>(&d))
                    if(auto* fn=as<es::FunctionExpressionNode>(decl->_init))return fn;
        if(auto* es_=as<es::ExpressionStatementNode>(&s))
            if(auto* asg=as<es::AssignmentExpressionNode>(es_->_expression))
                if(auto* fn=as<es::FunctionExpressionNode>(asg->_right))return fn;
    }
    return nullptr;
}

// Precise seed classification: parse the candidate region and check the matchers.
enum class SeedKind { None, Nsig, Timestamp };
SeedKind classifySeed(const std::string& regionSrc){
    auto ctx=std::make_shared<hermes::Context>();
    ctx->getSourceErrorManager().setDiagHandler([](const llvh::SMDiagnostic&,void*){},nullptr);
    auto buf=llvh::MemoryBuffer::getMemBuffer(regionSrc,"r.js");
    auto id=ctx->getSourceErrorManager().addNewSourceBuffer(std::move(buf));
    hermes::parser::JSParser parser(*ctx,id,hermes::parser::FullParse);
    auto parsed=parser.parse(); if(!parsed)return SeedKind::None;
    es::FunctionExpressionNode* fn=topLevelFunctionInit(*parsed);
    if(functionIsNsig(fn))return SeedKind::Nsig;
    // timestamp: any function whose body has an ObjectExpression with signatureTimestamp
    bool ts=false;
    walkAst(*parsed,[&](Node* n,Node*)->int{
        if(auto* obj=as<es::ObjectExpressionNode>(n)){
            for(Node& pr:obj->_properties) if(auto* p=as<es::PropertyNode>(&pr))
                if(auto* k=as<es::IdentifierNode>(p->_key)) if(labelStr(k->_name)=="signatureTimestamp"){ts=true;return 2;}
        }
        return 0;
    });
    return ts?SeedKind::Timestamp:SeedKind::None;
}

// Produces the reduced program containing the nsig/timestamp closure.
std::string selectClosure(const std::string& src){
    Iife iife=findIife(src);
    if(!iife.ok)return src;
    std::vector<Region> regions=splitRegions(src,iife.bodyStart,iife.bodyEnd);

    // name/base indexes
    std::unordered_map<std::string,std::vector<int>> nameToRegions, baseToRegions;
    auto addIdx=[&](std::unordered_map<std::string,std::vector<int>>& m,const std::string& k,int idx){ m[k].push_back(idx); };
    for(int r=0;r<(int)regions.size();r++){
        for(auto& d:regions[r].decls){ addIdx(nameToRegions,d,r); std::string bse=d.substr(0,d.find_first_of(".[")); if(!bse.empty())addIdx(baseToRegions,bse,r); }
        for(auto& b:regions[r].bases)addIdx(baseToRegions,b,r);
    }

    // prototype-alias attribution
    struct Assign{ int idx; std::string base; };
    std::unordered_map<std::string,std::vector<Assign>> aliasAssigns;
    std::unordered_set<std::string> aliasExprs;
    for(int r=0;r<(int)regions.size();r++){
        std::string text=regionText(src,regions[r]);
        if(text.find(".prototype")==std::string::npos)continue;
        std::string a,b; if(matchAlias(text,a,b)){ aliasExprs.insert(a); aliasAssigns[a].push_back({r,b}); }
    }
    std::unordered_map<std::string,std::vector<int>> ownerAssignRegions, ownerMemberRegions;
    if(!aliasExprs.empty()){
        for(auto& kv:aliasAssigns) for(auto& a:kv.second) ownerAssignRegions[a.base].push_back(a.idx);
        for(int r=0;r<(int)regions.size();r++){
            for(auto& d:regions[r].decls){
                for(auto& ae:aliasExprs){
                    if(d.size()>ae.size()&&d.compare(0,ae.size(),ae)==0&&d[ae.size()]=='.'){
                        auto& assigns=aliasAssigns[ae]; std::string owner;
                        for(int k=(int)assigns.size()-1;k>=0;k--){ if(assigns[k].idx<r){owner=assigns[k].base;break;} }
                        if(!owner.empty())ownerMemberRegions[owner].push_back(r);
                        break;
                    }
                }
            }
        }
    }

    std::unordered_set<int> kept; std::vector<int> worklist;
    auto includeRegions=[&](const std::vector<int>* v){ if(!v)return; for(int idx:*v) if(kept.insert(idx).second)worklist.push_back(idx); };
    auto get=[&](std::unordered_map<std::string,std::vector<int>>& m,const std::string& k)->const std::vector<int>*{ auto it=m.find(k); return it==m.end()?nullptr:&it->second; };

    // Seeds: substring pre-filter, then confirm with a precise parse+match so
    // false positives (regions that merely contain the substrings) don't drag in
    // their whole dependency trees — the dominant source of over-inclusion.
    for(int r=0;r<(int)regions.size();r++){
        std::string text=regionText(src,regions[r]);
        bool candNsig=text.find("alr")!=std::string::npos&&text.find("yes")!=std::string::npos;
        bool candTs=text.find("signatureTimestamp")!=std::string::npos;
        if(!candNsig&&!candTs)continue;
        SeedKind k=classifySeed(text);
        if(k==SeedKind::Nsig){
            // nsig: include and follow its dependency tree.
            if(kept.insert(r).second)worklist.push_back(r);
        } else if(k==SeedKind::Timestamp){
            // timestamp: extraction uses collectDependencies=false — only the sts
            // value is read from the region; its deps are not part of the closure.
            kept.insert(r);
        }
    }

    auto includeAliasesOf=[&](const std::string& key){ includeRegions(get(ownerAssignRegions,key)); includeRegions(get(ownerMemberRegions,key)); };
    std::function<void(const std::string&)> includeIdentifier=[&](const std::string& nm){ includeRegions(get(nameToRegions,nm)); includeAliasesOf(nm); };
    auto includeMember=[&](const std::string& chain){
        std::string cur=chain;
        for(;;){ auto* ex=get(nameToRegions,cur); if(ex){ includeRegions(ex); includeAliasesOf(cur); return; }
            size_t dot=cur.rfind('.'); if(dot==std::string::npos||dot==0)break; cur=cur.substr(0,dot); }
        includeIdentifier(chain.substr(0,chain.find('.')));
    };

    std::unordered_map<int,std::string> stubs; // regionIdx -> stubbed source
    while(!worklist.empty()){
        int r=worklist.back(); worklist.pop_back();
        std::string text=regionText(src,regions[r]);
        if(text.empty())continue;
        Refs refs; std::string stub;
        bool stubbed=processRegion(text,refs,stub);
        if(stubbed){ stubs[r]=stub; continue; } // value is stripped; don't follow deps
        for(auto& ch:refs.members)includeMember(ch);
        for(auto& nm:refs.ids){ if(nm==iife.param)continue; includeIdentifier(nm); }
    }

    if(std::getenv("EPO_CHUNK_DBG"))std::fprintf(stderr,"[chunk] regions=%zu kept=%zu stubs=%zu\n",regions.size(),kept.size(),stubs.size());
    std::vector<int> sorted(kept.begin(),kept.end());
    std::sort(sorted.begin(),sorted.end());
    std::string out; out.reserve(1<<19);
    out+="(function("; out+=iife.param; out+="){var window=this;\n";
    for(int idx:sorted){
        auto it=stubs.find(idx);
        if(it!=stubs.end()){ if(!it->second.empty()){ out+=it->second; out+="\n"; } } // empty stub = omit
        else { out+=regionText(src,regions[idx]); out+=";\n"; }
    }
    out+="})(this);\n";
    return out;
}

} // namespace chunk

// ===========================================================================
// Public entry
// ===========================================================================
result extract_from_source(const std::string& source_js) {
    result r;
    auto context = std::make_shared<hermes::Context>();
    auto buf = llvh::MemoryBuffer::getMemBuffer(source_js, "base.js");
    auto id = context->getSourceErrorManager().addNewSourceBuffer(std::move(buf));
    const char* base = context->getSourceErrorManager().getSourceBuffer(id)->getBufferStart();

    hermes::parser::JSParser parser(*context, id, hermes::parser::FullParse);
    auto parsed = parser.parse();
    if (!parsed) { r.error = "parse failed"; return r; }

    Analyzer analyzer(source_js, base, *parsed);
    analyzer.extractions.push_back({"nsigFunction",
        [&](Node* n) { return nsigMatcher(analyzer, n); }, true});
    analyzer.extractions.push_back({"signatureTimestampVar",
        [&](Node* n) { return timestampMatcher(analyzer, n); }, false});

    analyzer.analyze();

    // signature timestamp from the matched Property node's value.
    for (auto& e : analyzer.extractions) {
        if (e.friendlyName == "signatureTimestampVar" && e.matchContext) {
            if (auto* p = as<es::PropertyNode>(e.matchContext)) {
                std::string v = analyzer.nodeSrc(p->_value);
                Analyzer::trim(v);
                try { r.sts = std::stoi(v); } catch (...) { r.sts = 0; }
            }
        }
    }

    Extractor extractor(analyzer);
    auto built = extractor.buildScript("nsigFunction");
    r.output = built.output;
    r.ok = built.exportedNsig && !built.output.empty();
    if (!r.ok && r.error.empty()) r.error = "failed to extract nsig decipher function";
    return r;
}

result extract(const std::string& player_js) {
    // Select just the nsig dependency closure so the analyzer parses ~1.2 MB
    // instead of the full 2.5 MB (keeps the AST arena small). Falls back to the
    // whole file if the reduced source somehow fails to yield the function.
    std::string reduced = chunk::selectClosure(player_js);
    result r = extract_from_source(reduced);
    if (!r.ok) {
        result full = extract_from_source(player_js);
        if (full.ok) return full;
    }
    return r;
}

} // namespace epotoken::nsig_extract
