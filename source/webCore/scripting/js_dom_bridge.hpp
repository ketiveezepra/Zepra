#pragma once

/**
 * @file js_dom_bridge.hpp
 * @brief Bridge connecting ZepraScript JavaScript engine with WebCore DOM
 */

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

// Forward declarations
namespace Zepra::Runtime {
    class VM;
    class Value;
    class Object;
    class Context;
}

namespace Zepra::WebCore::Mini {
    class DOMDocument;
    class DOMElement;
    class DOMNode;
    class DOMEvent;
}

namespace Zepra::Bridge {

/**
 * @brief Wraps DOM nodes as JavaScript objects
 */
class JSDOMBridge {
public:
    JSDOMBridge();
    ~JSDOMBridge();
    
    // Initialize bridge with VM and document
    void initialize(Runtime::VM* vm, WebCore::Mini::DOMDocument* document);
    
    // Create JS wrappers for DOM objects
    Runtime::Object* wrapDocument(WebCore::Mini::DOMDocument* doc);
    Runtime::Object* wrapElement(WebCore::Mini::DOMElement* element);
    Runtime::Object* wrapNode(WebCore::Mini::DOMNode* node);
    Runtime::Object* wrapEvent(WebCore::Mini::DOMEvent* event);
    
    // Unwrap JS objects to DOM nodes
    WebCore::Mini::DOMNode* unwrapNode(Runtime::Object* obj);
    WebCore::Mini::DOMElement* unwrapElement(Runtime::Object* obj);
    
    // Expose global DOM APIs
    void exposeWindowObject();
    void exposeDocumentObject();
    void exposeConsoleObject();
    
    // Execute JavaScript in DOM context
    Runtime::Value executeScript(const std::string& code);
    
    // Event handling
    void dispatchEventToJS(WebCore::Mini::DOMEvent* event);
    
private:
    Runtime::VM* vm_ = nullptr;
    WebCore::Mini::DOMDocument* document_ = nullptr;
    
    // Cache of wrapped objects
    std::unordered_map<void*, Runtime::Object*> wrappedObjects_;
    
    // Built-in functions for DOM manipulation
    static Runtime::Value js_getElementById(Runtime::Context* ctx, 
                                            const std::vector<Runtime::Value>& args);
    static Runtime::Value js_getElementsByTagName(Runtime::Context* ctx,
                                                   const std::vector<Runtime::Value>& args);
    static Runtime::Value js_querySelector(Runtime::Context* ctx,
                                           const std::vector<Runtime::Value>& args);
    static Runtime::Value js_querySelectorAll(Runtime::Context* ctx,
                                              const std::vector<Runtime::Value>& args);
    static Runtime::Value js_createElement(Runtime::Context* ctx,
                                           const std::vector<Runtime::Value>& args);
    static Runtime::Value js_appendChild(Runtime::Context* ctx,
                                         const std::vector<Runtime::Value>& args);
    static Runtime::Value js_removeChild(Runtime::Context* ctx,
                                         const std::vector<Runtime::Value>& args);
    static Runtime::Value js_addEventListener(Runtime::Context* ctx,
                                              const std::vector<Runtime::Value>& args);
    static Runtime::Value js_setAttribute(Runtime::Context* ctx,
                                          const std::vector<Runtime::Value>& args);
    static Runtime::Value js_getAttribute(Runtime::Context* ctx,
                                          const std::vector<Runtime::Value>& args);
    static Runtime::Value js_setInnerHTML(Runtime::Context* ctx,
                                          const std::vector<Runtime::Value>& args);
    static Runtime::Value js_getInnerHTML(Runtime::Context* ctx,
                                          const std::vector<Runtime::Value>& args);
};

/**
 * @brief Global window object exposed to JavaScript
 */
class JSWindow {
public:
    static void expose(Runtime::VM* vm, JSDOMBridge* bridge);
    
    // Window methods
    static Runtime::Value js_alert(Runtime::Context* ctx,
                                   const std::vector<Runtime::Value>& args);
    static Runtime::Value js_confirm(Runtime::Context* ctx,
                                     const std::vector<Runtime::Value>& args);
    static Runtime::Value js_prompt(Runtime::Context* ctx,
                                    const std::vector<Runtime::Value>& args);
    static Runtime::Value js_setTimeout(Runtime::Context* ctx,
                                        const std::vector<Runtime::Value>& args);
    static Runtime::Value js_setInterval(Runtime::Context* ctx,
                                         const std::vector<Runtime::Value>& args);
    static Runtime::Value js_clearTimeout(Runtime::Context* ctx,
                                          const std::vector<Runtime::Value>& args);
    static Runtime::Value js_clearInterval(Runtime::Context* ctx,
                                           const std::vector<Runtime::Value>& args);
    static Runtime::Value js_requestAnimationFrame(Runtime::Context* ctx,
                                                   const std::vector<Runtime::Value>& args);
    static Runtime::Value js_fetch(Runtime::Context* ctx,
                                   const std::vector<Runtime::Value>& args);
};

} // namespace Zepra::Bridge
