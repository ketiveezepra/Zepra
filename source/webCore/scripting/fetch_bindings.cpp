// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file fetch_bindings.cpp
 * @brief fetch(), XMLHttpRequest and Headers for page scripts
 *
 * A request goes to the FetchHandler the browser installed, which performs
 * it on a worker thread and calls back from there. The callback only drops
 * the response into an inbox; the promise is resolved (or the XHR's events
 * fired) on the main thread from processNetworkResponses(), which the timer
 * pump calls each frame. So script never runs off the main thread, and a
 * tab closed while a request is in flight just loses the response.
 */

#include "scripting/script_context.hpp"

#include "runtime/async/promise.hpp"
#include "runtime/execution/js_error.hpp"
#include "runtime/execution/vm.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

namespace Zepra::WebCore {

using namespace Zepra::Runtime;

/// Responses that arrived, waiting for the main thread
struct FetchInbox {
    struct Done {
        int id;
        bool xhr;
        FetchResponse response;
    };
    std::mutex mutex;
    std::condition_variable arrived;
    std::vector<Done> done;
    int inFlight = 0;
};

namespace {

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    return s;
}

bool equalsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    }
    return true;
}

Value str(const std::string& s) { return Value::string(new String(s)); }

bool isFunction(const Value& v) { return v.isObject() && v.asObject()->isFunction(); }

/// The header fields of a `Headers` object or a plain {name: value} object
std::vector<std::pair<std::string, std::string>> headersFromObject(Object* obj) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!obj) return out;
    for (const std::string& name : obj->getOwnPropertyNames()) {
        Value v = obj->get(name);
        if (isFunction(v) || name.rfind("__", 0) == 0) continue;
        out.emplace_back(name, v.toString());
    }
    return out;
}

/// A Headers-like object: header names are its own properties (lower-cased),
/// with get/has/set/append/delete/forEach
Object* makeHeadersObject(VM* vm, const std::vector<std::pair<std::string, std::string>>& headers) {
    auto* obj = new Object();
    if (vm) obj->setPrototype(vm->intrinsic(VM::Intrinsic::Object));
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    };
    for (const auto& [name, value] : headers) {
        std::string key = lower(name);
        if (obj->has(key) && !isFunction(obj->get(key))) {
            obj->set(key, str(obj->get(key).toString() + ", " + value));
        } else {
            obj->set(key, str(value));
        }
    }
    auto method = [obj](const char* name, size_t arity, NativeFn fn) {
        obj->set(name, Value::object(createNativeFunction(name, std::move(fn), arity)));
    };
    method("get", 1, [obj, lower](Context*, const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value::null();
        std::string key = lower(args[0].toString());
        Value v = obj->get(key);
        return (v.isUndefined() || isFunction(v)) ? Value::null() : str(v.toString());
    });
    method("has", 1, [obj, lower](Context*, const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value::boolean(false);
        Value v = obj->get(lower(args[0].toString()));
        return Value::boolean(!v.isUndefined() && !isFunction(v));
    });
    method("set", 2, [obj, lower](Context*, const std::vector<Value>& args) -> Value {
        if (args.size() >= 2) obj->set(lower(args[0].toString()), str(args[1].toString()));
        return Value::undefined();
    });
    method("append", 2, [obj, lower](Context*, const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value::undefined();
        std::string key = lower(args[0].toString());
        Value v = obj->get(key);
        if (v.isUndefined() || isFunction(v)) obj->set(key, str(args[1].toString()));
        else obj->set(key, str(v.toString() + ", " + args[1].toString()));
        return Value::undefined();
    });
    method("delete", 1, [obj, lower](Context*, const std::vector<Value>& args) -> Value {
        if (!args.empty()) obj->deleteProperty(lower(args[0].toString()));
        return Value::undefined();
    });
    method("forEach", 1, [obj, vm](Context*, const std::vector<Value>& args) -> Value {
        if (args.empty() || !isFunction(args[0])) return Value::undefined();
        for (const auto& [name, value] : headersFromObject(obj)) {
            callValue(vm, args[0], Value::undefined(), {str(value), str(name), Value::object(obj)});
        }
        return Value::undefined();
    });
    return obj;
}

/// A ProgressEvent-like object for XHR listeners
Object* makeXhrEvent(VM* vm, const std::string& type, Object* target, size_t loaded) {
    auto* ev = new Object();
    if (vm) ev->setPrototype(vm->intrinsic(VM::Intrinsic::Object));
    ev->set("type", str(type));
    ev->set("target", Value::object(target));
    ev->set("currentTarget", Value::object(target));
    ev->set("lengthComputable", Value::boolean(false));
    ev->set("loaded", Value::number((double)loaded));
    ev->set("total", Value::number((double)loaded));
    ev->set("preventDefault", Value::object(createNativeFunction("preventDefault",
        [](Context*, const std::vector<Value>&) -> Value { return Value::undefined(); }, 0)));
    ev->set("stopPropagation", Value::object(createNativeFunction("stopPropagation",
        [](Context*, const std::vector<Value>&) -> Value { return Value::undefined(); }, 0)));
    return ev;
}

/// Calls xhr.on<type> and the addEventListener listeners of `type`
void fireXhrEvent(VM* vm, Object* xhr, const std::string& type, size_t loaded) {
    Value event = Value::object(makeXhrEvent(vm, type, xhr, loaded));
    Value handler = xhr->get("on" + type);
    if (isFunction(handler)) callValue(vm, handler, Value::object(xhr), {event});
    Value list = xhr->get("__listeners_" + type);
    if (list.isObject()) {
        auto* arr = dynamic_cast<Array*>(list.asObject());
        if (arr) {
            size_t n = arr->length();
            for (size_t i = 0; i < n; i++) {
                Value fn = arr->get((uint32_t)i);
                if (isFunction(fn)) callValue(vm, fn, Value::object(xhr), {event});
            }
        }
    }
}

} // namespace

/// Per-XMLHttpRequest state the native methods share
struct XhrState {
    std::string method = "GET";
    std::string url;
    bool async = true;
    std::vector<std::pair<std::string, std::string>> requestHeaders;
    std::vector<std::pair<std::string, std::string>> responseHeaders;
    int id = 0;          // request id while sent, 0 otherwise
    bool failed = false; // the last response was a network failure
};

// ============================================================================
// Globals
// ============================================================================

void ScriptContext::setupFetchGlobals(Object* windowObj) {
    fetchInbox_ = std::make_shared<FetchInbox>();
    Object* objectProto = vm_->intrinsic(VM::Intrinsic::Object);

    // --- fetch(input, init) -> Promise<Response> ---
    auto* fetchFn = createNativeFunction("fetch",
        [this](Context*, const std::vector<Value>& args) -> Value {
            auto* promise = new Promise();
            Value result = Value::object(promise);
            if (args.empty() || args[0].isNullOrUndefined()) {
                promise->reject(makeScriptError("TypeError",
                    "Failed to execute 'fetch': 1 argument required, but only 0 present."));
                return result;
            }
            FetchRequest req;
            if (args[0].isObject()) {
                Object* request = args[0].asObject();
                req.url = request->get("url").toString();
                if (request->has("method")) req.method = upper(request->get("method").toString());
                Value h = request->get("headers");
                if (h.isObject()) req.headers = headersFromObject(h.asObject());
            } else {
                req.url = args[0].toString();
            }
            if (args.size() > 1 && args[1].isObject()) {
                Object* init = args[1].asObject();
                if (init->has("method")) req.method = upper(init->get("method").toString());
                Value h = init->get("headers");
                if (h.isObject()) req.headers = headersFromObject(h.asObject());
                Value body = init->get("body");
                if (!body.isNullOrUndefined()) req.body = body.toString();
            }
            if (req.method.empty()) req.method = "GET";
            int id = nextFetchId_++;
            callbackStore_->set("F" + std::to_string(id), result);
            startFetch(id, false, req);
            return result;
        }, 1);
    vm_->setGlobal("fetch", Value::object(fetchFn));
    windowObj->set("fetch", Value::object(fetchFn));

    // --- Headers ---
    auto* headersCtor = createNativeFunction("Headers",
        [this](Context*, const std::vector<Value>& args) -> Value {
            std::vector<std::pair<std::string, std::string>> initial;
            if (!args.empty() && args[0].isObject()) {
                Object* init = args[0].asObject();
                auto* arr = dynamic_cast<Array*>(init);
                if (arr) {
                    // [[name, value], ...]
                    for (size_t i = 0; i < arr->length(); i++) {
                        Value pair = arr->get((uint32_t)i);
                        if (!pair.isObject()) continue;
                        initial.emplace_back(pair.asObject()->get(0u).toString(),
                                             pair.asObject()->get(1u).toString());
                    }
                } else {
                    initial = headersFromObject(init);
                }
            }
            return Value::object(makeHeadersObject(vm_, initial));
        }, 1);
    vm_->setGlobal("Headers", Value::object(headersCtor));
    windowObj->set("Headers", Value::object(headersCtor));

    // --- XMLHttpRequest ---
    auto* xhrCtor = createNativeFunction("XMLHttpRequest",
        [this, objectProto](Context*, const std::vector<Value>&) -> Value {
            auto* xhr = new Object();
            xhr->setPrototype(objectProto);
            auto state = std::make_shared<XhrState>();

            xhr->set("readyState", Value::number(0));
            xhr->set("status", Value::number(0));
            xhr->set("statusText", str(""));
            xhr->set("responseText", str(""));
            xhr->set("response", str(""));
            xhr->set("responseType", str(""));
            xhr->set("responseURL", str(""));
            xhr->set("timeout", Value::number(0));
            xhr->set("withCredentials", Value::boolean(false));
            for (const char* ev : {"onreadystatechange", "onload", "onerror", "onloadend",
                                   "onabort", "ontimeout", "onprogress", "onloadstart"}) {
                xhr->set(ev, Value::null());
            }
            xhr->set("UNSENT", Value::number(0));
            xhr->set("OPENED", Value::number(1));
            xhr->set("HEADERS_RECEIVED", Value::number(2));
            xhr->set("LOADING", Value::number(3));
            xhr->set("DONE", Value::number(4));
            auto* upload = new Object();
            upload->setPrototype(objectProto);
            upload->set("addEventListener", Value::object(createNativeFunction("addEventListener",
                [](Context*, const std::vector<Value>&) -> Value { return Value::undefined(); }, 2)));
            xhr->set("upload", Value::object(upload));

            auto method = [xhr](const char* name, size_t arity, NativeFn fn) {
                xhr->set(name, Value::object(createNativeFunction(name, std::move(fn), arity)));
            };

            method("open", 2, [this, xhr, state](Context*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) {
                    throw JSError("TypeError", "Failed to execute 'open' on 'XMLHttpRequest': 2 arguments required.");
                }
                state->method = upper(args[0].toString());
                state->url = args[1].toString();
                state->async = args.size() < 3 || args[2].isUndefined() || args[2].toBoolean();
                state->requestHeaders.clear();
                state->responseHeaders.clear();
                state->id = 0;
                xhr->set("readyState", Value::number(1));
                xhr->set("status", Value::number(0));
                xhr->set("statusText", str(""));
                xhr->set("responseText", str(""));
                xhr->set("response", str(""));
                fireXhrEvent(vm_, xhr, "readystatechange", 0);
                return Value::undefined();
            });
            method("setRequestHeader", 2, [state](Context*, const std::vector<Value>& args) -> Value {
                if (args.size() >= 2) state->requestHeaders.emplace_back(args[0].toString(), args[1].toString());
                return Value::undefined();
            });
            method("send", 1, [this, xhr, state](Context*, const std::vector<Value>& args) -> Value {
                if (xhr->get("readyState").toNumber() != 1 || state->id != 0) {
                    throw JSError("InvalidStateError",
                                  "Failed to execute 'send' on 'XMLHttpRequest': The object's state must be OPENED.");
                }
                FetchRequest req;
                req.method = state->method;
                req.url = state->url;
                req.headers = state->requestHeaders;
                req.xhr = true;
                if (!args.empty() && !args[0].isNullOrUndefined()) req.body = args[0].toString();
                int id = nextFetchId_++;
                state->id = id;
                callbackStore_->set("X" + std::to_string(id), Value::object(xhr));  // keeps xhr alive
                xhrInFlight_[id] = state;
                fireXhrEvent(vm_, xhr, "loadstart", 0);
                startFetch(id, true, req);
                if (!state->async) {
                    // Synchronous XHR: wait for the handler here, then settle in place
                    FetchResponse response;
                    if (!waitForFetch(id, response, 30000)) {
                        response = FetchResponse{};
                        response.error = "Synchronous request timed out";
                    }
                    callbackStore_->deleteProperty("X" + std::to_string(id));
                    xhrInFlight_.erase(id);
                    state->id = 0;
                    settleXhrNow(xhr, state, response);
                }
                return Value::undefined();
            });
            method("abort", 0, [this, xhr, state](Context*, const std::vector<Value>&) -> Value {
                if (state->id) {
                    callbackStore_->deleteProperty("X" + std::to_string(state->id));
                    xhrInFlight_.erase(state->id);
                    state->id = 0;
                }
                xhr->set("readyState", Value::number(0));
                xhr->set("status", Value::number(0));
                fireXhrEvent(vm_, xhr, "abort", 0);
                return Value::undefined();
            });
            method("getResponseHeader", 1, [state](Context*, const std::vector<Value>& args) -> Value {
                if (args.empty()) return Value::null();
                std::string name = args[0].toString();
                std::string found;
                bool any = false;
                for (const auto& [n, v] : state->responseHeaders) {
                    if (!equalsIgnoreCase(n, name)) continue;
                    found += (any ? ", " : "") + v;
                    any = true;
                }
                return any ? str(found) : Value::null();
            });
            method("getAllResponseHeaders", 0, [state](Context*, const std::vector<Value>&) -> Value {
                std::string all;
                for (const auto& [n, v] : state->responseHeaders) all += n + ": " + v + "\r\n";
                return str(all);
            });
            method("addEventListener", 2, [xhr](Context*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2 || !isFunction(args[1])) return Value::undefined();
                std::string key = "__listeners_" + args[0].toString();
                Value list = xhr->get(key);
                Array* arr = list.isObject() ? dynamic_cast<Array*>(list.asObject()) : nullptr;
                if (!arr) {
                    arr = new Array();
                    xhr->set(key, Value::object(arr));
                }
                for (size_t i = 0; i < arr->length(); i++) {
                    Value fn = arr->get((uint32_t)i);
                    if (fn.isObject() && fn.asObject() == args[1].asObject()) return Value::undefined();
                }
                arr->push(args[1]);
                return Value::undefined();
            });
            method("removeEventListener", 2, [xhr](Context*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2 || !isFunction(args[1])) return Value::undefined();
                std::string key = "__listeners_" + args[0].toString();
                Value list = xhr->get(key);
                Array* arr = list.isObject() ? dynamic_cast<Array*>(list.asObject()) : nullptr;
                if (!arr) return Value::undefined();
                auto* kept = new Array();
                for (size_t i = 0; i < arr->length(); i++) {
                    Value fn = arr->get((uint32_t)i);
                    if (!(fn.isObject() && fn.asObject() == args[1].asObject())) kept->push(fn);
                }
                xhr->set(key, Value::object(kept));
                return Value::undefined();
            });
            method("overrideMimeType", 1, [](Context*, const std::vector<Value>&) -> Value {
                return Value::undefined();
            });

            return Value::object(xhr);
        }, 0);
    xhrCtor->set("UNSENT", Value::number(0));
    xhrCtor->set("OPENED", Value::number(1));
    xhrCtor->set("HEADERS_RECEIVED", Value::number(2));
    xhrCtor->set("LOADING", Value::number(3));
    xhrCtor->set("DONE", Value::number(4));
    vm_->setGlobal("XMLHttpRequest", Value::object(xhrCtor));
    windowObj->set("XMLHttpRequest", Value::object(xhrCtor));
}

// ============================================================================
// Requests and the inbox
// ============================================================================

void ScriptContext::startFetch(int id, bool xhr, const FetchRequest& request) {
    std::weak_ptr<FetchInbox> inbox = fetchInbox_;
    {
        std::lock_guard<std::mutex> lock(fetchInbox_->mutex);
        fetchInbox_->inFlight++;
    }
    FetchDone done = [inbox, id, xhr](FetchResponse response) {
        std::shared_ptr<FetchInbox> box = inbox.lock();
        if (!box) return;  // the page is gone
        std::lock_guard<std::mutex> lock(box->mutex);
        box->done.push_back({id, xhr, std::move(response)});
        box->inFlight--;
        box->arrived.notify_all();
    };
    if (!fetchHandler_) {
        FetchResponse response;
        response.error = "No network available to scripts";
        done(std::move(response));
        return;
    }
    fetchHandler_(request, std::move(done));
}

bool ScriptContext::waitForFetch(int id, FetchResponse& response, int timeoutMs) {
    std::unique_lock<std::mutex> lock(fetchInbox_->mutex);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        auto it = std::find_if(fetchInbox_->done.begin(), fetchInbox_->done.end(),
                               [id](const FetchInbox::Done& d) { return d.id == id; });
        if (it != fetchInbox_->done.end()) {
            response = std::move(it->response);
            fetchInbox_->done.erase(it);
            return true;
        }
        if (fetchInbox_->arrived.wait_until(lock, deadline) == std::cv_status::timeout) {
            return false;
        }
    }
}

int ScriptContext::pendingFetchCount() const {
    if (!fetchInbox_) return 0;
    std::lock_guard<std::mutex> lock(fetchInbox_->mutex);
    return fetchInbox_->inFlight + (int)fetchInbox_->done.size();
}

void ScriptContext::processNetworkResponses() {
    if (!fetchInbox_ || !vm_) return;
    std::vector<FetchInbox::Done> ready;
    {
        std::lock_guard<std::mutex> lock(fetchInbox_->mutex);
        if (fetchInbox_->done.empty()) return;
        ready.swap(fetchInbox_->done);
    }
    for (auto& d : ready) {
        if (d.xhr) settleXhr(d.id, d.response);
        else settleFetch(d.id, d.response);
    }
}

// ============================================================================
// Settling
// ============================================================================

Value ScriptContext::makeScriptError(const std::string& type, const std::string& message) {
    Value thrown;
    catchAsValue(vm_, [&] { throw JSError(type, message); }, thrown);
    return thrown;
}

Object* ScriptContext::makeFetchResponse(const std::shared_ptr<const FetchResponse>& response) {
    auto* res = new Object();
    res->setPrototype(vm_->intrinsic(VM::Intrinsic::Object));
    const int status = response->status;
    res->set("ok", Value::boolean(status >= 200 && status < 300));
    res->set("status", Value::number(status));
    res->set("statusText", str(response->statusText));
    res->set("url", str(response->url));
    res->set("redirected", Value::boolean(false));
    res->set("type", str("basic"));
    res->set("bodyUsed", Value::boolean(false));
    res->set("headers", Value::object(makeHeadersObject(vm_, response->headers)));

    auto method = [res](const char* name, NativeFn fn) {
        res->set(name, Value::object(createNativeFunction(name, std::move(fn), 0)));
    };
    auto consume = [this, res, response](Value& error) -> bool {
        if (res->get("bodyUsed").toBoolean()) {
            error = makeScriptError("TypeError", "Failed to execute 'text' on 'Response': body stream already read");
            return false;
        }
        res->set("bodyUsed", Value::boolean(true));
        return true;
    };
    method("text", [consume, response](Context*, const std::vector<Value>&) -> Value {
        Value error;
        if (!consume(error)) return Value::object(Promise::rejected(error));
        return Value::object(Promise::resolved(str(response->body)));
    });
    method("json", [this, consume, response](Context*, const std::vector<Value>&) -> Value {
        Value error;
        if (!consume(error)) return Value::object(Promise::rejected(error));
        Value json = globalObject_ ? globalObject_->get("JSON") : Value::undefined();
        Value parse = json.isObject() ? json.asObject()->get("parse") : Value::undefined();
        if (!isFunction(parse)) {
            return Value::object(Promise::rejected(makeScriptError("TypeError", "JSON.parse is not available")));
        }
        Value parsed, thrown;
        bool ok = catchAsValue(vm_, [&] {
            parsed = callValue(vm_, parse, json, {str(response->body)});
        }, thrown);
        return Value::object(ok ? Promise::resolved(parsed) : Promise::rejected(thrown));
    });
    method("arrayBuffer", [this, consume](Context*, const std::vector<Value>&) -> Value {
        Value error;
        if (!consume(error)) return Value::object(Promise::rejected(error));
        return Value::object(Promise::rejected(
            makeScriptError("TypeError", "Response.arrayBuffer() is not supported yet")));
    });
    res->set("blob", res->get("arrayBuffer"));
    method("clone", [this, response](Context*, const std::vector<Value>&) -> Value {
        return Value::object(makeFetchResponse(response));
    });
    return res;
}

void ScriptContext::settleFetch(int id, const FetchResponse& response) {
    const std::string key = "F" + std::to_string(id);
    if (!callbackStore_ || !callbackStore_->has(key)) return;
    Value stored = callbackStore_->get(key);
    callbackStore_->deleteProperty(key);
    Promise* promise = stored.isObject() ? dynamic_cast<Promise*>(stored.asObject()) : nullptr;
    if (!promise) return;
    auto shared = std::make_shared<const FetchResponse>(response);
    runTask([&] {
        if (!response.error.empty()) {
            promise->reject(makeScriptError("TypeError", "Failed to fetch: " + response.error));
        } else {
            promise->resolve(Value::object(makeFetchResponse(shared)));
        }
    }, "fetch");
}

void ScriptContext::settleXhrNow(Object* xhr, const std::shared_ptr<XhrState>& state,
                                 const FetchResponse& response) {
    state->responseHeaders = response.headers;
    state->failed = !response.error.empty();
    xhr->set("readyState", Value::number(4));
    xhr->set("status", Value::number(state->failed ? 0 : response.status));
    xhr->set("statusText", str(state->failed ? "" : response.statusText));
    xhr->set("responseURL", str(response.url));
    std::string responseType = xhr->get("responseType").toString();
    if (state->failed) {
        xhr->set("responseText", str(""));
        xhr->set("response", responseType == "json" ? Value::null() : str(""));
    } else if (responseType == "json") {
        xhr->set("responseText", str(response.body));
        Value json = globalObject_ ? globalObject_->get("JSON") : Value::undefined();
        Value parse = json.isObject() ? json.asObject()->get("parse") : Value::undefined();
        Value parsed = Value::null(), thrown;
        if (isFunction(parse)) {
            if (!catchAsValue(vm_, [&] { parsed = callValue(vm_, parse, json, {str(response.body)}); }, thrown)) {
                parsed = Value::null();
            }
        }
        xhr->set("response", parsed);
    } else {
        xhr->set("responseText", str(response.body));
        xhr->set("response", str(response.body));
    }
    fireXhrEvent(vm_, xhr, "readystatechange", response.body.size());
    fireXhrEvent(vm_, xhr, state->failed ? "error" : "load", response.body.size());
    fireXhrEvent(vm_, xhr, "loadend", response.body.size());
}

void ScriptContext::settleXhr(int id, const FetchResponse& response) {
    const std::string key = "X" + std::to_string(id);
    if (!callbackStore_ || !callbackStore_->has(key)) return;  // aborted
    Value stored = callbackStore_->get(key);
    callbackStore_->deleteProperty(key);
    if (!stored.isObject()) return;
    Object* xhr = stored.asObject();
    auto it = xhrInFlight_.find(id);
    if (it == xhrInFlight_.end()) return;
    std::shared_ptr<XhrState> state = it->second;
    xhrInFlight_.erase(it);
    if (state->id != id) return;  // re-opened since
    state->id = 0;
    runTask([&] { settleXhrNow(xhr, state, response); }, "XMLHttpRequest");
}

} // namespace Zepra::WebCore
