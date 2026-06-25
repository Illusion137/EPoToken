// Node.js N-API addon (node-addon-api).
//
// Exports three functions that handle the C++ side of PoToken generation.
// The JS host (index.ts) uses bgutils-js for BotGuard execution, mirroring
// exactly how potoken.node.ts works in the original TypeScript implementation.
//
//   getAttestationChallenge(visitorData: string)
//     → Promise<{ interpreterUrl, program, globalName }>
//
//   postGenerateIT(snapshot: string)
//     → Promise<string>   (integrity token)
//
//   generatePlaceholderToken(identifier: string, clientState?: number)
//     → string            (synchronous, pure C++)

#include <napi.h>

#include "../../include/epotoken.h"
#include "../../src/challenge.h"
#include "../../src/innertube_client.h"

// ---------------------------------------------------------------------------
// getAttestationChallenge
// ---------------------------------------------------------------------------

class GetChallengeWorker final : public Napi::AsyncWorker {
public:
    GetChallengeWorker(Napi::Env env,
                       Napi::Promise::Deferred deferred,
                       std::string visitor_data)
        : Napi::AsyncWorker(env)
        , deferred_(std::move(deferred))
        , visitor_data_(std::move(visitor_data))
    {}

    void Execute() override {
        result_ = epotoken::get_attestation_challenge(visitor_data_);
    }

    void OnOK() override {
        Napi::HandleScope scope(Env());
        if (auto* ch = std::get_if<epotoken::bg_challenge>(&result_)) {
            auto obj = Napi::Object::New(Env());
            obj.Set("interpreterUrl", Napi::String::New(Env(), ch->interpreter_url));
            obj.Set("program",        Napi::String::New(Env(), ch->program));
            obj.Set("globalName",     Napi::String::New(Env(), ch->global_name));
            deferred_.Resolve(obj);
        } else {
            const auto& err = std::get<epotoken::challenge_error>(result_);
            deferred_.Reject(Napi::Error::New(Env(), err.message).Value());
        }
    }

    void OnError(const Napi::Error& e) override {
        deferred_.Reject(e.Value());
    }

private:
    Napi::Promise::Deferred      deferred_;
    std::string                  visitor_data_;
    epotoken::challenge_outcome  result_;
};

Napi::Value GetAttestationChallenge(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto deferred = Napi::Promise::Deferred::New(env);

    if (info.Length() < 1 || !info[0].IsString()) {
        deferred.Reject(
            Napi::TypeError::New(env, "visitorData must be a string").Value());
        return deferred.Promise();
    }

    std::string vd = info[0].As<Napi::String>().Utf8Value();
    auto* worker = new GetChallengeWorker(env, deferred, std::move(vd));
    worker->Queue();
    return deferred.Promise();
}

// ---------------------------------------------------------------------------
// postGenerateIT
// ---------------------------------------------------------------------------

class PostGenerateITWorker final : public Napi::AsyncWorker {
public:
    PostGenerateITWorker(Napi::Env env,
                         Napi::Promise::Deferred deferred,
                         std::string snapshot)
        : Napi::AsyncWorker(env)
        , deferred_(std::move(deferred))
        , snapshot_(std::move(snapshot))
    {}

    void Execute() override {
        result_ = epotoken::post_generate_it(snapshot_);
    }

    void OnOK() override {
        Napi::HandleScope scope(Env());
        if (auto* s = std::get_if<std::string>(&result_)) {
            deferred_.Resolve(Napi::String::New(Env(), *s));
        } else {
            const auto& err = std::get<epotoken::challenge_error>(result_);
            deferred_.Reject(Napi::Error::New(Env(), err.message).Value());
        }
    }

    void OnError(const Napi::Error& e) override {
        deferred_.Reject(e.Value());
    }

private:
    Napi::Promise::Deferred deferred_;
    std::string             snapshot_;
    std::variant<std::string, epotoken::challenge_error> result_;
};

Napi::Value PostGenerateIT(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto deferred = Napi::Promise::Deferred::New(env);

    if (info.Length() < 1 || !info[0].IsString()) {
        deferred.Reject(
            Napi::TypeError::New(env, "snapshot must be a string").Value());
        return deferred.Promise();
    }

    std::string snap = info[0].As<Napi::String>().Utf8Value();
    auto* worker = new PostGenerateITWorker(env, deferred, std::move(snap));
    worker->Queue();
    return deferred.Promise();
}

// ---------------------------------------------------------------------------
// generatePlaceholderToken  (synchronous — pure C++, sub-millisecond)
// ---------------------------------------------------------------------------

Napi::Value GeneratePlaceholderToken(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "identifier must be a string")
            .ThrowAsJavaScriptException();
        return env.Undefined();
    }

    const std::string id = info[0].As<Napi::String>().Utf8Value();
    const uint8_t client_state =
        (info.Length() >= 2 && info[1].IsNumber())
        ? static_cast<uint8_t>(info[1].As<Napi::Number>().Uint32Value())
        : 1;

    auto result = generate_placeholder_token(id, client_state);
    if (auto* s = std::get_if<std::string>(&result)) {
        return Napi::String::New(env, *s);
    }
    const auto& err = std::get<epotoken::error>(result);
    Napi::Error::New(env, err.message).ThrowAsJavaScriptException();
    return env.Undefined();
}

// ---------------------------------------------------------------------------
// Module init
// ---------------------------------------------------------------------------

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("getAttestationChallenge",
        Napi::Function::New(env, GetAttestationChallenge));
    exports.Set("postGenerateIT",
        Napi::Function::New(env, PostGenerateIT));
    exports.Set("generatePlaceholderToken",
        Napi::Function::New(env, GeneratePlaceholderToken));
    return exports;
}

NODE_API_MODULE(epotoken_napi, Init)
