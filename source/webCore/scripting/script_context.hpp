/**
 * @file script_context.hpp
 * @brief High-level JavaScript execution context for browser integration
 */

#pragma once

#include "browser/dom.hpp"
#include "browser/event.hpp"
#include <string>
#include <functional>
#include <vector>
#include <unordered_map>
#include <memory>

// Forward declarations for ZepraScript internals
namespace Zepra::Runtime { 
    class VM; 
    class Context; // Internal context
    class Object;
    class Value;
}

namespace Zepra::WebCore {

using ConsoleHandler = std::function<void(const std::string& level, const std::string& message)>;

struct ScriptResult {
    bool success = false;
    std::string value;
    std::string error;
    int line = 0;
    int column = 0;
};

struct TimerCallback {
    int id;
    std::function<void()> callback;
    int delay;
    bool repeating;
    double scheduledTime;
};

/**
 * @brief Alert dialog handler callback
 */
class CanvasRegistry;
class DOMBindings;

/**
 * @brief A request made by fetch() or XMLHttpRequest
 */
struct FetchRequest {
    std::string method = "GET";
    std::string url;                 ///< as the script gave it (the handler resolves it)
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool xhr = false;                ///< XMLHttpRequest rather than fetch()
};

/**
 * @brief What came back. A non-empty `error` means no response arrived
 * (DNS, connection, TLS); an HTTP error status is a response like any other.
 */
struct FetchResponse {
    int status = 0;
    std::string statusText;
    std::string url;                 ///< final URL after redirects
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string error;
};

using FetchDone = std::function<void(FetchResponse response)>;
/**
 * @brief Performs a FetchRequest and calls `done` once, from any thread.
 * The browser installs one that runs the request on a worker thread; the
 * script context settles the promise on the main thread in processTimers().
 */
using FetchHandler = std::function<void(const FetchRequest& request, FetchDone done)>;
struct FetchInbox;
struct XhrState;

using AlertHandler = std::function<void(const std::string& message)>;
using ConfirmHandler = std::function<bool(const std::string& message)>;
using PromptHandler = std::function<std::string(const std::string& message, const std::string& defaultValue)>;

class ScriptContext {
public:
    ScriptContext();
    ~ScriptContext();
    
    // Initialize with DOM
    void initialize(DOMDocument* document);
    
    // Script execution
    ScriptResult evaluate(const std::string& code, const std::string& filename = "<script>");
    
    // Console
    void setConsoleHandler(ConsoleHandler handler) { consoleHandler_ = handler; }
    void log(const std::string& message);
    
    // Timers
    int setTimeout(std::function<void()> callback, int delay);
    int setInterval(std::function<void()> callback, int interval);
    void clearTimeout(int id);
    void clearInterval(int id);
    void processTimers();

    // ====== fetch / XMLHttpRequest ======

    /// Where fetch() and XMLHttpRequest requests go. Without one they fail.
    void setFetchHandler(FetchHandler handler) { fetchHandler_ = std::move(handler); }
    /// Settles the fetch()/XHR requests whose responses have arrived
    /// (processTimers() calls this; tests call it directly)
    void processNetworkResponses();
    /// Requests handed to the handler whose response has not been settled yet
    int pendingFetchCount() const;

    /// Runs the requestAnimationFrame callbacks registered before this frame,
    /// passing the performance.now() timestamp
    void runAnimationFrames();
    
    // Global variables
    void setGlobal(const std::string& name, const std::string& value);
    
    // Access internals
    Zepra::Runtime::VM* vm() { return vm_; }

    /// The page's <canvas> bitmaps (for painting them)
    CanvasRegistry* canvases() { return canvasRegistry_.get(); }
    
    // ====== NEW: Lifecycle Events ======
    
    /**
     * @brief Fire DOMContentLoaded event to all registered listeners
     * Call after DOM parsing completes but before resources load
     */
    void fireDOMContentLoaded();
    
    /**
     * @brief Fire load event to all registered listeners
     * Call after all resources (images, scripts) have loaded
     */
    void fireLoadEvent();
    
    /**
     * @brief Add event listener for window/document events
     */
    void addEventListener(const std::string& eventType, std::function<void()> callback);

    /**
     * @brief Fire a mouse event (click, mousedown, ...) at a page element:
     * its listeners run, then the microtasks they queued
     * @return false when a listener canceled it (preventDefault)
     */
    bool dispatchMouseEvent(DOMElement* target, const std::string& type, double clientX, double clientY);

    /**
     * @brief Free the DOM nodes scripts removed and can no longer reach.
     * Call after the layout tree was rebuilt, when nothing points at them.
     */
    void releaseDetachedNodes();

    /// The live DOM wrappers of this page
    DOMBindings* domBindings() { return domBindings_.get(); }
    
    // ====== NEW: Dialog Handlers ======
    
    /**
     * @brief Set handler for alert() calls (default: console output)
     */
    void setAlertHandler(AlertHandler handler) { alertHandler_ = handler; }
    
    /**
     * @brief Set handler for confirm() calls (default: returns true)
     */
    void setConfirmHandler(ConfirmHandler handler) { confirmHandler_ = handler; }
    
    /**
     * @brief Set handler for prompt() calls (default: returns defaultValue)
     */
    void setPromptHandler(PromptHandler handler) { promptHandler_ = handler; }
    
    // Dialog functions (called by JS alert/confirm/prompt)
    void alert(const std::string& message);
    bool confirm(const std::string& message);
    std::string prompt(const std::string& message, const std::string& defaultValue = "");
    
private:
    void setupGlobals();
    void setupWindowGlobals();
    void setupDocumentGlobals();
    /// Compiles and runs `code`; `value` (optional) receives its completion value
    ScriptResult run(const std::string& code, const std::string& filename, Zepra::Runtime::Value* value);
    /// Runs host-initiated script work (a callback, an event) as a task: under
    /// the run limits, followed by the microtasks it queued
    void runTask(const std::function<void()>& task, const char* what);
    
    DOMDocument* document_ = nullptr;
    ConsoleHandler consoleHandler_;
    std::string lastError_;
    
    // ZepraScript Engine (Per-instance VM — each tab gets its own)
    Zepra::Runtime::VM* vm_ = nullptr;
    std::unique_ptr<Zepra::Runtime::VM> ownedVm_;
    std::unique_ptr<Zepra::Runtime::Context> ownedContext_;
    bool vmReady_ = false;
    
    // Timers
    std::vector<TimerCallback> timers_;
    int nextTimerId_ = 1;
    int scheduleTimer(int id, std::function<void()> callback, int delay, bool repeating);

    // JS functions passed to setTimeout/setInterval/requestAnimationFrame are
    // kept in a hidden global object so the garbage collector keeps them alive
    Zepra::Runtime::Object* callbackStore_ = nullptr;
    Zepra::Runtime::Object* globalObject_ = nullptr;  // the engine's built-ins (Object, Array, JSON, ...)
    std::vector<int> animationFrameQueue_;
    double timeOriginMs_ = 0;   // performance.now() origin
    void callStoredCallback(const std::string& key, bool passTimestamp, double timestampMs);
    void removeStoredCallback(const std::string& key);
    
    // Event listeners for lifecycle events
    std::vector<std::function<void()>> domContentLoadedListeners_;
    std::vector<std::function<void()>> loadListeners_;
    
    // fetch / XMLHttpRequest (fetch_bindings.cpp)
    FetchHandler fetchHandler_;
    std::shared_ptr<FetchInbox> fetchInbox_;
    int nextFetchId_ = 1;
    void setupFetchGlobals(Zepra::Runtime::Object* windowObj);
    void startFetch(int id, bool xhr, const FetchRequest& request);
    bool waitForFetch(int id, FetchResponse& response, int timeoutMs);
    void settleFetch(int id, const FetchResponse& response);
    void settleXhr(int id, const FetchResponse& response);
    void settleXhrNow(Zepra::Runtime::Object* xhr, const std::shared_ptr<XhrState>& state,
                      const FetchResponse& response);
    std::unordered_map<int, std::shared_ptr<XhrState>> xhrInFlight_;   // by request id, while sent
    Zepra::Runtime::Object* makeFetchResponse(const std::shared_ptr<const FetchResponse>& response);
    Zepra::Runtime::Value makeScriptError(const std::string& type, const std::string& message);

    // Dialog handlers
    AlertHandler alertHandler_;
    ConfirmHandler confirmHandler_;
    PromptHandler promptHandler_;

    // Go before the VM whose objects point into them
    std::unique_ptr<DOMBindings> domBindings_;
    std::unique_ptr<CanvasRegistry> canvasRegistry_;
};

// ... DevTools classes ...
class DevToolsConsole {
public:
    DevToolsConsole();
    struct LogEntry {
        std::string level;
        std::string message;
        std::string source;
        int line = 0;
        double timestamp = 0;
    };
    void log(const std::string& message, const std::string& source = "", int line = 0);
    // ... other methods omitted for brevity, same as before ...
    void warn(const std::string&, const std::string&, int) {} 
    void error(const std::string&, const std::string&, int) {}
    void info(const std::string&, const std::string&, int) {}
    void debug(const std::string&, const std::string&, int) {}
    void clear();
    const std::vector<LogEntry>& entries() const { return entries_; }
    void setOnLog(std::function<void(const LogEntry&)> handler) { onLog_ = handler; }
private:
    void addEntry(const std::string& level, const std::string& message, const std::string& source, int line);
    std::vector<LogEntry> entries_;
    std::function<void(const LogEntry&)> onLog_;
    size_t maxEntries_ = 1000;
};

class DevToolsPanel {
public:
    DevToolsPanel();
    enum class Tab { Console, Elements, Network, Sources, Performance };
    void setActiveTab(Tab tab) { activeTab_ = tab; }
    Tab activeTab() const { return activeTab_; }
    DevToolsConsole& console() { return console_; }
    void show() { visible_ = true; }
    void hide() { visible_ = false; }
    void toggle() { visible_ = !visible_; }
    bool isVisible() const { return visible_; }
    void setHeight(float height) { height_ = height; }
    float height() const { return height_; }
private:
    Tab activeTab_ = Tab::Console;
    DevToolsConsole console_;
    bool visible_ = false;
    float height_ = 200;
};

} // namespace Zepra::WebCore
