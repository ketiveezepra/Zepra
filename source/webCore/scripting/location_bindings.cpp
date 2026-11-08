// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

/**
 * @file location_bindings.cpp
 * @brief window.location and window.history over the page's URL
 *
 * `location` reads the URL the browser gave the page (setPageUrl) as the
 * URL Standard splits it, and writing it asks the browser to navigate: the
 * request goes to the navigation handler, which loads the page once the
 * running script task is over. A change of the fragment alone loads
 * nothing: the URL changes in place and `hashchange` fires on window.
 * history.pushState/replaceState change the URL the same way (same origin
 * only); they keep `history.state` and `history.length`. popstate and
 * going back to a pushed entry are not there yet.
 */

#include "scripting/script_context.hpp"
#include "scripting/dom_bindings.hpp"
#include "scripting/page_url.hpp"

#include "runtime/execution/js_error.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"

namespace Zepra::WebCore {

using namespace Zepra::Runtime;

namespace {

Value str(const std::string& text) { return Value::string(new String(text)); }

std::string argText(const std::vector<Value>& args, size_t index) {
    return index < args.size() ? args[index].toString() : std::string("undefined");
}

void defineAccessor(Object* target, const std::string& name, NativeFn getter, NativeFn setter) {
    PropertyDescriptor desc;
    desc.getter = Value::object(createNativeFunction("get " + name, std::move(getter), 0));
    if (setter) desc.setter = Value::object(createNativeFunction("set " + name, std::move(setter), 1));
    desc.attributes = PropertyAttribute::Enumerable;
    target->defineProperty(name, desc);
}

void defineMethod(Object* target, const std::string& name, size_t arity, NativeFn fn) {
    PropertyDescriptor desc;
    desc.value = Value::object(createNativeFunction(name, std::move(fn), arity));
    desc.attributes = PropertyAttribute::Enumerable;
    target->defineProperty(name, desc);
}

// The DOMException browsers throw for a URL that does not parse
[[noreturn]] void throwBadUrl(const std::string& url) {
    throw JSError("SyntaxError", "'" + url + "' is not a valid URL.");
}

std::string stripLeading(const std::string& text, char c) {
    return !text.empty() && text[0] == c ? text.substr(1) : text;
}

} // namespace

void ScriptContext::navigateFromScript(const std::string& url, NavigationKind kind) {
    PageUrl current = PageUrl::parse(pageUrl_);
    PageUrl next = PageUrl::parse(url);
    if (kind != NavigationKind::Reload && current.valid && next.valid && next.hasFragment &&
        current.withoutFragment() == next.withoutFragment()) {
        // Navigating to a fragment: nothing loads
        const std::string oldUrl = current.href();
        const std::string newUrl = next.href();
        if (oldUrl == newUrl) return;
        setPageUrl(newUrl);
        if (navigationHandler_) navigationHandler_({newUrl, NavigationKind::SameDocument});
        scheduleTimer(nextTimerId_++, [this, oldUrl, newUrl] {
            if (!domBindings_) return;
            runTask([this, oldUrl, newUrl] {
                domBindings_->fireEvent(nullptr, "hashchange", false, false,
                                        {{"oldURL", str(oldUrl)}, {"newURL", str(newUrl)}});
            }, "hashchange listener");
        }, 0, false);
        return;
    }
    if (navigationHandler_) navigationHandler_({kind == NavigationKind::Reload ? pageUrl_ : url, kind});
}

void ScriptContext::setupLocationAndHistory(Object* windowObj) {
    auto* location = new Object();

    // Resolves `text` against the page and navigates there
    auto go = [this](const std::string& text, NavigationKind kind) {
        PageUrl target = PageUrl::resolve(pageUrl_, text);
        if (!target.valid) throwBadUrl(text);
        navigateFromScript(target.href(), kind);
    };
    // A component getter, and a setter that navigates to the URL with that
    // component changed
    auto component = [this, location](const char* name, std::function<std::string(const PageUrl&)> read,
                                      std::function<void(PageUrl&, const std::string&)> write) {
        NativeFn setter;
        if (write) {
            setter = [this, write](Context*, const std::vector<Value>& args) -> Value {
                PageUrl url = PageUrl::parse(pageUrl_);
                if (!url.valid) return Value::undefined();
                write(url, argText(args, 0));
                navigateFromScript(url.href(), NavigationKind::Assign);
                return Value::undefined();
            };
        }
        defineAccessor(location, name, [this, read](Context*, const std::vector<Value>&) -> Value {
            return str(read(PageUrl::parse(pageUrl_)));
        }, setter);
    };

    defineAccessor(location, "href", [this](Context*, const std::vector<Value>&) -> Value {
        return str(pageUrl_);
    }, [go](Context*, const std::vector<Value>& args) -> Value {
        go(argText(args, 0), NavigationKind::Assign);
        return Value::undefined();
    });
    component("origin", [](const PageUrl& u) { return u.origin(); }, nullptr);
    component("protocol", [](const PageUrl& u) { return u.protocol; },
              [](PageUrl& u, const std::string& v) {
                  std::string scheme = v.substr(0, v.find(':'));
                  if (!scheme.empty()) u = PageUrl::parse(scheme + ":" + u.href().substr(u.protocol.size()));
              });
    component("host", [](const PageUrl& u) { return u.host(); },
              [](PageUrl& u, const std::string& v) {
                  PageUrl parsed = PageUrl::parse(u.protocol + "//" + v);
                  if (parsed.valid && u.hierarchical) { u.hostname = parsed.hostname; u.port = parsed.port; }
              });
    component("hostname", [](const PageUrl& u) { return u.hostname; },
              [](PageUrl& u, const std::string& v) {
                  PageUrl parsed = PageUrl::parse(u.protocol + "//" + v);
                  if (parsed.valid && u.hierarchical) u.hostname = parsed.hostname;
              });
    component("port", [](const PageUrl& u) { return u.port; },
              [](PageUrl& u, const std::string& v) {
                  PageUrl parsed = PageUrl::parse(u.protocol + "//" + u.hostname + ":" + v);
                  if (parsed.valid && u.hierarchical) u.port = parsed.port;
              });
    component("pathname", [](const PageUrl& u) { return u.pathname; },
              [](PageUrl& u, const std::string& v) {
                  if (!u.hierarchical) return;
                  u.pathname = PageUrl::resolve(u.protocol + "//" + u.host() + "/",
                                                v.empty() || v[0] != '/' ? "/" + v : v).pathname;
              });
    component("search", [](const PageUrl& u) { return u.search(); },
              [](PageUrl& u, const std::string& v) {
                  u.query = stripLeading(v, '?');
                  u.hasQuery = !u.query.empty();
              });
    // Setting the hash changes the fragment only (an empty one keeps the '#')
    component("hash", [](const PageUrl& u) { return u.hash(); },
              [](PageUrl& u, const std::string& v) {
                  u.fragment = stripLeading(v, '#');
                  u.hasFragment = true;
              });

    defineMethod(location, "assign", 1, [go](Context*, const std::vector<Value>& args) -> Value {
        go(argText(args, 0), NavigationKind::Assign);
        return Value::undefined();
    });
    defineMethod(location, "replace", 1, [go](Context*, const std::vector<Value>& args) -> Value {
        go(argText(args, 0), NavigationKind::Replace);
        return Value::undefined();
    });
    defineMethod(location, "reload", 0, [this](Context*, const std::vector<Value>&) -> Value {
        navigateFromScript(pageUrl_, NavigationKind::Reload);
        return Value::undefined();
    });
    defineMethod(location, "toString", 0, [this](Context*, const std::vector<Value>&) -> Value {
        return str(pageUrl_);
    });

    // window.location (and a bare `location`): assigning it sets href
    defineAccessor(windowObj, "location", [location](Context*, const std::vector<Value>&) -> Value {
        return Value::object(location);
    }, [go](Context*, const std::vector<Value>& args) -> Value {
        go(argText(args, 0), NavigationKind::Assign);
        return Value::undefined();
    });

    // history
    auto* history = new Object();
    historyObject_ = history;
    history->set("state", Value::null());
    defineAccessor(history, "length", [this](Context*, const std::vector<Value>&) -> Value {
        return Value::number(historyLength_);
    }, nullptr);
    defineAccessor(history, "scrollRestoration", [](Context*, const std::vector<Value>&) -> Value {
        return str("auto");
    }, [](Context*, const std::vector<Value>&) -> Value { return Value::undefined(); });
    auto changeState = [this, history](const std::vector<Value>& args, bool push) {
        if (args.size() >= 3 && !args[2].isUndefined() && !args[2].isNull()) {
            const std::string text = args[2].toString();
            PageUrl current = PageUrl::parse(pageUrl_);
            PageUrl target = PageUrl::resolve(pageUrl_, text);
            if (!target.valid) throwBadUrl(text);
            // Same origin; local files may push other local paths
            const bool sameOrigin = target.origin() != "null" ? target.origin() == current.origin()
                                                              : target.protocol == "file:" && current.protocol == "file:";
            if (!sameOrigin) {
                throw JSError("SecurityError", "history state URL '" + target.href() +
                              "' is not of the same origin as '" + pageUrl_ + "'");
            }
            if (target.href() != pageUrl_) {
                setPageUrl(target.href());
                if (navigationHandler_) navigationHandler_({target.href(), NavigationKind::SameDocument});
            }
        }
        history->set("state", args.empty() ? Value::null() : args[0]);
        if (push) historyLength_++;
    };
    defineMethod(history, "pushState", 2, [changeState](Context*, const std::vector<Value>& args) -> Value {
        changeState(args, true);
        return Value::undefined();
    });
    defineMethod(history, "replaceState", 2, [changeState](Context*, const std::vector<Value>& args) -> Value {
        changeState(args, false);
        return Value::undefined();
    });
    // Moving through the session history is the browser's (not wired yet)
    auto noop = [](Context*, const std::vector<Value>&) -> Value { return Value::undefined(); };
    defineMethod(history, "back", 0, noop);
    defineMethod(history, "forward", 0, noop);
    defineMethod(history, "go", 1, noop);
    windowObj->set("history", Value::object(history));
}

} // namespace Zepra::WebCore
