#include "v8_runner.h"

#include "bgutils_bundle.h"
#include "bg_full_bundle.h"
#include "constants.h"
#include "http_client.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <libplatform/libplatform.h>
#include <v8.h>

// Suppress warn_unused_result from v8::Maybe return values on Set/Resolve/Reject.
// Every call site is fire-and-forget inside a sandboxed JS environment.
#if defined(__clang__)
#  pragma clang diagnostic ignored "-Wunused-result"
#elif defined(__GNUC__)
#  pragma GCC diagnostic ignored "-Wunused-result"
#endif

namespace epotoken::v8_runner {

// ---------------------------------------------------------------------------
// V8 platform (singleton)
// ---------------------------------------------------------------------------

static std::unique_ptr<v8::Platform> g_platform;
static std::once_flag               g_init_flag;

void init_v8(const char* exec_path) {
    std::call_once(g_init_flag, [exec_path]() {
        v8::V8::InitializeICUDefaultLocation(exec_path);
        v8::V8::InitializeExternalStartupData(exec_path);
        g_platform = v8::platform::NewDefaultPlatform();
        v8::V8::InitializePlatform(g_platform.get());
        v8::V8::Initialize();
    });
}

// ---------------------------------------------------------------------------
// Timer queue (setTimeout / setInterval)
// ---------------------------------------------------------------------------

struct timer_entry {
    int64_t id;
    std::chrono::steady_clock::time_point fire_at;
    bool     repeat;
    int64_t  interval_ms;
    v8::Global<v8::Function> callback;
};

// ---------------------------------------------------------------------------
// Runner struct
// ---------------------------------------------------------------------------

struct runner {
    v8::Isolate*                      isolate   = nullptr;
    v8::ArrayBuffer::Allocator*       allocator = nullptr;
    v8::Global<v8::Context>           context;

    std::vector<timer_entry> timers;
    int64_t next_timer_id = 1;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static v8::Local<v8::String> v8str(v8::Isolate* iso, const char* s) {
    return v8::String::NewFromUtf8(iso, s, v8::NewStringType::kNormal).ToLocalChecked();
}
static v8::Local<v8::String> v8str(v8::Isolate* iso, const std::string& s) {
    return v8::String::NewFromUtf8(iso, s.c_str(), v8::NewStringType::kNormal,
                                   static_cast<int>(s.size())).ToLocalChecked();
}
static std::string to_std(v8::Isolate* iso, v8::Local<v8::Value> val) {
    v8::String::Utf8Value utf8(iso, val);
    return *utf8 ? std::string(*utf8, utf8.length()) : std::string{};
}

static v8::MaybeLocal<v8::Value> run_script(runner* r, const std::string& src,
                                             const std::string& name = "<epo>") {
    auto iso = r->isolate;
    auto ctx = r->context.Get(iso);
    v8::Context::Scope ctx_scope(ctx);
    v8::TryCatch try_catch(iso);
    v8::ScriptOrigin origin(v8str(iso, name));
    auto script = v8::Script::Compile(ctx, v8str(iso, src), &origin);
    if (script.IsEmpty()) return {};
    return script.ToLocalChecked()->Run(ctx);
}

// Returns nullptr on failure; caller must check try_catch.
static v8::MaybeLocal<v8::Value> eval(runner* r, const std::string& src) {
    return run_script(r, src);
}

// ---------------------------------------------------------------------------
// setTimeout / setInterval / clearTimeout / clearInterval
// ---------------------------------------------------------------------------

static runner* runner_from_data(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // runner pointer was packed into a BigInt to avoid v8::External's sandbox tag requirement.
    uint64_t ptr_val = args.Data().As<v8::BigInt>()->Uint64Value();
    return reinterpret_cast<runner*>(static_cast<uintptr_t>(ptr_val));
}

static void js_set_timeout(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* r   = runner_from_data(args);
    auto  iso = args.GetIsolate();
    if (args.Length() < 1 || !args[0]->IsFunction()) {
        args.GetReturnValue().Set(v8::Integer::New(iso, 0));
        return;
    }
    const int64_t delay_ms = args.Length() >= 2 ? args[1]->IntegerValue(
        iso->GetCurrentContext()).FromMaybe(0) : 0;

    timer_entry entry;
    entry.id          = r->next_timer_id++;
    entry.fire_at     = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(delay_ms < 0 ? 0 : delay_ms);
    entry.repeat      = false;
    entry.interval_ms = 0;
    entry.callback.Reset(iso, args[0].As<v8::Function>());

    r->timers.push_back(std::move(entry));
    args.GetReturnValue().Set(v8::Integer::New(iso, static_cast<int32_t>(r->timers.back().id)));
}

static void js_set_interval(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* r   = runner_from_data(args);
    auto  iso = args.GetIsolate();
    if (args.Length() < 1 || !args[0]->IsFunction()) {
        args.GetReturnValue().Set(v8::Integer::New(iso, 0));
        return;
    }
    const int64_t interval_ms = args.Length() >= 2 ? args[1]->IntegerValue(
        iso->GetCurrentContext()).FromMaybe(0) : 0;

    timer_entry entry;
    entry.id          = r->next_timer_id++;
    entry.fire_at     = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(interval_ms < 0 ? 0 : interval_ms);
    entry.repeat      = true;
    entry.interval_ms = interval_ms;
    entry.callback.Reset(iso, args[0].As<v8::Function>());

    r->timers.push_back(std::move(entry));
    args.GetReturnValue().Set(v8::Integer::New(iso, static_cast<int32_t>(r->timers.back().id)));
}

static void js_clear_timer(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* r = runner_from_data(args);
    auto  iso = args.GetIsolate();
    if (args.Length() >= 1 && args[0]->IsInt32()) {
        const int64_t id = args[0]->IntegerValue(iso->GetCurrentContext()).FromMaybe(-1);
        auto& tv = r->timers;
        tv.erase(std::remove_if(tv.begin(), tv.end(),
            [id](const timer_entry& e) { return e.id == id; }), tv.end());
    }
}

// ---------------------------------------------------------------------------
// console
// ---------------------------------------------------------------------------

static void js_console_log(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto iso = args.GetIsolate();
    for (int i = 0; i < args.Length(); ++i) {
        if (i) fprintf(stderr, " ");
        v8::String::Utf8Value s(iso, args[i]);
        if (*s) fprintf(stderr, "%s", *s);
    }
    fprintf(stderr, "\n");
}

// ---------------------------------------------------------------------------
// crypto.getRandomValues
// ---------------------------------------------------------------------------

static void js_get_random_values(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto iso = args.GetIsolate();
    if (args.Length() < 1 || !args[0]->IsTypedArray()) return;

    auto typed_array = args[0].As<v8::TypedArray>();
    auto backing = typed_array->Buffer()->GetBackingStore();
    uint8_t* data = static_cast<uint8_t*>(backing->Data()) + typed_array->ByteOffset();

    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<int> dist(0, 255);
    for (size_t i = 0; i < typed_array->ByteLength(); ++i) {
        data[i] = static_cast<uint8_t>(dist(rng));
    }
    args.GetReturnValue().Set(args[0]);
}

// ---------------------------------------------------------------------------
// fetch (synchronous libcurl implementation exposed as Promise-returning fn)
// ---------------------------------------------------------------------------

static void js_fetch(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto iso = args.GetIsolate();
    auto ctx = iso->GetCurrentContext();

    if (args.Length() < 1) {
        iso->ThrowException(v8::Exception::TypeError(v8str(iso, "fetch: url required")));
        return;
    }

    const std::string url = to_std(iso, args[0]);

    // Parse options (2nd arg)
    http::request_options opts;
    if (args.Length() >= 2 && args[1]->IsObject()) {
        auto options = args[1].As<v8::Object>();

        // method
        auto method_key = v8str(iso, "method");
        auto method_val = options->Get(ctx, method_key);
        if (!method_val.IsEmpty() && !method_val.ToLocalChecked()->IsUndefined()) {
            opts.method = to_std(iso, method_val.ToLocalChecked());
        }

        // body
        auto body_key = v8str(iso, "body");
        auto body_val = options->Get(ctx, body_key);
        if (!body_val.IsEmpty() && !body_val.ToLocalChecked()->IsUndefined()) {
            opts.body = to_std(iso, body_val.ToLocalChecked());
        }

        // headers
        auto headers_key = v8str(iso, "headers");
        auto headers_val = options->Get(ctx, headers_key);
        if (!headers_val.IsEmpty() && headers_val.ToLocalChecked()->IsObject()) {
            auto hdrs_obj = headers_val.ToLocalChecked().As<v8::Object>();
            auto prop_names = hdrs_obj->GetOwnPropertyNames(ctx).ToLocalChecked();
            for (uint32_t i = 0; i < prop_names->Length(); ++i) {
                auto k = prop_names->Get(ctx, i).ToLocalChecked();
                auto v = hdrs_obj->Get(ctx, k).ToLocalChecked();
                opts.headers[to_std(iso, k)] = to_std(iso, v);
            }
        }
    }

    // Make synchronous HTTP call
    auto http_result = http::request(url, opts);

    // Create Response object
    auto resp_obj = v8::Object::New(iso);

    if (auto* err = std::get_if<http::http_error>(&http_result)) {
        // Reject the promise on network error
        auto resolver = v8::Promise::Resolver::New(ctx).ToLocalChecked();
        resolver->Reject(ctx, v8::Exception::Error(v8str(iso, err->message)));
        args.GetReturnValue().Set(resolver->GetPromise());
        return;
    }

    const auto& resp = std::get<http::response>(http_result);
    const std::string body_copy = resp.body;
    const long status = resp.status;

    resp_obj->Set(ctx, v8str(iso, "ok"),
        v8::Boolean::New(iso, status >= 200 && status < 300));
    resp_obj->Set(ctx, v8str(iso, "status"),
        v8::Integer::New(iso, static_cast<int32_t>(status)));

    // Body is stored as a V8 String in the Data slot — avoids v8::External sandbox tags.
    auto body_v8 = v8str(iso, body_copy);

    // .text() → Promise.resolve(body)
    auto text_fn = v8::Function::New(ctx,
        [](const v8::FunctionCallbackInfo<v8::Value>& fn_args) {
            auto fn_iso = fn_args.GetIsolate();
            auto fn_ctx = fn_iso->GetCurrentContext();
            auto resolver = v8::Promise::Resolver::New(fn_ctx).ToLocalChecked();
            resolver->Resolve(fn_ctx, fn_args.Data());
            fn_args.GetReturnValue().Set(resolver->GetPromise());
        },
        body_v8
    ).ToLocalChecked();

    // .json() → Promise.resolve(JSON.parse(body))
    auto json_fn = v8::Function::New(ctx,
        [](const v8::FunctionCallbackInfo<v8::Value>& fn_args) {
            auto fn_iso = fn_args.GetIsolate();
            auto fn_ctx = fn_iso->GetCurrentContext();
            v8::TryCatch tc(fn_iso);
            auto json_obj = v8::JSON::Parse(fn_ctx, fn_args.Data().As<v8::String>());
            auto resolver = v8::Promise::Resolver::New(fn_ctx).ToLocalChecked();
            if (json_obj.IsEmpty()) {
                resolver->Reject(fn_ctx, v8::Exception::SyntaxError(
                    v8str(fn_iso, "JSON.parse failed")));
            } else {
                resolver->Resolve(fn_ctx, json_obj.ToLocalChecked());
            }
            fn_args.GetReturnValue().Set(resolver->GetPromise());
        },
        body_v8
    ).ToLocalChecked();

    // .arrayBuffer() → Promise.resolve(ArrayBuffer)
    auto ab_fn = v8::Function::New(ctx,
        [](const v8::FunctionCallbackInfo<v8::Value>& fn_args) {
            auto fn_iso = fn_args.GetIsolate();
            auto fn_ctx = fn_iso->GetCurrentContext();
            std::string body_str = to_std(fn_iso, fn_args.Data());
            auto ab = v8::ArrayBuffer::New(fn_iso, body_str.size());
            if (!body_str.empty()) {
                memcpy(ab->GetBackingStore()->Data(), body_str.data(), body_str.size());
            }
            auto resolver = v8::Promise::Resolver::New(fn_ctx).ToLocalChecked();
            resolver->Resolve(fn_ctx, ab);
            fn_args.GetReturnValue().Set(resolver->GetPromise());
        },
        body_v8
    ).ToLocalChecked();

    resp_obj->Set(ctx, v8str(iso, "text"),   text_fn);
    resp_obj->Set(ctx, v8str(iso, "json"),   json_fn);
    resp_obj->Set(ctx, v8str(iso, "arrayBuffer"), ab_fn);

    auto resolver = v8::Promise::Resolver::New(ctx).ToLocalChecked();
    resolver->Resolve(ctx, resp_obj);
    args.GetReturnValue().Set(resolver->GetPromise());
}

// ---------------------------------------------------------------------------
// queueMicrotask
// ---------------------------------------------------------------------------

static void js_queue_microtask(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto iso = args.GetIsolate();
    if (args.Length() < 1 || !args[0]->IsFunction()) return;
    iso->EnqueueMicrotask(args[0].As<v8::Function>());
}

// ---------------------------------------------------------------------------
// requestAnimationFrame (stub: setTimeout(cb, 16))
// ---------------------------------------------------------------------------

static void js_request_animation_frame(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* r   = runner_from_data(args);
    auto  iso = args.GetIsolate();
    if (args.Length() < 1 || !args[0]->IsFunction()) {
        args.GetReturnValue().Set(v8::Integer::New(iso, 0));
        return;
    }
    timer_entry entry;
    entry.id          = r->next_timer_id++;
    entry.fire_at     = std::chrono::steady_clock::now() + std::chrono::milliseconds(16);
    entry.repeat      = false;
    entry.interval_ms = 0;
    entry.callback.Reset(iso, args[0].As<v8::Function>());
    r->timers.push_back(std::move(entry));
    args.GetReturnValue().Set(v8::Integer::New(iso, static_cast<int32_t>(r->timers.back().id)));
}

// ---------------------------------------------------------------------------
// cancelAnimationFrame
// ---------------------------------------------------------------------------

static void js_cancel_animation_frame(const v8::FunctionCallbackInfo<v8::Value>& args) {
    js_clear_timer(args); // same logic
}

// ---------------------------------------------------------------------------
// Browser environment setup
// ---------------------------------------------------------------------------

static void setup_browser_env(runner* r) {
    using namespace constants;

    auto iso = r->isolate;
    v8::HandleScope hs(iso);
    auto ctx = r->context.Get(iso);
    v8::Context::Scope cs(ctx);

    auto global = ctx->Global();
    // Pack the runner pointer into a BigInt — v8::External now requires a sandbox
    // type tag we can't provide, so BigInt is the portable way to smuggle a pointer.
    auto runner_data = v8::BigInt::NewFromUnsigned(iso, reinterpret_cast<uint64_t>(r));

    auto set_fn = [&](const char* name, v8::FunctionCallback cb,
                      v8::Local<v8::Value> data = v8::Local<v8::Value>{}) {
        auto fn = v8::Function::New(ctx, cb,
            data.IsEmpty() ? runner_data.As<v8::Value>() : data).ToLocalChecked();
        global->Set(ctx, v8str(iso, name), fn);
    };

    // timers
    set_fn("setTimeout",  js_set_timeout);
    set_fn("setInterval", js_set_interval);
    set_fn("clearTimeout",   js_clear_timer);
    set_fn("clearInterval",  js_clear_timer);
    set_fn("setImmediate",   js_set_timeout);  // node compat: setTimeout(cb, 0)
    set_fn("clearImmediate", js_clear_timer);
    set_fn("requestAnimationFrame",  js_request_animation_frame);
    set_fn("cancelAnimationFrame",   js_cancel_animation_frame);
    set_fn("queueMicrotask",         js_queue_microtask);

    // fetch
    set_fn("fetch", js_fetch, v8::Undefined(iso));

    // console
    {
        auto con = v8::Object::New(iso);
        auto log_fn = v8::Function::New(ctx, js_console_log).ToLocalChecked();
        con->Set(ctx, v8str(iso, "log"),   log_fn);
        con->Set(ctx, v8str(iso, "error"), log_fn);
        con->Set(ctx, v8str(iso, "warn"),  log_fn);
        con->Set(ctx, v8str(iso, "info"),  log_fn);
        con->Set(ctx, v8str(iso, "debug"), log_fn);
        global->Set(ctx, v8str(iso, "console"), con);
    }

    // performance
    {
        auto perf = v8::Object::New(iso);
        auto now_fn = v8::Function::New(ctx,
            [](const v8::FunctionCallbackInfo<v8::Value>& a) {
                using namespace std::chrono;
                static const auto start = steady_clock::now();
                double ms = duration<double, std::milli>(
                    steady_clock::now() - start).count();
                a.GetReturnValue().Set(v8::Number::New(a.GetIsolate(), ms));
            }).ToLocalChecked();
        perf->Set(ctx, v8str(iso, "now"), now_fn);
        global->Set(ctx, v8str(iso, "performance"), perf);
    }

    // crypto
    {
        auto cryp = v8::Object::New(iso);
        set_fn("__grv__", js_get_random_values, v8::Undefined(iso));
        auto grv_fn = v8::Function::New(ctx, js_get_random_values).ToLocalChecked();
        cryp->Set(ctx, v8str(iso, "getRandomValues"), grv_fn);
        auto rand_uuid_fn = v8::Function::New(ctx,
            [](const v8::FunctionCallbackInfo<v8::Value>& a) {
                // Minimal RFC-4122 v4 UUID
                static std::random_device rd;
                static std::mt19937 rng(rd());
                static std::uniform_int_distribution<int> d(0, 15);
                static const char* hex = "0123456789abcdef";
                std::string uuid(36, '-');
                for (int i = 0, j = 0; i < 36; ++i) {
                    if (i == 8 || i == 13 || i == 18 || i == 23) continue;
                    uuid[i] = hex[d(rng)];
                }
                uuid[14] = '4';
                uuid[19] = "89ab"[d(rng) % 4];
                a.GetReturnValue().Set(v8str(a.GetIsolate(), uuid));
            }).ToLocalChecked();
        cryp->Set(ctx, v8str(iso, "randomUUID"), rand_uuid_fn);
        global->Set(ctx, v8str(iso, "crypto"), cryp);
    }

    // navigator
    {
        auto nav = v8::Object::New(iso);
        nav->Set(ctx, v8str(iso, "userAgent"),         v8str(iso, USER_AGENT));
        nav->Set(ctx, v8str(iso, "language"),          v8str(iso, "en-US"));
        nav->Set(ctx, v8str(iso, "platform"),          v8str(iso, "MacIntel"));
        nav->Set(ctx, v8str(iso, "vendor"),            v8str(iso, "Google Inc."));
        nav->Set(ctx, v8str(iso, "appName"),           v8str(iso, "Netscape"));
        nav->Set(ctx, v8str(iso, "appCodeName"),       v8str(iso, "Mozilla"));
        nav->Set(ctx, v8str(iso, "product"),           v8str(iso, "Gecko"));
        nav->Set(ctx, v8str(iso, "onLine"),            v8::Boolean::New(iso, true));
        nav->Set(ctx, v8str(iso, "cookieEnabled"),     v8::Boolean::New(iso, true));
        nav->Set(ctx, v8str(iso, "doNotTrack"),        v8::Null(iso));
        nav->Set(ctx, v8str(iso, "hardwareConcurrency"),
            v8::Integer::New(iso, HARDWARE_CONCURRENCY));
        nav->Set(ctx, v8str(iso, "maxTouchPoints"),    v8::Integer::New(iso, 0));
        nav->Set(ctx, v8str(iso, "webdriver"),         v8::Boolean::New(iso, false));
        // languages array
        auto langs = v8::Array::New(iso, 2);
        langs->Set(ctx, 0, v8str(iso, "en-US"));
        langs->Set(ctx, 1, v8str(iso, "en"));
        nav->Set(ctx, v8str(iso, "languages"), langs);
        // plugins (empty PluginArray-like)
        auto plugins = v8::Array::New(iso, 0);
        nav->Set(ctx, v8str(iso, "plugins"), plugins);
        // mimeTypes (empty)
        auto mime = v8::Array::New(iso, 0);
        nav->Set(ctx, v8str(iso, "mimeTypes"), mime);
        // permissions stub
        auto perms = v8::Object::New(iso);
        auto query_fn = v8::Function::New(ctx,
            [](const v8::FunctionCallbackInfo<v8::Value>& a) {
                auto pi = a.GetIsolate();
                auto pc = pi->GetCurrentContext();
                auto resolver = v8::Promise::Resolver::New(pc).ToLocalChecked();
                auto result = v8::Object::New(pi);
                result->Set(pc, v8str(pi, "state"), v8str(pi, "granted"));
                resolver->Resolve(pc, result);
                a.GetReturnValue().Set(resolver->GetPromise());
            }).ToLocalChecked();
        perms->Set(ctx, v8str(iso, "query"), query_fn);
        nav->Set(ctx, v8str(iso, "permissions"), perms);
        // sendBeacon stub
        auto sb_fn = v8::Function::New(ctx,
            [](const v8::FunctionCallbackInfo<v8::Value>& a) {
                a.GetReturnValue().Set(v8::Boolean::New(a.GetIsolate(), true));
            }).ToLocalChecked();
        nav->Set(ctx, v8str(iso, "sendBeacon"), sb_fn);

        // Use defineProperty so navigator is non-configurable (like in browsers)
        v8::PropertyDescriptor nav_desc(nav, false);
        global->DefineProperty(ctx, v8str(iso, "navigator"), nav_desc);
    }

    // screen
    {
        auto scr = v8::Object::New(iso);
        scr->Set(ctx, v8str(iso, "width"),       v8::Integer::New(iso, SCREEN_WIDTH));
        scr->Set(ctx, v8str(iso, "height"),      v8::Integer::New(iso, SCREEN_HEIGHT));
        scr->Set(ctx, v8str(iso, "availWidth"),  v8::Integer::New(iso, SCREEN_WIDTH));
        scr->Set(ctx, v8str(iso, "availHeight"), v8::Integer::New(iso, SCREEN_HEIGHT - 25));
        scr->Set(ctx, v8str(iso, "colorDepth"),  v8::Integer::New(iso, 24));
        scr->Set(ctx, v8str(iso, "pixelDepth"),  v8::Integer::New(iso, 24));
        scr->Set(ctx, v8str(iso, "orientation"), [&]() {
            auto ori = v8::Object::New(iso);
            ori->Set(ctx, v8str(iso, "type"),  v8str(iso, "landscape-primary"));
            ori->Set(ctx, v8str(iso, "angle"), v8::Integer::New(iso, 0));
            return ori;
        }());
        global->Set(ctx, v8str(iso, "screen"), scr);
    }

    // location
    {
        auto loc = v8::Object::New(iso);
        loc->Set(ctx, v8str(iso, "href"),     v8str(iso, "https://www.youtube.com/"));
        loc->Set(ctx, v8str(iso, "origin"),   v8str(iso, "https://www.youtube.com"));
        loc->Set(ctx, v8str(iso, "protocol"), v8str(iso, "https:"));
        loc->Set(ctx, v8str(iso, "host"),     v8str(iso, "www.youtube.com"));
        loc->Set(ctx, v8str(iso, "hostname"), v8str(iso, "www.youtube.com"));
        loc->Set(ctx, v8str(iso, "port"),     v8str(iso, ""));
        loc->Set(ctx, v8str(iso, "pathname"), v8str(iso, "/"));
        loc->Set(ctx, v8str(iso, "search"),   v8str(iso, ""));
        loc->Set(ctx, v8str(iso, "hash"),     v8str(iso, ""));
        auto assign_fn = v8::Function::New(ctx,
            [](const v8::FunctionCallbackInfo<v8::Value>&) {}).ToLocalChecked();
        loc->Set(ctx, v8str(iso, "assign"),   assign_fn);
        loc->Set(ctx, v8str(iso, "replace"),  assign_fn);
        loc->Set(ctx, v8str(iso, "reload"),   assign_fn);
        global->Set(ctx, v8str(iso, "location"), loc);
    }

    // origin (string, same as location.origin)
    global->Set(ctx, v8str(iso, "origin"), v8str(iso, "https://www.youtube.com"));

    // document (minimal)
    {
        auto doc = v8::Object::New(iso);

        // createElement(tag) — returns minimal element; canvas gets stubbed getContext
        auto create_el_fn = v8::Function::New(ctx,
            [](const v8::FunctionCallbackInfo<v8::Value>& a) {
                auto ei = a.GetIsolate();
                auto ec = ei->GetCurrentContext();
                std::string tag = a.Length() >= 1 ? to_std(ei, a[0]) : "div";
                // uppercase tag name
                for (char& c : tag) c = static_cast<char>(toupper(c));

                auto el = v8::Object::New(ei);
                el->Set(ec, v8str(ei, "tagName"),   v8str(ei, tag));
                el->Set(ec, v8str(ei, "id"),        v8str(ei, ""));
                el->Set(ec, v8str(ei, "className"), v8str(ei, ""));
                el->Set(ec, v8str(ei, "innerHTML"), v8str(ei, ""));
                el->Set(ec, v8str(ei, "textContent"), v8str(ei, ""));
                el->Set(ec, v8str(ei, "style"),     v8::Object::New(ei));
                el->Set(ec, v8str(ei, "children"),  v8::Array::New(ei, 0));

                auto noop = v8::Function::New(ec,
                    [](const v8::FunctionCallbackInfo<v8::Value>&) {}).ToLocalChecked();
                auto ret_null = v8::Function::New(ec,
                    [](const v8::FunctionCallbackInfo<v8::Value>& fa) {
                        fa.GetReturnValue().SetNull();
                    }).ToLocalChecked();
                auto ret_self = v8::Function::New(ec,
                    [](const v8::FunctionCallbackInfo<v8::Value>& fa) {
                        if (fa.Length() >= 1) fa.GetReturnValue().Set(fa[0]);
                    }).ToLocalChecked();

                el->Set(ec, v8str(ei, "getAttribute"),    ret_null);
                el->Set(ec, v8str(ei, "setAttribute"),    noop);
                el->Set(ec, v8str(ei, "removeAttribute"), noop);
                el->Set(ec, v8str(ei, "hasAttribute"),
                    v8::Function::New(ec, [](const v8::FunctionCallbackInfo<v8::Value>& fa) {
                        fa.GetReturnValue().Set(v8::Boolean::New(fa.GetIsolate(), false));
                    }).ToLocalChecked());
                el->Set(ec, v8str(ei, "appendChild"),     ret_self);
                el->Set(ec, v8str(ei, "removeChild"),     ret_self);
                el->Set(ec, v8str(ei, "addEventListener"),    noop);
                el->Set(ec, v8str(ei, "removeEventListener"), noop);
                el->Set(ec, v8str(ei, "dispatchEvent"),   noop);
                el->Set(ec, v8str(ei, "getBoundingClientRect"),
                    v8::Function::New(ec, [](const v8::FunctionCallbackInfo<v8::Value>& fa) {
                        auto ri = fa.GetIsolate();
                        auto rc = ri->GetCurrentContext();
                        auto rect = v8::Object::New(ri);
                        for (const char* k : {"top","left","bottom","right","width","height","x","y"}) {
                            rect->Set(rc, v8str(ri, k), v8::Number::New(ri, 0.0));
                        }
                        fa.GetReturnValue().Set(rect);
                    }).ToLocalChecked());

                // canvas-specific: getContext → null, toDataURL → ""
                if (tag == "CANVAS") {
                    el->Set(ec, v8str(ei, "getContext"), ret_null);
                    el->Set(ec, v8str(ei, "toDataURL"),
                        v8::Function::New(ec, [](const v8::FunctionCallbackInfo<v8::Value>& fa) {
                            fa.GetReturnValue().Set(v8str(fa.GetIsolate(), "data:,"));
                        }).ToLocalChecked());
                    el->Set(ec, v8str(ei, "width"),  v8::Integer::New(ei, 300));
                    el->Set(ec, v8str(ei, "height"), v8::Integer::New(ei, 150));
                }

                a.GetReturnValue().Set(el);
            }).ToLocalChecked();

        auto noop = v8::Function::New(ctx,
            [](const v8::FunctionCallbackInfo<v8::Value>&) {}).ToLocalChecked();
        auto ret_null = v8::Function::New(ctx,
            [](const v8::FunctionCallbackInfo<v8::Value>& a) {
                a.GetReturnValue().SetNull();
            }).ToLocalChecked();
        auto ret_empty_arr = v8::Function::New(ctx,
            [](const v8::FunctionCallbackInfo<v8::Value>& a) {
                a.GetReturnValue().Set(v8::Array::New(a.GetIsolate(), 0));
            }).ToLocalChecked();

        doc->Set(ctx, v8str(iso, "createElement"),          create_el_fn);
        doc->Set(ctx, v8str(iso, "getElementById"),         ret_null);
        doc->Set(ctx, v8str(iso, "querySelector"),          ret_null);
        doc->Set(ctx, v8str(iso, "querySelectorAll"),       ret_empty_arr);
        doc->Set(ctx, v8str(iso, "getElementsByTagName"),   ret_empty_arr);
        doc->Set(ctx, v8str(iso, "getElementsByClassName"), ret_empty_arr);
        doc->Set(ctx, v8str(iso, "createTextNode"),         [&]() {
            return v8::Function::New(ctx,
                [](const v8::FunctionCallbackInfo<v8::Value>& a) {
                    auto ei = a.GetIsolate();
                    auto node = v8::Object::New(ei);
                    node->Set(ei->GetCurrentContext(), v8str(ei, "nodeType"),
                        v8::Integer::New(ei, 3));
                    node->Set(ei->GetCurrentContext(), v8str(ei, "textContent"),
                        a.Length() >= 1 ? a[0] : v8str(ei, "").As<v8::Value>());
                    a.GetReturnValue().Set(node);
                }).ToLocalChecked();
        }());
        doc->Set(ctx, v8str(iso, "addEventListener"),    noop);
        doc->Set(ctx, v8str(iso, "removeEventListener"), noop);
        doc->Set(ctx, v8str(iso, "dispatchEvent"),       noop);
        doc->Set(ctx, v8str(iso, "cookie"),    v8str(iso, ""));
        doc->Set(ctx, v8str(iso, "referrer"),  v8str(iso, "https://www.youtube.com/"));
        doc->Set(ctx, v8str(iso, "domain"),    v8str(iso, "www.youtube.com"));
        doc->Set(ctx, v8str(iso, "title"),     v8str(iso, "YouTube"));
        doc->Set(ctx, v8str(iso, "readyState"),v8str(iso, "complete"));
        doc->Set(ctx, v8str(iso, "URL"),       v8str(iso, "https://www.youtube.com/"));
        doc->Set(ctx, v8str(iso, "documentURI"),v8str(iso, "https://www.youtube.com/"));
        doc->Set(ctx, v8str(iso, "characterSet"), v8str(iso, "UTF-8"));
        doc->Set(ctx, v8str(iso, "charset"),   v8str(iso, "UTF-8"));
        doc->Set(ctx, v8str(iso, "nodeType"),  v8::Integer::New(iso, 9));

        // body / head / documentElement — minimal elements
        auto make_el = [&](const char* tag) {
            auto el = v8::Object::New(iso);
            el->Set(ctx, v8str(iso, "tagName"),    v8str(iso, tag));
            el->Set(ctx, v8str(iso, "id"),         v8str(iso, ""));
            el->Set(ctx, v8str(iso, "className"),  v8str(iso, ""));
            el->Set(ctx, v8str(iso, "innerHTML"),  v8str(iso, ""));
            el->Set(ctx, v8str(iso, "style"),      v8::Object::New(iso));
            el->Set(ctx, v8str(iso, "children"),   v8::Array::New(iso, 0));
            el->Set(ctx, v8str(iso, "appendChild"), v8::Function::New(ctx,
                [](const v8::FunctionCallbackInfo<v8::Value>& fa) {
                    if (fa.Length() >= 1) fa.GetReturnValue().Set(fa[0]);
                }).ToLocalChecked());
            el->Set(ctx, v8str(iso, "removeChild"), v8::Function::New(ctx,
                [](const v8::FunctionCallbackInfo<v8::Value>& fa) {
                    if (fa.Length() >= 1) fa.GetReturnValue().Set(fa[0]);
                }).ToLocalChecked());
            el->Set(ctx, v8str(iso, "addEventListener"), noop);
            el->Set(ctx, v8str(iso, "removeEventListener"), noop);
            return el;
        };

        doc->Set(ctx, v8str(iso, "body"),            make_el("BODY"));
        doc->Set(ctx, v8str(iso, "head"),            make_el("HEAD"));
        doc->Set(ctx, v8str(iso, "documentElement"), make_el("HTML"));

        // location = same as window.location
        auto loc = global->Get(ctx, v8str(iso, "location")).ToLocalChecked();
        doc->Set(ctx, v8str(iso, "location"), loc);

        global->Set(ctx, v8str(iso, "document"), doc);
    }

    // window = the global itself (circular ref — standard browser behaviour)
    global->Set(ctx, v8str(iso, "window"), global);
    // self = also the global (used by some BotGuard versions)
    global->Set(ctx, v8str(iso, "self"), global);
    // global = Node's name for the global object (JSDOM bundles expect it)
    global->Set(ctx, v8str(iso, "global"), global);
    // globalThis already == global in V8 scripts

    // getComputedStyle stub
    {
        auto gcs_fn = v8::Function::New(ctx,
            [](const v8::FunctionCallbackInfo<v8::Value>& a) {
                a.GetReturnValue().Set(v8::Object::New(a.GetIsolate()));
            }).ToLocalChecked();
        global->Set(ctx, v8str(iso, "getComputedStyle"), gcs_fn);
    }

    // matchMedia stub
    {
        auto mm_fn = v8::Function::New(ctx,
            [](const v8::FunctionCallbackInfo<v8::Value>& a) {
                auto mi = a.GetIsolate();
                auto mc = mi->GetCurrentContext();
                auto mq = v8::Object::New(mi);
                mq->Set(mc, v8str(mi, "matches"),  v8::Boolean::New(mi, false));
                mq->Set(mc, v8str(mi, "media"),    a.Length() >= 1 ? a[0] : v8str(mi, "").As<v8::Value>());
                mq->Set(mc, v8str(mi, "onchange"), v8::Null(mi));
                auto noop2 = v8::Function::New(mc,
                    [](const v8::FunctionCallbackInfo<v8::Value>&) {}).ToLocalChecked();
                mq->Set(mc, v8str(mi, "addListener"),    noop2);
                mq->Set(mc, v8str(mi, "removeListener"), noop2);
                mq->Set(mc, v8str(mi, "addEventListener"),    noop2);
                mq->Set(mc, v8str(mi, "removeEventListener"), noop2);
                a.GetReturnValue().Set(mq);
            }).ToLocalChecked();
        global->Set(ctx, v8str(iso, "matchMedia"), mm_fn);
    }

    // addEventListener / removeEventListener (window-level)
    {
        auto noop2 = v8::Function::New(ctx,
            [](const v8::FunctionCallbackInfo<v8::Value>&) {}).ToLocalChecked();
        global->Set(ctx, v8str(iso, "addEventListener"),    noop2);
        global->Set(ctx, v8str(iso, "removeEventListener"), noop2);
        global->Set(ctx, v8str(iso, "dispatchEvent"),       noop2);
    }

    // atob / btoa (implemented in JS via Buffer-less base64 — pure JS for simplicity)
    {
        const char* b64_impl = R"JS(
(function() {
    var chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    globalThis.btoa = function(s) {
        var i, b = '', l = s.length;
        for (i = 0; i < l; i += 3) {
            var c0 = s.charCodeAt(i), c1 = i+1<l ? s.charCodeAt(i+1) : 0, c2 = i+2<l ? s.charCodeAt(i+2) : 0;
            b += chars[c0>>2] + chars[((c0&3)<<4)|(c1>>4)] + (i+1<l ? chars[((c1&15)<<2)|(c2>>6)] : '=') + (i+2<l ? chars[c2&63] : '=');
        }
        return b;
    };
    globalThis.atob = function(s) {
        s = s.replace(/[^A-Za-z0-9+/]/g, '');
        var i, n = s.length, b = '';
        for (i = 0; i < n; i += 4) {
            var a = chars.indexOf(s[i]), c = chars.indexOf(s[i+1]), d = chars.indexOf(s[i+2]), e = chars.indexOf(s[i+3]);
            b += String.fromCharCode((a<<2)|(c>>4));
            if (d !== -1) b += String.fromCharCode(((c&15)<<4)|(d>>2));
            if (e !== -1) b += String.fromCharCode(((d&3)<<6)|e);
        }
        return b;
    };
})();
)JS";
        run_script(r, b64_impl, "<atob-btoa>");
    }

    // TextEncoder / TextDecoder (JS polyfill — V8 doesn't include these by default)
    {
        const char* td_impl = R"JS(
(function() {
    globalThis.TextEncoder = function TextEncoder() {};
    TextEncoder.prototype.encoding = 'utf-8';
    TextEncoder.prototype.encode = function(s) {
        var out = [], i = 0, n = s.length;
        while (i < n) {
            var c = s.charCodeAt(i++);
            if (c < 0x80) { out.push(c); }
            else if (c < 0x800) { out.push(0xC0|(c>>6), 0x80|(c&63)); }
            else if (c >= 0xD800 && c < 0xDC00 && i < n) {
                var c2 = s.charCodeAt(i);
                if (c2 >= 0xDC00 && c2 < 0xE000) {
                    var cp = 0x10000 + ((c & 0x3FF) << 10) + (c2 & 0x3FF);
                    out.push(0xF0|(cp>>18), 0x80|((cp>>12)&63), 0x80|((cp>>6)&63), 0x80|(cp&63));
                    i++;
                } else { out.push(0xEF, 0xBF, 0xBD); }
            } else if (c >= 0xD800 && c < 0xE000) { out.push(0xEF,0xBF,0xBD); }
            else { out.push(0xE0|(c>>12), 0x80|((c>>6)&63), 0x80|(c&63)); }
        }
        return new Uint8Array(out);
    };

    globalThis.TextDecoder = function TextDecoder(enc) { this.encoding = enc || 'utf-8'; };
    TextDecoder.prototype.decode = function(buf) {
        if (!buf) return '';
        var bytes = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
        var s = '', i = 0;
        while (i < bytes.length) {
            var b = bytes[i++];
            if (b < 0x80) { s += String.fromCharCode(b); }
            else if (b < 0xE0) { s += String.fromCharCode(((b&31)<<6)|(bytes[i++]&63)); }
            else if (b < 0xF0) {
                var b2=bytes[i++],b3=bytes[i++];
                s += String.fromCharCode(((b&15)<<12)|((b2&63)<<6)|(b3&63));
            } else {
                var b2=bytes[i++],b3=bytes[i++],b4=bytes[i++];
                var cp = ((b&7)<<18)|((b2&63)<<12)|((b3&63)<<6)|(b4&63);
                cp -= 0x10000;
                s += String.fromCharCode(0xD800+(cp>>10), 0xDC00+(cp&0x3FF));
            }
        }
        return s;
    };
})();
)JS";
        run_script(r, td_impl, "<text-encoder-decoder>");
    }

    // URL stub (minimal, needed by some BotGuard helpers)
    {
        const char* url_impl = R"JS(
(function() {
    globalThis.URL = function URL(url, base) {
        if (typeof url !== 'string') throw new TypeError('URL is not a string');
        this.href = url;
        var m = url.match(/^([a-z][a-z0-9+\-.]*:)\/\/([^/?#]*)([^?#]*)(\?[^#]*)?(#.*)?$/i);
        if (m) {
            this.protocol = m[1]; this.host = m[2]; this.pathname = m[3]||'/';
            this.search = m[4]||''; this.hash = m[5]||''; this.origin = m[1]+'//'+m[2];
        } else { this.href = url; this.protocol=''; this.host=''; this.pathname=url; this.search=''; this.hash=''; this.origin=''; }
        this.username=''; this.password=''; this.port='';
        var hm = this.host.match(/^([^:]+):(\d+)$/);
        if (hm) { this.hostname = hm[1]; this.port = hm[2]; } else { this.hostname = this.host; }
    };
    URL.prototype.toString = function() { return this.href; };
    URL.createObjectURL = function() { return ''; };
    URL.revokeObjectURL = function() {};
    URL.parse = function(url, base) {
        try { return new URL(url, base); } catch (e) { return null; }
    };
    URL.canParse = function(url, base) {
        try { new URL(url, base); return true; } catch (e) { return false; }
    };
})();
)JS";
        run_script(r, url_impl, "<URL>");
    }

    // Event stub
    {
        const char* ev_impl = R"JS(
(function() {
    globalThis.Event = function Event(type, opts) { this.type = type; this.bubbles = !!(opts&&opts.bubbles); this.cancelable = !!(opts&&opts.cancelable); };
    globalThis.CustomEvent = function CustomEvent(type, opts) { this.type = type; this.detail = opts&&opts.detail; };
    globalThis.MessageEvent = function MessageEvent(type, opts) { this.type = type; this.data = opts&&opts.data; };
})();
)JS";
        run_script(r, ev_impl, "<Event>");
    }

    // HTMLCanvasElement prototype stub on the global so legacy checks pass
    {
        const char* canvas_impl = R"JS(
(function() {
    globalThis.HTMLCanvasElement = function HTMLCanvasElement() {};
    HTMLCanvasElement.prototype.getContext = function() { return null; };
    HTMLCanvasElement.prototype.toDataURL  = function() { return 'data:,'; };
    globalThis.HTMLElement = function HTMLElement() {};
    globalThis.HTMLInputElement = function HTMLInputElement() {};
    globalThis.HTMLVideoElement = function HTMLVideoElement() {};
    globalThis.HTMLImageElement = function HTMLImageElement() {};
    globalThis.Image = function Image(w,h) { this.width=w||0; this.height=h||0; };
})();
)JS";
        run_script(r, canvas_impl, "<HTMLElement>");
    }

    // WebGL stubs (BotGuard may probe these)
    {
        const char* webgl_impl = R"JS(
(function() {
    globalThis.WebGLRenderingContext = function WebGLRenderingContext() {};
    globalThis.WebGL2RenderingContext = function WebGL2RenderingContext() {};
})();
)JS";
        run_script(r, webgl_impl, "<WebGL>");
    }

    // ------------------------------------------------------------------
    // Extra browser-API stubs that BotGuard fingerprints against.
    // These do not have to behave correctly — they only need to exist
    // and report reasonable values when probed, so the snapshot is
    // accepted as coming from a real browser.
    // ------------------------------------------------------------------
    {
        const char* extras_impl = R"JS(
(function() {
    // chrome object — present on Chrome UA pages
    if (!globalThis.chrome) {
        globalThis.chrome = {
            runtime: {},
            app:     { isInstalled: false },
            csi:     function() { return {}; },
            loadTimes: function() { return { firstPaintTime: 0 }; }
        };
    }

    // Storage / localStorage / sessionStorage — simple in-memory implementations
    function makeStorage() {
        var data = {};
        return {
            get length() { return Object.keys(data).length; },
            key: function(i) { return Object.keys(data)[i] || null; },
            getItem: function(k) { return Object.prototype.hasOwnProperty.call(data, k) ? data[k] : null; },
            setItem: function(k, v) { data[k] = String(v); },
            removeItem: function(k) { delete data[k]; },
            clear: function() { data = {}; }
        };
    }
    if (!globalThis.localStorage)   globalThis.localStorage   = makeStorage();
    if (!globalThis.sessionStorage) globalThis.sessionStorage = makeStorage();
    if (!globalThis.Storage)        globalThis.Storage        = function Storage() {};

    // IndexedDB stub — present, but never resolves a real DB
    if (!globalThis.indexedDB) {
        globalThis.indexedDB = {
            open: function() {
                var req = { onsuccess: null, onerror: null, onupgradeneeded: null, result: null, error: null };
                setTimeout(function() {
                    if (typeof req.onerror === 'function')
                        req.onerror({ type: 'error', target: req });
                }, 0);
                return req;
            },
            deleteDatabase: function() { return { onsuccess: null, onerror: null }; },
            databases:      function() { return Promise.resolve([]); }
        };
        globalThis.IDBDatabase    = function IDBDatabase() {};
        globalThis.IDBTransaction = function IDBTransaction() {};
        globalThis.IDBRequest     = function IDBRequest() {};
        globalThis.IDBFactory     = function IDBFactory() {};
        globalThis.IDBObjectStore = function IDBObjectStore() {};
        globalThis.IDBIndex       = function IDBIndex() {};
        globalThis.IDBKeyRange    = function IDBKeyRange() {};
        globalThis.IDBCursor      = function IDBCursor() {};
    }

    // Worker / SharedWorker / BroadcastChannel — never actually spawn anything
    if (!globalThis.Worker)            globalThis.Worker            = function Worker() { throw new Error('Worker not supported'); };
    if (!globalThis.SharedWorker)      globalThis.SharedWorker      = function SharedWorker() { throw new Error('SharedWorker not supported'); };
    if (!globalThis.BroadcastChannel)  globalThis.BroadcastChannel  = function BroadcastChannel() { this.postMessage = function() {}; this.close = function() {}; this.addEventListener = function() {}; this.removeEventListener = function() {}; };
    if (!globalThis.MessageChannel)    globalThis.MessageChannel    = function MessageChannel() { this.port1 = {}; this.port2 = {}; };
    if (!globalThis.MessagePort)       globalThis.MessagePort       = function MessagePort() {};

    // Observers
    var noopObserver = function() { this.observe = function() {}; this.unobserve = function() {}; this.disconnect = function() {}; this.takeRecords = function() { return []; }; };
    if (!globalThis.MutationObserver)      globalThis.MutationObserver      = noopObserver;
    if (!globalThis.IntersectionObserver)  globalThis.IntersectionObserver  = noopObserver;
    if (!globalThis.ResizeObserver)        globalThis.ResizeObserver        = noopObserver;
    if (!globalThis.PerformanceObserver)   globalThis.PerformanceObserver   = noopObserver;
    if (!globalThis.ReportingObserver)     globalThis.ReportingObserver     = noopObserver;

    // Notification (some BG variants probe permission)
    if (!globalThis.Notification) {
        globalThis.Notification = function Notification() {};
        globalThis.Notification.permission = 'default';
        globalThis.Notification.requestPermission = function() { return Promise.resolve('default'); };
    }

    // Fetch API extras — Headers / Request / Response (very minimal)
    if (!globalThis.Headers) {
        globalThis.Headers = function Headers(init) {
            var map = {};
            if (init && typeof init === 'object') {
                for (var k in init) map[String(k).toLowerCase()] = String(init[k]);
            }
            this.append = function(k, v) { map[String(k).toLowerCase()] = String(v); };
            this.delete = function(k) { delete map[String(k).toLowerCase()]; };
            this.get = function(k) { var v = map[String(k).toLowerCase()]; return v === undefined ? null : v; };
            this.has = function(k) { return Object.prototype.hasOwnProperty.call(map, String(k).toLowerCase()); };
            this.set = function(k, v) { map[String(k).toLowerCase()] = String(v); };
            this.forEach = function(cb) { for (var k in map) cb(map[k], k, this); };
        };
    }
    if (!globalThis.Request)  globalThis.Request  = function Request() {};
    if (!globalThis.Response) globalThis.Response = function Response() {};
    if (!globalThis.Blob)     globalThis.Blob     = function Blob() { this.size = 0; this.type = ''; };
    if (!globalThis.File)     globalThis.File     = function File()     {};
    if (!globalThis.FormData) globalThis.FormData = function FormData() { this.append = function() {}; this.delete = function() {}; this.get = function() { return null; }; this.has = function() { return false; }; this.set = function() {}; };
    if (!globalThis.AbortController) {
        globalThis.AbortController = function AbortController() {
            var self = this;
            this.signal = { aborted: false, addEventListener: function() {}, removeEventListener: function() {}, dispatchEvent: function() { return true; } };
            this.abort  = function() { self.signal.aborted = true; };
        };
    }
    if (!globalThis.AbortSignal) globalThis.AbortSignal = function AbortSignal() {};

    // EventTarget — many DOM classes inherit; provide a permissive base
    if (!globalThis.EventTarget) {
        globalThis.EventTarget = function EventTarget() {};
        globalThis.EventTarget.prototype.addEventListener    = function() {};
        globalThis.EventTarget.prototype.removeEventListener = function() {};
        globalThis.EventTarget.prototype.dispatchEvent       = function() { return true; };
    }

    // crypto.subtle — present, never resolves real cryptographic results
    if (globalThis.crypto && !globalThis.crypto.subtle) {
        function rejector(name) {
            return function() { return Promise.reject(new Error('subtle.' + name + ' not supported')); };
        }
        try {
            Object.defineProperty(globalThis.crypto, 'subtle', {
                value: {
                    encrypt:    rejector('encrypt'),
                    decrypt:    rejector('decrypt'),
                    sign:       rejector('sign'),
                    verify:     rejector('verify'),
                    digest:     rejector('digest'),
                    generateKey:rejector('generateKey'),
                    deriveKey:  rejector('deriveKey'),
                    deriveBits: rejector('deriveBits'),
                    importKey:  rejector('importKey'),
                    exportKey:  rejector('exportKey'),
                    wrapKey:    rejector('wrapKey'),
                    unwrapKey:  rejector('unwrapKey')
                }
            });
        } catch (e) {}
    }
    if (!globalThis.SubtleCrypto) globalThis.SubtleCrypto = function SubtleCrypto() {};
    if (!globalThis.CryptoKey)    globalThis.CryptoKey    = function CryptoKey()    {};

    // History / window dimensions / devicePixelRatio
    if (!globalThis.history) {
        globalThis.history = {
            length: 1, scrollRestoration: 'auto', state: null,
            back: function() {}, forward: function() {}, go: function() {},
            pushState: function() {}, replaceState: function() {}
        };
    }
    if (typeof globalThis.devicePixelRatio !== 'number') globalThis.devicePixelRatio = 1;
    if (typeof globalThis.innerWidth        !== 'number') globalThis.innerWidth        = 1280;
    if (typeof globalThis.innerHeight       !== 'number') globalThis.innerHeight       = 720;
    if (typeof globalThis.outerWidth        !== 'number') globalThis.outerWidth        = 1280;
    if (typeof globalThis.outerHeight       !== 'number') globalThis.outerHeight       = 720;
    if (typeof globalThis.scrollX           !== 'number') globalThis.scrollX           = 0;
    if (typeof globalThis.scrollY           !== 'number') globalThis.scrollY           = 0;

    // requestIdleCallback / cancelIdleCallback
    if (!globalThis.requestIdleCallback)  globalThis.requestIdleCallback  = function(cb) { return setTimeout(function() { cb({ didTimeout: false, timeRemaining: function() { return 0; } }); }, 1); };
    if (!globalThis.cancelIdleCallback)   globalThis.cancelIdleCallback   = function(id) { clearTimeout(id); };

    // performance.timing / performance.getEntries
    if (globalThis.performance) {
        var t = Date.now();
        if (!globalThis.performance.timing) {
            globalThis.performance.timing = {
                navigationStart: t, fetchStart: t, domainLookupStart: t, domainLookupEnd: t,
                connectStart: t, connectEnd: t, secureConnectionStart: 0,
                requestStart: t, responseStart: t, responseEnd: t,
                domLoading: t, domInteractive: t, domContentLoadedEventStart: t, domContentLoadedEventEnd: t,
                domComplete: t, loadEventStart: t, loadEventEnd: t,
                unloadEventStart: 0, unloadEventEnd: 0, redirectStart: 0, redirectEnd: 0
            };
        }
        if (!globalThis.performance.navigation) {
            globalThis.performance.navigation = { type: 0, redirectCount: 0 };
        }
        if (typeof globalThis.performance.timeOrigin !== 'number') {
            globalThis.performance.timeOrigin = t;
        }
        if (!globalThis.performance.getEntries)         globalThis.performance.getEntries         = function() { return []; };
        if (!globalThis.performance.getEntriesByType)   globalThis.performance.getEntriesByType   = function() { return []; };
        if (!globalThis.performance.getEntriesByName)   globalThis.performance.getEntriesByName   = function() { return []; };
        if (!globalThis.performance.mark)               globalThis.performance.mark               = function() {};
        if (!globalThis.performance.measure)            globalThis.performance.measure            = function() {};
        if (!globalThis.performance.clearMarks)         globalThis.performance.clearMarks         = function() {};
        if (!globalThis.performance.clearMeasures)      globalThis.performance.clearMeasures      = function() {};
        if (!globalThis.performance.clearResourceTimings) globalThis.performance.clearResourceTimings = function() {};
    }

    // Intl — V8 has this; ensure DateTimeFormat reports a reasonable TZ
    // (V8 picks ICU default; no-op here, just a presence check.)
    if (!globalThis.Intl) globalThis.Intl = {};

    // MediaDevices, MediaQueryList, AudioContext, RTCPeerConnection
    if (globalThis.navigator) {
        if (!globalThis.navigator.mediaDevices) {
            globalThis.navigator.mediaDevices = {
                enumerateDevices: function() { return Promise.resolve([]); },
                getUserMedia:     function() { return Promise.reject(new Error('No media')); },
                getDisplayMedia:  function() { return Promise.reject(new Error('No media')); }
            };
        }
        if (!globalThis.navigator.connection) {
            globalThis.navigator.connection = {
                effectiveType: '4g', rtt: 50, downlink: 10, saveData: false, type: 'wifi',
                addEventListener: function() {}, removeEventListener: function() {}
            };
        }
        if (!globalThis.navigator.serviceWorker) {
            globalThis.navigator.serviceWorker = {
                ready:       Promise.resolve({ active: null, installing: null, waiting: null, update: function() {}, unregister: function() {} }),
                controller:  null,
                register:    function() { return Promise.reject(new Error('No SW')); },
                getRegistration:  function() { return Promise.resolve(undefined); },
                getRegistrations: function() { return Promise.resolve([]); },
                addEventListener: function() {}, removeEventListener: function() {}
            };
        }
        if (!globalThis.navigator.storage) {
            globalThis.navigator.storage = {
                estimate: function() { return Promise.resolve({ usage: 0, quota: 0 }); },
                persist:  function() { return Promise.resolve(false); },
                persisted:function() { return Promise.resolve(false); }
            };
        }
        if (typeof globalThis.navigator.deviceMemory  !== 'number') globalThis.navigator.deviceMemory  = 8;
        if (typeof globalThis.navigator.pdfViewerEnabled !== 'boolean') globalThis.navigator.pdfViewerEnabled = true;
    }
    if (!globalThis.AudioContext)        globalThis.AudioContext        = function AudioContext() {};
    if (!globalThis.OfflineAudioContext) globalThis.OfflineAudioContext = function OfflineAudioContext() {};
    if (!globalThis.RTCPeerConnection)   globalThis.RTCPeerConnection   = function RTCPeerConnection() {};
    if (!globalThis.RTCSessionDescription) globalThis.RTCSessionDescription = function RTCSessionDescription() {};
    if (!globalThis.RTCIceCandidate)     globalThis.RTCIceCandidate     = function RTCIceCandidate() {};

    // CSS classes (some BG probes look for CSSStyleSheet.prototype.cssRules)
    if (!globalThis.CSSStyleSheet)       globalThis.CSSStyleSheet       = function CSSStyleSheet() { this.cssRules = []; };
    if (!globalThis.CSSStyleDeclaration) globalThis.CSSStyleDeclaration = function CSSStyleDeclaration() {};
    if (!globalThis.CSS) {
        globalThis.CSS = {
            supports: function() { return false; },
            escape:   function(s) { return String(s); }
        };
    }

    // FontFace
    if (!globalThis.FontFace) globalThis.FontFace = function FontFace() {};

    // Document classes (for instanceof checks)
    if (!globalThis.Document)      globalThis.Document      = function Document() {};
    if (!globalThis.HTMLDocument)  globalThis.HTMLDocument  = function HTMLDocument() {};
    if (!globalThis.Window)        globalThis.Window        = function Window() {};
    if (!globalThis.Navigator)     globalThis.Navigator     = function Navigator() {};
    if (!globalThis.Screen)        globalThis.Screen        = function Screen() {};
    if (!globalThis.Location)      globalThis.Location      = function Location() {};

    // PluginArray / MimeTypeArray classes
    if (!globalThis.PluginArray)   globalThis.PluginArray   = function PluginArray() {};
    if (!globalThis.MimeTypeArray) globalThis.MimeTypeArray = function MimeTypeArray() {};
    if (!globalThis.Plugin)        globalThis.Plugin        = function Plugin() {};
    if (!globalThis.MimeType)      globalThis.MimeType      = function MimeType() {};

    // DOMException / DOMParser / XMLSerializer (rarely used but probed)
    if (!globalThis.DOMException)   globalThis.DOMException   = function DOMException(msg, name) { this.message = msg || ''; this.name = name || 'Error'; };
    if (!globalThis.DOMParser)      globalThis.DOMParser      = function DOMParser()      {};
    if (!globalThis.XMLSerializer)  globalThis.XMLSerializer  = function XMLSerializer()  {};
    if (!globalThis.XMLHttpRequest) globalThis.XMLHttpRequest = function XMLHttpRequest() {};
    if (!globalThis.XPathEvaluator) globalThis.XPathEvaluator = function XPathEvaluator() {};

    // SVG (sometimes referenced)
    if (!globalThis.SVGElement)        globalThis.SVGElement        = function SVGElement()        {};
    if (!globalThis.SVGSVGElement)     globalThis.SVGSVGElement     = function SVGSVGElement()     {};
})();
)JS";
        run_script(r, extras_impl, "<browser-extras>");
    }
}

// ---------------------------------------------------------------------------
// Event loop pump
// ---------------------------------------------------------------------------

// Pumps until condition() returns true, or timeout_ms elapses.
// Returns true if condition was met, false if timed out.
static bool pump_event_loop(runner* r, std::function<bool()> condition,
                            int64_t timeout_ms = 15000) {
    auto iso = r->isolate;
    auto ctx = r->context.Get(iso);
    v8::Context::Scope cs(ctx);

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);

    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) return false;

        const auto now = std::chrono::steady_clock::now();

        // Snapshot which timer ids are ready (to avoid iterator invalidation)
        std::vector<int64_t> ready_ids;
        for (const auto& t : r->timers) {
            if (t.fire_at <= now) ready_ids.push_back(t.id);
        }

        for (int64_t id : ready_ids) {
            // Find the timer again — it might have been cleared by a previous callback
            auto it = std::find_if(r->timers.begin(), r->timers.end(),
                [id](const timer_entry& e) { return e.id == id; });
            if (it == r->timers.end()) continue;

            v8::HandleScope hs(iso);
            v8::Global<v8::Function> cb_global(iso, it->callback.Get(iso));
            const bool repeat      = it->repeat;
            const int64_t interval = it->interval_ms;

            if (!repeat) {
                it->callback.Reset();
                r->timers.erase(it);
            } else {
                it->fire_at = now + std::chrono::milliseconds(interval);
            }

            auto fn = cb_global.Get(iso);
            v8::TryCatch tc(iso);
            fn->Call(ctx, ctx->Global(), 0, nullptr);
            cb_global.Reset();
        }

        // Drain microtask queue
        iso->PerformMicrotaskCheckpoint();

        if (!condition()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return true;
}

// Read a nested JS property path from the global, e.g. "__epo_state__.done"
static v8::Local<v8::Value> get_nested(runner* r, const char* path) {
    auto iso = r->isolate;
    auto ctx = r->context.Get(iso);
    std::string p(path);
    v8::Local<v8::Value> val = ctx->Global();
    size_t start = 0;
    while (start < p.size()) {
        auto dot = p.find('.', start);
        std::string key = dot == std::string::npos ? p.substr(start) : p.substr(start, dot - start);
        if (!val->IsObject()) return v8::Undefined(iso);
        val = val.As<v8::Object>()->Get(ctx, v8str(iso, key.c_str())).FromMaybe(v8::Undefined(iso).As<v8::Value>());
        start = dot == std::string::npos ? p.size() : dot + 1;
    }
    return val;
}

static bool js_bool(runner* r, const char* path) {
    return get_nested(r, path)->BooleanValue(r->isolate);
}
static std::string js_string(runner* r, const char* path) {
    return to_std(r->isolate, get_nested(r, path));
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::variant<runner*, run_error> create_runner(
        const std::string& interpreter_js,
        const std::string& program,
        const std::string& global_name) {

    init_v8();

    auto* r        = new runner;
    r->allocator   = v8::ArrayBuffer::Allocator::NewDefaultAllocator();

    v8::Isolate::CreateParams cp;
    cp.array_buffer_allocator = r->allocator;
    r->isolate = v8::Isolate::New(cp);

    // Run all JS initialization inside the isolate scope.
    // Collect any error as a string so we can destroy the runner after exiting the scope.
    std::string init_error;
    {
        v8::Isolate::Scope iso_scope(r->isolate);
        v8::HandleScope    hs(r->isolate);

        auto ctx = v8::Context::New(r->isolate);
        r->context.Reset(r->isolate, ctx);

        v8::Context::Scope cs(ctx);

        // 1. Browser globals — fetch, setTimeout, console, performance, crypto
        //    (the JSDOM bundle in step 2 sets up window/document/navigator itself.)
        setup_browser_env(r);

        // 2. Full BG bundle = bgutils-js + jsdom-minimal + entrypoint.
        //    After eval, globalThis exposes runBotguard / newMinter / mint.
        if (init_error.empty()) {
            v8::TryCatch tc(r->isolate);
            auto res = run_script(r, BG_FULL_BUNDLE_JS, "<bg-full-bundle>");
            if (res.IsEmpty()) {
                init_error = tc.HasCaught()
                    ? to_std(r->isolate, tc.Exception()) : "bg-full bundle eval failed";
            }
        }

        // 3. Stash interpreter_js / program / globalName as globals for the snapshot driver.
        if (init_error.empty()) {
            auto g = ctx->Global();
            g->Set(ctx, v8str(r->isolate, "__epo_interp_src__"),  v8str(r->isolate, interpreter_js));
            g->Set(ctx, v8str(r->isolate, "__epo_program__"),     v8str(r->isolate, program));
            g->Set(ctx, v8str(r->isolate, "__epo_global_name__"), v8str(r->isolate, global_name));
            g->Set(ctx, v8str(r->isolate, "__epo_user_agent__"),  v8str(r->isolate, constants::USER_AGENT));
        }
    } // iso_scope, hs released here

    if (!init_error.empty()) {
        destroy_runner(r); // now safe — scope is released
        return run_error{init_error};
    }

    return r;
}

snapshot_outcome run_snapshot(runner* r) {
    auto iso = r->isolate;
    v8::Isolate::Scope iso_scope(iso);
    v8::HandleScope hs(iso);
    auto ctx = r->context.Get(iso);
    v8::Context::Scope cs(ctx);

    // Driver script: runs the bundle's runBotguard(interpreter, program, global_name, ua)
    // which sets up JSDOM, evals the interpreter, and produces a snapshot. The bundle
    // also stores `webPoSignalOutput` on globalThis for the later minter call.
    const char* driver = R"JS(
(function() {
    globalThis.__epo_state__ = null;
    globalThis.runBotguard(
        globalThis.__epo_interp_src__,
        globalThis.__epo_program__,
        globalThis.__epo_global_name__,
        globalThis.__epo_user_agent__
    ).then(function(snapshot) {
        globalThis.__epo_state__ = { snapshot: snapshot, done: true, error: null };
    }).catch(function(e) {
        globalThis.__epo_state__ = {
            done: true,
            error: e && e.message ? e.message : String(e)
        };
    });
})();
)JS";

    {
        v8::TryCatch tc(iso);
        auto res = run_script(r, driver, "<snapshot-driver>");
        if (res.IsEmpty()) {
            return run_error{tc.HasCaught() ? to_std(iso, tc.Exception()) : "driver script failed"};
        }
    }

    // Pump until __epo_state__ != null && __epo_state__.done == true
    bool ok = pump_event_loop(r, [&]() {
        auto state = get_nested(r, "__epo_state__");
        if (state->IsNull() || state->IsUndefined()) return false;
        return js_bool(r, "__epo_state__.done");
    });

    if (!ok) return run_error{"Timed out waiting for BotGuard snapshot"};

    const std::string err = js_string(r, "__epo_state__.error");
    if (!err.empty() && err != "null") return run_error{"BotGuard snapshot error: " + err};

    const std::string snapshot = js_string(r, "__epo_state__.snapshot");
    if (snapshot.empty()) return run_error{"BotGuard snapshot returned empty string"};

    return snapshot_result{snapshot};
}

po_token_outcome run_mint(runner* r,
                          const std::string& integrity_token,
                          const std::string& content_binding) {
    auto iso = r->isolate;
    v8::Isolate::Scope iso_scope(iso);
    v8::HandleScope hs(iso);
    auto ctx = r->context.Get(iso);
    v8::Context::Scope cs(ctx);

    // Pass integrity token + content binding as globals
    auto g = ctx->Global();
    g->Set(ctx, v8str(iso, "__epo_integrity_token__"),  v8str(iso, integrity_token));
    g->Set(ctx, v8str(iso, "__epo_content_binding__"),  v8str(iso, content_binding));

    const char* driver = R"JS(
(function() {
    globalThis.__epo_mint_state__ = null;
    globalThis.newMinter(globalThis.__epo_integrity_token__)
        .then(function() { return globalThis.mint(globalThis.__epo_content_binding__); })
        .then(function(po_token) {
            globalThis.__epo_mint_state__ = { po_token: po_token, done: true, error: null };
        })
        .catch(function(e) {
            globalThis.__epo_mint_state__ = {
                done: true,
                error: e && e.message ? e.message : String(e)
            };
        });
})();
)JS";

    {
        v8::TryCatch tc(iso);
        auto res = run_script(r, driver, "<mint-driver>");
        if (res.IsEmpty()) {
            return run_error{tc.HasCaught() ? to_std(iso, tc.Exception()) : "mint driver failed"};
        }
    }

    bool ok = pump_event_loop(r, [&]() {
        auto state = get_nested(r, "__epo_mint_state__");
        if (state->IsNull() || state->IsUndefined()) return false;
        return js_bool(r, "__epo_mint_state__.done");
    });

    if (!ok) return run_error{"Timed out waiting for PoToken minting"};

    const std::string err = js_string(r, "__epo_mint_state__.error");
    if (!err.empty() && err != "null") return run_error{"PoToken minting error: " + err};

    const std::string token = js_string(r, "__epo_mint_state__.po_token");
    if (token.empty()) return run_error{"Minted PoToken is empty"};

    return po_token_result{token};
}

void destroy_runner(runner* r) {
    if (!r) return;
    {
        v8::Isolate::Scope iso_scope(r->isolate);
        v8::HandleScope hs(r->isolate);
        r->context.Reset();
        for (auto& t : r->timers) t.callback.Reset();
        r->timers.clear();
    }
    r->isolate->Dispose();
    delete r->allocator;
    delete r;
}

} // namespace epotoken::v8_runner
