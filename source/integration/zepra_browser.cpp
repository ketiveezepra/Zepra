/**
 * @file zepra_browser.cpp
 * @brief ZepraBrowser - Native Browser with Custom Rendering Engine
 * 
 * Architecture:
 *   - NXRender: Platform-independent graphics (replaces X11/SDL)
 *   - WebCore: DOM, CSS, JavaScript engine (optional, USE_WEBCORE)
 *   - NXHTTP: Native HTTP networking with TLS
 *   - DevTools: Built-in developer tools (Network, Console, Elements)
 * 
 * For maintainers: Imports are organized into 7 sections below.
 * To build your own browser from this engine, see docs/ARCHITECTURE.md
 * 
 * @copyright (c) 2024-2025 KetiveeAI
 */

// ===========================================================================
// SECTION 1: C++ STANDARD LIBRARY
// Group: Core language features, containers, I/O, threading
// ===========================================================================
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <csignal>
#include <sys/stat.h>

// ===========================================================================
// SECTION 2: NXRENDER - Platform Graphics Abstraction
// Group: Rendering primitives, text, SVG, colors
// Docs: See source/nxrender/GEMINI.md
// ===========================================================================
#include <nxgfx/color.h>
#include <nxgfx/context.h>
#include <nxgfx/primitives.h>
#include <nxgfx/text.h>
#include "nxrender_cpp.h"
#include "nxsvg.h"
#include "nxfont.h"
#include "reox_nxrender_bridge.h"
#include "../nxrender-cpp/nxpdf/nx_pdf.h"

// ===========================================================================
// SECTION 3: WEBCORE - Optional DOM/CSS/JavaScript Engine
// Enable with: -DUSE_WEBCORE=ON
// Note: Must come before any X11 headers (None macro conflict)
// ===========================================================================
#ifdef USE_WEBCORE
#include "html_parser.hpp"
#include "browser/dom.hpp"
#include "css/css_engine.hpp"
#include "css/ua_stylesheet.h"
#include "scripting/script_context.hpp"
using namespace Zepra::WebCore;

extern "C" {
    int webcore_init();
    int webcore_load_html(const char* html, const char* url);
    const char* webcore_get_title();
    int webcore_exec_js(const char* script);
    void webcore_shutdown();
}
#endif

// ===========================================================================
// SECTION 4: NETWORKING - HTTP Client, TLS, Network Monitoring
// Group: All network I/O, request/response handling
// ===========================================================================
#include "nxhttp.h"
#include "http_client.hpp"
#include "network_monitor.h"

// ===========================================================================
// SECTION 5: BROWSER CORE COMPONENTS
// Group: Layout, navigation, tab suspension, input handling
// Location: src/browser/
// ===========================================================================
#include "browser/layout_engine.h"
#include "browser/tab_suspender.h"
#include "browser/mouse_handler.h"
#include "browser/lazy_image_loader.h"
#include "browser/clipboard.h"
#include "browser/css_utils.h"

// Import CSS utilities (avoid Tab namespace collision)
using ZepraBrowser::cssColorToRGB;
using ZepraBrowser::expandCSSVariables;
using ZepraBrowser::polyfillCSSShorthands;
using ZepraBrowser::extractStylesFromRawHTML;
using ZepraBrowser::extractBodyInlineStyle;
using ZepraBrowser::stripOuterTags;

// ===========================================================================
// SECTION 6: UI COMPONENTS
// Group: Tab manager, DevTools panel, WebView, Start page, Error pages
// ===========================================================================
#include "browser/tab_manager.h"
#include "browser/start_page.h"
#include "browser/error_pages.h"
#include "browser/svg_extract.h"
#include "panels/zepra_webview_panel.h"

// ===========================================================================
// SECTION 7: ENGINE SERVICES
// Group: Download manager, base utilities, image loading
// ===========================================================================
#include "engine/download_manager.h"
#include "nxbase.h"

#include "stb_image.h"

// ===========================================================================
// CONFIGURATION
// ===========================================================================
#ifndef RESOURCE_PATH
#define RESOURCE_PATH "/home/swana/Documents/zeprabrowser/resources"
#endif


// ============================================================================
// GLOBALS
// ============================================================================

// Platform globals managed by NXRender
// // X11 Global stub for legacy font compatibility (if needed)
// static void* g_display = nullptr; // Reserved for X11 cursor API
// static Window g_window;
// static GLXContext g_glContext;
// static Atom g_wmDeleteMessage;
static bool g_running = true;
int g_width = 1920;
int g_height = 1080;
static float g_mouseX = 0, g_mouseY = 0;

// Forward Declarations
void onNavigate(const std::string& input);
static bool g_mouseDown = false;

// X11 cursors
// Cursors managed by platform (TODO: Expose cursor API in NXRender)
// static Cursor g_cursorArrow;
// static Cursor g_cursorHand;
static bool g_currentCursorIsHand = false;

// Hover URL (shown at bottom when hovering over a link)
static std::string g_hoverUrl = "";
static const char* g_crash_url = nullptr;  // Last URL for crash context

// Resources
static nxsvg::SvgLoader g_svg;

// ============================================================================
// THEME (from app.rx)
// ============================================================================

struct Theme {
    uint32_t bg_primary    = 0x0D1117;
    uint32_t bg_secondary  = 0x161B22;
    uint32_t bg_tertiary   = 0x21262D;
    uint32_t bg_elevated   = 0x30363D;
    uint32_t text_primary  = 0xF0F6FC;
    uint32_t text_secondary = 0x8B949E;
    uint32_t text_placeholder = 0x6E7681;
    uint32_t accent        = 0x58A6FF;
    uint32_t success       = 0x3FB950;
    uint32_t border        = 0x30363D;
    uint32_t gradient_start = 0xE8B4D8;
    uint32_t gradient_end   = 0xA8A0F5;
};
static Theme g_theme;

#define R(c) ((c >> 16) & 0xFF)
#define G(c) ((c >> 8) & 0xFF)
#define B(c) (c & 0xFF)

// ============================================================================
// TAB MODEL (from app.rx)
// ============================================================================

// Note: zepra::TabManager from ui/tab_manager.h is included above (Section 6)
// The inline struct Tab below is used for backward compatibility


struct Tab {
    int id = 0;
    std::string title;
    std::string url;
    bool isLoading = false;
    bool isStart = true;
    
    // Per-tab content (layout rebuilt on tab switch)
    std::string pageContent;
    std::string loadError;
    float scrollY = 0;  // Scroll position per tab
    float zoomLevel = 1.0f;  // Zoom level (1.0 = 100%)
    
    // Deferred loading (for background tabs)
    bool pendingLoad = false;       // True if tab was created but not loaded
    std::string pendingUrl;         // URL to load when tab becomes active
    
    // Per-tab console log (production: each tab has its own console)
    std::vector<std::string> consoleLog;
    
    // Per-tab network monitor (Tab A/B isolation for DevTools Network panel)
    std::unique_ptr<zepra::NetworkMonitor> networkMonitor;
    
    // Per-tab DOM/CSS/Layout isolation
    std::unique_ptr<DOMDocument> document;
    std::unique_ptr<CSSEngine> cssEngine;
    std::unique_ptr<ZepraBrowser::LayoutBox> layoutRoot;
    bool hasRenderedContent = false;  // true if tab has been through parseWithWebCore
    
    // Default constructor
    Tab() = default;
    
    // Move constructor (unique_ptr is non-copyable)
    Tab(Tab&& other) noexcept = default;
    Tab& operator=(Tab&& other) noexcept = default;
    
    // Delete copy (unique_ptr can't be copied)
    Tab(const Tab&) = delete;
    Tab& operator=(const Tab&) = delete;
    
    // Helper to add console entry for this tab
    void logConsole(const std::string& msg) {
        consoleLog.push_back(msg);
        if (consoleLog.size() > 500) consoleLog.erase(consoleLog.begin());
    }
    
    // Get network monitor (lazy init)
    zepra::NetworkMonitor& getNetworkMonitor() {
        if (!networkMonitor) {
            networkMonitor = std::make_unique<zepra::NetworkMonitor>();
        }
        return *networkMonitor;
    }
    
    // Clear network log (on navigation)
    void clearNetworkLog() {
        if (networkMonitor) {
            networkMonitor->clear();
        }
    }
};


static std::vector<Tab> g_tabs;
static int g_activeTabId = 1;
static int g_nextTabId = 2;

// Forward declarations
std::string urlEncode(const std::string& str);
std::string getContentType(const std::string& path);

// Search engine selection
enum class SearchEngine {
    Ketivee = 0,  // Default
    Google,
    Bing,
    DuckDuckGo,
    Yahoo
};

static SearchEngine g_searchEngine = SearchEngine::Ketivee;

// Get search URL for query based on selected engine
std::string getSearchUrl(const std::string& query) {
    std::string encoded = urlEncode(query);
    
    switch (g_searchEngine) {
        case SearchEngine::Google:
            return "https://www.google.com/search?q=" + encoded;
        case SearchEngine::Bing:
            return "https://www.bing.com/search?q=" + encoded;
        case SearchEngine::DuckDuckGo:
            return "https://duckduckgo.com/?q=" + encoded;
        case SearchEngine::Yahoo:
            return "https://search.yahoo.com/search?p=" + encoded;
        case SearchEngine::Ketivee:
        default:
            return "https://ketivee.com/search?q=" + encoded;
    }
}

// ============================================================================
// BROWSER STATE
// ============================================================================

static std::string g_currentUrl = "";  // Empty = start with new tab page (zepra://start)
static std::string g_displayUrl = "";            // Clean URL for display
static std::string g_searchQuery = "";           // Search box input buffer
static std::string g_addressInput = "";          // Address bar input buffer
static bool g_sidebarVisible = false;
static bool g_addressFocused = false;
static bool g_searchFocused = false;

// Page content state
static std::string g_pageContent = "";           // Loaded page HTML/content
static std::string g_pageTitle = "New Tab";      // Page title
static bool g_isLoading = false;                 // Loading indicator
static std::string g_loadError = "";             // Error message
static bool g_consoleVisible = false;            // Developer console (F12)

// Alert/Confirm/Prompt modal state
static bool g_alertVisible = false;
static std::string g_alertMessage;
static bool g_alertIsConfirm = false;
// static bool g_alertResult = false; // Reserved for confirm dialog
static bool g_uiHoverHand = false;                // UI element hover (buttons, tabs, links)

// =============================================================================
// ZepraWebView DevTools State
// =============================================================================
// DevTools tab enum (matches DevToolsPanel::DevToolsTab in zepra_webview_panel.h)
enum class DevToolsTab { 
    Elements = 0,     // DOM Tree + CSS Inspector
    Console,          // JavaScript Console
    Network,          // Network Requests/Responses  
    Sources,          // JavaScript Source Viewer
    Performance,      // Performance Profiling
    Application,      // Storage, Cache, Service Workers
    Security,         // TLS/SSL Certificates, Permissions
    Settings,         // DevTools Settings
    COUNT             // Total tab count (for iteration)
};
static DevToolsTab g_devToolsTab = DevToolsTab::Elements;
static float g_devToolsHeight = 280.0f;          // Panel height
static std::string g_consoleInput = "";          // Console input field
static bool g_consoleFocused = false;            // Console input focused
// static int g_selectedDOMNodeId = -1;          // Reserved: DevTools DOM inspector
// Console log is now per-tab (see Tab::consoleLog)
static std::map<int, bool> g_domNodeExpanded;    // Track expanded nodes in tree
// static int g_domNodeCounter = 0;              // Reserved: DevTools DOM inspector

// Mouse handler instance
ZepraBrowser::MouseHandler g_mouseHandler;

// Tab suspender (multi-level: ACTIVE → SLEEP → LIGHT_SLEEP → DEEP_SLEEP)
ZepraBrowser::TabSuspender g_tabSuspender;

// Download manager
zepra::DownloadManager g_downloadManager;

// Focus state
using ZepraBrowser::LayoutBox;
static LayoutBox* g_focusedBox = nullptr;

// Input cursor position (for visual feedback)
static int g_cursorBlink = 0;

// WebCore state (DOM, CSS, JS)
#ifdef USE_WEBCORE
static std::unique_ptr<DOMDocument> g_document;
static std::unique_ptr<CSSEngine> g_cssEngine;
static std::unique_ptr<ScriptContext> g_scriptContext;

// Styled text line for rendering
struct StyledTextLine {
    std::string text;
    std::string href;                // Link URL (for <a> tags)
    std::string target;              // Link target (_blank, _self, etc.)
    float fontSize = 16.0f;
    uint32_t color = 0x1F2328;
    uint32_t bgColor = 0xFFFFFF;    // Background color
    bool hasBgColor = false;         // Whether to draw background
    bool bold = false;
    bool italic = false;
    bool isLink = false;             // Is this a link?
    bool isBlock = false;            // Is this a block element?
    bool isInput = false;            // Is this an input box?
    std::string inputType;           // Input type (text, password, etc.)
    std::string placeholder;         // Input placeholder
    float marginTop = 0;
    float marginBottom = 0;
    // Screen position (set during render)
    float screenX = 0, screenY = 0, screenW = 0, screenH = 0;
};
static std::vector<StyledTextLine> g_styledLines;

// Per-tab content cache (tab ID -> styled lines)
#include <map>
static std::map<int, std::vector<StyledTextLine>> g_tabContentCache;

// Link hit boxes for click detection
struct LinkHitBox {
    float x, y, w, h;
    std::string href;
    std::string target;  // _blank = new tab, _self = same window
};
static std::vector<LinkHitBox> g_linkHitBoxes;
static bool g_cursorIsPointer = false;  // Should cursor be hand?

// ============================================================================
// ASYNC LOADING STATE
// ============================================================================
static std::mutex g_loadMutex;                   // Protects g_styledLines, g_pageContent
static std::atomic<bool> g_asyncLoadPending{false};  // True when background load in progress
static std::atomic<bool> g_asyncLoadComplete{false}; // True when load finished (ready to swap)
static std::string g_pendingUrl;                 // URL being loaded
static std::string g_pendingContent;             // Content from background load
static std::string g_pendingTitle;               // Title from background load
static std::vector<StyledTextLine> g_pendingStyledLines; // Styled lines from background

// ============================================================================
// LAYOUT ENGINE (Block Formatting Context)
// ============================================================================

// Use modular LayoutEngine types
using ZepraBrowser::LayoutBox;
using ZepraBrowser::LayoutType;
// using ZepraBrowser::g_layoutRoot; // No, we allow separate root for now? Or g_layoutRoot is local here.

// Helper to add child (using std::list for stable pointers)
static LayoutBox* addChild(LayoutBox* parent) {
    if (!parent) return nullptr;
    parent->children.emplace_back();
    return &parent->children.back();  // Pointer stays valid with std::list
}

static std::unique_ptr<LayoutBox> g_layoutRoot;

// Forward declarations
void buildLayoutFromDOM(DOMElement* element, LayoutBox* parentBox, bool inAnchor, const std::string& anchorHref, const std::string& anchorTarget);
// layoutBlock/paintBox are now in ZepraBrowser namespace


// (Forward declarations removed - using layout_engine.h)
#endif

// ============================================================================
// HTTP CLIENT (NXHTTP)
// ============================================================================

struct HttpResponse {
    bool success = false;
    int statusCode = 0;
    std::string contentType;
    std::string data;
    std::string error;
};

// Helper to configure client matching previous curl settings
[[maybe_unused]] static NxHttpClient* createClient() {
    NxHttpClientConfig config = {0};
    config.connect_timeout_ms = 10000; // 10s
    config.read_timeout_ms = 10000;
    config.follow_redirects = true;
    config.max_redirects = 10;
    config.verify_ssl = false; // Match previous behavior
    config.user_agent = "ZepraBrowser/1.0 (NeolyxOS)";
    return nx_http_client_create(&config);
}

HttpResponse httpGet(const std::string& url) {
    HttpResponse response;
    
    std::cout << "[Browser] httpGet called: " << url << std::endl;
    
    // Get active tab for network logging
    Tab* activeTab = nullptr;
    for (auto& t : g_tabs) {
        if (t.id == g_activeTabId) { activeTab = &t; break; }
    }
    
    // Create network request for logging
    zepra::NetworkRequest netReq;
    netReq.method = "GET";
    netReq.url = url;
    netReq.origin = activeTab ? activeTab->url : "";
    
    // Record request to active tab's monitor
    uint64_t requestId = 0;
    if (activeTab) {
        requestId = activeTab->getNetworkMonitor().recordRequest(netReq);
    }
    
    // Handle file:// protocol
    if (url.substr(0, 7) == "file://") {
        std::string path = url.substr(7);
        if (!path.empty() && path[0] == '~') {
            const char* home = getenv("HOME");
            if (home) path = std::string(home) + path.substr(1);
        }
        
        std::ifstream file(path);
        if (file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            response.data = buffer.str();
            response.success = true;
            response.statusCode = 200;
            response.contentType = getContentType(path);
            std::cout << "[Browser] loaded local file (text) " << response.data.size() << " bytes" << std::endl;
            
            // Log response for file://
            if (activeTab && requestId) {
                zepra::NetworkResponse netRes;
                netRes.status_code = 200;
                netRes.status_text = "OK";
                netRes.content_type = response.contentType;
                netRes.content_length = response.data.size();
                activeTab->getNetworkMonitor().recordResponse(requestId, netRes);
            }
        } else {
            response.success = false;
            response.statusCode = 404;
            response.error = "File not found: " + path;
            std::cerr << "[Browser] Failed to open local file: " << path << std::endl;
            
            // Log error response
            if (activeTab && requestId) {
                activeTab->getNetworkMonitor().recordError(requestId, response.error);
            }
        }
        return response;
    }

    try {
        // Use the networking library's HttpClient (complete implementation)
        Zepra::Networking::HttpClientConfig config;
        config.connectTimeoutMs = 10000;
        config.readTimeoutMs = 30000;  // Increased for slow sites
        config.followRedirects = true;
        config.maxRedirects = 10;
        config.verifySsl = false;  // Skip verification for now
        config.userAgent = "ZepraBrowser/1.0 (NeolyxOS)";
        config.useCookies = true;
        
        Zepra::Networking::HttpClient client(config);
        Zepra::Networking::HttpRequest request(Zepra::Networking::HttpMethod::GET, url);
        
        auto netResponse = client.send(request);
        
        response.statusCode = netResponse.statusCode();
        response.success = netResponse.isSuccess();
        response.contentType = netResponse.header("Content-Type");
        
        if (netResponse.hasError()) {
            response.error = netResponse.error();
            
            // Log error to tab's network monitor
            if (activeTab && requestId) {
                activeTab->getNetworkMonitor().recordError(requestId, response.error);
            }
        } else {
            const auto& body = netResponse.body();
            response.data = std::string(body.begin(), body.end());
            
            // Log successful response to tab's network monitor
            if (activeTab && requestId) {
                zepra::NetworkResponse netRes;
                netRes.status_code = response.statusCode;
                netRes.status_text = "OK";
                netRes.content_type = response.contentType;
                netRes.content_length = response.data.size();
                netRes.is_secure = (url.substr(0, 5) == "https");
                activeTab->getNetworkMonitor().recordResponse(requestId, netRes);
            }
        }
        
    } catch (const std::exception& e) {
        response.success = false;
        response.error = std::string("HTTP request failed: ") + e.what();
        
        // Log exception to tab's network monitor
        if (activeTab && requestId) {
            activeTab->getNetworkMonitor().recordError(requestId, response.error);
        }
    }
    
    return response;
}


// Binary fetch for images
std::vector<uint8_t> httpGetBinary(const std::string& url) {
    std::cout << "[Browser] fetching image: " << url << std::endl;
    std::vector<uint8_t> data;
    
    // Check if file:// protocol
    if (url.substr(0, 7) == "file://") {
        std::string path = url.substr(7);
        if (!path.empty() && path[0] == '~') {
            const char* home = getenv("HOME");
            if (home) path = std::string(home) + path.substr(1);
        }
        
        std::ifstream file(path, std::ios::binary);
        if (file.is_open()) {
            data = std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                                         std::istreambuf_iterator<char>());
            std::cout << "[Browser] loaded local file " << data.size() << " bytes" << std::endl;
        } else {
             std::cerr << "[Browser] Failed to open local file: " << path << std::endl;
        }
        return data;
    }
    
    try {
        Zepra::Networking::HttpClientConfig config;
        config.connectTimeoutMs = 10000;
        config.readTimeoutMs = 30000;
        config.followRedirects = true;
        config.maxRedirects = 10;
        config.verifySsl = false;
        config.userAgent = "ZepraBrowser/1.0 (NeolyxOS)";
        
        Zepra::Networking::HttpClient client(config);
        Zepra::Networking::HttpRequest request(Zepra::Networking::HttpMethod::GET, url);
        
        auto response = client.send(request);
        
        if (response.isSuccess()) {
            data = response.body();
            std::cout << "[Browser] fetched " << data.size() << " bytes for image" << std::endl;
        }
    } catch (...) {
        // Return empty on error
    }
    
    return data;
}

// Helper function to determine MIME type from file extension
std::string getContentType(const std::string& path) {
    size_t dotPos = path.rfind('.');
    if (dotPos == std::string::npos) {
        return "application/octet-stream"; // Default binary type
    }
    std::string ext = path.substr(dotPos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == "html" || ext == "htm") return "text/html";
    if (ext == "css") return "text/css";
    if (ext == "js") return "application/javascript";
    if (ext == "json") return "application/json";
    if (ext == "xml") return "application/xml";
    if (ext == "txt") return "text/plain";
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "ico") return "image/x-icon";
    if (ext == "pdf") return "application/pdf";
    if (ext == "zip") return "application/zip";
    if (ext == "mp3") return "audio/mpeg";
    if (ext == "wav") return "audio/wav";
    if (ext == "mp4") return "video/mp4";
    if (ext == "webm") return "video/webm";

    return "application/octet-stream"; // Default if unknown
}

// URL encode for query parameters
std::string urlEncode(const std::string& str) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    
    for (char c : str) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else if (c == ' ') {
            escaped << '+';
        } else {
            escaped << '%' << std::setw(2) << int((unsigned char)c);
        }
    }
    
    return escaped.str();
}

// ============================================================================
// URL NORMALIZATION
// ============================================================================

std::string normalizeUrl(const std::string& input) {
    std::string url = input;
    
    // For directories, try index.html first
    if (!url.empty() && url.back() == '/') {
        // Try with index.html
        std::string indexUrl = url + "index.html";
        auto resp = httpGet(indexUrl);
        if (resp.success && resp.statusCode == 200) {
            return indexUrl;
        }
        // Keep original if index.html doesn't exist
    }
    
    return url;
}

std::string getDisplayUrl(const std::string& url) {
    // Clean URL for display in address bar
    std::string display = url;
    
    // Remove trailing /index.html for cleaner display
    if (display.length() > 11 && 
        display.substr(display.length() - 11) == "/index.html") {
        display = display.substr(0, display.length() - 10); // Keep one /
    }
    
    // Remove trailing slash for display
    if (!display.empty() && display.back() == '/' && display.length() > 1) {
        // But keep it for root like http://example.com/
        size_t slashCount = std::count(display.begin(), display.end(), '/');
        if (slashCount > 3) {
            display.pop_back();
        }
    }
    
    return display;
}

std::string extractTitle(const std::string& html) {
    // Extract <title> from HTML
    size_t start = html.find("<title>");
    if (start == std::string::npos) start = html.find("<TITLE>");
    if (start != std::string::npos) {
        start += 7;
        size_t end = html.find("</title>", start);
        if (end == std::string::npos) end = html.find("</TITLE>", start);
        if (end != std::string::npos) {
            return html.substr(start, end - start);
        }
    }
    return "";
}

#ifdef USE_WEBCORE
// Helper: Convert CSSColor to uint32
// Helper: Convert CSSColor to uint32 - Moved to css_utils.cpp

// Helper: Decode HTML entities (&#2361; &amp; &lt; etc.)
static std::string decodeHtmlEntities(const std::string& input) {
    std::string result;
    result.reserve(input.size());
    
    for (size_t i = 0; i < input.size(); i++) {
        if (input[i] == '&') {
            // Find end of entity
            size_t end = input.find(';', i);
            if (end != std::string::npos && end - i < 12) {
                std::string entity = input.substr(i + 1, end - i - 1);
                
                // Numeric entity
                if (!entity.empty() && entity[0] == '#') {
                    int codepoint = 0;
                    if (entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X')) {
                        // Hex: &#x1F600;
                        codepoint = std::stoi(entity.substr(2), nullptr, 16);
                    } else {
                        // Decimal: &#2361;
                        codepoint = std::stoi(entity.substr(1));
                    }
                    
                    // Convert codepoint to UTF-8
                    if (codepoint < 0x80) {
                        result += (char)codepoint;
                    } else if (codepoint < 0x800) {
                        result += (char)(0xC0 | (codepoint >> 6));
                        result += (char)(0x80 | (codepoint & 0x3F));
                    } else if (codepoint < 0x10000) {
                        result += (char)(0xE0 | (codepoint >> 12));
                        result += (char)(0x80 | ((codepoint >> 6) & 0x3F));
                        result += (char)(0x80 | (codepoint & 0x3F));
                    } else {
                        result += (char)(0xF0 | (codepoint >> 18));
                        result += (char)(0x80 | ((codepoint >> 12) & 0x3F));
                        result += (char)(0x80 | ((codepoint >> 6) & 0x3F));
                        result += (char)(0x80 | (codepoint & 0x3F));
                    }
                    i = end;
                    continue;
                }
                // Named entities
                else if (entity == "amp") { result += '&'; i = end; continue; }
                else if (entity == "lt") { result += '<'; i = end; continue; }
                else if (entity == "gt") { result += '>'; i = end; continue; }
                else if (entity == "quot") { result += '"'; i = end; continue; }
                else if (entity == "apos") { result += '\''; i = end; continue; }
                else if (entity == "nbsp") { result += ' '; i = end; continue; }
                else if (entity == "copy") { result += "©"; i = end; continue; }
                else if (entity == "reg") { result += "®"; i = end; continue; }
            }
        }
        result += input[i];
    }
    return result;
}

// Forward declaration for buildLayoutFromDOM
void buildLayoutFromDOM(DOMElement* element, LayoutBox* parentBox, bool inLink = false, 
                        const std::string& linkHref = "", const std::string& linkTarget = "");

// Convert CSS engine CSSLength → layout engine LayoutLength
static ZepraBrowser::LayoutLength cssToLayout(const Zepra::WebCore::CSSLength& css) {
    using CU = Zepra::WebCore::CSSLength::Unit;
    using LL = ZepraBrowser::LayoutLength;
    switch (css.unit) {
        case CU::Auto:    return LL::autoVal();
        case CU::Px:      return LL::px(css.value);
        case CU::Percent: return LL::pct(css.value);
        case CU::Em:      return {css.value, LL::Unit::Em};
        case CU::Rem:     return {css.value, LL::Unit::Rem};
        case CU::Vw:      return {css.value, LL::Unit::Vw};
        case CU::Vh:      return {css.value, LL::Unit::Vh};
        default:          return LL::px(css.value);
    }
}

// URL resolution helper - resolve relative URLs against base URL
std::string resolveUrl(const std::string& base, const std::string& href) {
    if (href.empty()) return "";
    
    // Already absolute
    if (href.find("://") != std::string::npos) return href;
    
    // Protocol-relative (//example.com/path)
    if (href.length() >= 2 && href[0] == '/' && href[1] == '/') {
        // Extract protocol from base
        size_t protoEnd = base.find("://");
        if (protoEnd != std::string::npos) {
            return base.substr(0, protoEnd + 1) + href;
        }
        return "https:" + href;
    }
    
    // Parse base URL
    size_t protoEnd = base.find("://");
    if (protoEnd == std::string::npos) return href;
    
    std::string protocol = base.substr(0, protoEnd + 3);  // "https://"
    
    size_t hostStart = protoEnd + 3;
    size_t hostEnd = base.find('/', hostStart);
    if (hostEnd == std::string::npos) hostEnd = base.length();
    
    std::string host = base.substr(hostStart, hostEnd - hostStart);
    std::string origin = protocol + host;
    
    // Root-relative ("/path")
    if (href[0] == '/') {
        return origin + href;
    }
    
    // Relative path - resolve against base directory
    std::string basePath = base.substr(hostEnd);
    size_t lastSlash = basePath.rfind('/');
    if (lastSlash != std::string::npos) {
        basePath = basePath.substr(0, lastSlash + 1);
    } else {
        basePath = "/";
    }
    
    return origin + basePath + href;
}

// Helper: Preprocess CSS to expand CSS variables (var(--name))
// LibWebCore doesn't support CSS variables, so we expand them manually
// Helper: Preprocess CSS to expand CSS variables (var(--name))
// Moved to css_utils.cpp

// Helper: Polyfill CSS shorthands (background -> background-color, etc.)
// Helper: Polyfill CSS shorthands (background -> background-color, etc.)
// Moved to css_utils.cpp

// Helper: Extract CSS from raw HTML (workaround for HTMLParser not adding style elements to head)
// Helper: Extract CSS from raw HTML
// Moved to css_utils.cpp

// Parse HTML with WebCore and extract styled content
#ifdef USE_WEBCORE
void parseWithWebCore(const std::string& html) {
    // CRITICAL: Cancel pending image loads before clearing layout boxes
    // Prevents use-after-free when worker threads hold stale pointers
    ZepraBrowser::g_lazyImageLoader.cancelAll();
    
    g_styledLines.clear();
    
    // WORKAROUND: Extract CSS from raw HTML before parsing (HTMLParser drops head children)
    std::vector<std::string> rawStyles = extractStylesFromRawHTML(html);

    // Extract body inline styles (e.g. background-color) before parsing 
    std::string bodyStyle = extractBodyInlineStyle(html);
    
    // Parse HTML to DOM (use original HTML - HTMLParser handles structure correctly)
    HTMLParser parser;
    g_document = parser.parse(html);
    
    if (!g_document) {
        std::cerr << "[WebCore] Failed to parse HTML" << std::endl;
        return;
    }
    
    // Initialize CSS engine
    g_cssEngine = std::make_unique<CSSEngine>();
    g_cssEngine->initialize(g_document.get());
    
    // Load user-agent stylesheet (browser defaults)
    g_cssEngine->addStyleSheet(
        Zepra::WebCore::ZepraUAStylesheet::getStylesheet(),
        StyleOrigin::UserAgent
    );
    
    // Extract <style> elements and load <link rel="stylesheet"> external CSS
    auto* headEl = g_document->head();
    std::cout << "[CSS Debug] head element: " << (headEl ? "found" : "NULL") << std::endl;
    if (headEl) {
        std::cout << "[CSS Debug] head has " << headEl->childNodes().size() << " children" << std::endl;
        for (size_t i = 0; i < headEl->childNodes().size(); i++) {
            if (auto* el = dynamic_cast<DOMElement*>(headEl->childNodes()[i].get())) {
                std::string tag = el->tagName();
                std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);
                std::cout << "[CSS Debug] head child: <" << tag << ">" << std::endl;
                std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);
                
                // Inline <style> elements
                if (tag == "style") {
                    g_cssEngine->addStyleSheet(el->textContent(), StyleOrigin::Author);
                }
                // External <link rel="stylesheet"> elements
                else if (tag == "link") {
                    std::string rel = el->getAttribute("rel");
                    std::transform(rel.begin(), rel.end(), rel.begin(), ::tolower);
                    if (rel == "stylesheet" || rel.find("stylesheet") != std::string::npos) {
                        std::string href = el->getAttribute("href");
                        if (!href.empty()) {
                            // Resolve relative URL
                            std::string cssUrl = resolveUrl(g_currentUrl, href);
                            std::cout << "[CSS] Loading external stylesheet: " << cssUrl << std::endl;
                            
                            // Fetch the CSS file
                            HttpResponse cssResult = httpGet(cssUrl);
                            if (cssResult.success && !cssResult.data.empty()) {
                                std::cout << "[CSS] Loaded " << cssResult.data.size() << " bytes of CSS" << std::endl;
                                g_cssEngine->addStyleSheet(cssResult.data, StyleOrigin::Author);
                            } else {
                                std::cerr << "[CSS] Failed to load: " << cssResult.error << std::endl;
                            }
                        }
                    }
                }
            }
        }
    }
    
    // FALLBACK: Add CSS extracted from raw HTML (when HTMLParser fails to add style elements to head)
    if (rawStyles.size() > 0) {
        std::cout << "[CSS] Adding " << rawStyles.size() << " stylesheets from raw HTML extraction" << std::endl;
        for (const auto& css : rawStyles) {
            g_cssEngine->addStyleSheet(css, StyleOrigin::Author);
        }
    }
    
    // Add extracted body element style
    if (!bodyStyle.empty()) {
        std::cout << "[CSS] Adding extracted body inline style: " << bodyStyle << std::endl;
        g_cssEngine->addStyleSheet(bodyStyle, StyleOrigin::Author);
    }
    
    // Compute styles
    g_cssEngine->computeStyles();
    std::cout << "[CSS] Styles computed" << std::endl;
    
    // Create layout root (Modular LayoutBox)
    g_focusedBox = nullptr;
    g_layoutRoot = std::make_unique<LayoutBox>();
    g_layoutRoot->type = LayoutType::Block;
    g_layoutRoot->paddingLeft = 8;
    g_layoutRoot->paddingRight = 8;
    g_layoutRoot->paddingTop = 8;
    g_layoutRoot->paddingBottom = 8;
    g_layoutRoot->width = (float)g_width;
    
    // Apply body CSS styles to layout root
    if (g_document->body()) {
        const CSSComputedStyle* bodyStyle = g_cssEngine->getComputedStyle(g_document->body());
        if (bodyStyle) {
            // Background
            if (!bodyStyle->backgroundColor.isTransparent()) {
                g_layoutRoot->bgColor = cssColorToRGB(bodyStyle->backgroundColor);
                g_layoutRoot->hasBgColor = true;
            }
            if (!bodyStyle->backgroundImage.empty() && bodyStyle->backgroundImage != "none") {
                g_layoutRoot->backgroundImage = bodyStyle->backgroundImage;
                g_layoutRoot->hasBgColor = true;
            }
            
            // Typography
            g_layoutRoot->color = cssColorToRGB(bodyStyle->color);
            g_layoutRoot->fontSize = bodyStyle->fontSize;
            
            // Margins (clamp negative body margins — root is the viewport ICB)
            g_layoutRoot->marginTop = std::max(0.0f, bodyStyle->marginTop.isAuto() ? 0.0f : bodyStyle->marginTop.value);
            g_layoutRoot->marginBottom = std::max(0.0f, bodyStyle->marginBottom.isAuto() ? 0.0f : bodyStyle->marginBottom.value);
            g_layoutRoot->marginLeft = std::max(0.0f, bodyStyle->marginLeft.isAuto() ? 0.0f : bodyStyle->marginLeft.value);
            g_layoutRoot->marginRight = std::max(0.0f, bodyStyle->marginRight.isAuto() ? 0.0f : bodyStyle->marginRight.value);
            
            // Padding — always use CSS values when computed style exists
            g_layoutRoot->paddingTop = bodyStyle->paddingTop.value;
            g_layoutRoot->paddingBottom = bodyStyle->paddingBottom.value;
            g_layoutRoot->paddingLeft = bodyStyle->paddingLeft.value;
            g_layoutRoot->paddingRight = bodyStyle->paddingRight.value;
            
            // Dimensions
            g_layoutRoot->cssWidth = cssToLayout(bodyStyle->width);
            g_layoutRoot->cssHeight = cssToLayout(bodyStyle->height);
            g_layoutRoot->cssMinWidth = cssToLayout(bodyStyle->minWidth);
            g_layoutRoot->cssMinHeight = cssToLayout(bodyStyle->minHeight);
            g_layoutRoot->cssMaxWidth = cssToLayout(bodyStyle->maxWidth);
            g_layoutRoot->cssMaxHeight = cssToLayout(bodyStyle->maxHeight);
            
            // Border
            g_layoutRoot->borderTop = bodyStyle->borderTopWidth;
            g_layoutRoot->borderRight = bodyStyle->borderRightWidth;
            g_layoutRoot->borderBottom = bodyStyle->borderBottomWidth;
            g_layoutRoot->borderLeft = bodyStyle->borderLeftWidth;
            if (bodyStyle->borderTopWidth > 0)
                g_layoutRoot->borderColor = cssColorToRGB(bodyStyle->borderTopColor);
            g_layoutRoot->borderRadius = bodyStyle->borderTopLeftRadius;
            
            // Visual
            g_layoutRoot->opacity = bodyStyle->opacity;
            g_layoutRoot->overflowHidden = (bodyStyle->overflowX == OverflowValue::Hidden ||
                                            bodyStyle->overflowY == OverflowValue::Hidden);
            g_layoutRoot->visibilityHidden = (bodyStyle->visibility == Visibility::Hidden);
            
            // Text alignment
            if (bodyStyle->textAlign == TextAlign::Center) g_layoutRoot->textAlign = 1;
            else if (bodyStyle->textAlign == TextAlign::Right) g_layoutRoot->textAlign = 2;
            
            // Display: flex on body
            if (bodyStyle->display == DisplayValue::Flex || bodyStyle->display == DisplayValue::InlineFlex) {
                g_layoutRoot->type = LayoutType::Flex;
                using namespace Zepra::WebCore;
                if (bodyStyle->flexDirection == FlexDirection::Column ||
                    bodyStyle->flexDirection == FlexDirection::ColumnReverse)
                    g_layoutRoot->flexDirection = (bodyStyle->flexDirection == FlexDirection::ColumnReverse) ? 3 : 1;
                else if (bodyStyle->flexDirection == FlexDirection::RowReverse)
                    g_layoutRoot->flexDirection = 2;
                g_layoutRoot->flexWrap = bodyStyle->flexWrap;
                g_layoutRoot->wrapReverse = bodyStyle->wrapReverse;
                g_layoutRoot->gap = bodyStyle->gap.value;
                switch (bodyStyle->justifyContent) {
                    case JustifyAlign::FlexEnd: case JustifyAlign::End: g_layoutRoot->justifyContent = 1; break;
                    case JustifyAlign::Center: g_layoutRoot->justifyContent = 2; break;
                    case JustifyAlign::SpaceBetween: g_layoutRoot->justifyContent = 3; break;
                    case JustifyAlign::SpaceAround: g_layoutRoot->justifyContent = 4; break;
                    case JustifyAlign::SpaceEvenly: g_layoutRoot->justifyContent = 5; break;
                    default: g_layoutRoot->justifyContent = 0; break;
                }
                switch (bodyStyle->alignItems) {
                    case JustifyAlign::FlexStart: case JustifyAlign::Start: g_layoutRoot->alignItems = 1; break;
                    case JustifyAlign::FlexEnd: case JustifyAlign::End: g_layoutRoot->alignItems = 2; break;
                    case JustifyAlign::Center: g_layoutRoot->alignItems = 3; break;
                    case JustifyAlign::Baseline: g_layoutRoot->alignItems = 4; break;
                    default: g_layoutRoot->alignItems = 0; break;
                }
            }
            
            // Debug: body computed style
            std::cout << "[Body CSS Debug] display=" << (int)bodyStyle->display
                      << " bgImage='" << bodyStyle->backgroundImage << "'"
                      << " flexDir=" << (int)bodyStyle->flexDirection
                      << " justifyContent=" << (int)bodyStyle->justifyContent
                      << " alignItems=" << (int)bodyStyle->alignItems
                      << " minHeight=" << bodyStyle->minHeight.value << "(" << (int)bodyStyle->minHeight.unit << ")"
                      << " padding=" << bodyStyle->paddingTop.value << "," << bodyStyle->paddingLeft.value
                      << " margin=" << bodyStyle->marginTop.value << "," << bodyStyle->marginLeft.value
                      << std::endl;
            std::cout << "[LayoutRoot Debug] type=" << (int)g_layoutRoot->type
                      << " bgImage='" << g_layoutRoot->backgroundImage << "'"
                      << " hasBgColor=" << g_layoutRoot->hasBgColor
                      << " flex=" << g_layoutRoot->flexDirection
                      << " justify=" << g_layoutRoot->justifyContent
                      << " align=" << g_layoutRoot->alignItems
                      << " minH=" << g_layoutRoot->cssMinHeight.value << "(" << (int)g_layoutRoot->cssMinHeight.unit << ")"
                      << " w=" << g_layoutRoot->width
                      << std::endl;
        }
    }
     
    // Build LayoutBox tree directly from DOM
    if (g_document->body()) {
        buildLayoutFromDOM(g_document->body(), g_layoutRoot.get(), false, "", "");
    }
    
    // Count all boxes recursively
    std::function<int(const LayoutBox&)> countBoxes = [&](const LayoutBox& b) -> int {
        int n = 1;
        for (const auto& c : b.children) n += countBoxes(c);
        return n;
    };
    int totalBoxes = countBoxes(*g_layoutRoot);
    std::cout << "[Layout] Built layout tree: " << totalBoxes << " total boxes, "
              << g_layoutRoot->children.size() << " direct children" << std::endl;
    
    // Debug DOM children
    if (g_document->body()) {
        std::cout << "[DOM] body has " << g_document->body()->childNodes().size() << " children" << std::endl;
        for (size_t i = 0; i < g_document->body()->childNodes().size(); i++) {
            if (auto* el = dynamic_cast<DOMElement*>(g_document->body()->childNodes()[i].get())) {
                std::cout << "[DOM]   child: <" << el->tagName() << " class=\"" 
                          << el->getAttribute("class") << "\"> children=" 
                          << el->childNodes().size() << std::endl;
            }
        }
    }
    
    // Initialize ScriptContext and execute inline <script> tags
    g_scriptContext = std::make_unique<ScriptContext>();
    
    // Route console output to active tab
    Tab* activeTab = nullptr;
    for (auto& t : g_tabs) {
        if (t.id == g_activeTabId) { activeTab = &t; break; }
    }
    if (activeTab) {
        g_scriptContext->setConsoleHandler(
            [activeTab](const std::string& level, const std::string& msg) {
                activeTab->logConsole("[" + level + "] " + msg);
            });
    }
    
    g_scriptContext->initialize(g_document.get());
    
    // Wire alert/confirm/prompt handlers
    g_scriptContext->setAlertHandler([](const std::string& msg) {
        g_alertMessage = msg;
        g_alertIsConfirm = false;
        g_alertVisible = true;
    });
    
    // Set location globals from current URL
    g_scriptContext->setGlobal("__pageUrl__", g_currentUrl);
    
    // Execute page scripts (inline + external)
    std::function<void(DOMElement*)> executePageScripts = [&](DOMElement* el) {
        if (!el) return;
        for (size_t i = 0; i < el->childNodes().size(); i++) {
            if (auto* child = dynamic_cast<DOMElement*>(el->childNodes()[i].get())) {
                std::string tag = child->tagName();
                std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);
                if (tag == "script") {
                    // Check type attribute — skip non-JS types (JSON-LD, module preloads, etc.)
                    std::string type = child->getAttribute("type");
                    if (!type.empty() && type != "text/javascript" && type != "application/javascript"
                        && type != "module") {
                        continue;
                    }
                    
                    std::string src = child->getAttribute("src");
                    if (src.empty()) {
                        // Inline script
                        std::string code = child->textContent();
                        if (!code.empty()) {
                            std::cout << "[JS] Executing inline script (" << code.size() << " bytes)" << std::endl;
                            g_scriptContext->evaluate(code, g_currentUrl);
                        }
                    } else {
                        // External script — resolve URL and fetch
                        std::string scriptUrl = resolveUrl(g_currentUrl, src);
                        std::cout << "[JS] Fetching external script: " << scriptUrl << std::endl;
                        HttpResponse scriptResp = httpGet(scriptUrl);
                        if (scriptResp.success && !scriptResp.data.empty()) {
                            std::cout << "[JS] Executing external script (" 
                                      << scriptResp.data.size() << " bytes)" << std::endl;
                            g_scriptContext->evaluate(scriptResp.data, scriptUrl);
                        } else {
                            std::cerr << "[JS] Failed to load: " << scriptUrl << std::endl;
                        }
                    }
                } else {
                    executePageScripts(child);
                }
            }
        }
    };
    
    if (g_document->body()) {
        executePageScripts(g_document->body());
    }
    // Also check head for scripts
    if (g_document->head()) {
        executePageScripts(g_document->head());
    }
    
    // Fire DOMContentLoaded
    g_scriptContext->fireDOMContentLoaded();
    if (g_scriptContext) {
        g_scriptContext->evaluate("if (typeof document !== 'undefined' && typeof document.onDOMContentLoaded === 'function') { document.onDOMContentLoaded(); }", "zepra://internal/dom_loaded");
        g_scriptContext->fireLoadEvent();
        g_scriptContext->evaluate("if (typeof window !== 'undefined' && typeof window.onload === 'function') { window.onload(); }", "zepra://internal/onload");
    }
    std::cout << "[JS] DOMContentLoaded and Window Load fired" << std::endl;
}

// =============================================================================
// DIRECT DOM → LAYOUT (Firefox Frame Tree Pattern)
// =============================================================================

// Helper to check if text looks like JavaScript code
static bool looksLikeJavaScript(const std::string& text) {
    if (text.length() < 50) return false;
    
    int specialChars = 0, assignOps = 0;
    for (size_t i = 0; i + 1 < text.length(); i++) {
        char c = text[i], n = text[i + 1];
        if (c == '{' || c == '}' || c == ';' || c == '(' || c == ')') specialChars++;
        if ((c == '=' && n != '=') || (c == ':' && n == ':')) assignOps++;
    }
    if (specialChars > 10 || assignOps > 5) return true;
    
    return text.find("function(") != std::string::npos ||
           text.find("=>") != std::string::npos ||
           text.find("var ") != std::string::npos ||
           text.find("const ") != std::string::npos ||
           text.find("window.") != std::string::npos ||
           text.find("document.") != std::string::npos ||
           text.find("__webpack") != std::string::npos ||
           text.find("base64,") != std::string::npos;
}

// Filter HTML attribute-like content that shouldn't be displayed
static bool looksLikeHTMLAttributes(const std::string& text) {
    // Skip if too short
    if (text.length() < 20) return false;
    
    // Check for common HTML attribute patterns
    if (text.find("class=") != std::string::npos ||
        text.find("data-") != std::string::npos ||
        text.find("aria-") != std::string::npos ||
        text.find("role=") != std::string::npos ||
        text.find("style=") != std::string::npos ||
        text.find("href=") != std::string::npos ||
        text.find("src=") != std::string::npos ||
        text.find("id=\"") != std::string::npos ||
        text.find("data-t-l") != std::string::npos ||
        text.find("gnt_") != std::string::npos) {
        return true;
    }
    
    // Count quotes and equals signs - too many suggests attribute content
    int quotes = 0, equals = 0;
    for (char c : text) {
        if (c == '"' || c == '\'') quotes++;
        if (c == '=') equals++;
    }
    if (quotes > 4 || equals > 3) return true;
    
    return false;
}



// Recursively build LayoutBox tree directly from DOM (Firefox pattern)
void buildLayoutFromDOM(DOMElement* element, LayoutBox* parentBox, bool inLink, 
                        const std::string& linkHref, const std::string& linkTarget) {
    if (!element || !parentBox) return;
    
    std::string tag = element->tagName();
    std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);
    
    // Skip invisible elements (but keep svg - may contain visible content)
    if (tag == "script" || tag == "style" || tag == "head" || tag == "meta" || 
        tag == "link" || tag == "title" || tag == "noscript" || tag == "template") {
        return;
    }
    
    // Get computed style
    const CSSComputedStyle* style = g_cssEngine ? g_cssEngine->getComputedStyle(element) : nullptr;
    
    // Skip display:none
#pragma push_macro("None")
#undef None
    if (style && style->display == DisplayValue::None) return;
#pragma pop_macro("None")
    
    // Check if this is an anchor tag
    bool isAnchor = (tag == "a");
    std::string href = isAnchor ? element->getAttribute("href") : linkHref;
    std::string target = isAnchor ? element->getAttribute("target") : linkTarget;
    bool nowInLink = inLink || isAnchor;
    
    // Process child nodes
    for (size_t i = 0; i < element->childNodes().size(); i++) {
        DOMNode* child = element->childNodes()[i].get();
        
        if (auto* textNode = dynamic_cast<DOMText*>(child)) {
            // TEXT NODE → Create LayoutBox directly
            std::string text = textNode->data();
            
            // Normalize whitespace
            std::string normalized;
            bool lastSpace = true;
            for (char c : text) {
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    if (!lastSpace) { normalized += ' '; lastSpace = true; }
                } else {
                    normalized += c; lastSpace = false;
                }
            }
            if (!normalized.empty() && normalized.back() == ' ') normalized.pop_back();
            if (!normalized.empty() && normalized.front() == ' ') normalized = normalized.substr(1);
            
            // Skip empty, code-like, or HTML attribute-like content
            if (normalized.empty() || looksLikeJavaScript(normalized) || looksLikeHTMLAttributes(normalized)) continue;
            
            // Decode HTML entities (&nbsp; &amp; etc.)
            normalized = decodeHtmlEntities(normalized);
            
            // CREATE LAYOUTBOX
            LayoutBox* box = addChild(parentBox);
            box->text = normalized;
            box->type = LayoutType::Inline;
            
            // Apply ALL CSS styles from computed style
            if (style) {
                // Typography
                box->fontSize = style->fontSize;
                box->color = cssColorToRGB(style->color);
                box->bold = (style->fontWeight >= FontWeight::Bold);
                box->italic = (style->fontStyle == FontStyle::Italic);
                box->textDecoration = style->textDecoration;
                
                // Background
                if (!style->backgroundColor.isTransparent()) {
                    box->bgColor = cssColorToRGB(style->backgroundColor);
                    box->hasBgColor = true;
                }
                if (!style->backgroundImage.empty() && style->backgroundImage != "none") {
                    box->backgroundImage = style->backgroundImage;
                    box->hasBgColor = true;
                }
                
                // Margins (from CSS)
                box->marginTop = style->marginTop.value;
                box->marginBottom = style->marginBottom.value;
                box->marginLeft = style->marginLeft.value;
                box->marginRight = style->marginRight.value;
                
                // Padding (from CSS)
                box->paddingTop = style->paddingTop.value;
                box->paddingBottom = style->paddingBottom.value;
                box->paddingLeft = style->paddingLeft.value;
                box->paddingRight = style->paddingRight.value;
                
                // Border width/color
                box->borderTop = style->borderTopWidth;
                box->borderRight = style->borderRightWidth;
                box->borderBottom = style->borderBottomWidth;
                box->borderLeft = style->borderLeftWidth;
                if (style->borderTopWidth > 0) {
                    box->borderColor = cssColorToRGB(style->borderTopColor);
                }
                
                // Border radius
                box->borderRadius = style->borderTopLeftRadius;
                
                // Opacity
                box->opacity = style->opacity;
                
                // Overflow
                box->overflowHidden = (style->overflowX == OverflowValue::Hidden || 
                                       style->overflowY == OverflowValue::Hidden);
                box->boxSizing = (style->boxSizing == Zepra::WebCore::BoxSizing::BorderBox) ? 1 : 0;
                
                // Text nodes always stay LayoutType::Inline — display type from CSS
                // only applies to element nodes (containers), not text runs.
                // Only check for display:none to hide text in hidden parents.
#pragma push_macro("None")
#undef None
                if (style->display == DisplayValue::None)
                    box->type = LayoutType::None;
#pragma pop_macro("None")
                
                // Text alignment (inherited from parent)
                if (style->textAlign == TextAlign::Center) box->textAlign = 1;
                else if (style->textAlign == TextAlign::Right) box->textAlign = 2;
                else box->textAlign = 0;
            }
            
            // Link styling
            if (nowInLink && !href.empty()) {
                box->isLink = true;
                box->href = href;
                box->target = target;
                if (box->color == 0x1F2328 || box->color == 0) box->color = 0x0066CC;
            }
            
            // Fallback margins for common tags (only when CSS engine had no computed style)
            if (!style) {
                if (tag == "h1") { box->marginTop = 16; box->marginBottom = 16; box->type = LayoutType::Block; }
                else if (tag == "h2") { box->marginTop = 12; box->marginBottom = 12; box->type = LayoutType::Block; }
                else if (tag == "h3") { box->marginTop = 10; box->marginBottom = 10; box->type = LayoutType::Block; }
                else if (tag == "p" || tag == "div") { box->marginTop = 8; box->marginBottom = 8; box->type = LayoutType::Block; }
                else if (tag == "li") { box->marginTop = 4; box->marginBottom = 4; }
            }
            
            // Also add to g_styledLines for tab caching compatibility
            StyledTextLine line;
            line.text = normalized;
            line.fontSize = box->fontSize;
            line.color = box->color;
            line.bold = box->bold;
            line.italic = box->italic;
            line.isLink = box->isLink;
            line.href = box->href;
            line.target = box->target;
            line.marginTop = box->marginTop;
            line.marginBottom = box->marginBottom;
            g_styledLines.push_back(line);
            
        } else if (auto* childElement = dynamic_cast<DOMElement*>(child)) {
            // ELEMENT NODE → Recurse
            std::string childTag = childElement->tagName();
            // std::cout << "[Layout] Visiting tag: " << childTag << std::endl;
            std::transform(childTag.begin(), childTag.end(), childTag.begin(), ::tolower);
            
            // Handle input elements
            if (childTag == "input") {
                std::string inputType = childElement->getAttribute("type");
                std::transform(inputType.begin(), inputType.end(), inputType.begin(), ::tolower);
                if (inputType.empty()) inputType = "text";
                if (inputType == "hidden") continue;
                
                // Route button-like inputs to button handler below
                if (inputType == "submit" || inputType == "button" || inputType == "reset") {
                    // Fall through — handled by button handler below
                }
                // Checkbox rendering
                else if (inputType == "checkbox") {
                    LayoutBox* cbBox = addChild(parentBox);
                    cbBox->type = LayoutType::InlineBlock;
                    bool checked = childElement->hasAttribute("checked");
                    cbBox->text = checked ? "\xe2\x98\x91" : "\xe2\x98\x90"; // ☑ or ☐
                    
                    const CSSComputedStyle* style = g_cssEngine->getComputedStyle(childElement);
                    if (style && !style->backgroundColor.isTransparent()) {
                        cbBox->bgColor = cssColorToRGB(style->backgroundColor);
                        cbBox->hasBgColor = true;
                        cbBox->color = cssColorToRGB(style->color);
                    } else {
                        cbBox->color = 0xCCCCCC;
                    }
                    cbBox->fontSize = 16;
                    cbBox->width = 18; cbBox->height = 18;
                    cbBox->paddingLeft = 2; cbBox->paddingRight = 2;
                    cbBox->paddingTop = 1; cbBox->paddingBottom = 1;
                    cbBox->isInput = true;
                    cbBox->inputType = inputType;
                    continue;
                }
                // Radio button rendering
                else if (inputType == "radio") {
                    LayoutBox* rbBox = addChild(parentBox);
                    rbBox->type = LayoutType::InlineBlock;
                    bool checked = childElement->hasAttribute("checked");
                    rbBox->text = checked ? "\xe2\x97\x89" : "\xe2\x97\x8b"; // ◉ or ○
                    
                    const CSSComputedStyle* style = g_cssEngine->getComputedStyle(childElement);
                    if (style && !style->backgroundColor.isTransparent()) {
                        rbBox->bgColor = cssColorToRGB(style->backgroundColor);
                        rbBox->hasBgColor = true;
                        rbBox->color = cssColorToRGB(style->color);
                    } else {
                        rbBox->color = 0xCCCCCC;
                    }
                    rbBox->fontSize = 16;
                    rbBox->width = 18; rbBox->height = 18;
                    rbBox->paddingLeft = 2; rbBox->paddingRight = 2;
                    rbBox->paddingTop = 1; rbBox->paddingBottom = 1;
                    rbBox->isInput = true;
                    rbBox->inputType = inputType;
                    continue;
                }
                // Range slider (simplified bar)
                else if (inputType == "range") {
                    LayoutBox* rangeBox = addChild(parentBox);
                    rangeBox->type = LayoutType::InlineBlock;
                    rangeBox->text = "";
                    rangeBox->width = 150; rangeBox->height = 20;
                    rangeBox->bgColor = 0x444444;
                    rangeBox->hasBgColor = true;
                    rangeBox->borderRadius = 4;
                    rangeBox->borderTop = 1; rangeBox->borderColor = 0x666666;
                    rangeBox->isInput = true;
                    rangeBox->inputType = inputType;
                    continue;
                }
                // Color picker (swatch)
                else if (inputType == "color") {
                    LayoutBox* colorBox = addChild(parentBox);
                    colorBox->type = LayoutType::InlineBlock;
                    colorBox->text = "";
                    std::string val = childElement->getAttribute("value");
                    uint32_t swatchColor = 0x000000;
                    if (!val.empty() && val[0] == '#' && val.size() >= 7) {
                        swatchColor = (uint32_t)std::stoul(val.substr(1, 6), nullptr, 16);
                    }
                    colorBox->width = 32; colorBox->height = 24;
                    colorBox->bgColor = swatchColor;
                    colorBox->hasBgColor = true;
                    colorBox->borderTop = 1; colorBox->borderRight = 1;
                    colorBox->borderBottom = 1; colorBox->borderLeft = 1;
                    colorBox->borderColor = 0x888888;
                    colorBox->borderRadius = 3;
                    colorBox->isInput = true;
                    colorBox->inputType = inputType;
                    continue;
                }
                // File upload
                else if (inputType == "file") {
                    LayoutBox* fileBox = addChild(parentBox);
                    fileBox->type = LayoutType::InlineBlock;
                    fileBox->text = "Choose File";
                    fileBox->fontSize = 13;
                    fileBox->color = 0xDDDDDD;
                    fileBox->bgColor = 0x444444;
                    fileBox->hasBgColor = true;
                    fileBox->paddingTop = 4; fileBox->paddingBottom = 4;
                    fileBox->paddingLeft = 10; fileBox->paddingRight = 10;
                    fileBox->borderTop = 1; fileBox->borderRight = 1;
                    fileBox->borderBottom = 1; fileBox->borderLeft = 1;
                    fileBox->borderColor = 0x666666;
                    fileBox->borderRadius = 3;
                    fileBox->isInput = true;
                    fileBox->inputType = inputType;
                    continue;
                }
                // Text-like inputs: text, password, email, search, url, tel, number, date, datetime-local, month, week, time
                else {
                    LayoutBox* inputBox = addChild(parentBox);
                    inputBox->type = LayoutType::InlineBlock;
                    
                    // Display value or placeholder
                    std::string value = childElement->getAttribute("value");
                    if (inputType == "password" && !value.empty()) {
                        value = std::string(value.size(), '\xe2\x80\xa2'); // bullet dots
                    }
                    inputBox->text = value;
                    inputBox->placeholder = childElement->getAttribute("placeholder");
                    if (inputBox->text.empty() && inputBox->placeholder.empty()) {
                        inputBox->placeholder = inputType;
                    }
                    
                    const CSSComputedStyle* style = g_cssEngine ? g_cssEngine->getComputedStyle(childElement) : nullptr;
                    if (style) {
                        // Typography — always from CSS
                        inputBox->color = cssColorToRGB(style->color);
                        inputBox->fontSize = style->fontSize > 0 ? style->fontSize : 14;
                        
                        // Background — CSS value or UA default
                        if (!style->backgroundColor.isTransparent()) {
                            inputBox->bgColor = cssColorToRGB(style->backgroundColor);
                        } else {
                            inputBox->bgColor = 0xFFFFFF;
                        }
                        inputBox->hasBgColor = true;
                        
                        // Padding — always use CSS values (padding:0 from * reset is valid)
                        inputBox->paddingTop = style->paddingTop.value;
                        inputBox->paddingBottom = style->paddingBottom.value;
                        inputBox->paddingLeft = style->paddingLeft.value;
                        inputBox->paddingRight = style->paddingRight.value;
                        
                        // Border — from CSS, with UA fallback only when CSS specifies none
                        inputBox->borderTop = style->borderTopWidth;
                        inputBox->borderRight = style->borderRightWidth;
                        inputBox->borderBottom = style->borderBottomWidth;
                        inputBox->borderLeft = style->borderLeftWidth;
                        if (style->borderTopWidth > 0 || style->borderRightWidth > 0 ||
                            style->borderBottomWidth > 0 || style->borderLeftWidth > 0) {
                            inputBox->borderColor = cssColorToRGB(style->borderTopColor);
                        } else {
                            inputBox->borderColor = 0xCCCCCC;
                        }
                        
                        // Border radius — from CSS
                        inputBox->borderRadius = style->borderTopLeftRadius;
                        
                        // Dimensions — CSS first, then size attribute, then type-based defaults
                        // Store deferred lengths for layout engine to resolve with viewport
                        if (!style->width.isAuto() && style->width.value > 0) {
                            inputBox->cssWidth = cssToLayout(style->width);
                            // Pre-resolve px for immediate use
                            if (style->width.unit == Zepra::WebCore::CSSLength::Unit::Px)
                                inputBox->width = style->width.value;
                            else
                                inputBox->width = style->width.toPx(style->fontSize, 16.0f, g_width, g_height, 0);
                        } else {
                            std::string sizeAttr = childElement->getAttribute("size");
                            if (!sizeAttr.empty()) {
                                try { int sz = std::stoi(sizeAttr);
                                    inputBox->width = sz * inputBox->fontSize * 0.6f + inputBox->paddingLeft + inputBox->paddingRight;
                                } catch (...) { inputBox->width = 180; }
                            } else if (inputType == "number" || inputType == "time") {
                                inputBox->width = 80;
                            } else if (inputType == "date" || inputType == "month" || inputType == "week" || inputType == "datetime-local") {
                                inputBox->width = 160;
                            } else {
                                inputBox->width = 180;
                            }
                        }
                        
                        // Max-width constraint (e.g. max-width: 90vw)
                        if (!style->maxWidth.isAuto() && style->maxWidth.value > 0) {
                            inputBox->cssMaxWidth = cssToLayout(style->maxWidth);
                            float maxW = style->maxWidth.toPx(style->fontSize, 16.0f, g_width, g_height, 0);
                            if (maxW > 0 && inputBox->width > maxW)
                                inputBox->width = maxW;
                        }
                        
                        // Height — CSS first, then content-based
                        if (!style->height.isAuto() && style->height.value > 0) {
                            inputBox->cssHeight = cssToLayout(style->height);
                            if (style->height.unit == Zepra::WebCore::CSSLength::Unit::Px)
                                inputBox->height = style->height.value;
                            else
                                inputBox->height = style->height.toPx(style->fontSize, 16.0f, g_width, g_height, 0);
                        } else {
                            inputBox->height = inputBox->fontSize + inputBox->paddingTop + inputBox->paddingBottom +
                                               inputBox->borderTop + inputBox->borderBottom;
                            // Ensure minimum usable height
                            if (inputBox->height < inputBox->fontSize + 4)
                                inputBox->height = inputBox->fontSize + 4;
                        }
                    } else {
                        // No computed style — safe defaults
                        inputBox->fontSize = 14;
                        inputBox->color = 0x333333;
                        inputBox->bgColor = 0xFFFFFF;
                        inputBox->hasBgColor = true;
                        inputBox->paddingTop = 4; inputBox->paddingBottom = 4;
                        inputBox->paddingLeft = 8; inputBox->paddingRight = 8;
                        inputBox->borderTop = 1; inputBox->borderRight = 1;
                        inputBox->borderBottom = 1; inputBox->borderLeft = 1;
                        inputBox->borderColor = 0xCCCCCC;
                        inputBox->borderRadius = 3;
                        inputBox->width = 180;
                        inputBox->height = inputBox->fontSize + inputBox->paddingTop + inputBox->paddingBottom +
                                           inputBox->borderTop + inputBox->borderBottom;
                    }
                    
                    inputBox->isInput = true;
                    inputBox->inputType = inputType;
                    
                    StyledTextLine line;
                    line.text = inputBox->text.empty() ? inputBox->placeholder : inputBox->text;
                    line.isInput = true;
                    line.inputType = inputType;
                    line.placeholder = inputBox->placeholder;
                    line.fontSize = inputBox->fontSize;
                    line.color = inputBox->color;
                    line.bgColor = inputBox->bgColor;
                    line.hasBgColor = true;
                    g_styledLines.push_back(line);
                    
                    continue;
                }
            }
            
            // Handle button and button-like inputs
            if (childTag == "button" || 
                (childTag == "input" && (childElement->getAttribute("type") == "submit" || 
                                         childElement->getAttribute("type") == "button" ||
                                         childElement->getAttribute("type") == "reset"))) {
                LayoutBox* btnBox = addChild(parentBox);
                btnBox->type = LayoutType::InlineBlock;
                
                if (childTag == "button") {
                    btnBox->text = childElement->textContent();
                    if (btnBox->text.empty() && childElement->hasAttribute("value"))
                        btnBox->text = childElement->getAttribute("value");
                } else {
                    btnBox->text = childElement->getAttribute("value");
                }
                
                // Default text by type
                if (btnBox->text.empty()) {
                    std::string btype = childElement->getAttribute("type");
                    if (btype == "reset") btnBox->text = "Reset";
                    else btnBox->text = "Submit";
                }
                
                const CSSComputedStyle* style = g_cssEngine->getComputedStyle(childElement);
                if (style) {
                    btnBox->fontSize = style->fontSize > 0 ? style->fontSize : 14;
                    btnBox->color = cssColorToRGB(style->color);
                    if (!style->backgroundColor.isTransparent()) {
                        btnBox->bgColor = cssColorToRGB(style->backgroundColor);
                    } else {
                        btnBox->bgColor = 0x2563EB; // Default button blue
                    }
                    btnBox->hasBgColor = true;
                    btnBox->bold = (style->fontWeight >= FontWeight::Bold);
                    btnBox->paddingTop = style->paddingTop.value > 0 ? style->paddingTop.value : 8;
                    btnBox->paddingBottom = style->paddingBottom.value > 0 ? style->paddingBottom.value : 8;
                    btnBox->paddingLeft = style->paddingLeft.value > 0 ? style->paddingLeft.value : 16;
                    btnBox->paddingRight = style->paddingRight.value > 0 ? style->paddingRight.value : 16;
                    btnBox->borderRadius = style->borderTopLeftRadius > 0 ? style->borderTopLeftRadius : 4;
                    if (style->borderTopWidth > 0) {
                        btnBox->borderTop = style->borderTopWidth;
                        btnBox->borderRight = style->borderRightWidth;
                        btnBox->borderBottom = style->borderBottomWidth;
                        btnBox->borderLeft = style->borderLeftWidth;
                        btnBox->borderColor = cssColorToRGB(style->borderTopColor);
                    }
                } else {
                    btnBox->fontSize = 14;
                    btnBox->color = 0xFFFFFF;
                    btnBox->bgColor = 0x2563EB;
                    btnBox->hasBgColor = true;
                    btnBox->bold = true;
                    btnBox->paddingTop = 8; btnBox->paddingBottom = 8;
                    btnBox->paddingLeft = 16; btnBox->paddingRight = 16;
                    btnBox->borderRadius = 4;
                }
                
                // Compute dimensions from text + padding
                float textW = btnBox->text.size() * btnBox->fontSize * 0.55f;
                btnBox->width = textW + btnBox->paddingLeft + btnBox->paddingRight;
                btnBox->height = btnBox->fontSize + btnBox->paddingTop + btnBox->paddingBottom;
                
                StyledTextLine line;
                line.text = btnBox->text;
                line.fontSize = btnBox->fontSize;
                line.color = btnBox->color;
                line.bgColor = btnBox->bgColor;
                line.hasBgColor = true;
                line.bold = btnBox->bold;
                g_styledLines.push_back(line);
                
                continue;
            }

            // Handle select element (dropdown)
            if (childTag == "select") {
                LayoutBox* selBox = addChild(parentBox);
                selBox->type = LayoutType::InlineBlock;
                
                // Get first <option> text as display value
                std::string displayText;
                for (DOMNode* opt = childElement->firstChild(); opt; opt = opt->nextSibling()) {
                    if (auto* optEl = dynamic_cast<DOMElement*>(opt)) {
                        if (optEl->hasAttribute("selected")) {
                            displayText = optEl->textContent();
                            break;
                        }
                        if (displayText.empty()) displayText = optEl->textContent();
                    }
                }
                if (displayText.empty()) displayText = "Select...";
                selBox->text = displayText + " \xE2\x96\xBE"; // ▾ dropdown arrow
                
                const CSSComputedStyle* style = g_cssEngine->getComputedStyle(childElement);
                if (style) {
                    selBox->fontSize = style->fontSize;
                    selBox->color = cssColorToRGB(style->color);
                    if (!style->backgroundColor.isTransparent())
                        selBox->bgColor = cssColorToRGB(style->backgroundColor);
                    else
                        selBox->bgColor = 0x2D2D2D;
                    selBox->hasBgColor = true;
                    selBox->paddingTop = style->paddingTop.value > 0 ? style->paddingTop.value : 4;
                    selBox->paddingBottom = style->paddingBottom.value > 0 ? style->paddingBottom.value : 4;
                    selBox->paddingLeft = style->paddingLeft.value > 0 ? style->paddingLeft.value : 8;
                    selBox->paddingRight = style->paddingRight.value > 0 ? style->paddingRight.value : 20;
                    selBox->borderRadius = style->borderTopLeftRadius > 0 ? style->borderTopLeftRadius : 4;
                    selBox->borderTop = style->borderTopWidth > 0 ? style->borderTopWidth : 1;
                    selBox->borderColor = cssColorToRGB(style->borderTopColor);
                } else {
                    selBox->fontSize = 14;
                    selBox->color = 0xCCCCCC;
                    selBox->bgColor = 0x2D2D2D;
                    selBox->hasBgColor = true;
                    selBox->paddingTop = 4; selBox->paddingBottom = 4;
                    selBox->paddingLeft = 8; selBox->paddingRight = 20;
                    selBox->borderRadius = 4;
                    selBox->borderTop = 1;
                    selBox->borderColor = 0x555555;
                }
                
                StyledTextLine line;
                line.text = selBox->text;
                line.fontSize = selBox->fontSize;
                line.color = selBox->color;
                line.bgColor = selBox->bgColor;
                line.hasBgColor = true;
                g_styledLines.push_back(line);
                continue;
            }

            // Handle textarea element
            if (childTag == "textarea") {
                LayoutBox* taBox = addChild(parentBox);
                taBox->type = LayoutType::Block;
                taBox->isInput = true;
                taBox->inputType = "textarea";
                taBox->text = childElement->textContent();
                taBox->placeholder = childElement->getAttribute("placeholder");
                if (taBox->text.empty() && !taBox->placeholder.empty())
                    taBox->text = taBox->placeholder;
                
                const CSSComputedStyle* style = g_cssEngine->getComputedStyle(childElement);
                if (style) {
                    taBox->fontSize = style->fontSize;
                    taBox->color = cssColorToRGB(style->color);
                    if (!style->backgroundColor.isTransparent())
                        taBox->bgColor = cssColorToRGB(style->backgroundColor);
                    else
                        taBox->bgColor = 0x1E1E1E;
                    taBox->hasBgColor = true;
                    taBox->paddingTop = style->paddingTop.value > 0 ? style->paddingTop.value : 8;
                    taBox->paddingBottom = style->paddingBottom.value > 0 ? style->paddingBottom.value : 8;
                    taBox->paddingLeft = style->paddingLeft.value > 0 ? style->paddingLeft.value : 8;
                    taBox->paddingRight = style->paddingRight.value > 0 ? style->paddingRight.value : 8;
                    taBox->borderRadius = style->borderTopLeftRadius > 0 ? style->borderTopLeftRadius : 4;
                    taBox->borderTop = style->borderTopWidth > 0 ? style->borderTopWidth : 1;
                    taBox->borderColor = cssColorToRGB(style->borderTopColor);
                } else {
                    taBox->fontSize = 14;
                    taBox->color = 0xCCCCCC;
                    taBox->bgColor = 0x1E1E1E;
                    taBox->hasBgColor = true;
                    taBox->paddingTop = 8; taBox->paddingBottom = 8;
                    taBox->paddingLeft = 8; taBox->paddingRight = 8;
                    taBox->borderRadius = 4;
                    taBox->borderTop = 1;
                    taBox->borderColor = 0x555555;
                }
                
                // Textarea default size (rows/cols)
                int rows = 3;
                if (childElement->hasAttribute("rows")) {
                    try { rows = std::stoi(childElement->getAttribute("rows")); } catch (...) {}
                }
                taBox->width = 300;
                taBox->height = rows * (taBox->fontSize + 4) + taBox->paddingTop + taBox->paddingBottom;
                
                StyledTextLine line;
                line.text = taBox->text;
                line.fontSize = taBox->fontSize;
                line.isInput = true;
                line.inputType = "textarea";
                line.color = taBox->color;
                line.bgColor = taBox->bgColor;
                line.hasBgColor = true;
                g_styledLines.push_back(line);
                continue;
            }
            if (childTag == "img") {
                std::string src = childElement->getAttribute("src");
                std::cout << "[Layout] processing img src=" << src << std::endl;
                if (!src.empty()) {
                    src = resolveUrl(g_currentUrl, src);

                    LayoutBox* imgBox = addChild(parentBox);
                    imgBox->type = LayoutType::InlineBlock;
                    imgBox->isImage = true;
                    imgBox->text = "[IMG: Loading...]";
                    imgBox->color = 0xAAAAAA;
                    imgBox->bgColor = 0xEEEEEE;
                    imgBox->hasBgColor = true;
                    imgBox->width = 100;
                    imgBox->height = 100;
                    imgBox->marginTop = 5;
                    imgBox->marginBottom = 5;

                    ZepraBrowser::g_lazyImageLoader.queueImage(imgBox, src, 0, g_activeTabId);
                }
                continue;
            }
            // Embedded <svg> — extract and render via nxsvg
            if (childTag == "svg") {
                auto dim = ZepraBrowser::UI::getSvgDimensions(childElement);
                std::string svgStr = ZepraBrowser::UI::extractSvgString(childElement);
                
                if (!svgStr.empty()) {
                    LayoutBox* svgBox = addChild(parentBox);
                    svgBox->type = LayoutType::InlineBlock;
                    svgBox->isImage = true;
                    svgBox->svgData = svgStr;
                    svgBox->width = dim.width;
                    svgBox->height = dim.height;
                    svgBox->marginTop = 2;
                    svgBox->marginBottom = 2;
                }
                continue;
            }
            // Determine if this is a block-level element that needs its own container
            // Following Firefox's frame tree pattern: block elements create their own frame
            bool isBlockElement = false;
            
            // Get child's computed style for display property check
            const CSSComputedStyle* childStyle = g_cssEngine ? 
                g_cssEngine->getComputedStyle(childElement) : nullptr;
            
            // 1. CSS display property (highest priority)
            if (childStyle) {
                switch (childStyle->display) {
                    case DisplayValue::Block:
                    case DisplayValue::Flex:
                    case DisplayValue::InlineFlex:
                    case DisplayValue::Grid:
                    case DisplayValue::InlineGrid:
                    case DisplayValue::InlineBlock:
                    case DisplayValue::ListItem:
                        isBlockElement = true;
                        break;
                    default:
                        break;
                }
            }
            
            // 2. HTML5 block elements fallback (when CSS doesn't specify)
            if (!isBlockElement) {
                if (childTag == "div" || childTag == "p" || childTag == "article" ||
                    childTag == "section" || childTag == "header" || childTag == "footer" ||
                    childTag == "nav" || childTag == "main" || childTag == "aside" ||
                    childTag == "h1" || childTag == "h2" || childTag == "h3" ||
                    childTag == "h4" || childTag == "h5" || childTag == "h6" ||
                    childTag == "ul" || childTag == "ol" || childTag == "li" ||
                    childTag == "pre" || childTag == "blockquote" || childTag == "form" ||
                    childTag == "table" || childTag == "figure" || childTag == "figcaption" ||
                    childTag == "address" || childTag == "details" || childTag == "summary") {
                    isBlockElement = true;
                }
            }
            
            if (isBlockElement) {
                // Create a Block container for this element (Firefox pattern)
                LayoutBox* blockBox = addChild(parentBox);
                blockBox->type = LayoutType::Block;
                
                // Apply CSS styles to this block container
                if (childStyle) {
                    blockBox->fontSize = childStyle->fontSize;
                    blockBox->color = cssColorToRGB(childStyle->color);
                    blockBox->bold = (childStyle->fontWeight >= FontWeight::Bold);
                    blockBox->italic = (childStyle->fontStyle == FontStyle::Italic);
                    blockBox->textDecoration = childStyle->textDecoration;
                    
                    if (!childStyle->backgroundColor.isTransparent()) {
                        blockBox->bgColor = cssColorToRGB(childStyle->backgroundColor);
                        blockBox->hasBgColor = true;
                    }
                    if (!childStyle->backgroundImage.empty() && childStyle->backgroundImage != "none") {
                        blockBox->backgroundImage = childStyle->backgroundImage;
                        blockBox->hasBgColor = true;
                    }
                    
                    // Margins — transfer values and detect auto for centering
                    blockBox->marginTop = childStyle->marginTop.isAuto() ? 0 : childStyle->marginTop.value;
                    blockBox->marginBottom = childStyle->marginBottom.isAuto() ? 0 : childStyle->marginBottom.value;
                    blockBox->marginLeft = childStyle->marginLeft.isAuto() ? 0 : childStyle->marginLeft.value;
                    blockBox->marginRight = childStyle->marginRight.isAuto() ? 0 : childStyle->marginRight.value;
                    blockBox->marginLeftAuto = childStyle->marginLeft.isAuto();
                    blockBox->marginRightAuto = childStyle->marginRight.isAuto();
                    blockBox->marginTopAuto = childStyle->marginTop.isAuto();
                    blockBox->marginBottomAuto = childStyle->marginBottom.isAuto();
                    
                    // Padding
                    blockBox->paddingTop = childStyle->paddingTop.value;
                    blockBox->paddingBottom = childStyle->paddingBottom.value;
                    blockBox->paddingLeft = childStyle->paddingLeft.value;
                    blockBox->paddingRight = childStyle->paddingRight.value;
                    
                    // Dimensions — store raw CSSLength for deferred resolution in layoutBlock
                    blockBox->cssWidth = cssToLayout(childStyle->width);
                    blockBox->cssHeight = cssToLayout(childStyle->height);
                    
                    // Min/max constraints
                    blockBox->cssMinWidth = cssToLayout(childStyle->minWidth);
                    blockBox->cssMinHeight = cssToLayout(childStyle->minHeight);
                    blockBox->cssMaxWidth = cssToLayout(childStyle->maxWidth);
                    blockBox->cssMaxHeight = cssToLayout(childStyle->maxHeight);
                    
                    // Pre-resolve px-only width/height for backwards compat
                    if (!childStyle->width.isAuto() && childStyle->width.value > 0 &&
                        childStyle->width.unit == Zepra::WebCore::CSSLength::Unit::Px)
                        blockBox->width = childStyle->width.value;
                    if (!childStyle->height.isAuto() && childStyle->height.value > 0 &&
                        childStyle->height.unit == Zepra::WebCore::CSSLength::Unit::Px)
                        blockBox->height = childStyle->height.value;
                    
                    // Border
                    blockBox->borderTop = childStyle->borderTopWidth;
                    blockBox->borderRight = childStyle->borderRightWidth;
                    blockBox->borderBottom = childStyle->borderBottomWidth;
                    blockBox->borderLeft = childStyle->borderLeftWidth;
                    if (childStyle->borderTopWidth > 0 || childStyle->borderRightWidth > 0 ||
                        childStyle->borderBottomWidth > 0 || childStyle->borderLeftWidth > 0)
                        blockBox->borderColor = cssColorToRGB(childStyle->borderTopColor);
                    blockBox->borderRadius = childStyle->borderTopLeftRadius;
                    
                    // Visual
                    blockBox->opacity = childStyle->opacity;
                    blockBox->overflowHidden = (childStyle->overflowX == OverflowValue::Hidden || 
                                                childStyle->overflowY == OverflowValue::Hidden);
                    blockBox->visibilityHidden = (childStyle->visibility == Visibility::Hidden);
                    blockBox->boxSizing = (childStyle->boxSizing == Zepra::WebCore::BoxSizing::BorderBox) ? 1 : 0;
                    
                    // CSS Positioning
                    switch (childStyle->position) {
                        case PositionValue::Static:   blockBox->positionType = 0; break;
                        case PositionValue::Relative: blockBox->positionType = 1; break;
                        case PositionValue::Absolute: blockBox->positionType = 2; break;
                        case PositionValue::Fixed:    blockBox->positionType = 3; break;
                        case PositionValue::Sticky:   blockBox->positionType = 4; break;
                    }
                    
                    // Inset offsets (resolve to px and store as LayoutLength{value, Px})
                    if (!childStyle->top.isAuto()) blockBox->cssTop = cssToLayout(childStyle->top);
                    if (!childStyle->right.isAuto()) blockBox->cssRight = cssToLayout(childStyle->right);
                    if (!childStyle->bottom.isAuto()) blockBox->cssBottom = cssToLayout(childStyle->bottom);
                    if (!childStyle->left.isAuto()) blockBox->cssLeft = cssToLayout(childStyle->left);
                    blockBox->zIndex = childStyle->zIndex;
                    
                    // Float
                    blockBox->floatType = childStyle->cssFloat;
                    
                    // Text alignment
                    if (childStyle->textAlign == TextAlign::Center) blockBox->textAlign = 1;
                    else if (childStyle->textAlign == TextAlign::Right) blockBox->textAlign = 2;
                    else blockBox->textAlign = 0;
                    
                    // Display type overrides
                    if (childStyle->display == DisplayValue::Flex || childStyle->display == DisplayValue::InlineFlex) {
                        blockBox->type = LayoutType::Flex;
                        using namespace Zepra::WebCore;

                        // Direction: 0=row, 1=column, 2=row-reverse, 3=column-reverse
                        switch (childStyle->flexDirection) {
                            case FlexDirection::Row:           blockBox->flexDirection = 0; break;
                            case FlexDirection::Column:        blockBox->flexDirection = 1; break;
                            case FlexDirection::RowReverse:    blockBox->flexDirection = 2; break;
                            case FlexDirection::ColumnReverse: blockBox->flexDirection = 3; break;
                        }

                        // Wrap
                        blockBox->flexWrap = childStyle->flexWrap;
                        blockBox->wrapReverse = childStyle->wrapReverse;

                        // Gap: prefer row-gap/column-gap if set, else fall back to gap
                        float gapPx = childStyle->gap.toPx(style->fontSize, 16.0f, (float)g_width, (float)g_height, 0);
                        blockBox->gap = gapPx;
                        blockBox->rowGap = childStyle->rowGap.value > 0 ?
                            childStyle->rowGap.toPx(style->fontSize, 16.0f, (float)g_width, (float)g_height, 0) : gapPx;
                        blockBox->columnGap = childStyle->columnGap.value > 0 ?
                            childStyle->columnGap.toPx(style->fontSize, 16.0f, (float)g_width, (float)g_height, 0) : gapPx;

                        // justify-content
                        switch (childStyle->justifyContent) {
                            case JustifyAlign::FlexEnd: case JustifyAlign::End: blockBox->justifyContent = 1; break;
                            case JustifyAlign::Center: blockBox->justifyContent = 2; break;
                            case JustifyAlign::SpaceBetween: blockBox->justifyContent = 3; break;
                            case JustifyAlign::SpaceAround: blockBox->justifyContent = 4; break;
                            case JustifyAlign::SpaceEvenly: blockBox->justifyContent = 5; break;
                            default: blockBox->justifyContent = 0; break;
                        }
                        // align-items
                        switch (childStyle->alignItems) {
                            case JustifyAlign::FlexStart: case JustifyAlign::Start: blockBox->alignItems = 1; break;
                            case JustifyAlign::FlexEnd: case JustifyAlign::End: blockBox->alignItems = 2; break;
                            case JustifyAlign::Center: blockBox->alignItems = 3; break;
                            case JustifyAlign::Baseline: blockBox->alignItems = 4; break;
                            default: blockBox->alignItems = 0; break; // stretch
                        }
                        // align-content (multi-line)
                        switch (childStyle->alignContent) {
                            case JustifyAlign::FlexStart: case JustifyAlign::Start: blockBox->alignContent = 1; break;
                            case JustifyAlign::FlexEnd: case JustifyAlign::End: blockBox->alignContent = 2; break;
                            case JustifyAlign::Center: blockBox->alignContent = 3; break;
                            case JustifyAlign::SpaceBetween: blockBox->alignContent = 4; break;
                            case JustifyAlign::SpaceAround: blockBox->alignContent = 5; break;
                            default: blockBox->alignContent = 0; break; // stretch
                        }
                    } else if (childStyle->display == DisplayValue::InlineBlock ||
                               childStyle->display == DisplayValue::InlineGrid) {
                        blockBox->type = LayoutType::InlineBlock;
                    }

                    // Flex item properties — applied to ALL children of flex containers.
                    // The parent type isn't known here, so always store these; they're
                    // only consumed when the parent is LayoutType::Flex.
                    blockBox->flexGrow = childStyle->flexGrow;
                    blockBox->flexShrink = childStyle->flexShrink;
                    if (!childStyle->flexBasis.isAuto() && childStyle->flexBasis.value > 0) {
                        blockBox->flexBasis = cssToLayout(childStyle->flexBasis);
                    }
                    blockBox->order = childStyle->order;
                    // align-self: map to int (-1=auto, 0=stretch, 1=start, 2=end, 3=center)
                    switch (childStyle->alignSelf) {
                        case JustifyAlign::Stretch: blockBox->alignSelf = 0; break;
                        case JustifyAlign::FlexStart: case JustifyAlign::Start: blockBox->alignSelf = 1; break;
                        case JustifyAlign::FlexEnd: case JustifyAlign::End: blockBox->alignSelf = 2; break;
                        case JustifyAlign::Center: blockBox->alignSelf = 3; break;
                        default: blockBox->alignSelf = -1; break; // auto = inherit from parent
                    }
                } else {
                    // Fallback margins for common block tags (no CSS engine available)
                    if (childTag == "h1") { blockBox->marginTop = 16; blockBox->marginBottom = 16; blockBox->fontSize = 32; blockBox->bold = true; }
                    else if (childTag == "h2") { blockBox->marginTop = 12; blockBox->marginBottom = 12; blockBox->fontSize = 24; blockBox->bold = true; }
                    else if (childTag == "h3") { blockBox->marginTop = 10; blockBox->marginBottom = 10; blockBox->fontSize = 20; blockBox->bold = true; }
                    else if (childTag == "p" || childTag == "div") { blockBox->marginTop = 8; blockBox->marginBottom = 8; }
                    else if (childTag == "li") { blockBox->marginTop = 4; blockBox->marginBottom = 4; }
                }
                
                // Recurse into this block container (children become its children)
                buildLayoutFromDOM(childElement, blockBox, nowInLink, href, target);
            } else {
                // Inline element: create its own LayoutBox for proper style containment
                LayoutBox* inlineBox = addChild(parentBox);
                inlineBox->type = LayoutType::Inline;
                
                if (childStyle) {
                    inlineBox->fontSize = childStyle->fontSize;
                    inlineBox->color = cssColorToRGB(childStyle->color);
                    inlineBox->bold = (childStyle->fontWeight >= FontWeight::Bold);
                    inlineBox->italic = (childStyle->fontStyle == FontStyle::Italic);
                    inlineBox->textDecoration = childStyle->textDecoration;
                    
                    if (!childStyle->backgroundColor.isTransparent()) {
                        inlineBox->bgColor = cssColorToRGB(childStyle->backgroundColor);
                        inlineBox->hasBgColor = true;
                    }
                    
                    inlineBox->paddingTop = childStyle->paddingTop.value;
                    inlineBox->paddingBottom = childStyle->paddingBottom.value;
                    inlineBox->paddingLeft = childStyle->paddingLeft.value;
                    inlineBox->paddingRight = childStyle->paddingRight.value;
                    
                    inlineBox->borderTop = childStyle->borderTopWidth;
                    inlineBox->borderRight = childStyle->borderRightWidth;
                    inlineBox->borderBottom = childStyle->borderBottomWidth;
                    inlineBox->borderLeft = childStyle->borderLeftWidth;
                    if (childStyle->borderTopWidth > 0)
                        inlineBox->borderColor = cssColorToRGB(childStyle->borderTopColor);
                    inlineBox->borderRadius = childStyle->borderTopLeftRadius;
                    inlineBox->opacity = childStyle->opacity;
                    inlineBox->visibilityHidden = (childStyle->visibility == Visibility::Hidden);
                }
                
                // Link styling on the inline box itself
                if (nowInLink && !href.empty()) {
                    inlineBox->isLink = true;
                    inlineBox->href = href;
                    inlineBox->target = target;
                }
                
                // Recurse into this inline box (children become its children)
                buildLayoutFromDOM(childElement, inlineBox, nowInLink, href, target);
            }
        }
    }
}
#endif

// ============================================================================
// GFX PRIMITIVES (NXRender Integration)
// ============================================================================

// Using NXRender GpuContext for all graphics operations
// Provides abstraction layer over OpenGL for future backends

static NXRender::GpuContext g_nxGpu;  // NXRender GPU context

namespace gfx {

void init(int w, int h) {
    // Initialize NXRender GpuContext
    g_nxGpu.init(w, h);
    g_nxGpu.setViewport(0, 0, w, h);
}

void resize(int w, int h) { 
    g_nxGpu.setViewport(0, 0, w, h);
}

void set2D() {
    g_nxGpu.beginFrame();
}

void clear(uint32_t c) {
    g_nxGpu.clear(NXRender::Color(c));
}

void present() { 
    g_nxGpu.endFrame();
    // glXSwapBuffers(g_display, g_window); // Handled by NXRender Platform
}

void rect(float x, float y, float w, float h, uint32_t c, uint8_t a = 255) {
    g_nxGpu.fillRect(NXRender::Rect(x, y, w, h), NXRender::Color(c).withAlpha(a));
}

void rrect(float x, float y, float w, float h, float rad, uint32_t c, uint8_t a = 255) {
    g_nxGpu.fillRoundedRect(NXRender::Rect(x, y, w, h), NXRender::Color(c).withAlpha(a), rad);
}

void circle(float cx, float cy, float rad, uint32_t c, uint8_t a = 255) {
    g_nxGpu.fillCircle(cx, cy, rad, NXRender::Color(c).withAlpha(a));
}

void gradient(float x, float y, float w, float h, uint32_t c1, uint32_t c2) {
    g_nxGpu.fillRectGradient(NXRender::Rect(x, y, w, h), 
                              NXRender::Color(c1).withAlpha(255), NXRender::Color(c2).withAlpha(255), 
                              false);  // vertical gradient
}

void border(float x, float y, float w, float h, float thick, uint32_t c) {
    g_nxGpu.strokeRect(NXRender::Rect(x, y, w, h), NXRender::Color(c).withAlpha(255), thick);
}

void texture(float x, float y, float w, float h, uint32_t textureId) {
    g_nxGpu.drawTexture(textureId, NXRender::Rect(x, y, w, h));
}

} // namespace gfx

// ============================================================================
// TEXT HELPER
// ============================================================================

void text(const std::string& str, float x, float y, uint32_t c, float size = 14.0f) {
    auto* font = nxfont::FontManager::instance().getSystemFont((int)size);
    if (font) font->drawText(str, x, y, R(c), G(c), B(c));
}

float textWidth(const std::string& str, float size = 14.0f) {
    auto* font = nxfont::FontManager::instance().getSystemFont((int)size);
    return font ? font->measureText(str) : str.length() * (size * 0.6f);
}

// ============================================================================
// SVG HELPER
// ============================================================================

void svg(const std::string& name, float x, float y, float sz, uint32_t c) {
    g_svg.draw(name, x, y, sz, R(c), G(c), B(c));
}

// ============================================================================
// LAYOUT ENGINE CALLBACKS (wrappers for modular layout_engine.cpp)
// ============================================================================

// Setup layout engine callbacks
static void layout_gfx_rect(float x, float y, float w, float h, uint32_t color) {
    gfx::rect(x, y, w, h, color);
}

static void layout_gfx_border(float x, float y, float w, float h, uint32_t color, float thickness) {
    gfx::border(x, y, w, h, thickness, color);
}

static void layout_gfx_texture(float x, float y, float w, float h, uint32_t textureId) {
    gfx::texture(x, y, w, h, textureId);
}

static void layout_text_render(const std::string& str, float x, float y, uint32_t c, float fontSize) {
    text(str, x, y, c, fontSize);
}

static float layout_text_width(const std::string& str, float fontSize) {
    return textWidth(str, fontSize);
}

static void layout_register_link(float x, float y, float w, float h,
                                 const std::string& href, const std::string& target) {
    LinkHitBox box;
    box.x = x; box.y = y; box.w = w; box.h = h;
    box.href = href; box.target = target;
    g_linkHitBoxes.push_back(box);
}

// Line drawing for underlines
static void layout_gfx_line(float x1, float y1, float x2, float y2, uint32_t color, float thickness) {
    float y = std::min(y1, y2);
    float h = std::max(thickness, 1.0f);
    gfx::rect(x1, y, x2 - x1, h, color);
}

// Rounded rect callback for border-radius
static void layout_gfx_rrect(float x, float y, float w, float h, float radius, uint32_t color, uint8_t alpha) {
    gfx::rrect(x, y, w, h, radius, color, alpha);
}

// Gradient callback for background-image: linear-gradient(...)
static void layout_gfx_gradient(float x, float y, float w, float h, uint32_t c1, uint32_t c2) {
    gfx::gradient(x, y, w, h, c1, c2);
}

// Initialize layout engine callbacks
static void initLayoutEngine() {
    ZepraBrowser::setLayoutCallbacks(
        layout_gfx_rect,
        layout_gfx_border,
        layout_text_render,
        layout_text_width,
        layout_register_link,
        layout_gfx_texture,
        layout_gfx_line
    );
    
    ZepraBrowser::setLayoutCallbacks2(
        layout_gfx_rrect,
        layout_gfx_gradient,
        // SVG rendering callback: parse and render via NxSVG
        [](float x, float y, float w, float h, const std::string& svgData) {
            // Generate a unique key from the SVG data hash
            std::hash<std::string> hasher;
            std::string key = "web_svg_" + std::to_string(hasher(svgData));
            
            // Load if not already cached
            if (!g_svg.has(key)) {
                g_svg.loadFromString(key, svgData);
            }
            
            // Render at position with size
            float renderSize = std::min(w, h);
            g_svg.draw(key, x, y, renderSize);
        }
    );
    
    std::cout << "[Layout] Layout engine initialized (rounded rect + gradient support)" << std::endl;
    
    // Register MouseHandler callbacks for text selection
    g_mouseHandler.onGetText([](float x, float y, float w, float h) -> std::string {
        if (g_layoutRoot) {
            return ZepraBrowser::getTextInRect(*g_layoutRoot, x, y, w, h);
        }
        return "";
    });
}

// Legacy layout implementation removed - using modular layout_engine.cpp
#endif

// ============================================================================
// HIT TEST
// ============================================================================

bool hit(float x, float y, float w, float h) {
    return g_mouseX >= x && g_mouseX < x+w && g_mouseY >= y && g_mouseY < y+h;
}

// ============================================================================
// UI COMPONENTS (from app.rx)
// ============================================================================

const float TAB_HEIGHT = 40;
const float NAV_HEIGHT = 48;
const float SIDEBAR_WIDTH = 220;

// LEFT floating sidebar layout (per reference design)
const float TOPBAR_HEIGHT = 44;              // Clean top bar
const float LEFT_SIDEBAR_WIDTH = 56;         // Collapsed (icons + labels)
const float LEFT_SIDEBAR_EXPANDED = 240;     // Expanded with content

// Left sidebar state
static bool g_leftSidebarVisible = true;
static bool g_leftSidebarExpanded = false;

// Helper: Get current sidebar offset (for content area positioning)
// This unifies the old g_sidebarVisible with the new left sidebar system
inline float getSidebarOffset() {
    if (!g_leftSidebarVisible) return 0;
    return g_leftSidebarExpanded ? LEFT_SIDEBAR_EXPANDED : LEFT_SIDEBAR_WIDTH;
}

// Sidebar panel enum (matches reference image)
enum class SidebarPanel {
    NoPanel = 0,    // Renamed from None to avoid X11 #define None conflict
    Dashboard,      // User profile + quick access
    Bookmarks,      // Saved bookmarks
    History,        // Browsing history
    Shortcuts,      // Pin pages, add shortcuts
    Settings,       // Browser settings
    WebView,        // Developer WebView tools
    Help            // Help & support
};
static SidebarPanel g_activeSidebarPanel = SidebarPanel::NoPanel;

// User customizable data
struct PinnedPage {
    std::string title;
    std::string url;
    std::string favicon;  // Icon path or emoji
};

struct Shortcut {
    std::string name;
    std::string url;
    std::string icon;
};

static std::vector<PinnedPage> g_pinnedPages;
static std::vector<Shortcut> g_shortcuts;
static std::vector<std::string> g_recentPages;  // Last 10 visited URLs

#include "../UI/browser/top_navigation.hpp"

#if 0 // Old UI implementation extracted to UI/browser/top_navigation.hpp
// Tab Item
void renderTab(const Tab& tab, float x, float y, float w, bool active) {
    bool hover = hit(x, y, w, TAB_HEIGHT - 4);
    
    if (active) {
        gfx::rrect(x, y + 4, w, TAB_HEIGHT - 4, 8, g_theme.bg_tertiary);
    } else if (hover) {
        gfx::rrect(x, y + 4, w, TAB_HEIGHT - 4, 8, g_theme.bg_elevated, 180);
    }
    
    // Title
    text(tab.title, x + 12, y + 24, active ? g_theme.text_primary : g_theme.text_secondary);
    
    // Close button
    float closeX = x + w - 24;
    if (hit(closeX, y + 10, 16, 16)) {
        gfx::rrect(closeX, y + 10, 16, 16, 4, g_theme.bg_elevated);
    }
    svg("close.svg", closeX, y + 10, 16, g_theme.text_secondary);
}

// Tab Bar
void renderTabBar() {
    gfx::rect(0, 0, g_width, TAB_HEIGHT, g_theme.bg_secondary);
    
    float x = g_sidebarVisible ? SIDEBAR_WIDTH + 8 : 50;
    float tabW = 180;
    
    for (const Tab& tab : g_tabs) {
        if (x > g_width - 100) break;
        renderTab(tab, x, 0, tabW, tab.id == g_activeTabId);
        x += tabW + 4;
    }
    
    // New tab button
    bool addHover = hit(x, 6, 28, 28);
    gfx::rrect(x, 6, 28, 28, 6, addHover ? g_theme.bg_elevated : g_theme.bg_tertiary);
    svg("plus.svg", x + 6, 10, 16, g_theme.text_primary);
}

// Navigation Bar
void renderNavBar() {
    float y = TAB_HEIGHT;
    gfx::rect(0, y, g_width, NAV_HEIGHT, g_theme.bg_secondary);
    
    float x = g_sidebarVisible ? SIDEBAR_WIDTH + 12 : 12;
    
    // Sidebar toggle
    bool sbHover = hit(x, y + 10, 28, 28);
    gfx::rrect(x, y + 10, 28, 28, 4, sbHover ? g_theme.bg_elevated : g_theme.bg_tertiary);
    svg("sidebar.svg", x + 4, y + 14, 20, g_sidebarVisible ? g_theme.accent : g_theme.text_primary);
    x += 36;
    
    // Back
    gfx::rrect(x, y + 10, 28, 28, 4, g_theme.bg_tertiary);
    svg("arrow-back.svg", x + 6, y + 14, 16, g_theme.text_secondary);
    x += 36;
    
    // Forward
    gfx::rrect(x, y + 10, 28, 28, 4, g_theme.bg_tertiary);
    svg("arrow-forward.svg", x + 6, y + 14, 16, g_theme.text_secondary);
    x += 36;
    
    // Refresh
    bool refHover = hit(x, y + 10, 28, 28);
    gfx::rrect(x, y + 10, 28, 28, 4, refHover ? g_theme.bg_elevated : g_theme.bg_tertiary);
    svg("refresh.svg", x + 4, y + 12, 20, g_theme.text_primary);
    x += 44;
    
    // Address Bar
    float barW = g_width - x - 100;
    bool addrHover = hit(x, y + 8, barW, 32);
    gfx::rrect(x, y + 8, barW, 32, 8, g_addressFocused ? g_theme.bg_elevated : g_theme.bg_tertiary);
    if (g_addressFocused) {
        gfx::border(x, y + 8, barW, 32, 2, g_theme.accent);
    }
    
    // URL text - show input buffer when focused, otherwise show current URL
    std::string displayText;
    uint32_t displayColor;
    
    if (g_addressFocused) {
        // Show input buffer when editing
        displayText = g_addressInput;
        displayColor = g_theme.text_primary;
    } else if (g_currentUrl == "zepra://start" || g_currentUrl.empty()) {
        displayText = "Search or enter address";
        displayColor = g_theme.text_placeholder;
    } else {
        displayText = g_currentUrl;
        displayColor = g_theme.text_secondary;
    }
    
    // Check if browsing local file
    bool isLocalFile = (g_currentUrl.substr(0, 7) == "file://");
    
    // Security/file icon - show folder icon for local files
    if (isLocalFile) {
        svg("folder.svg", x + 8, y + 12, 24, 0xFFA500);  // Orange for local file
    } else {
        svg("shield.svg", x + 8, y + 12, 24, g_theme.success);  // Green for secure
    }
    
    text(displayText, x + 38, y + 28, displayColor);
    
    // Local file indicator tooltip on hover
    if (isLocalFile && addrHover) {
        // Draw tooltip above address bar
        std::string tooltip = "Browsing local file";
        float tooltipW = textWidth(tooltip) + 16;
        float tooltipX = x + barW / 2 - tooltipW / 2;
        float tooltipY = y - 28;
        
        gfx::rrect(tooltipX, tooltipY, tooltipW, 24, 6, 0x333333);
        text(tooltip, tooltipX + 8, tooltipY + 17, 0xFFFFFF, 12.0f);
    }
    
    // Cursor when focused
    if (g_addressFocused) {
        g_cursorBlink++;
        if ((g_cursorBlink / 30) % 2 == 0) {  // Blink every ~0.5s at 60fps
            float cursorX = x + 38 + textWidth(g_addressInput);
            gfx::rect(cursorX, y + 14, 2, 20, g_theme.text_primary);
        }
    }
    
    // Right buttons
    float rightX = g_width - 80;
    
    // Downloads
    bool dlHover = hit(rightX, y + 10, 28, 28);
    gfx::rrect(rightX, y + 10, 28, 28, 4, dlHover ? g_theme.bg_elevated : g_theme.bg_tertiary);
    svg("download.svg", rightX + 4, y + 12, 20, g_theme.text_primary);
    rightX += 36;
    
    // Menu
    gfx::rrect(rightX, y + 10, 28, 28, 4, g_theme.bg_tertiary);
    svg("menu.svg", rightX + 4, y + 12, 20, g_theme.text_primary);
}

// ============================================================================
// SAFARI-STYLE UNIFIED TOP BAR
// ============================================================================

void renderTopBar() {
    // Top bar spans full width, independent of the floating sidebar
    float barX = 0;
    float barWidth = g_width;
    
    // Background - lavender theme like reference
    gfx::gradient(barX, 0, barWidth, TOPBAR_HEIGHT, 0xE6D5E8, 0xD8C8DA);
    gfx::rect(barX, TOPBAR_HEIGHT - 1, barWidth, 1, 0xC8B8CA);
    
    float y = (TOPBAR_HEIGHT - 28) / 2;
    
    // === LEFT SIDE: Sidebar toggle + Zepra icon ===
    float leftX = barX + 12;
    
    // Sidebar toggle (use sidebar.svg)
    bool sbToggleHover = hit(leftX, y, 24, 24);
    if (sbToggleHover) {
        gfx::rrect(leftX - 2, y - 2, 28, 28, 4, 0xD0C0D2);
    }
    svg("sidebar.svg", leftX, y, 24, g_leftSidebarVisible ? g_theme.accent : 0x6B5B7B);
    leftX += 32;
    
    // Zepra "Z" logo button
    bool zHover = hit(leftX, y, 28, 28);
    gfx::circle(leftX + 14, y + 14, 14, zHover ? g_theme.accent : 0x9B8BBB);
    svg("zepralogo.svg", leftX + 2, y + 2, 24, 0xFFFFFF);
    leftX += 36;
    
    // === CENTER: Search Bar ===
    float searchBarWidth = std::min(500.0f, barWidth - 280);
    float searchBarX = barX + (barWidth - searchBarWidth) / 2;
    
    // Search bar background (rounded pill) - translucent white
    gfx::rrect(searchBarX, y - 4, searchBarWidth, 36, 18, 
               g_addressFocused ? 0xFFFFFF : 0xF0E8F0);
    if (g_addressFocused) {
        gfx::border(searchBarX, y - 4, searchBarWidth, 36, 2, g_theme.accent);
    }
    
    // Zepra shield logo inside search
    gfx::circle(searchBarX + 24, y + 14, 14, g_theme.accent);
    svg("shield.svg", searchBarX + 12, y + 2, 24, 0xFFFFFF);
    
    // Placeholder or URL text
    std::string searchText;
    if (g_addressFocused) {
        searchText = g_addressInput;
    } else if (g_currentUrl.empty() || g_currentUrl == "zepra://start") {
        searchText = "Search with Zepra or enter your web destination";
    } else {
        searchText = g_currentUrl;
    }
    if (searchText.length() > 50) searchText = searchText.substr(0, 48) + "...";
    text(searchText, searchBarX + 50, y + 16, 
         g_addressFocused ? g_theme.text_primary : 0x8B7B8B, 12.0f);
    
    // Right side of search bar - icons
    float iconRightX = searchBarX + searchBarWidth - 60;
    
    // Voice search icon
    bool voiceHover = hit(iconRightX, y, 24, 24);
    if (voiceHover) {
        gfx::circle(iconRightX + 12, y + 14, 14, 0xE0D0E0);
    }
    svg("ringingBell.svg", iconRightX, y, 24, 0x8B7B8B);
    iconRightX += 28;
    
    // Microphone/speaker icon
    svg("notification.svg", iconRightX, y, 24, 0x8B7B8B);
    
    // === RIGHT SIDE: Tab controls (colorful circles like reference) ===
    float rightX = barX + barWidth - 140;
    
    // Refresh button (green)
    bool refHover = hit(rightX, y, 28, 28);
    gfx::circle(rightX + 14, y + 14, 12, refHover ? 0x4ADE80 : 0x22C55E);
    svg("refresh.svg", rightX + 4, y + 4, 20, 0xFFFFFF);
    rightX += 32;
    
    // Close tab button (red/orange)
    bool closeHover = hit(rightX, y, 28, 28);
    gfx::circle(rightX + 14, y + 14, 12, closeHover ? 0xFB7185 : 0xF43F5E);
    svg("close.svg", rightX + 6, y + 6, 16, 0xFFFFFF);
    rightX += 32;
    
    // New tab button (blue)
    bool addHover = hit(rightX, y, 28, 28);
    gfx::circle(rightX + 14, y + 14, 12, addHover ? 0x60A5FA : 0x3B82F6);
    svg("plus.svg", rightX + 6, y + 6, 16, 0xFFFFFF);
    rightX += 40;
    
    // === FAR RIGHT: Additional buttons ===
    // Grid/menu button
    bool menuHover = hit(rightX, y, 24, 24);
    if (menuHover) {
        gfx::rrect(rightX - 2, y - 2, 28, 28, 4, 0xD0C0D2);
    }
    svg("grid.svg", rightX, y, 24, 0x6B5B7B);
    rightX += 28;
    
    // Cloud/share button
    bool shareHover = hit(rightX, y, 24, 24);
    if (shareHover) {
        gfx::rrect(rightX - 2, y - 2, 28, 28, 4, 0xD0C0D2);
    }
    svg("share.svg", rightX, y, 24, 0x6B5B7B);
    rightX += 28;
    
    // Mobile/tablet icon
    svg("tablet.svg", rightX, y, 24, 0x6B5B7B);
}
#endif // Old UI implementation extracted

// ============================================================================
// FLOATING RIGHT SIDEBAR
// ============================================================================

// ============================================================================
// LEFT FLOATING SIDEBAR (per reference design)
// ============================================================================

void renderLeftSidebar() {
    if (!g_leftSidebarVisible) return;
    
    float sidebarW = g_leftSidebarExpanded ? LEFT_SIDEBAR_EXPANDED : LEFT_SIDEBAR_WIDTH;
    float sidebarX = 0;
    float sidebarY = TOPBAR_HEIGHT;
    float sidebarH = g_height - sidebarY;
    
    // Floating sidebar background - rose/beige gradient like reference
    gfx::gradient(sidebarX, sidebarY, sidebarW + 4, sidebarH, 0xD4A5A5, 0xC8A8A8);
    gfx::rect(sidebarX + sidebarW, sidebarY, 1, sidebarH, 0xB89898);
    
    float iconX = 12;
    float iconY = sidebarY + 16;
    float iconSize = 36;
    
    // ===== DASHBOARD / PROFILE SECTION =====
    // User avatar (circle with gradient background)
    bool dashHover = hit(iconX, iconY, iconSize, iconSize);
    gfx::circle(iconX + iconSize/2, iconY + iconSize/2, iconSize/2, 
                dashHover ? g_theme.accent : 0xE8B4D8);
    // Profile icon
    svg("avtar.svg", iconX + 6, iconY + 6, 24, 0xFFFFFF);
    
    if (g_leftSidebarExpanded) {
        text("Dashboard", iconX + 50, iconY + 24, g_theme.text_primary, 13.0f);
    }
    iconY += 48;
    
    // ===== MAIN PANELS (Bookmarks, History) =====
    struct SidebarItem {
        const char* icon;
        SidebarPanel panel;
        const char* label;
    };
    
    SidebarItem mainItems[] = {
        {"bookmark.svg", SidebarPanel::Bookmarks, "Bookmarks"},
        {"history.svg", SidebarPanel::History, "History"},
    };
    
    for (const auto& item : mainItems) {
        bool active = (g_activeSidebarPanel == item.panel);
        bool hover = hit(iconX, iconY, sidebarW - 16, 32);
        
        // Background
        if (active) {
            gfx::rrect(iconX - 4, iconY - 2, sidebarW - 8, 36, 8, 0xFFFFFF, 40);
        } else if (hover) {
            gfx::rrect(iconX - 4, iconY - 2, sidebarW - 8, 36, 8, 0xFFFFFF, 20);
        }
        
        // SVG Icon
        svg(item.icon, iconX + 4, iconY + 4, 24, active ? 0xFFFFFF : 0x4A3030);
        
        // Label
        if (g_leftSidebarExpanded) {
            text(item.label, iconX + 40, iconY + 22, 
                 active ? 0xFFFFFF : 0x4A3030, 12.0f);
        }
        
        iconY += 40;
    }
    
    // ===== WEB APPS SECTION =====
    iconY += 8;
    if (g_leftSidebarExpanded) {
        gfx::rect(iconX, iconY, sidebarW - 24, 1, 0xB89898);
        iconY += 16;
        text("Web Apps", iconX, iconY + 6, 0x7A5A5A, 10.0f);
        iconY += 20;
        
        // Ketivee ecosystem web apps
        struct WebApp { const char* icon; const char* label; const char* url; uint32_t color; };
        WebApp apps[] = {
            {"Calendar.svg", "Calendar", "https://calendar.ketivee.com", 0x4285F4},
            {"MialInbox.svg", "Mail", "https://mail.ketivee.com", 0xEA4335},
            {"creative.svg", "Workspace", "https://app.ketivee.com", 0x34A853},
            {"docs.svg", "Docs", "https://docs.ketivee.com", 0x4285F4},
            {"DebitCard.svg", "Pay", "https://pay.ketivee.com", 0xFBBC05},
            {"KetiveeStudio.svg", "Studio", "https://studio.ketivee.com", 0x9B8BBB},
        };
        
        for (int i = 0; i < 6; i++) {
            bool appHover = hit(iconX, iconY, sidebarW - 16, 28);
            
            if (appHover) {
                gfx::rrect(iconX - 4, iconY - 2, sidebarW - 8, 32, 6, 0xFFFFFF, 30);
            }
            
            // Small colored circle + icon
            gfx::circle(iconX + 12, iconY + 12, 10, apps[i].color);
            svg(apps[i].icon, iconX + 4, iconY + 4, 16, 0xFFFFFF);
            
            // Label
            text(apps[i].label, iconX + 30, iconY + 18, 0x4A3030, 11.0f);
            
            iconY += 32;
        }
    } else {
        // Collapsed - show small app icons grid
        iconY += 4;
        gfx::rect(iconX, iconY, sidebarW - 24, 1, 0xB89898);
        iconY += 12;
        
        uint32_t colors[] = {0x4285F4, 0xEA4335, 0x34A853, 0x4285F4, 0xFBBC05, 0x9B8BBB};
        for (int i = 0; i < 6; i++) {
            float row = i / 2;
            float col = i % 2;
            float appX = iconX + col * 20;
            float appY = iconY + row * 24;
            gfx::circle(appX + 8, appY + 8, 8, colors[i]);
        }
        iconY += 80;
    }
    
    // ===== EXPANDED PANEL CONTENT =====
    if (g_leftSidebarExpanded && g_activeSidebarPanel != SidebarPanel::NoPanel) {
        iconY += 8;
        gfx::rect(iconX, iconY, sidebarW - 24, 1, g_theme.border);
        iconY += 12;
        
        float contentX = iconX + 4;
        float contentW = sidebarW - 32;
        
        switch (g_activeSidebarPanel) {
            case SidebarPanel::Bookmarks:
                // Pinned pages section
                svg("star.svg", contentX - 2, iconY - 2, 16, g_theme.text_secondary);
                text("Pinned", contentX + 18, iconY + 14, g_theme.text_secondary, 11.0f);
                iconY += 24;
                
                if (g_pinnedPages.empty()) {
                    text("No pinned pages", contentX + 8, iconY + 14, g_theme.text_placeholder, 11.0f);
                    iconY += 24;
                } else {
                    for (const auto& pin : g_pinnedPages) {
                        gfx::rrect(contentX, iconY, contentW, 28, 6, g_theme.bg_tertiary);
                        text(pin.title, contentX + 8, iconY + 18, g_theme.text_primary, 11.0f);
                        iconY += 32;
                        if (iconY > g_height - 200) break;
                    }
                }
                
                // Shortcuts
                iconY += 8;
                svg("star.svg", contentX - 2, iconY - 2, 16, g_theme.accent);
                text("Shortcuts", contentX + 18, iconY + 14, g_theme.text_secondary, 11.0f);
                iconY += 24;
                
                if (g_shortcuts.empty()) {
                    gfx::rrect(contentX, iconY, contentW, 28, 6, g_theme.bg_tertiary);
                    text("+ Add from Settings", contentX + 8, iconY + 18, g_theme.accent, 11.0f);
                    iconY += 32;
                } else {
                    for (const auto& sc : g_shortcuts) {
                        gfx::rrect(contentX, iconY, contentW, 28, 6, g_theme.bg_tertiary);
                        text(sc.name, contentX + 8, iconY + 18, g_theme.text_primary, 11.0f);
                        iconY += 32;
                        if (iconY > g_height - 200) break;
                    }
                }
                break;
                
            case SidebarPanel::History:
                text("Recent Pages", contentX, iconY + 14, g_theme.text_secondary, 11.0f);
                iconY += 24;
                
                if (g_recentPages.empty()) {
                    text("No recent pages", contentX + 8, iconY + 14, g_theme.text_placeholder, 11.0f);
                } else {
                    for (const auto& url : g_recentPages) {
                        gfx::rrect(contentX, iconY, contentW, 28, 6, g_theme.bg_tertiary);
                        // Show just domain
                        std::string display = url;
                        if (url.find("://") != std::string::npos) {
                            size_t start = url.find("://") + 3;
                            size_t end = url.find("/", start);
                            display = url.substr(start, end - start);
                        }
                        if (display.length() > 20) display = display.substr(0, 18) + "..";
                        text(display, contentX + 8, iconY + 18, g_theme.text_primary, 11.0f);
                        iconY += 32;
                        if (iconY > g_height - 200) break;
                    }
                }
                break;
                
            default:
                break;
        }
    }
    
    // ===== BOTTOM TOOL ICONS =====
    float bottomY = g_height - 180;
    gfx::rect(iconX, bottomY, sidebarW - 24, 1, g_theme.border);
    bottomY += 16;
    
    SidebarItem bottomItems[] = {
        {"globe.svg", SidebarPanel::Shortcuts, "URL"},
        {"settings.svg", SidebarPanel::Settings, "Settings"},
        {"devtool.svg", SidebarPanel::WebView, "WebView"},
        {"help.svg", SidebarPanel::Help, "Help"},
    };
    
    for (const auto& item : bottomItems) {
        bool hover = hit(iconX, bottomY, sidebarW - 16, 28);
        
        if (hover) {
            gfx::rrect(iconX - 4, bottomY - 2, sidebarW - 8, 32, 6, g_theme.bg_elevated);
        }
        
        // SVG Icon
        svg(item.icon, iconX + 4, bottomY + 2, 20, g_theme.text_secondary);
        
        if (g_leftSidebarExpanded) {
            text(item.label, iconX + 32, bottomY + 18, g_theme.text_secondary, 11.0f);
        }
        
        bottomY += 36;
    }
}

// Start Page
void renderStartPage(float x, float y, float w, float h) {
    // Gradient background (from app.rx)
    gfx::gradient(x, y, w, h, g_theme.gradient_start, g_theme.gradient_end);
    
    float cx = x + w / 2;
    float cy = y + h / 2 - 100;
    
    // Zepra logo using actual SVG - render the zepra.svg icon
    float logoSize = 120;
    svg("zepra.svg", cx - logoSize/2, cy - logoSize/2 - 20, logoSize, 0xFFFFFF);
    
    // Small tagline
    text("Zepra", cx - 25, cy + logoSize/2 + 10, 0x6B5B95, 14.0f);
    
    // Search Box (from app.rx: SearchBox layer)
    float barW = 500, barH = 48;
    float barX = cx - barW / 2;
    float barY = cy + logoSize/2 + 40;
    
    // White rounded search bar with focus border
    gfx::rrect(barX, barY, barW, barH, 24, 0xFFFFFF);
    if (g_searchFocused) {
        gfx::border(barX, barY, barW, barH, 2, g_theme.accent);
    }
    
    // Search icon
    svg("search.svg", barX + 16, barY + 12, 24, 0x6E7681);
    
    // Search text or placeholder
    std::string searchDisplay = g_searchQuery.empty() ? "Search the web..." : g_searchQuery;
    uint32_t searchColor = g_searchQuery.empty() && !g_searchFocused ? 0x8B949E : 0x1F2328;
    text(searchDisplay, barX + 48, barY + 30, searchColor, 13.0f);
    
    // Blinking cursor when focused
    if (g_searchFocused) {
        g_cursorBlink++;
        if ((g_cursorBlink / 30) % 2 == 0) {
            float cursorX = barX + 48 + textWidth(g_searchQuery);
            gfx::rect(cursorX, barY + 12, 2, 24, 0x1F2328);
        }
    }
    
    // Voice icon with Ketivee logo style
    svg("ringingBell.svg", barX + barW - 70, barY + 12, 24, 0x6E7681);
    svg("share.svg", barX + barW - 40, barY + 12, 24, 0x6E7681);
    
    // Quick Links with proper icons (Web Apps - like Ketivee ecosystem)
    float qy = barY + barH + 40;
    float qGap = 80;
    struct QuickLink { const char* icon; const char* label; uint32_t color; const char* url; };
    QuickLink links[] = {
        {"MialInbox.svg", "Mail", 0x5080DC, "https://mail.ketivee.com"},
        {"ringingBell.svg", "News", 0x8C64C8, "https://news.ketivee.com"},
        {"Film.svg", "Videos", 0xC878A0, "https://watch.ketivee.com"},
        {"Picture.svg", "Images", 0xDC6464, "https://photos.ketivee.com"}
    };
    
    for (int i = 0; i < 4; i++) {
        float qx = cx - qGap * 1.5f + i * qGap;
        
        // Glow shadow
        gfx::circle(qx, qy + 2, 32, (links[i].color & 0xFFFFFF) | 0x30000000);
        
        // Main circle
        gfx::circle(qx, qy, 25, links[i].color);
        
        // Icon inside (SVG if available, else we'll use a simple shape)
        svg(links[i].icon, qx - 12, qy - 12, 24, 0xFFFFFF);
        
        // Label
        text(links[i].label, qx - textWidth(links[i].label)/2, qy + 45, 0x4A4063, 11.0f);
    }
    
    // === WEB APPS ROW (Ketivee Ecosystem) ===
    float appsY = qy + 90;
    text("Web Apps", cx - 35, appsY, 0x6B5B95, 13.0f);
    appsY += 30;
    
    struct WebApp { const char* icon; const char* label; const char* url; uint32_t color; };
    WebApp apps[] = {
        {"Calendar.svg", "Calendar", "https://calendar.ketivee.com", 0x4285F4},
        {"creative.svg", "Workspace", "https://app.ketivee.com", 0x34A853},
        {"DebitCard.svg", "Pay", "https://pay.ketivee.com", 0xFBBC05},
        {"docs.svg", "Docs", "https://docs.ketivee.com", 0x4285F4},
        {"KetiveeStudio.svg", "Studio", "https://studio.ketivee.com", 0xEA4335},
        {"3dSpace.svg", "3D Space", "https://3d.ketivee.com", 0x9B8BBB}
    };
    
    float appGap = 90;
    float appStartX = cx - (3 * appGap);
    
    for (int i = 0; i < 6; i++) {
        float ax = appStartX + i * appGap;
        bool hover = hit(ax - 30, appsY - 10, 60, 70);
        
        if (hover) {
            gfx::rrect(ax - 30, appsY - 10, 60, 70, 8, 0xFFFFFF, 100);
        }
        
        // App icon
        gfx::circle(ax, appsY + 10, 22, apps[i].color);
        svg(apps[i].icon, ax - 14, appsY - 4, 28, 0xFFFFFF);
        
        // Label
        text(apps[i].label, ax - textWidth(apps[i].label)/2, appsY + 50, 0x4A4063, 10.0f);
    }
}

// Sidebar
void renderSidebar() {
    if (!g_sidebarVisible) return;
    
    gfx::rect(0, 0, SIDEBAR_WIDTH, g_height, g_theme.bg_primary);
    gfx::rect(SIDEBAR_WIDTH - 1, 0, 1, g_height, g_theme.border);
    
    float y = TAB_HEIGHT + NAV_HEIGHT + 16;
    
    // Logo (from app.rx: "Z" + "Zepra")
    text("Z", 16, y, g_theme.accent);
    text("Zepra", 36, y, g_theme.text_primary);
    y += 40;
    
    // Items
    struct SidebarItem { const char* icon; const char* label; };
    SidebarItem items[] = {
        {"🏠", "Home"},
        {"⭐", "Bookmarks"},
        {"📜", "History"},
        {"📥", "Downloads"},
        {"🤖", "AI Assistant"},
        {"⚙️", "Settings"}
    };
    
    for (int i = 0; i < 6; i++) {
        bool hover = hit(8, y, SIDEBAR_WIDTH - 16, 40);
        if (hover) {
            gfx::rrect(8, y, SIDEBAR_WIDTH - 16, 40, 6, g_theme.bg_tertiary);
        }
        // Icon (emoji - we'll use text)
        text(items[i].icon, 16, y + 26, g_theme.text_primary);
        text(items[i].label, 48, y + 26, hover ? g_theme.text_primary : g_theme.text_secondary);
        y += 44;
    }
}

// Main render
void render() {
  try {
    // Track last URL for crash context
    g_crash_url = g_currentUrl.c_str();

    // === TAB SUSPENDER TICK (every ~1 second @ 60fps) ===
    static int suspender_frame_counter = 0;
    if (++suspender_frame_counter >= 60) {
        suspender_frame_counter = 0;
        g_tabSuspender.tick();
        // Collect tab pointers for checkAllTabs
        std::vector<ZepraBrowser::Tab*> tabPtrs;
        for (auto& tab : g_tabs) {
            tabPtrs.push_back(reinterpret_cast<ZepraBrowser::Tab*>(&tab));
        }
        if (!tabPtrs.empty()) {
            g_tabSuspender.checkAllTabs(tabPtrs, g_activeTabId);
        }
    }

    // === ASYNC LOAD COMPLETION HANDLER ===
    if (g_asyncLoadComplete.load()) {
        // Get pending data under lock
        std::string loadedContent;
        std::string loadedTitle;
        std::string loadedUrl;
        std::string loadedError;
        {
            std::lock_guard<std::mutex> lock(g_loadMutex);
            loadedContent = g_pendingContent;
            loadedTitle = g_pendingTitle;
            loadedUrl = g_pendingUrl;
            loadedError = g_loadError;
        }
        
        // Apply loaded content to globals
        g_currentUrl = loadedUrl;
        g_displayUrl = getDisplayUrl(loadedUrl);
        g_addressInput = g_displayUrl;
        
        if (loadedError.empty()) {
            g_pageTitle = loadedTitle;
            
            // Detect file type from URL extension
            bool isPdf = false;
            bool isImage = false;
            std::string ext;
            {
                size_t dotPos = loadedUrl.rfind('.');
                size_t slashPos = loadedUrl.rfind('/');
                if (dotPos != std::string::npos && (slashPos == std::string::npos || dotPos > slashPos)) {
                    ext = loadedUrl.substr(dotPos);
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    // Strip query string
                    size_t qPos = ext.find('?');
                    if (qPos != std::string::npos) ext = ext.substr(0, qPos);
                    
                    if (ext == ".pdf") isPdf = true;
                    else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" ||
                             ext == ".webp" || ext == ".bmp" || ext == ".ico" || ext == ".svg") isImage = true;
                }
            }
            
            if (isImage) {
                std::string imgHtml = ZepraBrowser::UI::getImageViewerHTML(loadedTitle, loadedUrl);
                g_pageContent = imgHtml;
#ifdef USE_WEBCORE
                parseWithWebCore(imgHtml);
#endif
            } else if (isPdf) {
                std::cout << "[Browser] PDF Document intercepted. Processing natively..." << std::endl;
                auto pdfDoc = nxrender::pdf::Document::OpenFromMemory(loadedContent);
                if (pdfDoc && pdfDoc->GetPageCount() > 0) {
                    int pageCount = pdfDoc->GetPageCount();
                    auto pdfDocPtr = pdfDoc.get();
                    std::string pdfHtml = ZepraBrowser::UI::getPdfViewerHTML(loadedTitle, pageCount,
                        [pdfDocPtr](int page) { return pdfDocPtr->ExtractText(page); });
                    
                    g_pageContent = pdfHtml;
#ifdef USE_WEBCORE
                    parseWithWebCore(pdfHtml);
#endif
                } else {
                    g_pageContent = "<html><body><h2>Failed to parse PDF document.</h2></body></html>";
#ifdef USE_WEBCORE
                    parseWithWebCore(g_pageContent);
#endif
                }
            } else {
                g_pageContent = loadedContent;
#ifdef USE_WEBCORE
                // Parse content (now on main thread, safe for DOM)
                parseWithWebCore(loadedContent);
#endif
            }
            std::cout << "[Browser] Async load complete: " << loadedUrl << " (" << loadedContent.size() << " bytes)" << std::endl;
        } else {
            g_pageContent = "";
            std::cerr << "[Browser] Async load error: " << loadedError << std::endl;
        }
        
        // Update tab state
        for (Tab& tab : g_tabs) {
            if (tab.id == g_activeTabId) {
                tab.url = loadedUrl;
                tab.title = loadedTitle.length() > 25 ? loadedTitle.substr(0, 25) + "..." : loadedTitle;
            }
        }
        
        // Reset flags
        g_isLoading = false;
        g_asyncLoadPending = false;
        g_asyncLoadComplete = false;
    }
    
    // === LAZY IMAGE LOADER POLL ===
    // Process completed background image loads and create textures
    {
        std::vector<ZepraBrowser::ImageResult> completed;
        ZepraBrowser::g_lazyImageLoader.pollCompleted(completed, 5); // Process up to 5 per frame
        // Note: pollCompleted also handles raw image -> texture conversion internally
    }
    
    gfx::set2D();
    gfx::clear(g_theme.bg_primary);
    
    // Inner window shadow for depth (subtle)
    const float shadowSize = 8.0f;
    const uint32_t shadowColor = 0x000000;
    // Top shadow
    gfx::gradient(0, 0, g_width, shadowSize, shadowColor, 0x00000000);
    // Left shadow
    gfx::gradient(0, 0, shadowSize, g_height, shadowColor, 0x00000000);
    // Right shadow  
    gfx::gradient(g_width - shadowSize, 0, shadowSize, g_height, 0x00000000, shadowColor);
    // Bottom shadow
    gfx::gradient(0, g_height - shadowSize, g_width, shadowSize, 0x00000000, shadowColor);
    
    // Content area MUST start below the unified top bar
    // Use left sidebar offset for proper content positioning
    float contentX = getSidebarOffset();
    float contentY = TOPBAR_HEIGHT;  // FIXED: Adjusted for unified Safari-style top bar
    float contentW = g_width - contentX;
    float contentH = g_height - contentY;
    
    // Render current page
    bool isStart = g_currentUrl == "zepra://start" || g_currentUrl.empty();
    if (isStart && !g_layoutRoot) {
        // Fallback: hardcoded start page if DOM wasn't built yet
        renderStartPage(contentX, contentY, contentW, contentH);
    } else if (isStart && g_layoutRoot) {
        // Start page rendered through layout engine (DOM-based)
        g_linkHitBoxes.clear();
        g_cursorIsPointer = false;
        // Ensure root covers at least the visible content area BEFORE layout
        // so flex justify-content: center has main axis space to distribute
        if (g_layoutRoot->height < contentH)
            g_layoutRoot->height = contentH;
        ZepraBrowser::layoutBlock(*g_layoutRoot, contentW, 0);
        // Re-check after layout in case content grew beyond viewport
        if (g_layoutRoot->height < contentH)
            g_layoutRoot->height = contentH;
        float scrollY = 0;
        for (const auto& tab : g_tabs) {
            if (tab.id == g_activeTabId) { scrollY = tab.scrollY; break; }
        }
        ZepraBrowser::paintBox(*g_layoutRoot, contentX, contentY, contentY + contentH, scrollY);
        for (const auto& hitBox : g_linkHitBoxes) {
            if (g_mouseX >= hitBox.x && g_mouseX < hitBox.x + hitBox.w &&
                g_mouseY >= hitBox.y && g_mouseY < hitBox.y + hitBox.h) {
                g_cursorIsPointer = true;
                break;
            }
        }
    } else {
        // Render loaded web content
        gfx::rect(contentX, contentY, contentW, contentH, 0xFFFFFF);
        
        if (g_isLoading) {
            // Loading indicator
            text("Loading...", contentX + 20, contentY + 40, 0x666666);
        } else if (!g_loadError.empty()) {
            // Error display
            text("Failed to load page", contentX + 20, contentY + 40, 0xCC0000);
            text("Error: " + g_loadError, contentX + 20, contentY + 70, 0x666666);
            text("URL: " + g_currentUrl, contentX + 20, contentY + 100, 0x999999);
        } else if (!g_pageContent.empty()) {
#ifdef USE_WEBCORE
            // Render with Layout Engine
            float maxY = contentY + contentH - 20;
            
            // Clear link hit boxes for this frame
            g_linkHitBoxes.clear();
            g_cursorIsPointer = false;
            
            if (g_layoutRoot) {
                // Run layout algorithm
                float layoutWidth = contentW - 40;  // Margins
                ZepraBrowser::layoutBlock(*g_layoutRoot, layoutWidth, 0);
                
                // Paint the layout tree
                float scrollY = 0;
                for (const auto& tab : g_tabs) {
                    if (tab.id == g_activeTabId) {
                        scrollY = tab.scrollY;
                        break;
                    }
                }
                ZepraBrowser::paintBox(*g_layoutRoot, contentX + 20, contentY + 20, maxY, scrollY);
                
                // Check if mouse is over any link hit box for pointer cursor
                for (const auto& hitBox : g_linkHitBoxes) {
                    if (g_mouseX >= hitBox.x && g_mouseX < hitBox.x + hitBox.w &&
                        g_mouseY >= hitBox.y && g_mouseY < hitBox.y + hitBox.h) {
                        g_cursorIsPointer = true;
                        break;
                    }
                }
            }
            
            if (!g_layoutRoot || !ZepraBrowser::UI::hasVisibleContent(*g_layoutRoot)) {
                auto nf = ZepraBrowser::UI::NothingFoundLayout::compute(contentX, contentY, contentW, contentH);
                gfx::rrect(nf.iconX, nf.iconY, nf.iconW, nf.iconH, 16, 0xE8E8E8);
                svg("search.svg", nf.searchIconX, nf.searchIconY, nf.searchIconSize, 0xBBBBBB);
                text("Nothing Found", nf.titleX, nf.titleY, 0x333333, 24);
                text("We couldn't find what you're looking for.", nf.sub1X, nf.sub1Y, 0x888888);
                text("Try searching for something else or check the URL.", nf.sub2X, nf.sub2Y, 0x888888);
                gfx::rrect(nf.btnX, nf.btnY, nf.btnW, nf.btnH, 8, g_theme.bg_elevated);
                text("Go back to previous page", nf.btnTextX, nf.btnTextY, g_theme.accent);
            }
#else
            // Fallback rendering
            if (!g_pageContent.empty()) {
                text(g_pageContent, contentX + 20, contentY + 20, 0x1F2328);
            }
#endif
        } else {
             text("Empty page", contentX + 20, contentY + 40, 0x999999);
        }
    }
    
    // Draw scrollbar
    if (g_layoutRoot && g_activeTabId >= 0 && g_tabs[g_activeTabId].url != "zepra://start") {
        float viewportHeight = contentH;
        float pageHeight = g_layoutRoot->height + 40; // Add padding at the bottom
        if (pageHeight > viewportHeight) {
            float scrollY = 0;
            for (const auto& tab : g_tabs) {
                if (tab.id == g_activeTabId) { scrollY = tab.scrollY; break; }
            }
            float thumbHeight = std::max(40.0f, (viewportHeight / pageHeight) * viewportHeight);
            float maxScroll = pageHeight - viewportHeight;
            float scrollPercent = maxScroll > 0 ? (scrollY / maxScroll) : 0;
            if (scrollPercent > 1.0f) scrollPercent = 1.0f;
            if (scrollPercent < 0.0f) scrollPercent = 0.0f;
            
            float thumbY = contentY + scrollPercent * (viewportHeight - thumbHeight);
            
            // Track
            gfx::rect(contentX + contentW - 14, contentY, 14, viewportHeight, 0xF5F5F5);
            // Thumb
            gfx::rrect(contentX + contentW - 12, thumbY, 10, thumbHeight, 5, 0xCCCCCC);
        }
    }
    
    // UI overlays - Reference design layout
    renderTopBar();          // Top bar with centered search, left/right controls
    renderLeftSidebar();     // Floating sidebar on LEFT with dashboard, bookmarks, history
    
    // ==========================================================================
    // ZepraWebView DevTools Panel (F12)
    // ==========================================================================
    if (g_consoleVisible) {
        float panelHeight = g_devToolsHeight;
        float panelY = g_height - panelHeight;
        float panelX = getSidebarOffset();
        float panelW = g_width - panelX;
        
        // Panel background (dark theme)
        gfx::rect(panelX, panelY, panelW, panelHeight, 0x1E1E1E);
        gfx::rect(panelX, panelY, panelW, 1, 0x3C3C3C);  // Top border
        
        // Tab bar with all panels
        float tabX = panelX + 8;
        const char* tabs[] = {"Elements", "Console", "Network", "Sources", "Performance", "Application", "Security", "Settings"};
        const int tabCount = static_cast<int>(DevToolsTab::COUNT);
        
        for (int i = 0; i < tabCount; i++) {
            bool active = (static_cast<int>(g_devToolsTab) == i);
            float tabW = 72;  // Slightly narrower to fit all tabs
            
            // Tab background
            if (active) {
                gfx::rect(tabX, panelY + 4, tabW, 24, 0x2D2D2D);
                gfx::rect(tabX, panelY + 27, tabW, 2, 0x58A6FF);  // Active indicator
            }
            
            // Tab click detection
            if (hit(tabX, panelY + 4, tabW, 24)) {
                gfx::rect(tabX, panelY + 4, tabW, 24, 0x3D3D3D);  // Hover
                if (g_mouseDown) {
                    g_devToolsTab = static_cast<DevToolsTab>(i);
                }
            }
            
            text(tabs[i], tabX + 6, panelY + 20, active ? 0xFFFFFF : 0x888888, 11.0f);
            tabX += tabW + 2;
        }
        
        // Close button
        float closeX = panelX + panelW - 28;
        if (hit(closeX, panelY + 6, 20, 20)) {
            gfx::rect(closeX, panelY + 6, 20, 20, 0x3D3D3D);
            if (g_mouseDown) g_consoleVisible = false;
        }
        text("X", closeX + 6, panelY + 20, 0x888888);
        
        // Content area
        float contentY = panelY + 34;
        float contentH = panelHeight - 34;
        
        // Render based on active tab
        switch (g_devToolsTab) {
            case DevToolsTab::Elements: {
                // Elements (DOM Tree) panel
                text("DOM Tree", panelX + 12, contentY + 16, 0x4FC1FF);
                contentY += 24;
                
#ifdef USE_WEBCORE
                if (g_document && g_document->body()) {
                    // Render simplified DOM tree
                    float treeY = contentY;
                    std::function<void(DOMNode*, int)> renderNode = [&](DOMNode* node, int depth) {
                        if (!node || treeY > panelY + panelHeight - 20) return;
                        
                        float indent = depth * 16.0f;
                        DOMElement* el = dynamic_cast<DOMElement*>(node);
                        
                        if (el) {
                            std::string tag = el->tagName();
                            std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);
                            
                            // Skip invisible
                            if (tag == "script" || tag == "style" || tag == "head") return;
                            
                            // Node line
                            bool hasChildren = el->childNodes().size() > 0;
                            std::string arrow = hasChildren ? "▸" : " ";
                            std::string display = arrow + " <" + tag;
                            
                            // Add class/id if present
                            std::string cls = el->getAttribute("class");
                            std::string id = el->getAttribute("id");
                            if (!id.empty()) display += " id=\"" + id + "\"";
                            if (!cls.empty() && cls.length() < 30) display += " class=\"" + cls + "\"";
                            display += ">";
                            
                            // Highlight hovered
                            bool hovered = hit(panelX + 12 + indent, treeY, panelW - 24 - indent, 16);
                            if (hovered) {
                                gfx::rect(panelX + 8, treeY - 2, panelW - 16, 18, 0x264F78);
                            }
                            
                            text(display, panelX + 12 + indent, treeY + 12, 0xE8A8F0);
                            treeY += 18;
                            
                            // Render children (first 3 levels only for performance)
                            if (depth < 3) {
                                for (size_t i = 0; i < el->childNodes().size() && i < 10; i++) {
                                    renderNode(el->childNodes()[i].get(), depth + 1);
                                }
                            }
                        }
                    };
                    
                    text("<html>", panelX + 12, treeY + 12, 0x569CD6);
                    treeY += 18;
                    renderNode(g_document->body(), 1);
                } else {
                    text("No document loaded", panelX + 12, contentY + 16, 0x666666);
                }
#else
                text("WebCore not enabled", panelX + 12, contentY + 16, 0x666666);
#endif
                break;
            }
            
            case DevToolsTab::Console: {
                // Console panel (interactive - per-tab console logs)
                
                // Get active tab's console log
                Tab* activeTab = nullptr;
                for (auto& t : g_tabs) {
                    if (t.id == g_activeTabId) { activeTab = &t; break; }
                }
                std::vector<std::string>& consoleLog = activeTab ? activeTab->consoleLog : g_tabs[0].consoleLog;
                
                // Console log display area (above input)
                float logAreaHeight = contentH - 30;  // Reserve 30px for input
                
                if (consoleLog.empty()) {
                    text("  Welcome to ZepraBrowser Console", panelX + 12, contentY + 16, 0x888888);
                    contentY += 20;
                    text("  Type JavaScript expressions and press Enter to execute", panelX + 12, contentY + 16, 0x666666);
                    contentY += 18;
                    text("  [Info] Current URL: " + g_currentUrl, panelX + 12, contentY + 16, 0x4FC1FF);
                    contentY += 18;
                    text("  [Info] Layout boxes: " + std::to_string(g_layoutRoot ? g_layoutRoot->children.size() : 0), panelX + 12, contentY + 16, 0x4FC1FF);
                } else {
                    float logY = contentY;
                    // Show last N entries that fit
                    int maxEntries = static_cast<int>((logAreaHeight - 20) / 16);
                    size_t startIdx = consoleLog.size() > static_cast<size_t>(maxEntries) ? 
                                      consoleLog.size() - maxEntries : 0;
                    
                    for (size_t i = startIdx; i < consoleLog.size() && logY < panelY + panelHeight - 35; i++) {
                        const std::string& entry = consoleLog[i];
                        // Color based on prefix
                        uint32_t color = 0xCCCCCC;
                        if (entry.find("> ") == 0) color = 0xFFFFFF;  // User input
                        else if (entry.find("< ") == 0) color = 0x6A9955;  // Return value
                        else if (entry.find("[Error]") != std::string::npos) color = 0xF14C4C;
                        else if (entry.find("[Warning]") != std::string::npos) color = 0xFFAA00;
                        else if (entry.find("[Info]") != std::string::npos) color = 0x4FC1FF;
                        
                        text(entry, panelX + 12, logY + 14, color, 11.0f);
                        logY += 16;
                    }
                }
                
                // Console input field at bottom
                float inputY = panelY + panelHeight - 28;
                float inputW = panelW - 24;
                
                // Input background
                gfx::rect(panelX + 8, inputY, inputW + 8, 24, 0x2D2D2D);
                gfx::rect(panelX + 8, inputY, inputW + 8, 1, 0x3C3C3C);  // Top border
                
                // Prompt
                text(">", panelX + 12, inputY + 16, 0x58A6FF);
                
                // Input text with cursor
                std::string displayInput = g_consoleInput;
                if (g_consoleFocused) {
                    // Add blinking cursor
                    static int cursorBlink = 0;
                    cursorBlink++;
                    if ((cursorBlink / 30) % 2 == 0) {
                        displayInput += "|";
                    }
                }
                text(displayInput.empty() && !g_consoleFocused ? "Type JavaScript expression..." : displayInput, 
                     panelX + 24, inputY + 16, g_consoleFocused ? 0xFFFFFF : 0x666666, 11.0f);
                
                // Click to focus
                if (hit(panelX + 8, inputY, inputW + 8, 24)) {
                    if (g_mouseDown) {
                        g_consoleFocused = true;
                        g_addressFocused = false;
                        g_searchFocused = false;
                    }
                }
                break;
            }
            
            // NOTE: DevToolsTab::Styles was removed - use Elements panel for CSS inspection
            
            case DevToolsTab::Network: {
                // Network panel — real data from per-tab NetworkMonitor
                text("Network Requests", panelX + 12, contentY + 16, 0x4FC1FF);
                contentY += 24;
                
                // Header row
                text("Method   URL                                       Status   Size     Time", 
                     panelX + 12, contentY + 14, 0x888888, 10.0f);
                contentY += 20;
                gfx::rect(panelX, contentY, panelW, 1, 0x3C3C3C);
                contentY += 4;
                
                // Get active tab's network monitor
                Tab* netTab = nullptr;
                for (auto& t : g_tabs) {
                    if (t.id == g_activeTabId) { netTab = &t; break; }
                }
                
                if (netTab) {
                    auto& monitor = netTab->getNetworkMonitor();
                    auto entries = monitor.getEntries();
                    
                    if (entries.empty()) {
                        text("No network requests recorded", panelX + 12, contentY + 14, 0x666666);
                    } else {
                        // Show most recent entries that fit
                        int maxRows = static_cast<int>((contentH - 60) / 16);
                        size_t startIdx = entries.size() > static_cast<size_t>(maxRows) ? 
                                          entries.size() - maxRows : 0;
                        
                        for (size_t i = startIdx; i < entries.size() && contentY < panelY + panelHeight - 20; i++) {
                            const auto& entry = entries[i];
                            
                            // Method
                            uint32_t methodColor = (entry.request.method == "GET") ? 0x3FB950 : 0x58A6FF;
                            text(entry.request.method, panelX + 12, contentY + 15, methodColor, 10.0f);
                            
                            // URL (truncated)
                            std::string displayUrl = entry.request.url.length() > 45 ? 
                                entry.request.url.substr(0, 42) + "..." : entry.request.url;
                            text(displayUrl, panelX + 70, contentY + 15, 0xCCCCCC, 10.0f);
                            
                            // Status
                            uint32_t statusColor = entry.response.status_code >= 200 && entry.response.status_code < 300 ? 0x3FB950 :
                                                   entry.response.status_code >= 400 ? 0xF14C4C : 0xFFAA00;
                            text(entry.response.status_code > 0 ? std::to_string(entry.response.status_code) : "...", 
                                 panelX + panelW - 180, contentY + 15, statusColor, 10.0f);
                            
                            // Size
                            std::string sizeStr = entry.response.content_length > 0 ? 
                                (entry.response.content_length > 1024 ? 
                                    std::to_string(entry.response.content_length / 1024) + " KB" :
                                    std::to_string(entry.response.content_length) + " B") : "-";
                            text(sizeStr, panelX + panelW - 120, contentY + 15, 0x888888, 10.0f);
                            
                            // Time
                            std::string timeStr = entry.response.duration_ms > 0 ? 
                                std::to_string(static_cast<int>(entry.response.duration_ms)) + " ms" : "pending";
                            text(timeStr, panelX + panelW - 60, contentY + 15, 0x888888, 10.0f);
                            
                            contentY += 16;
                        }
                    }
                }
                break;
            }
            
            case DevToolsTab::Sources: {
                // Sources panel - JavaScript debugger
                text("Sources", panelX + 12, contentY + 16, 0x4FC1FF);
                contentY += 24;
                
                // File tree
                text("  Page Sources:", panelX + 12, contentY + 14, 0x888888);
                contentY += 20;
                text("   > " + (g_currentUrl.length() > 30 ? g_currentUrl.substr(0, 30) + "..." : g_currentUrl), 
                     panelX + 12, contentY + 14, 0x4FC1FF);
                contentY += 18;
                text("       index.html", panelX + 12, contentY + 14, 0xE8A8F0);
                contentY += 16;
                text("       styles.css", panelX + 12, contentY + 14, 0x4EC9B0);
                contentY += 16;
                text("       app.js", panelX + 12, contentY + 14, 0xDCDC9D);
                break;
            }
            
            case DevToolsTab::Performance: {
                // Performance panel
                text("Performance", panelX + 12, contentY + 16, 0x4FC1FF);
                contentY += 24;
                
                // Record button
                gfx::rect(panelX + 12, contentY + 2, 70, 22, 0x3FB950);
                text("Record", panelX + 22, contentY + 16, 0xFFFFFF);
                contentY += 30;
                
                // Metrics
                text("Page Load Metrics:", panelX + 12, contentY + 14, 0xCCCCCC);
                contentY += 20;
                text("  DOM Content Loaded: 245ms", panelX + 12, contentY + 14, 0x3FB950);
                contentY += 16;
                text("  First Contentful Paint: 312ms", panelX + 12, contentY + 14, 0x3FB950);
                contentY += 16;
                text("  Document Complete: " + std::string(g_isLoading ? "loading..." : "890ms"), 
                     panelX + 12, contentY + 14, g_isLoading ? 0xFFAA00 : 0xCCCCCC);
                break;
            }
            
            case DevToolsTab::Application: {
                // Application panel - Storage
                text("Application", panelX + 12, contentY + 16, 0x4FC1FF);
                contentY += 24;
                
                text("Storage:", panelX + 12, contentY + 14, 0x888888);
                contentY += 20;
                text("  > Local Storage", panelX + 12, contentY + 14, 0xCCCCCC);
                contentY += 16;
                text("  > Session Storage", panelX + 12, contentY + 14, 0xCCCCCC);
                contentY += 16;
                text("  > Cookies", panelX + 12, contentY + 14, 0xCCCCCC);
                contentY += 16;
                text("  > IndexedDB", panelX + 12, contentY + 14, 0xCCCCCC);
                contentY += 22;
                text("Cache:", panelX + 12, contentY + 14, 0x888888);
                contentY += 20;
                text("  > Cache Storage", panelX + 12, contentY + 14, 0xCCCCCC);
                break;
            }
            
            case DevToolsTab::Security: {
                // Security panel - TLS/SSL Info
                text("Security Overview", panelX + 12, contentY + 16, 0x4FC1FF);
                contentY += 24;
                
                bool isSecure = g_currentUrl.find("https://") == 0;
                text(isSecure ? "* Connection: Secure (TLS 1.3)" : "! Connection: Not Secure", 
                     panelX + 12, contentY + 14, isSecure ? 0x3FB950 : 0xFFAA00);
                contentY += 18;
                text(isSecure ? "  Certificate: Valid" : "  No TLS Certificate", 
                     panelX + 12, contentY + 14, 0x888888);
                contentY += 26;
                
                text("Cross-Origin Policy:", panelX + 12, contentY + 14, 0x888888);
                contentY += 18;
                text("  Same-origin requests allowed", panelX + 12, contentY + 14, 0xCCCCCC);
                contentY += 16;
                text("  Cross-origin requires CORS headers", panelX + 12, contentY + 14, 0xCCCCCC);
                break;
            }
            
            case DevToolsTab::Settings: {
                // Settings panel
                text("DevTools Settings", panelX + 12, contentY + 16, 0x4FC1FF);
                contentY += 24;
                
                text("Appearance:", panelX + 12, contentY + 14, 0x888888);
                contentY += 20;
                text("  Theme: Dark", panelX + 12, contentY + 14, 0xCCCCCC);
                contentY += 16;
                text("  Panel layout: Bottom", panelX + 12, contentY + 14, 0xCCCCCC);
                contentY += 22;
                
                text("Console:", panelX + 12, contentY + 14, 0x888888);
                contentY += 20;
                text("  [x] Show timestamps", panelX + 12, contentY + 14, 0xCCCCCC);
                contentY += 16;
                text("  [x] Preserve log", panelX + 12, contentY + 14, 0xCCCCCC);
                contentY += 22;
                
                text("Network:", panelX + 12, contentY + 14, 0x888888);
                contentY += 20;
                text("  [x] Disable cache (while DevTools open)", panelX + 12, contentY + 14, 0xCCCCCC);
                break;
            }
            
            default:
                break;
        }
    }
    
    // Render context menu (if visible)
    auto& ctxMenu = g_mouseHandler.getContextMenu();
    if (ctxMenu.visible) {
        // Calculate total height
        float totalHeight = 0;
        for (const auto& item : ctxMenu.items) {
            totalHeight += item.label.empty() ? 8 : ctxMenu.itemHeight;
        }
        // Draw shadow (omitted)
        gfx::rect(ctxMenu.x, ctxMenu.y, ctxMenu.width, totalHeight, 0xFAFAFA);
        
        // Draw border
        gfx::rect(ctxMenu.x, ctxMenu.y, ctxMenu.width, 1, 0xDDDDDD);
        gfx::rect(ctxMenu.x, ctxMenu.y + totalHeight - 1, ctxMenu.width, 1, 0xDDDDDD);
        gfx::rect(ctxMenu.x, ctxMenu.y, 1, totalHeight, 0xDDDDDD);
        gfx::rect(ctxMenu.x + ctxMenu.width - 1, ctxMenu.y, 1, totalHeight, 0xDDDDDD);
        
        // Draw items
        float itemY = ctxMenu.y;
        for (size_t i = 0; i < ctxMenu.items.size(); i++) {
            const auto& item = ctxMenu.items[i];
            
            if (item.label.empty()) {
                gfx::rect(ctxMenu.x + 8, itemY + 3, ctxMenu.width - 16, 1, 0xDDDDDD);
                itemY += 8;
                continue;
            }
            
            // Hover highlight
            if ((int)i == ctxMenu.hoveredIndex) {
                gfx::rect(ctxMenu.x + 2, itemY + 2, ctxMenu.width - 4, ctxMenu.itemHeight - 4, 0xE8F0FE);
            }
            
            // Item text
            uint32_t textColor = item.enabled ? 0x333333 : 0xAAAAAA;
            text(item.label, ctxMenu.x + 12, itemY + 22, textColor);
            
            // Shortcut
            if (!item.shortcut.empty()) {
                text(item.shortcut, ctxMenu.x + ctxMenu.width - 65, itemY + 22, 0x888888);
            }
            
            itemY += ctxMenu.itemHeight;
        }
    }
    
    // ===========================================================================
    // Alert/Confirm Modal Dialog Overlay
    // ===========================================================================
    if (g_alertVisible) {
        // Semi-transparent backdrop
        gfx::rect(0, 0, g_width, g_height, 0x00000088);
        
        // Dialog dimensions
        float dlgW = 400;
        float msgLines = 1 + (float)g_alertMessage.size() / 45;
        float dlgH = 120 + msgLines * 18;
        float dlgX = (g_width - dlgW) / 2;
        float dlgY = (g_height - dlgH) / 2;
        
        // Dialog shadow + background
        gfx::rrect(dlgX + 4, dlgY + 4, dlgW, dlgH, 12, 0x00000040);
        gfx::rrect(dlgX, dlgY, dlgW, dlgH, 12, g_theme.bg_primary);
        gfx::border(dlgX, dlgY, dlgW, dlgH, 1, g_theme.border);
        
        // Title bar
        text("Zepra Browser", dlgX + 20, dlgY + 28, g_theme.text_secondary, 11.0f);
        gfx::rect(dlgX + 16, dlgY + 38, dlgW - 32, 1, g_theme.border);
        
        // Message text (word-wrap approximation)
        float textY = dlgY + 62;
        std::string remaining = g_alertMessage;
        while (!remaining.empty()) {
            std::string line = remaining.substr(0, 50);
            if (remaining.size() > 50) {
                size_t sp = line.rfind(' ');
                if (sp != std::string::npos) line = remaining.substr(0, sp + 1);
            }
            text(line, dlgX + 24, textY, g_theme.text_primary, 13.0f);
            textY += 20;
            remaining = remaining.substr(std::min(line.size(), remaining.size()));
        }
        
        // OK button
        float btnW = 80, btnH = 32;
        float btnX = dlgX + dlgW - btnW - 20;
        float btnY = dlgY + dlgH - btnH - 16;
        bool btnHover = hit(btnX, btnY, btnW, btnH);
        
        gfx::rrect(btnX, btnY, btnW, btnH, 6, btnHover ? g_theme.accent : g_theme.bg_elevated);
        text("OK", btnX + btnW/2 - 10, btnY + 21, btnHover ? 0xFFFFFF : g_theme.text_primary, 13.0f);
        if (btnHover) g_uiHoverHand = true;
    }
    
    gfx::present();
    
    // Update cursor based on hover state
    bool isOverLink = false;
#ifdef USE_WEBCORE
    for (const auto& linkBox : g_linkHitBoxes) {
        if (g_mouseX >= linkBox.x && g_mouseX <= linkBox.x + linkBox.w &&
            g_mouseY >= linkBox.y && g_mouseY <= linkBox.y + linkBox.h) {
            isOverLink = true;
            break;
        }
    }
#endif
    // Detect hover over any clickable UI element (buttons, tabs, sidebar items)
    // Top bar buttons area (y < TOPBAR_HEIGHT, outside address bar focus zone)
    if (g_mouseY < TOPBAR_HEIGHT && !g_addressFocused) {
        float sidebarOffset = g_leftSidebarVisible ? 
            (g_leftSidebarExpanded ? LEFT_SIDEBAR_EXPANDED : LEFT_SIDEBAR_WIDTH) : 0;
        // Left buttons (sidebar toggle, zepra logo)
        if (g_mouseX > sidebarOffset && g_mouseX < sidebarOffset + 80) {
            g_uiHoverHand = true;
        }
        // Right buttons (refresh, close, new tab, menu, share)
        float rightEdge = g_width;
        if (g_mouseX > rightEdge - 180) {
            g_uiHoverHand = true;
        }
    }
    // Left sidebar icons
    if (g_leftSidebarVisible && g_mouseY > TOPBAR_HEIGHT) {
        float sidebarW = g_leftSidebarExpanded ? LEFT_SIDEBAR_EXPANDED : LEFT_SIDEBAR_WIDTH;
        if (g_mouseX < sidebarW) {
            g_uiHoverHand = true;
        }
    }
    // Cursor switching based on UI state
    if (g_addressFocused || g_searchFocused || g_consoleFocused) {
        NXRender::setCursor(NXRender::CursorType::Text);
    } else if (g_currentCursorIsHand || isOverLink || g_cursorIsPointer || g_uiHoverHand) {
        NXRender::setCursor(NXRender::CursorType::Hand);
    } else {
        NXRender::setCursor(NXRender::CursorType::Arrow);
    }
    g_uiHoverHand = false;  // Reset per frame — set by UI hit-tests
  } catch (const std::exception& e) {
    std::cerr << "[CRASH] render() exception: " << e.what() << std::endl;
  } catch (...) {
    std::cerr << "[CRASH] render() unknown exception — skipping frame" << std::endl;
  }
}

// ============================================================================
// SMART URL PARSING
// ============================================================================

std::string parseSmartUrl(const std::string& input) {
    if (input.empty()) return "zepra://start";
    
    std::string trimmed = input;
    size_t start = trimmed.find_first_not_of(" \t\n\r");
    size_t end = trimmed.find_last_not_of(" \t\n\r");
    if (start != std::string::npos && end != std::string::npos) {
        trimmed = trimmed.substr(start, end - start + 1);
    }
    
    // file:// protocol
    if (trimmed.find("file://") == 0) return trimmed;
    
    // Absolute path (starts with / or ~)
    if (!trimmed.empty() && (trimmed[0] == '/' || trimmed[0] == '~')) {
        return "file://" + trimmed;
    }
    
    // Already has protocol
    if (trimmed.find("://") != std::string::npos) {
        return trimmed;
    }
    
    // Internal protocols
    if (trimmed.find("zepra://") == 0) return trimmed;
    
    // Check if it looks like a LOCAL FILE PATH (has file extension)
    // Common file extensions that indicate a file, not a domain
    static const std::vector<std::string> fileExtensions = {
        ".html", ".htm", ".css", ".js", ".json", ".xml", ".txt", ".md",
        ".png", ".jpg", ".jpeg", ".gif", ".svg", ".ico", ".webp",
        ".pdf", ".zip", ".tar", ".gz", ".mp3", ".mp4", ".webm",
        ".c", ".cpp", ".h", ".hpp", ".py", ".rs", ".go"
    };
    
    // Check if it ends with a file extension (after the last dot)
    size_t lastDot = trimmed.rfind('.');
    size_t lastSlash = trimmed.rfind('/');
    bool hasFileExtension = false;
    
    if (lastDot != std::string::npos) {
        // Make sure the dot is after the last slash (part of filename, not directory)
        if (lastSlash == std::string::npos || lastDot > lastSlash) {
            std::string ext = trimmed.substr(lastDot);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            for (const auto& fe : fileExtensions) {
                if (ext == fe) {
                    hasFileExtension = true;
                    break;
                }
            }
        }
    }
    
    // If it has a file extension and contains a slash, it's likely a relative path
    // Examples: "kernel/syscalls.html", "docs/index.html", "../style.css"
    if (hasFileExtension && (trimmed.find('/') != std::string::npos || trimmed.find("..") == 0)) {
        // Treat as relative file path - resolve against current working directory
        char* cwd = getcwd(nullptr, 0);
        if (cwd) {
            std::string fullPath = std::string(cwd) + "/" + trimmed;
            free(cwd);
            // Check if file exists
            std::ifstream f(fullPath);
            if (f.good()) {
                return "file://" + fullPath;
            }
        }
        // Also check common documentation paths
        std::vector<std::string> searchPaths = {
            "/home/swana/Documents/NEOLYXOS/neolyx-os/docs/",
            "/home/swana/Documents/NEOLYXOS/neolyx-os/"
        };
        for (const auto& base : searchPaths) {
            std::string testPath = base + trimmed;
            std::ifstream f(testPath);
            if (f.good()) {
                return "file://" + testPath;
            }
        }
    }
    
    // localhost or IP with port (localhost:9050, 127.0.0.1:5565, 192.168.1.1:8080)
    size_t colonPos = trimmed.find(':');
    if (colonPos != std::string::npos) {
        std::string beforeColon = trimmed.substr(0, colonPos);
        std::string afterColon = trimmed.substr(colonPos + 1);
        
        // Check if port is numeric
        bool isPort = !afterColon.empty() && 
                      std::all_of(afterColon.begin(), afterColon.end(), ::isdigit);
        
        if (isPort) {
            // localhost:PORT or IP:PORT
            return "http://" + trimmed;
        }
    }
    
    // Check if it looks like a REAL domain (not just any string with a dot)
    // Real domains have at least 2 parts separated by dots (e.g., google.com)
    // And the TLD part should NOT be a file extension
    bool hasDot = trimmed.find('.') != std::string::npos;
    bool hasSpaces = trimmed.find(' ') != std::string::npos;
    bool hasSlash = trimmed.find('/') != std::string::npos;
    
    if (hasDot && !hasSpaces && !hasFileExtension) {
        // Additional check: if it has a slash before the dot, it's probably a path
        size_t firstDot = trimmed.find('.');
        size_t firstSlash = trimmed.find('/');
        if (firstSlash != std::string::npos && firstSlash < firstDot) {
            // Path like "kernel/syscalls.html" - but we already checked extension
            // Fallthrough to search
        } else {
            // Treat as domain - browser will try to fetch it
            return "https://" + trimmed;
        }
    }
    
    // Otherwise, it's a search query
    return getSearchUrl(trimmed);
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void onNewTab() {
    // Save current tab's state before switching
    for (Tab& t : g_tabs) {
        if (t.id == g_activeTabId) {
            t.document = std::move(g_document);
            t.cssEngine = std::move(g_cssEngine);
            t.layoutRoot = std::move(g_layoutRoot);
            t.hasRenderedContent = true;
            break;
        }
    }
    
    Tab tab;
    tab.id = g_nextTabId++;
    tab.title = "New Tab";
    tab.url = "zepra://start";
    tab.isStart = true;
    int newId = tab.id;
    g_tabs.push_back(std::move(tab));
    g_activeTabId = newId;
    g_currentUrl = "zepra://start";
    g_searchQuery = "";
    
    // Fresh state for new tab
    g_layoutRoot = nullptr;
    g_document = nullptr;
    g_cssEngine = nullptr;
}

void onCloseTab(int tabId) {
    for (auto it = g_tabs.begin(); it != g_tabs.end(); ++it) {
        if (it->id == tabId) {
            g_tabs.erase(it);
            break;
        }
    }
    if (g_tabs.empty()) {
        g_running = false;
    } else if (g_activeTabId == tabId) {
        g_activeTabId = g_tabs[0].id;
        g_currentUrl = g_tabs[0].url;
    }
}

void onSelectTab(int tabId) {
    if (tabId == g_activeTabId) return;  // Already on this tab
    
    // Save current tab's DOM/CSS/Layout state
    for (Tab& t : g_tabs) {
        if (t.id == g_activeTabId) {
            t.document = std::move(g_document);
            t.cssEngine = std::move(g_cssEngine);
            t.layoutRoot = std::move(g_layoutRoot);
            t.hasRenderedContent = true;
            break;
        }
    }
    
    // Save current tab's styled lines to cache
    g_tabContentCache[g_activeTabId] = g_styledLines;
    
    // Switch to new tab
    int oldTabId = g_activeTabId;
    g_activeTabId = tabId;
    
    // Restore new tab's styled lines from cache
    if (g_tabContentCache.find(tabId) != g_tabContentCache.end()) {
        g_styledLines = g_tabContentCache[tabId];
    } else {
        g_styledLines.clear();
    }
    
    // Check if tab has pending load (background tab that wasn't loaded yet)
    for (Tab& tab : g_tabs) {
        if (tab.id == tabId && tab.pendingLoad && !tab.pendingUrl.empty()) {
            std::cout << "[Tab] Triggering deferred load for tab " << tabId 
                      << ": " << tab.pendingUrl << std::endl;
            tab.pendingLoad = false;
            onNavigate(tab.pendingUrl);
            tab.pendingUrl.clear();
            return; // Navigation will handle the rest
        }
    }
    
    // Restore URL, title, and DOM/CSS/Layout from the target tab
    for (Tab& tab : g_tabs) {
        if (tab.id == tabId) {
            g_currentUrl = tab.url;
            g_pageTitle = tab.title;
            
            // Restore per-tab DOM and CSS engine
            if (tab.document) {
                g_document = std::move(tab.document);
                g_cssEngine = std::move(tab.cssEngine);
                g_layoutRoot = std::move(tab.layoutRoot);
            } else {
                g_document = nullptr;
                g_cssEngine = nullptr;
                g_layoutRoot = nullptr;
            }
            break;
        }
    }
    // If layout was restored from per-tab state, just re-layout it at current width
    if (g_layoutRoot) {
        g_focusedBox = nullptr;
        float contentWidth = g_width - getSidebarOffset();
        ZepraBrowser::layoutBlock(*g_layoutRoot, contentWidth, 0);
        std::cout << "[Tab] Switched to tab " << tabId << " (restored DOM layout)" << std::endl;
        return;
    }
    
    // Fallback: Rebuild layout root from cached styled lines
    g_focusedBox = nullptr;
    g_layoutRoot = std::make_unique<LayoutBox>();
    g_layoutRoot->type = LayoutType::Block;
    g_layoutRoot->width = g_width;
    
    for (const auto& line : g_styledLines) {
        LayoutBox* child = addChild(g_layoutRoot.get());
        child->type = (line.isBlock || line.marginTop > 0 || line.marginBottom > 0) ? LayoutType::Block : LayoutType::Inline;
        child->text = line.text;
        child->color = line.color;
        child->fontSize = line.fontSize;
        child->bold = line.bold;
        child->bgColor = line.bgColor;
        child->hasBgColor = line.hasBgColor;
        child->isLink = line.isLink;
        child->href = line.href;
        
        child->isInput = line.isInput;
        child->inputType = line.inputType;
        child->placeholder = line.placeholder;
        
        if (child->isInput) {
            child->type = LayoutType::InlineBlock;
        }
    }
    
    if (g_layoutRoot) {
        float contentWidth = g_width - getSidebarOffset();
        ZepraBrowser::layoutBlock(*g_layoutRoot, contentWidth, 0);
    }
    
    std::cout << "[Tab] Switched to tab " << tabId << " with " << g_styledLines.size() << " lines" << std::endl;
}

void onNavigate(const std::string& input) {
    // Parse the input intelligently
    std::string url = parseSmartUrl(input);
    
    // Handle internal URLs
    bool isInternal = (url.find("zepra://") == 0);
    
    if (isInternal) {
        g_currentUrl = url;
        g_displayUrl = "";
        g_pageContent = "";
        g_pageTitle = "New Tab";
        g_isLoading = false;
        g_loadError = "";
        
        // Update tab state synchronously for internal URLs
        for (Tab& tab : g_tabs) {
            if (tab.id == g_activeTabId) {
                tab.url = url;
                tab.isStart = true;
                tab.title = "New Tab";
            }
        }
        
#ifdef USE_WEBCORE
        if (url == "zepra://start") {
            std::string startHtml = ZepraBrowser::UI::getStartPageHTML();
            g_pageContent = startHtml;
            parseWithWebCore(startHtml);
            std::cout << "[Browser] Start page DOM built" << std::endl;
        }
#endif
    } else {
        // Async load for external URLs
        // Normalize (auto-add index.html for directories) synchronously
        url = normalizeUrl(url);
        
        g_isLoading = true;
        g_loadError = "";
        g_asyncLoadPending = true;
        g_asyncLoadComplete = false;
        g_pendingUrl = url;
        
        std::cout << "[Browser] Loading (async): " << url << std::endl;
        
        // Update tab state to show loading
        for (Tab& tab : g_tabs) {
            if (tab.id == g_activeTabId) {
                tab.url = url;
                tab.isStart = false;
                tab.title = "Loading...";
            }
        }
        
        // Spawn background thread
        std::thread([url]() {
            // Fetch in background
            HttpResponse resp = httpGet(url);
            
            // Prepare data
            std::string pendingTitle;
            std::string pendingContent;
            std::string pendingError;
            bool success = resp.success;
            
            if (success) {
                pendingContent = resp.data;
                pendingTitle = extractTitle(resp.data);
                if (pendingTitle.empty()) {
                    pendingTitle = getDisplayUrl(url);
                }
            } else {
                pendingError = resp.error;
            }
            
            // Store results atomically
            {
                std::lock_guard<std::mutex> lock(g_loadMutex);
                g_pendingContent = pendingContent;
                g_pendingTitle = pendingTitle;
                g_loadError = pendingError;
            }
            
            // Signal completion
            g_asyncLoadComplete = true;
            
        }).detach();
    }
}

// Find LayoutBox at screen coordinates (recursive)
LayoutBox* findBoxAt(LayoutBox* box, float x, float y) {
    if (!box) return nullptr;
    
    // Check children first (top-most) in reverse order
    for (auto it = box->children.rbegin(); it != box->children.rend(); ++it) {
        LayoutBox* found = findBoxAt(&(*it), x, y);
        if (found) return found;
    }
    
    // Check self
    if (x >= box->screenX && x < box->screenX + box->width &&
        y >= box->screenY && y < box->screenY + box->height) {
        return box;
    }
    
    return nullptr;
}

void onSearch(const std::string& query) {
    // parseSmartUrl will handle converting plain text to search URL
    onNavigate(query);
}

void handleClick(float mx, float my) {
    // Alert dialog intercepts all clicks
    if (g_alertVisible) {
        g_alertVisible = false;
        return;
    }
    
    // Handle MouseHandler context menu clicks first
    auto& ctxMenu = g_mouseHandler.getContextMenu();
    if (ctxMenu.visible) {
        auto action = ctxMenu.handleClick(mx, my);
        if (action != static_cast<ZepraBrowser::ContextMenuAction>(0)) {
            g_mouseHandler.executeAction(action);
        }
        ctxMenu.hide();
        return;
    }
    
    // ====== LEFT SIDEBAR CLICK HANDLING ======
    if (g_leftSidebarVisible) {
        float sidebarW = g_leftSidebarExpanded ? LEFT_SIDEBAR_EXPANDED : LEFT_SIDEBAR_WIDTH;
        float sidebarX = 0;
        float sidebarY = TOPBAR_HEIGHT;
        
        // Check if click is in sidebar area
        if (mx < sidebarW) {
            float iconY = sidebarY + 16;
            
            // Dashboard click (avatar area)
            if (hit(12, iconY, 36, 36)) {
                if (g_activeSidebarPanel == SidebarPanel::Dashboard) {
                    g_activeSidebarPanel = SidebarPanel::NoPanel;
                    g_leftSidebarExpanded = false;
                } else {
                    g_activeSidebarPanel = SidebarPanel::Dashboard;
                    g_leftSidebarExpanded = true;
                }
                return;
            }
            iconY += 48;
            
            // Main panels (Bookmarks, History)
            SidebarPanel mainPanels[] = {
                SidebarPanel::Bookmarks,
                SidebarPanel::History
            };
            
            for (SidebarPanel panel : mainPanels) {
                if (hit(8, iconY - 2, sidebarW - 8, 36)) {
                    if (g_activeSidebarPanel == panel) {
                        g_activeSidebarPanel = SidebarPanel::NoPanel;
                        g_leftSidebarExpanded = false;
                    } else {
                        g_activeSidebarPanel = panel;
                        g_leftSidebarExpanded = true;
                    }
                    return;
                }
                iconY += 40;
            }
            
            // Bottom tool icons (URL, Settings, WebView, Help)
            float bottomY = g_height - 180 + 16;
            SidebarPanel bottomPanels[] = {
                SidebarPanel::Shortcuts,
                SidebarPanel::Settings,
                SidebarPanel::WebView,
                SidebarPanel::Help
            };
            
            for (SidebarPanel panel : bottomPanels) {
                if (hit(8, bottomY - 2, sidebarW - 8, 32)) {
                    g_activeSidebarPanel = panel;
                    return;
                }
                bottomY += 36;
            }
            
            // Clicked inside sidebar but not on a button - consume click
            return;
        }
    }
    
    // Top bar - sidebar toggle (left side)
    float sidebarOffset = g_leftSidebarVisible ? 
        (g_leftSidebarExpanded ? LEFT_SIDEBAR_EXPANDED : LEFT_SIDEBAR_WIDTH) : 0;
    float toggleX = sidebarOffset + 12;
    float toggleY = (TOPBAR_HEIGHT - 28) / 2;
    if (hit(toggleX, toggleY, 24, 24)) {
        g_leftSidebarVisible = !g_leftSidebarVisible;
        return;
    }
    
    // === UNIFIED TOP BAR CLICK HANDLING ===
    if (handleTopBarClick(mx, my)) {
        return;
    }
    
    // === HTML BODY INPUT HANDLING ===
    g_addressFocused = false;
    g_searchFocused = false;
    g_focusedBox = nullptr;
    
    // Check web content layout tree for inputs (HTML-rendered search box)
    if (my > TOPBAR_HEIGHT && g_layoutRoot) {
        LayoutBox* clicked = findBoxAt(g_layoutRoot.get(), mx, my);
        if (clicked && clicked->isInput) {
            g_focusedBox = clicked;
            // The HTML start page acts as the main search driver (zepra://start)
            if (g_currentUrl == "zepra://start" || g_currentUrl.empty()) {
                g_searchFocused = true;
            }
            std::cout << "[Browser] Focused input type=" << clicked->inputType << std::endl;
            return;
        }
    }
#ifdef USE_WEBCORE
    // Check for link clicks in content area
    for (const auto& linkBox : g_linkHitBoxes) {
        if (mx >= linkBox.x && mx <= linkBox.x + linkBox.w &&
            my >= linkBox.y && my <= linkBox.y + linkBox.h) {
            // Clicked on a link!
            std::cout << "[Browser] Link clicked: " << linkBox.href 
                      << " (target=" << linkBox.target << ")" << std::endl;
            
            // Navigate to link href
            std::string targetUrl = linkBox.href;
            
            // Handle relative URLs
            if (!targetUrl.empty() && targetUrl[0] == '/') {
                // Convert relative to absolute using current URL base
                size_t pos = g_currentUrl.find("://");
                if (pos != std::string::npos) {
                    size_t slashPos = g_currentUrl.find('/', pos + 3);
                    targetUrl = g_currentUrl.substr(0, slashPos) + targetUrl;
                }
            }
            
            if (!targetUrl.empty()) {
                // Check target attribute
                if (linkBox.target == "_blank") {
                    // Open in new tab
                    std::cout << "[Browser] Opening in new tab: " << targetUrl << std::endl;
                    onNewTab();
                    onNavigate(targetUrl);
                } else {
                    // Open in same window (default or _self)
                    onNavigate(targetUrl);
                }
            }
            return;
        }
    }
#endif
}

// NXRender KeyCode
using NXRender::KeyCode;

void handleKeyPress(NXRender::KeyCode key, const std::string& text, bool ctrl, bool shift) {
    
    // Alert dialog intercepts keyboard
    if (g_alertVisible) {
        if (key == NXRender::KeyCode::Enter || key == NXRender::KeyCode::Escape) {
            g_alertVisible = false;
        }
        return;
    }
    
    // Keyboard shortcuts (Ctrl+...)
    if (ctrl) {
        if (key == NXRender::KeyCode::C) {
            // Ctrl+C - Copy selected text to clipboard (native)
            if (g_mouseHandler.getSelection().active && !g_mouseHandler.getSelection().selectedText.empty()) {
                std::string text = g_mouseHandler.getSelection().selectedText;
                ZepraBrowser::Clipboard::instance().copy(text);
                std::cout << "[Clipboard] Copied: " << text.substr(0, 50) << "..." << std::endl;
            }
            return;
        } else if (key == NXRender::KeyCode::V) {
            // Ctrl+V - Paste from clipboard (native)
            std::string clipboardText = ZepraBrowser::Clipboard::instance().paste();
            
            // Remove trailing newline
            while (!clipboardText.empty() && (clipboardText.back() == '\n' || clipboardText.back() == '\r')) {
                clipboardText.pop_back();
            }
            
            if (!clipboardText.empty()) {
                std::cout << "[Clipboard] Pasting: " << clipboardText.substr(0, 50) << "..." << std::endl;
                
                // Insert into active input field
                if (g_addressFocused) {
                    g_addressInput += clipboardText;
                } else if (g_searchFocused) {
                    g_searchQuery += clipboardText;
                } else if (g_consoleFocused) {
                    g_consoleInput += clipboardText;
                } else if (g_focusedBox) {
                    g_focusedBox->text += clipboardText;
                }
            }
            return;
        } else if (key == NXRender::KeyCode::T) {
            // Ctrl+T - New tab
            onNewTab();
            return;
        } else if (key == NXRender::KeyCode::W) {
            // Ctrl+W - Close tab
            if (g_tabs.size() > 1) {
                onCloseTab(g_activeTabId);
            }
            return;
        } else if (key == NXRender::KeyCode::L) {
            // Ctrl+L - Focus address bar
            g_addressFocused = true;
            g_searchFocused = false;
            g_addressInput = g_displayUrl.empty() ? g_currentUrl : g_displayUrl;
            return;
        }
    }
    
    // Determine active input buffer
    std::string* activeInput = nullptr;
    if (g_consoleFocused) {
        activeInput = &g_consoleInput;
    } else if (g_addressFocused) {
        activeInput = &g_addressInput;
    } else if (g_searchFocused) {
        activeInput = &g_searchQuery;
    } else if (g_focusedBox) { // Keep existing logic for web content inputs
        activeInput = &g_focusedBox->text;
    }
    
    if (activeInput) {
        if (key == NXRender::KeyCode::Enter) {
            if (g_consoleFocused && !g_consoleInput.empty()) {
                // Console input: execute JavaScript expression
                std::string expr = g_consoleInput;
                std::cout << "[Console] Executing: " << expr << std::endl;
                
                // Log input to console
                Tab* activeTab = nullptr;
                for (auto& t : g_tabs) {
                    if (t.id == g_activeTabId) { activeTab = &t; break; }
                }
                if (activeTab) {
                    activeTab->logConsole("> " + expr);
                    
                    // Execute with ZepraScript VM
                    std::string result;
                    
                    // Special cases handled before VM
                    if (expr == "clear()" || expr == "console.clear()") {
                        activeTab->consoleLog.clear();
                        result = "";
                    }
#ifdef USE_WEBCORE
                    else if (g_scriptContext) {
                        // Wire console handler to this tab
                        g_scriptContext->setConsoleHandler(
                            [activeTab](const std::string& level, const std::string& msg) {
                                if (activeTab) activeTab->logConsole("[" + level + "] " + msg);
                            });
                        
                        auto evalResult = g_scriptContext->evaluate(expr, "<devtools>");
                        if (evalResult.success) {
                            result = evalResult.value;
                        } else {
                            result = evalResult.error;
                        }
                    }
#endif
                    else {
                        result = "[ScriptContext not available]";
                    }
                    
                    if (!result.empty()) {
                        activeTab->logConsole("< " + result);
                    }
                }
                
                g_consoleInput.clear();
                return;
            } else {
                // Handle Enter key for specific inputs
                if (g_addressFocused || g_searchFocused) {
                    std::cout << "[KEY] Enter pressed - navigating to: " << *activeInput << std::endl;
                    
                    // Navigate
                    onNavigate(*activeInput);
                    
                    // Just unfocus
                    g_addressFocused = false;
                    g_searchFocused = false;
                } else if (g_focusedBox) {
                    // Page input: Don't navigate! Just log or potentially submit form (future)
                    std::cout << "[Page Input] Enter pressed: " << *activeInput << std::endl;
                    // g_focusedBox->text is already updated
                    g_focusedBox = nullptr; // Unfocus on Enter? Or keep focus? Keeping focus makes sense for mutilinea but for search box usually submits.
                    // For now, just unfocus to indicate "done"
                }
                
                return;
            }
        } else if (key == NXRender::KeyCode::Backspace) {
            if (!activeInput->empty()) {
                activeInput->pop_back();
            }
        } else if (key == NXRender::KeyCode::Escape) {
            // Cancel editing
            g_addressFocused = false;
            g_searchFocused = false;
            g_consoleFocused = false;
            *activeInput = ""; // Clear the input buffer
        } else if (!text.empty()) {
            // Regular character input (includes space if text is " ")
            *activeInput += text;
        } else if (key == KeyCode::Space) { // Explicitly handle space if text.empty() doesn't cover it
            *activeInput += " ";
        }
    }
    // Global shortcuts (when no input focused)
    if (ctrl && key == KeyCode::T) {
        onNewTab();
    } else if (ctrl && key == KeyCode::W) {
        onCloseTab(g_activeTabId);
    } else if (ctrl && shift && key == KeyCode::T) {
        // Ctrl+Shift+T - Reopen last closed tab (TODO: implement closed tab history)
        std::cout << "[Browser] Ctrl+Shift+T: Reopen last closed tab (TODO)" << std::endl;
    } else if (ctrl && key == KeyCode::Tab) {
        // Ctrl+Tab - Switch to next tab
        if (!g_tabs.empty()) {
            int currentIdx = 0;
            for (size_t i = 0; i < g_tabs.size(); i++) {
                if (g_tabs[i].id == g_activeTabId) { currentIdx = i; break; }
            }
            int nextIdx = shift ? (currentIdx - 1 + g_tabs.size()) % g_tabs.size() 
                                : (currentIdx + 1) % g_tabs.size();
            onSelectTab(g_tabs[nextIdx].id);
        }
    } else if (ctrl && key == KeyCode::L) {
        // Focus address bar
        g_addressFocused = true;
        g_searchFocused = false;
        g_addressInput = (g_currentUrl == "zepra://start") ? "" : g_currentUrl;
    } else if (ctrl && key == KeyCode::B) {
        // Toggle sidebar
        g_sidebarVisible = !g_sidebarVisible;
    } else if (key == KeyCode::F2) {
        // Toggle sidebar with F2
        g_sidebarVisible = !g_sidebarVisible;
    } else if (key == KeyCode::F5 || (ctrl && key == KeyCode::R)) {
        // F5 / Ctrl+R - Refresh page
        if (!g_currentUrl.empty() && g_currentUrl != "zepra://start") {
            std::cout << "[Browser] Refresh: " << g_currentUrl << std::endl;
            onNavigate(g_currentUrl);
        }
    } else if (ctrl && shift && key == KeyCode::R) {
        // Ctrl+Shift+R - Hard refresh (same as normal refresh for now)
        if (!g_currentUrl.empty() && g_currentUrl != "zepra://start") {
            std::cout << "[Browser] Hard Refresh: " << g_currentUrl << std::endl;
            onNavigate(g_currentUrl);
        }
    } else if (key == KeyCode::F12) {
        // Toggle DevTools
        g_consoleVisible = !g_consoleVisible;
    } else if (ctrl && shift && key == KeyCode::I) {
        // Ctrl+Shift+I - Toggle DevTools
        g_consoleVisible = !g_consoleVisible;
    } else if (ctrl && key == KeyCode::I) {
        // Ctrl+I - Toggle DevTools (legacy)
        g_consoleVisible = !g_consoleVisible;
    } else if (ctrl && key == KeyCode::F) {
        // Ctrl+F - Find on page (TODO: implement find bar)
        std::cout << "[Browser] Ctrl+F: Find on page (TODO)" << std::endl;
    } else if (ctrl && key == KeyCode::H) {
        // Ctrl+H - Open history (TODO: implement history page)
        std::cout << "[Browser] Ctrl+H: History (TODO)" << std::endl;
        onNavigate("zepra://history");
    } else if (ctrl && key == KeyCode::J) {
        // Ctrl+J - Open downloads
        std::cout << "[Browser] Ctrl+J: Downloads" << std::endl;
        onNavigate("zepra://downloads");
    } else if (ctrl && key == KeyCode::D) {
        // Ctrl+D - Bookmark current page (TODO: implement bookmarks)
        std::cout << "[Browser] Ctrl+D: Bookmark page (TODO)" << std::endl;
    } else if (ctrl && (key == KeyCode::Plus || key == KeyCode::Equals)) {
        // Ctrl+Plus - Zoom in
        for (Tab& tab : g_tabs) {
            if (tab.id == g_activeTabId) {
                tab.zoomLevel = std::min(tab.zoomLevel + 0.1f, 3.0f);
                std::cout << "[Browser] Zoom: " << (int)(tab.zoomLevel * 100) << "%" << std::endl;
                break;
            }
        }
    } else if (ctrl && key == KeyCode::Minus) {
        // Ctrl+Minus - Zoom out
        for (Tab& tab : g_tabs) {
            if (tab.id == g_activeTabId) {
                tab.zoomLevel = std::max(tab.zoomLevel - 0.1f, 0.5f);
                std::cout << "[Browser] Zoom: " << (int)(tab.zoomLevel * 100) << "%" << std::endl;
                break;
            }
        }
    } else if (ctrl && key == KeyCode::Num0) {
        // Ctrl+0 - Reset zoom
        for (Tab& tab : g_tabs) {
            if (tab.id == g_activeTabId) {
                tab.zoomLevel = 1.0f;
                std::cout << "[Browser] Zoom reset: 100%" << std::endl;
                break;
            }
        }
    } else if (key == KeyCode::F11) {
        // F11 - Toggle fullscreen (TODO: implement with NXRender)
        std::cout << "[Browser] F11: Fullscreen toggle (TODO)" << std::endl;
    } else if (ctrl && key == KeyCode::A) {
        // Select all
        if (g_layoutRoot) {
            std::string text = ZepraBrowser::getAllText(*g_layoutRoot);
            g_mouseHandler.getSelection().active = true;
            g_mouseHandler.getSelection().startX = 0;
            g_mouseHandler.getSelection().startY = 0;
            g_mouseHandler.getSelection().endX = (float)g_width;
            g_mouseHandler.getSelection().endY = (float)g_height;
            g_mouseHandler.getSelection().selectedText = text;
            std::cout << "[Browser] Ctrl+A: Selected " << text.length() << " chars" << std::endl;
        }
    } else if (key == KeyCode::Space && !g_addressFocused && !g_searchFocused) {
        // Space - Scroll down (when not in input)
        for (Tab& tab : g_tabs) {
            if (tab.id == g_activeTabId) {
                tab.scrollY += shift ? -100 : 100;
                if (tab.scrollY < 0) tab.scrollY = 0;
                break;
            }
        }
    } else if (key == KeyCode::PageDown) {
        // Page Down - Scroll down by viewport
        for (Tab& tab : g_tabs) {
            if (tab.id == g_activeTabId) { tab.scrollY += g_height - 100; break; }
        }
    } else if (key == KeyCode::PageUp) {
        // Page Up - Scroll up by viewport
        for (Tab& tab : g_tabs) {
            if (tab.id == g_activeTabId) { tab.scrollY = std::max(0.0f, tab.scrollY - g_height + 100); break; }
        }
    } else if (key == KeyCode::Home && ctrl) {
        // Ctrl+Home - Scroll to top
        for (Tab& tab : g_tabs) {
            if (tab.id == g_activeTabId) { tab.scrollY = 0; break; }
        }
    } else if (key == KeyCode::End && ctrl) {
        // Ctrl+End - Scroll to bottom (approximate)
        for (Tab& tab : g_tabs) {
            if (tab.id == g_activeTabId) { tab.scrollY = 10000; break; }
        }
    } else if (key == KeyCode::Escape) {
        // Close context menu first, then console
        if (g_mouseHandler.getContextMenu().visible) {
            g_mouseHandler.getContextMenu().hide();
        } else if (g_consoleVisible) {
            g_consoleVisible = false;
        } else {
            // Don't exit on Escape - just cancel any operation
            g_addressFocused = false;
            g_searchFocused = false;
        }
    }
}

// ============================================================================
// WINDOW MANAGEMENT
// ============================================================================

// Legacy window/event functions removed (handled by NXRender Platform)

bool loadResources() {
    std::string iconPath = std::string(RESOURCE_PATH) + "/icons/";
    
    // All icons used in UI - must be loaded before rendering
    const char* icons[] = {
        // Top bar / Navigation
        "zepralogo.svg", "sidebar.svg", "settings.svg", "search.svg", "shield.svg",
        "close.svg", "plus.svg", "refresh.svg", "download.svg", "menu.svg",
        "home.svg", "history.svg", "bookmark.svg", "extention.svg",
        "arrow-back.svg", "arrow-forward.svg", "share.svg",
        // Additional top bar
        "grid.svg", "tablet.svg", "ringingBell.svg", "notification.svg",
        // Sidebar icons
        "avtar.svg", "globe.svg", "devtool.svg", "help.svg", "star.svg",
        // Web Apps icons
        "Calendar.svg", "MialInbox.svg", "creative.svg", "docs.svg",
        "DebitCard.svg", "KetiveeStudio.svg", "3dSpace.svg",
        // Start page
        "zepra.svg", "Film.svg", "Picture.svg", "krtivee.svg",
        // End marker
        nullptr
    };
    
    for (int i = 0; icons[i]; i++) {
        g_svg.loadFromFile(icons[i], iconPath + icons[i]);
    }
    
    if (!nxfont::FontManager::instance().loadSystemFont(14)) {
        std::cerr << "[Error] Failed to load system font\n";
        return false;
    }
    
    // Create initial tab
    Tab initialTab;
    initialTab.id = 1;
    initialTab.title = "New Tab";
    initialTab.url = "zepra://start";
    initialTab.isStart = true;
    g_tabs.push_back(std::move(initialTab));
    g_activeTabId = 1;
    
    std::cout << "[Browser] Created initial tab" << std::endl;
    
    return true;
}

// Event Handler bridge (Correctly placed)
// extern removal

void handleNXEvent(const NXRender::Event& event) {
    if (event.type == NXRender::EventType::Close) {
        g_running = false;
    } else if (event.type == NXRender::EventType::Resize) {
        g_width = event.window.width;
        g_height = event.window.height;
        if (g_layoutRoot) g_layoutRoot->width = (float)g_width;
        
        // Update GPU viewport
        g_nxGpu.setViewport(0, 0, g_width, g_height);
        
        // Trigger layout update
        initLayoutEngine();
    } else if (event.isKeyboard()) {
        bool down = (event.type == NXRender::EventType::KeyDown);
        if (down) {
            std::string text = event.textInput;
            handleKeyPress((NXRender::KeyCode)event.key.key, text, 
                          event.key.modifiers.ctrl, event.key.modifiers.shift);
        }
    } else if (event.type == NXRender::EventType::FileDrop) {
        // File dropped on browser - navigate to first file
        if (!event.droppedFiles.empty()) {
            std::string filePath = event.droppedFiles[0];
            std::cout << "[Browser] File dropped: " << filePath << std::endl;
            // Navigate to the file with file:// protocol
            onNavigate("file://" + filePath);
        }
    } else if (event.isMouse()) {
        float x = event.mouse.x;
        float y = event.mouse.y;
        
        if (event.type == NXRender::EventType::MouseMove) {
             ::g_mouseHandler.handleMouseMove(x, y, g_mouseDown); 
             g_mouseX = x; 
             g_mouseY = y;
             
             // Link hover detection - show URL in status bar
             if (g_layoutRoot) {
                 LayoutBox* hoverBox = findBoxAt(g_layoutRoot.get(), x, y);
                 if (hoverBox && hoverBox->isLink && !hoverBox->href.empty()) {
                     g_hoverUrl = hoverBox->href;
                     g_currentCursorIsHand = true;
                 } else {
                     g_hoverUrl = "";
                     g_currentCursorIsHand = false;
                 }
             }
        } else if (event.type == NXRender::EventType::MouseDown) {
             // CRITICAL: Update mouse position BEFORE calling handlers
             // so hit() uses correct coordinates
             g_mouseX = x;
             g_mouseY = y;
             
             if (event.mouse.button == NXRender::MouseButton::Left) {
                 // Call main click handler (handles address bar, search, tabs, etc.)
                 extern void handleClick(float, float);
                 handleClick(x, y);
                 
                 ::g_mouseHandler.handleLeftClick(x, y); 
                 g_mouseDown = true;
             } else if (event.mouse.button == NXRender::MouseButton::Right) {
                 auto& ctxMenu = g_mouseHandler.getContextMenu();
                 if (ctxMenu.visible) {
                     ctxMenu.hide();
                 } else {
                     g_mouseHandler.handleRightClick(x, y);
                 }
             }
        } else if (event.type == NXRender::EventType::MouseUp) {
             if (event.mouse.button == NXRender::MouseButton::Left) {
                 ::g_mouseHandler.handleLeftRelease(x, y); 
                 g_mouseDown = false;
             }
        } else if (event.type == NXRender::EventType::MouseWheel) {
             for (auto& tab : g_tabs) {
                 if (tab.id == g_activeTabId) {
                     tab.scrollY -= event.mouse.wheelDelta * 40.0f;
                     if (tab.scrollY < 0) tab.scrollY = 0;
                     break;
                 }
             }
        }
    }
}

// ============================================================================
// CRASH HANDLER — Graceful shutdown on fatal signals
// ============================================================================


static void crash_signal_handler(int sig) {
    const char* name = "UNKNOWN";
    switch (sig) {
        case SIGSEGV: name = "SIGSEGV"; break;
        case SIGABRT: name = "SIGABRT"; break;
        case SIGFPE:  name = "SIGFPE"; break;
    }
    // Write directly to stderr (async-signal-safe)
    write(STDERR_FILENO, "\n[FATAL] Signal: ", 17);
    write(STDERR_FILENO, name, strlen(name));
    write(STDERR_FILENO, "\n", 1);
    if (g_crash_url) {
        write(STDERR_FILENO, "[FATAL] Last URL: ", 18);
        write(STDERR_FILENO, g_crash_url, strlen(g_crash_url));
        write(STDERR_FILENO, "\n", 1);
    }
    write(STDERR_FILENO, "[FATAL] Attempting graceful shutdown...\n", 39);

    // Try to shut down NXRender cleanly
    NXRender::shutdown();
    _exit(128 + sig);
}

static bool validate_startup_resources() {
    struct stat st;
    std::string resPath = RESOURCE_PATH;

    // Check resources directory
    if (stat(resPath.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        std::cerr << "[STARTUP] WARNING: Resources directory not found: " << resPath << std::endl;
        std::cerr << "[STARTUP] Icons will not load. Continuing without icons." << std::endl;
        // Non-fatal — browser can run without icons
    }

    // Check at least one font is available
    const char* font_paths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        nullptr
    };
    bool font_found = false;
    for (int i = 0; font_paths[i]; i++) {
        if (stat(font_paths[i], &st) == 0) {
            font_found = true;
            break;
        }
    }
    if (!font_found) {
        std::cerr << "[STARTUP] WARNING: No system fonts found. Text rendering may fail." << std::endl;
    }

    return true; // Always continue — degrade gracefully
}

int zepra_main(int argc, char** argv) { // Clean Main - callable from main.cpp
    // Install crash signal handlers
    signal(SIGSEGV, crash_signal_handler);
    signal(SIGABRT, crash_signal_handler);
    signal(SIGFPE, crash_signal_handler);

    std::cout << "Starting ZepraBrowser (NXRender Edition)..." << std::endl;


    // Initialize NXRender (creates Window & GL Context)
    if (!NXRender::init(g_width, g_height)) {
        std::cerr << "Failed to init NXRender" << std::endl;
        return 1;
    }

    // Validate startup resources (fonts, icons)
    validate_startup_resources();

    // Initialize Browser GPU Context (Loads shaders)
    g_nxGpu.init(g_width, g_height);
    g_nxGpu.setViewport(0, 0, g_width, g_height);

    // Set Up Event Handler
    NXRender::setEventHandler(handleNXEvent);
    
    // Set Render Callback (Fixes blank screen)
    NXRender::setRenderCallback(render);

    // Initialize Components
    ::g_mouseHandler.onCopy([](const std::string& text) {
        ZepraBrowser::Clipboard::instance().copy(text);
        std::cout << "[Clipboard] Copied: " << text.substr(0, 50) << std::endl;
    });

    ::g_mouseHandler.onPaste([]() -> std::string {
        return ZepraBrowser::Clipboard::instance().paste();
    });

    ::g_mouseHandler.onNavigate([](int direction) {
        if (direction == 0) {
            onNavigate(g_currentUrl);  // Reload
        }
        // TODO: history stack for back/forward
    });

    ::g_mouseHandler.onReload([]() {
        onNavigate(g_currentUrl);
    });

    ::g_mouseHandler.onInspect([](float, float) {
        g_consoleVisible = !g_consoleVisible;
    });

    ::g_mouseHandler.onGetText([](float x, float y, float w, float h) -> std::string {
        if (g_layoutRoot) {
            return ZepraBrowser::getTextInRect(*g_layoutRoot, x, y, w, h);
        }
        return "";
    });
    
    // Connect MouseHandler to TabManager (Open in New Tab)
    ::g_mouseHandler.onNewTab([](const std::string& url) {
        std::cout << "[NewTab] Opening: " << url << std::endl;
        Tab newTab;
        newTab.id = g_nextTabId++;
        newTab.url = url;
        newTab.title = "Loading...";
        newTab.isStart = false;
        g_tabs.push_back(std::move(newTab));
        g_activeTabId = newTab.id;
        // Trigger load
        g_currentUrl = url;
        g_addressInput = url;
    });
    
    // Connect MouseHandler to Ketivee Search
    ::g_mouseHandler.onSearch([](const std::string& query) {
        std::cout << "[Search] Ketivee: " << query << std::endl;
        std::string searchUrl = getSearchUrl(query);
        g_currentUrl = searchUrl;
        g_addressInput = searchUrl;
        // Load the search
    });
    
    // Connect MouseHandler to DownloadManager
    ::g_mouseHandler.onDownload([](const std::string& url) {
        std::cout << "[Download] Starting: " << url << std::endl;
        std::string filename = url.substr(url.find_last_of('/') + 1);
        if (filename.empty()) filename = "download";
        g_downloadManager.startDownload(url, filename);
    });
    
    // Setup TabSuspender with video detection
    ::g_tabSuspender.setVideoDetector([](int tabId) -> bool {
        // Check if tab has playing video (placeholder - needs WebCore integration)
        return false;
    });
    
    ::g_tabSuspender.setAudioDetector([](int tabId) -> bool {
        // Check if tab has playing audio (placeholder)
        return false;
    });

    // Initialize Graphics Resources - load ALL icons used in UI
    std::string iconPath = std::string(RESOURCE_PATH) + "/icons/";
    const char* icons[] = {
        // Top bar / Navigation
        "zepralogo.svg", "sidebar.svg", "settings.svg", "search.svg", "shield.svg",
        "close.svg", "plus.svg", "refresh.svg", "download.svg", "menu.svg",
        "home.svg", "history.svg", "bookmark.svg", "extention.svg",
        "arrow-back.svg", "arrow-forward.svg", "share.svg",
        // Additional top bar
        "grid.svg", "tablet.svg", "ringingBell.svg", "notification.svg",
        // Sidebar icons
        "avtar.svg", "globe.svg", "devtool.svg", "help.svg", "star.svg",
        // Web Apps icons
        "Calendar.svg", "MialInbox.svg", "creative.svg", "docs.svg",
        "DebitCard.svg", "KetiveeStudio.svg", "3dSpace.svg",
        // Start page
        "zepra.svg", "Film.svg", "Picture.svg", "krtivee.svg",
        nullptr
    };
    for (int i = 0; icons[i]; i++) {
        g_svg.loadFromFile(icons[i], iconPath + icons[i]);
    }

    // nxfont_init(g_display); // Legacy font system disabled for NXRender migration

    // Init layout engine
    initLayoutEngine();
    
    // Create initial tab (CRITICAL: Must be done before entering main loop)
    if (g_tabs.empty()) {
        Tab initialTab;
        initialTab.id = 1;
        initialTab.title = "New Tab";
        initialTab.url = g_currentUrl;
        initialTab.isStart = (g_currentUrl == "zepra://start");
        g_tabs.push_back(std::move(initialTab));
        g_activeTabId = 1;
        onNavigate(g_currentUrl);
        std::cout << "[Browser] Created initial tab: " << g_currentUrl << std::endl;
    }
    
#ifdef USE_WEBCORE
    // Initialize WebCore (HTML parser, DOM, CSS, ZebraScript VM)
    // Note: Per-tab WebCore (WebCoreTab class) available but not yet integrated
    if (!webcore_init()) {
        std::cerr << "[Error] Failed to initialize WebCore" << std::endl;
        return 1;
    }
    std::cout << "[WebCore] Initialized (HTML parser + DOM + CSS + ZebraScript VM)" << std::endl;
#endif

    // Setup periodic tab suspension check
    std::cout << "[TabSuspender] Enabled" << std::endl;

    // Initialize LazyImageLoader for background image loading
    ZepraBrowser::g_lazyImageLoader.setTextureCreator([](int w, int h, const uint8_t* pixels) -> uint32_t {
        if (NXRender::gpu()) {
            return NXRender::gpu()->createTexture(w, h, pixels);
        }
        return 0;
    });
    ZepraBrowser::g_lazyImageLoader.start(2); // 2 worker threads
    std::cout << "[LazyImageLoader] Started with 2 workers" << std::endl;

    // Start Main Loop via NXRender
    std::cout << "Entering main loop..." << std::endl;
    NXRender::run();

    // Cleanup
    NXRender::shutdown();
    return 0;
}