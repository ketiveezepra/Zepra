// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file fetch_bindings.cpp
 * @brief fetch(), Request, Response, XMLHttpRequest and Headers for page scripts
 *
 * A request goes to the FetchHandler the browser installed, which performs
 * it on a worker thread and calls back from there. The callback only drops
 * the response into an inbox; the promise is resolved (or the XHR's events
 * fired) on the main thread from processNetworkResponses(), which the timer
 * pump calls each frame. So script never runs off the main thread, and a
 * tab closed while a request is in flight just loses the response.
 */

#include "scripting/script_context.hpp"
#include "scripting/page_url.hpp"

#include "builtins/typed_array.hpp"

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

/// A non-enumerable data property (an interface prototype's constructor)
void defineHiddenProperty(Object* target, const std::string& name, const Value& value) {
    PropertyDescriptor desc;
    desc.value = value;
    desc.attributes = PropertyAttribute::Writable | PropertyAttribute::Configurable;
    target->defineProperty(name, desc);
}

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
    // Headers.prototype when there is one (what `x instanceof Headers`
    // checks; the methods stay on each object)
    Object* proto = vm ? vm->intrinsic(VM::Intrinsic::Object) : nullptr;
    if (vm) {
        Value ctor = vm->getGlobal("Headers");
        if (ctor.isObject() && ctor.asObject()->isFunction()) {
            Value p = ctor.asObject()->get("prototype");
            if (p.isObject()) proto = p.asObject();
        }
    }
    if (proto) obj->setPrototype(proto);
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


/// The header list a HeadersInit gives: a Headers object, a record
/// ({name: value}) or a sequence of [name, value] pairs
std::vector<std::pair<std::string, std::string>> headersFromInit(const Value& init) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!init.isObject()) return out;
    Object* obj = init.asObject();
    if (auto* arr = dynamic_cast<Array*>(obj)) {
        for (size_t i = 0; i < arr->length(); i++) {
            Value pair = arr->get((uint32_t)i);
            if (!pair.isObject()) {
                throw JSError("TypeError", "Failed to construct 'Headers': The provided value cannot be converted to a sequence.");
            }
            Object* p = pair.asObject();
            auto* pairArray = dynamic_cast<Array*>(p);
            double count = pairArray ? (double)pairArray->length() : p->get("length").toNumber();
            if (count != 2) {
                throw JSError("TypeError", "Failed to construct 'Headers': Invalid value");
            }
            out.emplace_back(p->get(0u).toString(), p->get(1u).toString());
        }
        return out;
    }
    return headersFromObject(obj);
}

bool hasHeader(const std::vector<std::pair<std::string, std::string>>& headers, const std::string& name) {
    for (const auto& h : headers) {
        if (equalsIgnoreCase(h.first, name)) return true;
    }
    return false;
}

/**
 * @brief A BodyInit's bytes and the Content-Type it implies (Fetch
 * "extract a body"): strings are UTF-8 text, ArrayBuffer and views give their
 * bytes, URLSearchParams its serialization; other objects their string.
 */
void extractBody(VM* vm, const Value& body, std::string& bytes, std::string& contentType) {
    contentType.clear();
    if (body.isString()) {
        bytes = body.toString();
        contentType = "text/plain;charset=UTF-8";
        return;
    }
    if (body.isObject()) {
        Object* obj = body.asObject();
        if (auto* view = dynamic_cast<Builtins::TypedArrayObject*>(obj)) {
            bytes.assign(reinterpret_cast<const char*>(view->data()), view->byteLength());
            return;
        }
        if (auto* buffer = dynamic_cast<Builtins::ArrayBufferObject*>(obj)) {
            bytes.assign(reinterpret_cast<const char*>(buffer->data()), buffer->byteLength());
            return;
        }
        Value ctor = obj->get("constructor");
        if (ctor.isObject() && ctor.asObject()->get("name").toString() == "URLSearchParams") {
            contentType = "application/x-www-form-urlencoded;charset=UTF-8";
        }
        bytes = vm ? vm->toPrimitiveValue(body, true).toString() : body.toString();
        return;
    }
    bytes = body.toString();
    contentType = "text/plain;charset=UTF-8";
}

/// The method a script gave, normalized as the Fetch Standard says: the six
/// standard methods are upper-cased, CONNECT/TRACE/TRACK are forbidden
std::string normalizeMethod(const std::string& method) {
    if (method.empty()) {
        throw JSError("TypeError", "Failed to construct 'Request': '' is not a valid HTTP method.");
    }
    for (unsigned char c : method) {
        // an HTTP token
        if (c <= 0x20 || c >= 0x7f || std::string("\"(),/:;<=>?@[\\]{}").find((char)c) != std::string::npos) {
            throw JSError("TypeError", "Failed to construct 'Request': '" + method + "' is not a valid HTTP method.");
        }
    }
    std::string up = upper(method);
    if (up == "CONNECT" || up == "TRACE" || up == "TRACK") {
        throw JSError("TypeError", "Failed to construct 'Request': '" + method + "' HTTP method is unsupported.");
    }
    for (const char* standard : {"DELETE", "GET", "HEAD", "OPTIONS", "POST", "PUT"}) {
        if (up == standard) return up;
    }
    return method;
}

/// A Request's state; its fields are also plain properties scripts read
class RequestObject : public Object {
public:
    std::string url;
    std::string method = "GET";
    std::string body;
    bool hasBody = false;
};

/// What `new Request(input, init)` (and fetch(input, init)) computes
struct RequestData {
    std::string url;
    std::string method = "GET";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool hasBody = false;
    std::string mode = "cors";
    std::string credentials = "same-origin";
    std::string cache = "default";
    std::string redirect = "follow";
    std::string referrer = "about:client";
    std::string referrerPolicy;
    std::string integrity;
    bool keepalive = false;
    Value signal = Value::null();
};

/// Fetch's Request constructor steps, minus streams: copies a Request input,
/// resolves a URL input against `baseUrl` (left as given when there is no
/// usable base, the browser's handler resolves it then), applies `init`
RequestData readRequest(VM* vm, const std::string& baseUrl, const Value& input, const Value& initValue) {
    RequestData data;
    auto* source = input.isObject() ? dynamic_cast<RequestObject*>(input.asObject()) : nullptr;
    if (source) {
        data.url = source->url;
        data.method = source->method;
        Value h = source->get("headers");
        if (h.isObject()) data.headers = headersFromObject(h.asObject());
        data.body = source->body;
        data.hasBody = source->hasBody;
        for (auto [field, key] : {std::pair{&data.mode, "mode"}, {&data.credentials, "credentials"},
                                  {&data.cache, "cache"}, {&data.redirect, "redirect"},
                                  {&data.referrer, "referrer"}, {&data.referrerPolicy, "referrerPolicy"},
                                  {&data.integrity, "integrity"}}) {
            Value v = source->get(key);
            if (v.isString()) *field = v.toString();
        }
        data.keepalive = source->get("keepalive").toBoolean();
        data.signal = source->get("signal");
    } else {
        std::string text = input.isObject() && vm ? vm->toPrimitiveValue(input, true).toString() : input.toString();
        PageUrl resolved = PageUrl::resolve(baseUrl, text);
        data.url = resolved.valid && resolved.protocol != "about:" ? resolved.href() : text;
    }
    if (!initValue.isObject()) return data;
    Object* init = initValue.asObject();
    Value v = init->get("method");
    if (!v.isUndefined()) data.method = normalizeMethod(v.toString());
    v = init->get("headers");
    if (!v.isUndefined()) data.headers = headersFromInit(v);
    for (auto [field, key] : {std::pair{&data.mode, "mode"}, {&data.credentials, "credentials"},
                              {&data.cache, "cache"}, {&data.redirect, "redirect"},
                              {&data.referrer, "referrer"}, {&data.referrerPolicy, "referrerPolicy"},
                              {&data.integrity, "integrity"}}) {
        v = init->get(key);
        if (!v.isUndefined()) *field = v.toString();
    }
    v = init->get("keepalive");
    if (!v.isUndefined()) data.keepalive = v.toBoolean();
    v = init->get("signal");
    if (!v.isUndefined()) data.signal = v;
    v = init->get("body");
    if (!v.isNullOrUndefined()) {
        std::string contentType;
        extractBody(vm, v, data.body, contentType);
        data.hasBody = true;
        if (!contentType.empty() && !hasHeader(data.headers, "Content-Type")) {
            data.headers.emplace_back("Content-Type", contentType);
        }
    } else if (init->has("body")) {
        data.body.clear();
        data.hasBody = false;
    }
    if (data.hasBody && (data.method == "GET" || data.method == "HEAD")) {
        throw JSError("TypeError", "Failed to construct 'Request': Request with GET/HEAD method cannot have body.");
    }
    return data;
}

/// A Request object for `data`, inheriting `proto` (Request.prototype)
RequestObject* makeRequestObject(VM* vm, Object* proto, const RequestData& data) {
    auto* req = new RequestObject();
    if (proto) req->setPrototype(proto);
    req->url = data.url;
    req->method = data.method;
    req->body = data.body;
    req->hasBody = data.hasBody;
    req->set("url", str(data.url));
    req->set("method", str(data.method));
    req->set("headers", Value::object(makeHeadersObject(vm, data.headers)));
    req->set("mode", str(data.mode));
    req->set("credentials", str(data.credentials));
    req->set("cache", str(data.cache));
    req->set("redirect", str(data.redirect));
    req->set("referrer", str(data.referrer));
    req->set("referrerPolicy", str(data.referrerPolicy));
    req->set("integrity", str(data.integrity));
    req->set("keepalive", Value::boolean(data.keepalive));
    req->set("signal", data.signal);
    req->set("destination", str(""));
    req->set("bodyUsed", Value::boolean(false));
    return req;
}

/// The FetchRequest the browser performs for `data`
FetchRequest toFetchRequest(const RequestData& data) {
    FetchRequest req;
    req.url = data.url;
    req.method = data.method;
    req.headers = data.headers;
    if (data.hasBody) req.body = data.body;
    return req;
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
            // The Request constructor's steps; their errors reject the promise
            FetchRequest req;
            Value thrown;
            bool ok = catchAsValue(vm_, [&] {
                req = toFetchRequest(readRequest(vm_, pageUrl_, args[0],
                                                 args.size() > 1 ? args[1] : Value::undefined()));
            }, thrown);
            if (!ok) {
                promise->reject(thrown);
                return result;
            }
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
            std::vector<std::pair<std::string, std::string>> initial =
                headersFromInit(args.empty() ? Value::undefined() : args[0]);
            return Value::object(makeHeadersObject(vm_, initial));
        }, 1);
    // Headers.prototype, for instanceof
    auto* headersProto = new Object();
    headersProto->setPrototype(vm_->intrinsic(VM::Intrinsic::Object));
    headersCtor->set("prototype", Value::object(headersProto));
    defineHiddenProperty(headersProto, "constructor", Value::object(headersCtor));
    vm_->setGlobal("Headers", Value::object(headersCtor));
    windowObj->set("Headers", Value::object(headersCtor));

    // --- Request ---
    auto* requestProto = new Object();
    requestProto->setPrototype(objectProto);
    auto* requestCtor = createNativeFunction("Request",
        [this, requestProto](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                throw JSError("TypeError", "Failed to construct 'Request': 1 argument required, but only 0 present.");
            }
            RequestData data = readRequest(vm_, pageUrl_, args[0], args.size() > 1 ? args[1] : Value::undefined());
            return Value::object(makeRequestObject(vm_, requestProto, data));
        }, 1);
    requestCtor->set("prototype", Value::object(requestProto));
    defineHiddenProperty(requestProto, "constructor", Value::object(requestCtor));
    auto requestMethod = [requestProto](const char* name, BuiltinFn fn) {
        requestProto->set(name, Value::object(new Function(name, std::move(fn), 0)));
    };
    auto thisRequest = [](const FunctionCallInfo& info, const char* name) -> RequestObject* {
        Value t = info.thisValue();
        auto* req = t.isObject() ? dynamic_cast<RequestObject*>(t.asObject()) : nullptr;
        if (!req) throw JSError("TypeError", std::string("Illegal invocation: Request.prototype.") + name);
        return req;
    };
    // Reads the body once, as text
    auto consumeRequest = [this](RequestObject* req, Value& error) -> bool {
        if (req->get("bodyUsed").toBoolean()) {
            error = makeScriptError("TypeError", "Failed to execute 'text' on 'Request': body stream already read");
            return false;
        }
        if (req->hasBody) req->set("bodyUsed", Value::boolean(true));
        return true;
    };
    requestMethod("clone", [this, requestProto, thisRequest](const FunctionCallInfo& info) -> Value {
        RequestObject* req = thisRequest(info, "clone");
        if (req->get("bodyUsed").toBoolean()) {
            throw JSError("TypeError", "Failed to execute 'clone' on 'Request': Request body is already used");
        }
        RequestData data = readRequest(vm_, pageUrl_, Value::object(req), Value::undefined());
        return Value::object(makeRequestObject(vm_, requestProto, data));
    });
    requestMethod("text", [thisRequest, consumeRequest](const FunctionCallInfo& info) -> Value {
        RequestObject* req = thisRequest(info, "text");
        Value error;
        if (!consumeRequest(req, error)) return Value::object(Promise::rejected(error));
        return Value::object(Promise::resolved(str(req->body)));
    });
    requestMethod("json", [this, thisRequest, consumeRequest](const FunctionCallInfo& info) -> Value {
        RequestObject* req = thisRequest(info, "json");
        Value error;
        if (!consumeRequest(req, error)) return Value::object(Promise::rejected(error));
        Value json = globalObject_ ? globalObject_->get("JSON") : Value::undefined();
        Value parse = json.isObject() ? json.asObject()->get("parse") : Value::undefined();
        Value parsed, thrown;
        if (!isFunction(parse)) return Value::object(Promise::rejected(makeScriptError("TypeError", "JSON.parse is not available")));
        bool ok = catchAsValue(vm_, [&] { parsed = callValue(vm_, parse, json, {str(req->body)}); }, thrown);
        return Value::object(ok ? Promise::resolved(parsed) : Promise::rejected(thrown));
    });
    vm_->setGlobal("Request", Value::object(requestCtor));
    windowObj->set("Request", Value::object(requestCtor));

    // --- Response ---
    // Responses fetch() resolves with and `new Response(body, init)` share
    // Response.prototype; their methods are per object (makeFetchResponse)
    auto* responseProto = new Object();
    responseProto->setPrototype(objectProto);
    // A Response made by script: status 200-599, a body from any BodyInit
    auto makeScriptResponse = [this](const Value& bodyValue, const Value& initValue, const std::string& type) -> Object* {
        auto response = std::make_shared<FetchResponse>();
        response->status = 200;
        if (initValue.isObject()) {
            Object* init = initValue.asObject();
            Value v = init->get("status");
            if (!v.isUndefined()) {
                double status = v.toNumber();
                if (!(status >= 200 && status <= 599) || status != (double)(int)status) {
                    throw JSError("RangeError", "Failed to construct 'Response': The status provided (" +
                                  v.toString() + ") is outside the range [200, 599].");
                }
                response->status = (int)status;
            }
            v = init->get("statusText");
            if (!v.isUndefined()) response->statusText = v.toString();
            v = init->get("headers");
            if (!v.isUndefined()) response->headers = headersFromInit(v);
        }
        if (!bodyValue.isNullOrUndefined()) {
            int s = response->status;
            if (s == 101 || s == 103 || s == 204 || s == 205 || s == 304) {
                throw JSError("TypeError", "Failed to construct 'Response': Response with null body status cannot have body");
            }
            std::string contentType;
            extractBody(vm_, bodyValue, response->body, contentType);
            if (!contentType.empty() && !hasHeader(response->headers, "Content-Type")) {
                response->headers.emplace_back("Content-Type", contentType);
            }
        }
        Object* res = makeFetchResponse(response);
        res->set("type", str(type));
        return res;
    };
    auto* responseCtor = createNativeFunction("Response",
        [makeScriptResponse](Context*, const std::vector<Value>& args) -> Value {
            return Value::object(makeScriptResponse(args.empty() ? Value::undefined() : args[0],
                                                    args.size() > 1 ? args[1] : Value::undefined(), "default"));
        }, 0);
    responseCtor->set("prototype", Value::object(responseProto));
    defineHiddenProperty(responseProto, "constructor", Value::object(responseCtor));
    responseCtor->set("error", Value::object(createNativeFunction("error",
        [this](Context*, const std::vector<Value>&) -> Value {
            auto response = std::make_shared<FetchResponse>();
            Object* res = makeFetchResponse(response);
            res->set("type", str("error"));
            return Value::object(res);
        }, 0)));
    responseCtor->set("json", Value::object(createNativeFunction("json",
        [this, makeScriptResponse](Context*, const std::vector<Value>& args) -> Value {
            Value json = globalObject_ ? globalObject_->get("JSON") : Value::undefined();
            Value stringify = json.isObject() ? json.asObject()->get("stringify") : Value::undefined();
            if (!isFunction(stringify)) throw JSError("TypeError", "JSON.stringify is not available");
            Value text = callValue(vm_, stringify, json, {args.empty() ? Value::undefined() : args[0]});
            if (text.isUndefined()) {
                throw JSError("TypeError", "Failed to execute 'json' on 'Response': The data is not JSON serializable");
            }
            Object* res = makeScriptResponse(text, args.size() > 1 ? args[1] : Value::undefined(), "default");
            Value headers = res->get("headers");
            if (headers.isObject()) headers.asObject()->set("content-type", str("application/json"));
            return Value::object(res);
        }, 1)));
    responseCtor->set("redirect", Value::object(createNativeFunction("redirect",
        [this](Context*, const std::vector<Value>& args) -> Value {
            int status = args.size() > 1 ? (int)args[1].toNumber() : 302;
            if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) {
                throw JSError("RangeError", "Failed to execute 'redirect' on 'Response': Invalid status code");
            }
            auto response = std::make_shared<FetchResponse>();
            response->status = status;
            std::string target = args.empty() ? std::string() : args[0].toString();
            PageUrl resolved = PageUrl::resolve(pageUrl_, target);
            response->headers.emplace_back("Location", resolved.valid ? resolved.href() : target);
            Object* res = makeFetchResponse(response);
            res->set("type", str("default"));
            return Value::object(res);
        }, 1)));
    vm_->setGlobal("Response", Value::object(responseCtor));
    windowObj->set("Response", Value::object(responseCtor));

    // --- XMLHttpRequest ---
    // XMLHttpRequest.prototype exists (libraries read and patch it); the
    // methods themselves are per instance
    auto* xhrProto = new Object();
    xhrProto->setPrototype(objectProto);
    auto* xhrCtor = createNativeFunction("XMLHttpRequest",
        [this, objectProto, xhrProto](Context*, const std::vector<Value>&) -> Value {
            auto* xhr = new Object();
            xhr->setPrototype(xhrProto);
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
    xhrCtor->set("prototype", Value::object(xhrProto));
    defineHiddenProperty(xhrProto, "constructor", Value::object(xhrCtor));
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
    // Response.prototype, for instanceof (the methods stay on each object)
    Object* proto = vm_->intrinsic(VM::Intrinsic::Object);
    Value ctor = vm_->getGlobal("Response");
    if (ctor.isObject() && ctor.asObject()->isFunction()) {
        Value p = ctor.asObject()->get("prototype");
        if (p.isObject()) proto = p.asObject();
    }
    res->setPrototype(proto);
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
