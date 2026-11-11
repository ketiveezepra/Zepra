/**
 * @file script_context.hpp
 * @brief High-level JavaScript execution context for browser integration
 */

#pragma once

#include "browser/dom.hpp"
#include "browser/event.hpp"
#include "scripting/console_format.hpp"
#include <string>
#include <functional>
#include <vector>
#include <deque>
#include <unordered_map>
#include <memory>
#include <chrono>

// Forward declarations for ZepraScript internals
namespace Zepra::Runtime { 
    class VM; 
    class Context; // Internal context
    class Object;
    class Value;
}

namespace Zepra::WebCore {

class HTMLDocumentParser;

using ConsoleHandler = std::function<void(const std::string& level, const std::string& message)>;
/// Receives every console entry with its structured values (see ConsoleMessage).
/// When set, it replaces the plain ConsoleHandler.
using ConsoleMessageHandler = std::function<void(const ConsoleMessage& message)>;

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
class CSSComputedStyle;
class DOMBindings;
class PageGeometry;
class StorageArea;
struct KeyEventInit;
struct FormSubmission;

/// What a key press did on the page (ScriptContext::keyDown)
struct KeyResult {
    bool canceled = false;   ///< a listener called preventDefault(): no default action
    bool consumed = false;   ///< the focused control used the key (typing, Backspace, Enter, arrows)
    bool openPicker = false; ///< the key opens the focused <select>'s drop-down (Space, Alt+Down, F4)
};
/// What releasing the primary button did on the page (ScriptContext::mouseUp)
struct MouseUpResult {
    bool clicked = false;      ///< a click fired (press and release on the same element or inside one)
    bool canceled = false;     ///< a click listener called preventDefault(): links stay put
};
/// Receives a form submission no listener canceled (the browser navigates)
using SubmitHandler = std::function<void(const FormSubmission& submission)>;

/// What a script asked of the page's URL through `location` or `history`
enum class NavigationKind {
    Assign,        ///< location.href = url, location.assign(url): load url, a new history entry
    Replace,       ///< location.replace(url): load url in place of the current entry
    Reload,        ///< location.reload(): load the current URL again
    SameDocument,  ///< a fragment change or history.pushState/replaceState: the URL changed, nothing loads
};
struct NavigationRequest {
    std::string url;   ///< absolute
    NavigationKind kind = NavigationKind::Assign;
};
/// Receives what a script asked for; the browser loads the URL once the script
/// task is over (a script keeps running after it sets location.href)
using NavigationHandler = std::function<void(const NavigationRequest& request)>;

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

/// An external script's source as the network gave it (ScriptFetcher)
struct ScriptFetchResult {
    bool ok = false;          ///< a successful response (2xx, or a file that could be read)
    std::string body;
    std::string error;        ///< why not, when !ok
};
using ScriptFetchDone = std::function<void(ScriptFetchResult result)>;
/**
 * @brief Fetches the external script at `url` (absolute) and calls `done`
 * once, from any thread. The script context runs the script later, from its
 * own queue (runReadyScripts), never inside the fetcher's call.
 */
using ScriptFetcher = std::function<void(const std::string& url, ScriptFetchDone done)>;
struct ScriptInbox;     // script_elements.cpp
struct PendingScript;
struct DragState;   // drag_drop.cpp

using AlertHandler = std::function<void(const std::string& message)>;
using ConfirmHandler = std::function<bool(const std::string& message)>;
using PromptHandler = std::function<std::string(const std::string& message, const std::string& defaultValue)>;

class ScriptContext {
public:
    ScriptContext();
    ~ScriptContext();
    
    // Initialize with DOM
    void initialize(DOMDocument* document);
    
    // Script execution. `filename` names the script in stack traces and console
    // source links; `firstLine` is where the code starts in it (an inline
    // script's line in its document).
    ScriptResult evaluate(const std::string& code, const std::string& filename = "<script>", uint32_t firstLine = 1);

    /**
     * @brief Keeps the text of a script or document, for the DevTools source
     * view behind a console message's link. The same name replaces the old
     * text; the oldest texts go once all of them pass kSourceBudget bytes.
     */
    void registerSource(const std::string& name, const std::string& text);
    /// The text registered under `name`, or null
    std::shared_ptr<const std::string> sourceText(const std::string& name) const;
    /// The registered names, in the order they were registered
    std::vector<std::string> sourceNames() const;
    static constexpr size_t kSourceBudget = 4 * 1024 * 1024;

    /**
     * @brief Property names for console autocomplete: the globals when `path`
     * is empty, else the properties (own and inherited) of the value that
     * `path[0].path[1]...` names. Only data properties and native getters are
     * read on the way, so no page script runs. Empty when the path does not
     * resolve to an object or primitive.
     */
    std::vector<std::string> completionNames(const std::vector<std::string>& path);
    
    // Console
    void setConsoleHandler(ConsoleHandler handler) { consoleHandler_ = handler; }
    void setConsoleMessageHandler(ConsoleMessageHandler handler) { consoleMessageHandler_ = handler; }
    void log(const std::string& message);

    /**
     * @brief Runs a line typed into the DevTools console, with its helpers
     * ($0 = `inspected`, $, $$, $_, clear(), copy()) in place.
     * @return a "result" message holding the value (strings quoted), or an
     * "error" message: the thrown value, or "SyntaxError: ..." with its source position
     */
    ConsoleMessage evaluateConsole(const std::string& code, DOMNode* inspected = nullptr);
    /// Sets the console's $0 (the element selected in the Inspector)
    void setInspectedNode(DOMNode* node);
    
    // ====== <script> elements (script_elements.cpp) ======

    /// Where external scripts come from. Without one they fail (error event).
    void setScriptFetcher(ScriptFetcher fetcher) { scriptFetcher_ = std::move(fetcher); }
    /**
     * @brief HTML "prepare the script element": decides whether `script`
     * runs (connected, not already started, a JavaScript or module type, not
     * nomodule) and when. The parser calls it for each <script> it reaches
     * (`parserInserted`; `firstLine` is an inline script's line in the
     * page); the DOM calls it for scripts other scripts insert.
     *  - inline classic scripts run now (a script-inserted one inside the
     *    appendChild that connected it, as browsers do)
     *  - parser-inserted external classic scripts without async/defer block
     *    the parser: the parser's driver (advanceParsing, runParserScripts)
     *    runs them, once fetched, before it prepares the next script
     *  - defer scripts and parser-inserted modules wait for runDeferredScripts()
     *  - async scripts, and scripts other scripts insert (async unless they
     *    set async = false, which keeps those in insertion order), run from
     *    runReadyScripts() once their source arrived, each as its own task
     * External scripts get `load` after they ran, `error` when the fetch failed.
     */
    void prepareScript(DOMElement* script, bool parserInserted, uint32_t firstLine = 1);
    /// Prepares the parser's scripts (those in the document now), in
    /// document order, as the parser would on reaching each one.
    /// `firstLineOf` gives an inline script's first line in the page.
    /// It waits (blocks) for each parser-blocking script's source.
    void runParserScripts(const std::function<uint32_t(DOMElement*)>& firstLineOf);
    /// The parser is done: runs the defer scripts in document order (waiting
    /// for any still loading). Call before fireDOMContentLoaded().
    void runDeferredScripts();

    /// The same load without blocking, driven from the event loop: takes the
    /// parser's scripts (those in the document now, in document order), then
    /// each advanceParsing() call moves on as far as it can: parser scripts
    /// in order (a parser-blocking one stops it until its source arrived),
    /// then the defer scripts, then DOMContentLoaded, the async scripts
    /// already here and the window load event (which waits for pending
    /// scripts). It returns between two scripts once `deadline` passed.
    /// Returns true once DOMContentLoaded fired (at once on later calls).
    void beginParsing(std::function<uint32_t(DOMElement*)> firstLineOf);
    /// The same, with the document's own incremental parser (HTML §13.2):
    /// advanceParsing() runs `parser` up to each </script>, prepares that
    /// script with the tree built so far (inline ones run at once, a
    /// parser-blocking one stops the parser until it arrived and ran) and
    /// goes on; document.write() inserts at the parser's insertion point.
    /// `parser` builds the document this context was initialized with.
    void beginParsing(std::unique_ptr<HTMLDocumentParser> parser,
                      std::function<uint32_t(DOMElement*)> firstLineOf);
    bool advanceParsing(std::chrono::steady_clock::time_point deadline);
    /// The parser beginParsing() was given, while it has input to parse
    HTMLDocumentParser* activeParser() const;

    /// document.write(text): into the parser's input at its insertion
    /// point, tokenized (and its scripts run) before it returns. With no
    /// insertion point it is ignored while an external script runs or the
    /// page's parser is still active, else it implies document.open().
    void documentWrite(const std::string& text);
    /// document.open(): unless the page's parser is active, the document
    /// loses its children and a script-created parser takes the writes
    void documentOpen();
    /// document.close(): the script-created parser parses to its end
    void documentClose();
    /// Whether a load begun with beginParsing() is past DOMContentLoaded
    bool parsingDone() const { return parsePhase_ == ParsePhase::Idle || parsePhase_ == ParsePhase::Done; }
    /// Runs the async and script-inserted scripts whose source arrived, each
    /// as a task (processTimers() calls this). Fires the window's load event
    /// once the last script it was waiting for ran.
    void runReadyScripts();
    /// Scripts prepared but not run yet (defer, async, script-inserted)
    size_t pendingScriptCount() const;
    /// Fires a simple event (no bubbling, not cancelable) at an element, as
    /// a task: `load`/`error` of a resource the browser loaded for it
    void fireElementEvent(DOMElement* element, const std::string& type);
    /// Fires `scroll` at the document (it bubbles to the window), as a task:
    /// the viewport scrolled since the last frame (HTML "run the scroll
    /// steps", once per frame before the animation frame callbacks)
    void fireViewportScroll();
    /// Fires a CSS animation or transition event (animationstart, ...,
    /// transitionend) at a connected element: it bubbles, with
    /// animationName or propertyName, elapsedTime and pseudoElement ""
    void fireAnimationEvent(DOMElement* element, const std::string& type, const std::string& name,
                            double elapsedSeconds);

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
    /// Fires `change` at window.matchMedia() lists whose result changed
    /// (after the browser updated the media environment), as one task
    void evaluateMediaQueries();
    
    // Global variables
    void setGlobal(const std::string& name, const std::string& value);
    /// The page's URL: sets __pageUrl__ and picks the origin whose
    /// localStorage the page sees (web_storage.hpp)
    void setPageUrl(const std::string& url);
    
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

    // ====== User input (user_input.cpp) ======

    /**
     * @brief A press of the primary button on a page element: focus moves to
     * it or its nearest focusable ancestor (elsewhere the page takes focus),
     * then the click and its default action (a checkbox toggles, a submit
     * button submits its form, ...)
     * @return false when a click listener canceled it (links stay put)
     */
    bool pressElement(DOMElement* target, double clientX, double clientY);
    /**
     * @brief The primary button going down on a page element: mousedown,
     * then, unless a listener canceled it, focus moves to the element or its
     * nearest focusable ancestor (elsewhere the page takes focus)
     * @return false when a mousedown listener canceled it
     */
    bool mouseDown(DOMElement* target, double clientX, double clientY);
    /**
     * @brief The primary button coming up over `target` (null off the page):
     * mouseup there, then the click and its default action at the nearest
     * element holding both the pressed and the released element
     */
    MouseUpResult mouseUp(DOMElement* target, double clientX, double clientY);
    /**
     * @brief The pointer moving over `target` (null off the page): when the
     * element under it changed, mouseout/mouseleave on the old one and
     * mouseover/mouseenter on the new one; then mousemove
     */
    void mouseMove(DOMElement* target, double clientX, double clientY, bool buttonDown);
    /**
     * @brief True while a drag and drop started on the page is under way
     * (from a dragstart nobody canceled until the release): mouseMove and
     * mouseUp fire drag events instead of mouse events (drag_drop.cpp)
     */
    bool dragging() const;
    /**
     * @brief A key going down: keydown at the focused element (else the body),
     * keypress for keys that make a character, then, unless canceled, the
     * key's action in the focused control: `text` is typed (beforeinput,
     * input), Backspace deletes, Enter submits a single-line field's form or
     * starts a line in a textarea, Enter or Space click a focused button
     */
    KeyResult keyDown(const KeyEventInit& key, const std::string& text);
    /// A key going up: keyup at the focused element (else the body)
    void keyUp(const KeyEventInit& key);
    /**
     * @brief Text that comes without a key (pasted, IME): typed into the
     * focused text control with beforeinput and input
     * @return false when there is no such control or a listener canceled it
     */
    bool insertText(const std::string& text, const std::string& inputType = "insertText");
    /**
     * @brief The user picked option `index` of a <select> from its
     * drop-down: the selection changes, then input and change fire
     * @return false when nothing changed (same option, disabled, gone)
     */
    bool chooseOption(DOMElement* select, long index);
    /// The element with keyboard focus; null when the page itself has it
    DOMElement* focusedElement() const;
    /// Moves focus as focus() does (blur/focusout, focus/focusin; change when
    /// the user changed the value); null gives it to the page
    void focusElement(DOMElement* element);
    /// Where submissions go once no listener canceled them (form.submit(),
    /// requestSubmit(), Enter, submit buttons)
    void setSubmitHandler(SubmitHandler handler);
    /// Navigations scripts ask for through `location` and `history` (location_bindings.cpp)
    void setNavigationHandler(NavigationHandler handler) { navigationHandler_ = std::move(handler); }
    /// The URL `location` reports: the last setPageUrl, or a later fragment or
    /// pushState change ("about:blank" before any)
    const std::string& pageUrl() const { return pageUrl_; }

    /**
     * @brief Free the DOM nodes scripts removed and can no longer reach.
     * Call after the layout tree was rebuilt, when nothing points at them.
     */
    void releaseDetachedNodes();

    /// The live DOM wrappers of this page
    DOMBindings* domBindings() { return domBindings_.get(); }

    /**
     * @brief Where getComputedStyle() reads an element's style. The browser
     * passes its CSS engine, restyling first when the DOM changed since the
     * styles were computed. Without one every computed value is "".
     */
    void setComputedStyleSource(std::function<const CSSComputedStyle*(DOMElement*)> source);
    /// Where getComputedStyle(element, '::before') reads a pseudo-element's
    /// style ("before", "after", ...). Without one pseudo-elements read empty.
    void setPseudoElementStyleSource(std::function<const CSSComputedStyle*(DOMElement*, const std::string&)> source);
    /// The page's layout, for element geometry, the viewport size and
    /// scrolling (see page_geometry.hpp); null: no boxes, no scrolling
    void setPageGeometry(PageGeometry* geometry);
    
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
    /// Compiles and runs `code`; `value` (optional) receives its completion
    /// value, `thrown` (optional) the exception that ended it
    ScriptResult run(const std::string& code, const std::string& filename, Zepra::Runtime::Value* value,
                     Zepra::Runtime::Value* thrown = nullptr, uint32_t firstLine = 1);
    /// Reports a script's error to the console, at the position the engine gives
    void reportError(const std::string& text, const std::string& file, uint32_t line, uint32_t column = 0);
    /// Reports an uncaught value: an Error links to the first frame of its stack
    void reportThrown(const std::string& text, const Zepra::Runtime::Value& thrown);
    /// The script position the running native function was called from
    void setCallerSource(ConsoleMessage& message) const;
    /// Hands a console entry to the message handler, else the text handler, else the terminal
    void emitConsole(ConsoleMessage message);
    void emitConsole(const std::string& level, const std::string& text);
    void setupConsole(Zepra::Runtime::Object* consoleObj);
    void installConsoleHelpers();
    Zepra::Runtime::Value callDocumentMethod(const char* method, const std::vector<Zepra::Runtime::Value>& args);
    /// Runs host-initiated script work (a callback, an event) as a task: under
    /// the run limits, followed by the microtasks it queued
    void runTask(const std::function<void()>& task, const char* what);
    
    DOMDocument* document_ = nullptr;
    ConsoleHandler consoleHandler_;
    ConsoleMessageHandler consoleMessageHandler_;
    std::string lastError_;
    bool quietErrors_ = false;          // evaluateConsole reports its own errors
    bool consoleHelpersInstalled_ = false;
    int consoleGroupDepth_ = 0;
    std::unordered_map<std::string, int> consoleCounts_;
    std::unordered_map<std::string, double> consoleTimers_;
    /// registerSource texts, oldest first
    std::vector<std::pair<std::string, std::shared_ptr<const std::string>>> sources_;
    size_t sourceBytes_ = 0;
    
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
    // MessageChannel / postMessage tasks (ids of "m<id>" callbacks) and the
    // observer hooks (installWebTaskApis in script_context.cpp)
    std::deque<int> messageTasks_;
    int nextMessageTaskId_ = 1;
    void installWebTaskApis(Zepra::Runtime::Object* windowObj);
    void runMessageTasks();
    void deliverObservations();
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

    std::function<const CSSComputedStyle*(DOMElement*)> computedStyleSource_;
    std::function<const CSSComputedStyle*(DOMElement*, const std::string&)> pseudoStyleSource_;
    SubmitHandler submitHandler_;

    // window.location and window.history (location_bindings.cpp)
    NavigationHandler navigationHandler_;
    std::string pageUrl_ = "about:blank";
    Zepra::Runtime::Object* historyObject_ = nullptr;
    int historyLength_ = 1;
    void setupLocationAndHistory(Zepra::Runtime::Object* windowObj);
    /// Hands `url` (absolute) to the navigation handler; a URL that differs
    /// from the page's only in its fragment changes the URL in place and
    /// queues hashchange instead
    void navigateFromScript(const std::string& url, NavigationKind kind);

    // <script> elements (script_elements.cpp)
    ScriptFetcher scriptFetcher_;
    std::shared_ptr<ScriptInbox> scriptInbox_;
    /// The HTML spec's lists: defer scripts (parser order), scripts that run
    /// in insertion order (async = false), and the async set
    std::vector<std::shared_ptr<PendingScript>> deferredScripts_;
    std::vector<std::shared_ptr<PendingScript>> orderedScripts_;
    std::vector<std::shared_ptr<PendingScript>> asyncScripts_;
    int nextScriptId_ = 1;
    /// Scripts whose fetch is under way, by id
    std::unordered_map<int, std::weak_ptr<PendingScript>> fetchingScripts_;
    /// Wrappers of the elements of pending scripts, so a script removed from
    /// the document before it loads stays alive until it ran
    Zepra::Runtime::Object* pendingScriptStore_ = nullptr;
    bool loadEventDelayed_ = false;   // fireLoadEvent() came while scripts were pending
    bool runningReadyScripts_ = false;
    /// The parser's walk: its scripts wait in pendingScriptStore_ as p<i>
    enum class ParsePhase { Idle, ParserScripts, DeferredScripts, Done };
    ParsePhase parsePhase_ = ParsePhase::Idle;
    size_t parserScriptCount_ = 0;
    size_t parserScriptNext_ = 0;
    std::function<uint32_t(DOMElement*)> parserFirstLine_;
    /// The parser-blocking script the walk waits for
    std::shared_ptr<PendingScript> parserBlockingScript_;
    /// The document's incremental parser (beginParsing with a parser, or
    /// document.open()); null for a document parsed before scripts run
    std::unique_ptr<HTMLDocumentParser> parser_;
    /// External scripts running (document.write without an insertion
    /// point is ignored meanwhile: HTML's ignore-destructive-writes counter)
    int ignoreDestructiveWrites_ = 0;
    /// Keeps the elements the parser still refers to alive (in
    /// pendingScriptStore_) while scripts may remove them
    void pinParserElements();
    /// Prepares the script the parser stopped for
    void prepareParserScript(DOMElement* script);
    /// Parses up to the insertion point (document.write), running the
    /// scripts met on the way; stops at a parser-blocking script
    void parseWrittenInput();
    /// document.readyState changes, with readystatechange at the document
    void setDocumentReadiness(const std::string& state);
    void collectParserScripts(std::function<uint32_t(DOMElement*)> firstLineOf);
    /// Without `wait`, false when it stopped: a script's source is not here
    /// yet, or `deadline` passed. With `wait`, it blocks for sources.
    bool advanceParserScripts(bool wait, std::chrono::steady_clock::time_point deadline);
    bool advanceDeferredScripts(bool wait, std::chrono::steady_clock::time_point deadline);
    /// A script's source is here (or the wait gave up after kScriptWaitMs);
    /// without `wait` it only looks
    bool scriptSourceReady(PendingScript& script, bool wait);
    void setupScriptElements();
    void startScriptFetch(const std::shared_ptr<PendingScript>& script);
    /// Takes the fetch results that arrived and marks their scripts ready
    void collectArrivedScripts();
    bool waitForScript(PendingScript& script, int timeoutMs);
    /// HTML "execute the script element": runs it with document.currentScript
    /// set, then fires load (or error for a failed fetch). The one place
    /// scripts from <script> elements run.
    void executeScriptElement(PendingScript& script);
    void fireLoadEventNow();

    // Dialog handlers
    AlertHandler alertHandler_;
    ConfirmHandler confirmHandler_;
    PromptHandler promptHandler_;

    // Go before the VM whose objects point into them
    std::unique_ptr<DOMBindings> domBindings_;
    // localStorage's origin (empty: an opaque page, which gets opaqueStorage_)
    std::string storageOrigin_;
    std::unique_ptr<StorageArea> opaqueStorage_;
    std::unique_ptr<CanvasRegistry> canvasRegistry_;

    // Drag and drop (drag_drop.cpp): the press that may start a drag, then the drag
    std::shared_ptr<DragState> drag_;
    Zepra::Runtime::Object* dataTransferProto_ = nullptr;
    void setupDragAndDrop(Zepra::Runtime::Object* windowObj);
    void noteDragPress(DOMElement* target, double clientX, double clientY);
    /// A move with the button down: may start the drag; true when the drag
    /// took the move (no mouse events)
    bool dragMove(DOMElement* target, double clientX, double clientY, bool buttonDown);
    /// The release: drop or not, then dragend; true when a drag ended (no mouseup or click)
    bool dragRelease(DOMElement* target, double clientX, double clientY);
    void endDrag();
    /// The connected element behind a wrapper kept during the drag, else null
    DOMElement* dragElement(const Zepra::Runtime::Value& wrapper) const;
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
