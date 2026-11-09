// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file script_elements.cpp
 * @brief <script> elements: when they run (HTML "prepare the script
 *        element" and "execute the script element")
 *
 * Every script an element carries runs through executeScriptElement(), from
 * one of three places:
 *  - prepareScript(), right away: inline classic scripts, and the parser's
 *    blocking external scripts (fetched, waited for, run)
 *  - runDeferredScripts(), when the parser is done: defer scripts and
 *    parser-inserted modules, in document order
 *  - runReadyScripts(), from the timer pump: async scripts and the ones other
 *    scripts insert, each as a task of its own once its source arrived
 *    (those that set async = false in insertion order, the rest as they come)
 *
 * The fetch goes through the ScriptFetcher the browser installed, which may
 * answer at once or later from another thread: the answer only lands in an
 * inbox, so an external script a script inserts never runs inside the
 * appendChild that inserted it, but after that script is over.
 *
 * Module scripts run as classic scripts (the engine has no module loader
 * yet); they keep the module timing (deferred, or async) and
 * document.currentScript is null while they run, as for real modules.
 */

#include "scripting/script_context.hpp"
#include "scripting/dom_bindings.hpp"
#include "scripting/page_url.hpp"

#include "runtime/execution/vm.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>

namespace Zepra::WebCore {

using namespace Zepra::Runtime;

/// Fetch results that arrived, waiting for the main thread
struct ScriptInbox {
    std::mutex mutex;
    std::condition_variable arrived;
    std::vector<std::pair<int, ScriptFetchResult>> done;
};

/// A prepared script on its way to running
struct PendingScript {
    int id = 0;
    DOMElement* element = nullptr;   // valid while its wrapper is in pendingScriptStore_
    bool external = false;
    bool module = false;
    bool ready = false;              // the source is here (or the fetch failed)
    bool failed = false;             // fire error instead of running
    std::string url;                 // the script's URL (the page's, inline)
    std::string source;
    std::string error;
    uint32_t firstLine = 1;
};

namespace {

enum class ScriptKind { Classic, Module, None };

std::string trimmedLower(std::string s) {
    auto space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; };
    while (!s.empty() && space((unsigned char)s.back())) s.pop_back();
    size_t start = 0;
    while (start < s.size() && space((unsigned char)s[start])) start++;
    s = s.substr(start);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// The script's type: classic for the JavaScript MIME types (and no type),
// module for "module", none for anything else (JSON, templates, text/babel)
ScriptKind scriptKind(const DOMElement* script) {
    std::string type;
    if (script->hasAttribute("type")) {
        type = script->getAttribute("type");
        if (type.empty()) return ScriptKind::Classic;
    } else if (script->hasAttribute("language")) {
        std::string language = script->getAttribute("language");
        if (language.empty()) return ScriptKind::Classic;
        type = "text/" + language;
    } else {
        return ScriptKind::Classic;
    }
    type = trimmedLower(type);
    static const char* const kJavaScriptTypes[] = {
        "application/ecmascript", "application/javascript", "application/x-ecmascript",
        "application/x-javascript", "text/ecmascript", "text/javascript", "text/javascript1.0",
        "text/javascript1.1", "text/javascript1.2", "text/javascript1.3", "text/javascript1.4",
        "text/javascript1.5", "text/jscript", "text/livescript", "text/x-ecmascript", "text/x-javascript",
    };
    for (const char* js : kJavaScriptTypes) {
        if (type == js) return ScriptKind::Classic;
    }
    if (type == "module") return ScriptKind::Module;
    return ScriptKind::None;
}

// A script's text: its Text children, not its descendants'
std::string childText(const DOMElement* script) {
    std::string text;
    for (const auto& c : script->childNodes()) {
        if (c->nodeType() == NodeType::Text) text += c->nodeValue();
    }
    return text;
}

std::string storeKey(int id) { return "s" + std::to_string(id); }

constexpr int kScriptWaitMs = 60000;

} // namespace

void ScriptContext::setupScriptElements() {
    if (!vm_ || !domBindings_) return;
    scriptInbox_ = std::make_shared<ScriptInbox>();
    pendingScriptStore_ = new Object();
    vm_->setGlobal("__zepraPendingScripts__", Value::object(pendingScriptStore_));
    domBindings_->setScriptElementHook([this](DOMElement* script) { prepareScript(script, false); });
    domBindings_->setDocumentUrlSource([this] { return pageUrl_; });
}

void ScriptContext::prepareScript(DOMElement* element, bool parserInserted, uint32_t firstLine) {
    if (!element || !vm_ || !domBindings_ || element->scriptAlreadyStarted()) return;
    const bool hasSrc = element->hasAttribute("src");
    std::string text = childText(element);
    // An empty script waits for its text or src (it is prepared again then)
    if (!hasSrc && text.empty()) return;
    if (!domBindings_->isConnected(element)) return;
    const ScriptKind kind = scriptKind(element);
    if (kind == ScriptKind::None) return;
    if (parserInserted) element->setScriptForceAsync(false);
    element->setScriptAlreadyStarted(true);
    // nomodule: the fallback for browsers that cannot run the type="module" ones
    if (kind == ScriptKind::Classic && element->hasAttribute("nomodule")) return;

    auto script = std::make_shared<PendingScript>();
    script->id = nextScriptId_++;
    script->element = element;
    script->module = kind == ScriptKind::Module;
    script->firstLine = firstLine;
    pendingScriptStore_->set(storeKey(script->id), domBindings_->wrap(element));

    const bool async = element->hasAttribute("async");
    const bool defer = element->hasAttribute("defer");
    if (hasSrc) {
        script->external = true;
        std::string src = element->getAttribute("src");
        PageUrl url = src.find_first_not_of(" \t\n\r\f") == std::string::npos
            ? PageUrl() : PageUrl::resolve(pageUrl_, src);
        if (!url.valid) {
            // An empty or unusable src: an error event, as a task
            script->failed = script->ready = true;
            script->error = "invalid src '" + src + "'";
            asyncScripts_.push_back(script);
            return;
        }
        script->url = url.href();
        startScriptFetch(script);
    } else {
        script->url = pageUrl_;
        script->source = std::move(text);
        script->ready = true;
    }

    if (script->module) {
        if (parserInserted && !async) deferredScripts_.push_back(script);
        else if (script->external && !async && !element->scriptForceAsync()) orderedScripts_.push_back(script);
        else asyncScripts_.push_back(script);
        return;
    }
    if (!script->external) {
        executeScriptElement(*script);
        return;
    }
    if (parserInserted && !async && defer) {
        deferredScripts_.push_back(script);
    } else if (parserInserted && !async) {
        // Parser-blocking: the parser is not interleaved with scripts yet, so
        // "blocking" means it runs before the next script the walk reaches
        waitForScript(*script, kScriptWaitMs);
        runTask([&] { executeScriptElement(*script); }, "script");
    } else if (!async && !element->scriptForceAsync()) {
        orderedScripts_.push_back(script);
    } else {
        asyncScripts_.push_back(script);
    }
}

void ScriptContext::runParserScripts(const std::function<uint32_t(DOMElement*)>& firstLineOf) {
    if (!document_ || !vm_ || !domBindings_ || !pendingScriptStore_) return;
    // The scripts the parser made, in document order. Taken before any of
    // them runs: a script that removes itself (Cloudflare's email-decode,
    // analytics snippets) or inserts nodes must not make the walk skip or
    // repeat one. Their wrappers keep them alive meanwhile.
    std::vector<DOMElement*> scripts;
    std::function<void(DOMNode*)> collect = [&](DOMNode* node) {
        for (const auto& c : node->childNodes()) {
            if (c->nodeType() != NodeType::Element) continue;
            auto* e = static_cast<DOMElement*>(c.get());
            std::string tag = e->tagName();
            std::transform(tag.begin(), tag.end(), tag.begin(), [](unsigned char ch) { return std::tolower(ch); });
            if (tag == "script") scripts.push_back(e);
            else collect(e);
        }
    };
    collect(document_);
    for (size_t i = 0; i < scripts.size(); i++) {
        pendingScriptStore_->set("p" + std::to_string(i), domBindings_->wrap(scripts[i]));
    }
    for (size_t i = 0; i < scripts.size(); i++) {
        const std::string key = "p" + std::to_string(i);
        Value wrapper = pendingScriptStore_->get(key);
        pendingScriptStore_->deleteProperty(key);
        auto* element = wrapper.isObject() ? static_cast<DOMElement*>(DOMBindings::nodeOf(wrapper.asObject())) : nullptr;
        // One an earlier script removed does not run (prepareScript checks)
        if (element) prepareScript(element, true, firstLineOf ? firstLineOf(element) : 1);
    }
}

void ScriptContext::startScriptFetch(const std::shared_ptr<PendingScript>& script) {
    std::weak_ptr<ScriptInbox> inbox = scriptInbox_;
    int id = script->id;
    fetchingScripts_[id] = script;
    ScriptFetchDone done = [inbox, id](ScriptFetchResult result) {
        std::shared_ptr<ScriptInbox> box = inbox.lock();
        if (!box) return;   // the page is gone
        std::lock_guard<std::mutex> lock(box->mutex);
        box->done.emplace_back(id, std::move(result));
        box->arrived.notify_all();
    };
    std::cout << "[JS] Fetching " << (script->element->scriptForceAsync() ? "script-inserted " : "")
              << "script: " << script->url << std::endl;
    if (!scriptFetcher_) {
        ScriptFetchResult result;
        result.error = "no network for scripts";
        done(std::move(result));
        return;
    }
    scriptFetcher_(script->url, std::move(done));
}

void ScriptContext::collectArrivedScripts() {
    if (!scriptInbox_) return;
    std::vector<std::pair<int, ScriptFetchResult>> arrived;
    {
        std::lock_guard<std::mutex> lock(scriptInbox_->mutex);
        if (scriptInbox_->done.empty()) return;
        arrived.swap(scriptInbox_->done);
    }
    for (auto& [id, result] : arrived) {
        auto it = fetchingScripts_.find(id);
        if (it == fetchingScripts_.end()) continue;
        std::shared_ptr<PendingScript> script = it->second.lock();
        fetchingScripts_.erase(it);
        if (!script) continue;
        script->ready = true;
        script->failed = !result.ok;
        script->source = std::move(result.body);
        script->error = std::move(result.error);
    }
}

bool ScriptContext::waitForScript(PendingScript& script, int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        collectArrivedScripts();
        if (script.ready) return true;
        std::unique_lock<std::mutex> lock(scriptInbox_->mutex);
        if (scriptInbox_->done.empty() &&
            !scriptInbox_->arrived.wait_until(lock, deadline, [&] { return !scriptInbox_->done.empty(); })) {
            fetchingScripts_.erase(script.id);
            script.ready = script.failed = true;
            script.error = "timed out";
            return false;
        }
    }
}

void ScriptContext::executeScriptElement(PendingScript& script) {
    // The element, unless it was destroyed meanwhile (its wrapper kept it
    // alive while it waited, so that only happens to a page being torn down)
    DOMElement* element = nullptr;
    const std::string key = storeKey(script.id);
    if (pendingScriptStore_ && pendingScriptStore_->has(key)) {
        Value wrapper = pendingScriptStore_->get(key);
        if (wrapper.isObject()) element = static_cast<DOMElement*>(DOMBindings::nodeOf(wrapper.asObject()));
        pendingScriptStore_->deleteProperty(key);
    }
    if (!element || !domBindings_) return;

    if (script.failed) {
        std::cerr << "[JS] Failed to load: " << (script.url.empty() ? "<script>" : script.url)
                  << (script.error.empty() ? "" : " (" + script.error + ")") << std::endl;
        domBindings_->fireEvent(element, "error", false, false);
        return;
    }
    if (script.external) {
        std::cout << "[JS] Executing external script (" << script.source.size() << " bytes): " << script.url << std::endl;
        registerSource(script.url, script.source);
    } else {
        std::cout << "[JS] Executing inline script (" << script.source.size() << " bytes)" << std::endl;
    }
    DOMElement* previous = domBindings_->currentScript();
    domBindings_->setCurrentScript(script.module ? nullptr : element);
    ScriptResult result = evaluate(script.source, script.url, script.external ? 1 : script.firstLine);
    domBindings_->setCurrentScript(previous);
    if (!result.success) {
        std::cerr << "[JS] Uncaught error in " << (script.external ? script.url : std::string("inline script"))
                  << ": " << result.error << std::endl;
    }
    script.source.clear();
    script.source.shrink_to_fit();
    if (script.external) domBindings_->fireEvent(element, "load", false, false);
}

void ScriptContext::runDeferredScripts() {
    // Runs in order; a defer script cannot add another one (only the parser can)
    std::vector<std::shared_ptr<PendingScript>> deferred;
    deferred.swap(deferredScripts_);
    for (auto& script : deferred) {
        collectArrivedScripts();
        if (script->external) waitForScript(*script, kScriptWaitMs);
        runTask([&] { executeScriptElement(*script); }, "script");
    }
}

void ScriptContext::runReadyScripts() {
    if (runningReadyScripts_ || !vm_) return;
    runningReadyScripts_ = true;
    for (;;) {
        collectArrivedScripts();
        std::shared_ptr<PendingScript> next;
        // The async set, in the order the sources arrived (ready ones are
        // taken first-prepared-first; any order is allowed)
        auto it = std::find_if(asyncScripts_.begin(), asyncScripts_.end(),
                               [](const std::shared_ptr<PendingScript>& s) { return s->ready; });
        if (it != asyncScripts_.end()) {
            next = *it;
            asyncScripts_.erase(it);
        } else if (!orderedScripts_.empty() && orderedScripts_.front()->ready) {
            // async = false scripts: the first, once it is here
            next = orderedScripts_.front();
            orderedScripts_.erase(orderedScripts_.begin());
        }
        if (!next) break;
        runTask([&] { executeScriptElement(*next); }, "script");
    }
    runningReadyScripts_ = false;
    if (loadEventDelayed_ && asyncScripts_.empty() && orderedScripts_.empty() && deferredScripts_.empty()) {
        loadEventDelayed_ = false;
        fireLoadEventNow();
    }
}

size_t ScriptContext::pendingScriptCount() const {
    return deferredScripts_.size() + orderedScripts_.size() + asyncScripts_.size();
}

} // namespace Zepra::WebCore
