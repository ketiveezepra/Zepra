// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * webcore_dom_bindings_tests.cpp - Page scripts change the real DOM: live
 * wrappers (one object per node), every mutation reaching the C++ tree,
 * DOM events, the lifetime of nodes scripts remove, window as the global
 * object, and localStorage/sessionStorage.
 */

#include <gtest/gtest.h>

#include "browser/dom.hpp"
#include "html/html_parser.hpp"
#include "scripting/dom_bindings.hpp"
#include "scripting/page_url.hpp"
#include "scripting/script_context.hpp"
#include "scripting/web_storage.hpp"
#include <filesystem>
#include <fstream>

#include <map>
#include <memory>
#include <string>
#include <vector>

// Viewport size the layout engine reads (zepra_browser.cpp defines these)
int g_width = 1280;
int g_height = 800;

using namespace Zepra::WebCore;

namespace {

struct Page {
    std::unique_ptr<DOMDocument> doc;
    std::unique_ptr<ScriptContext> script;
    std::vector<std::string> console;

    explicit Page(const std::string& html) {
        HTMLParser parser;
        doc = parser.parse(html);
        script = std::make_unique<ScriptContext>();
        script->setConsoleHandler([this](const std::string& level, const std::string& message) {
            console.push_back(level + ": " + message);
        });
        script->initialize(doc.get());
    }

    /// Runs code and returns its completion value as a string
    std::string eval(const std::string& code) {
        ScriptResult r = script->evaluate(code, "test.js");
        EXPECT_TRUE(r.success) << r.error << "\n  in: " << code;
        return r.success ? r.value : "<error: " + r.error + ">";
    }

    DOMElement* byId(const std::string& id) { return doc->getElementById(id); }
};

const char* kPage = R"(<!DOCTYPE html><html><head><title> Live  DOM </title></head><body>
<div id="root" class="box main"><p id="first">one</p><p id="second" class="note">two</p></div>
<ul id="list"><li>a</li><li class="x">b</li></ul>
</body></html>)";

}  // namespace

TEST(DomBindings, OneWrapperPerNode) {
    Page page(kPage);
    EXPECT_EQ(page.eval("document.getElementById('root') === document.getElementById('root')"), "true");
    EXPECT_EQ(page.eval("document.querySelector('#first') === document.getElementById('root').firstElementChild"), "true");
    EXPECT_EQ(page.eval("document.body === document.documentElement.lastElementChild"), "true");
    EXPECT_EQ(page.eval("document.getElementById('first').parentNode === document.getElementById('root')"), "true");
    EXPECT_EQ(page.eval("document.getElementById('root').tagName + ' ' + document.title"), "DIV Live DOM");
}

TEST(DomBindings, InterfacesAndInstanceof) {
    Page page(kPage);
    EXPECT_EQ(page.eval("[document.body instanceof HTMLBodyElement, document.body instanceof HTMLElement,"
                        " document.body instanceof Element, document.body instanceof Node,"
                        " document.body instanceof EventTarget, document instanceof HTMLDocument,"
                        " document.createElement('canvas') instanceof HTMLCanvasElement,"
                        " document.createTextNode('t') instanceof Text, window instanceof EventTarget].join()"),
              "true,true,true,true,true,true,true,true,true");
    EXPECT_EQ(page.eval("String(document.getElementById('root'))"), "[object HTMLDivElement]");
    EXPECT_EQ(page.eval("Node.ELEMENT_NODE + document.body.TEXT_NODE"), "4");
}

TEST(MutationObserver, ChildListRecordsArriveInAMicrotask) {
    Page page(kPage);
    page.eval("var seen = []; var root = document.getElementById('root');"
              "var mo = new MutationObserver(function (records, observer) {"
              "  seen.push(records.length + ':' + (observer === mo) + ':' + (this === mo));"
              "  records.forEach(function (r) {"
              "    seen.push(r.type + ' +' + r.addedNodes.length + ' -' + r.removedNodes.length +"
              "              ' prev=' + (r.previousSibling && r.previousSibling.id) +"
              "              ' next=' + (r.nextSibling && r.nextSibling.id) + ' ' + (r instanceof MutationRecord));"
              "  });"
              "});"
              "mo.observe(root, { childList: true });"
              "var p = document.createElement('p'); p.id = 'third'; root.appendChild(p);"
              "root.removeChild(document.getElementById('first'));"
              "seen.push('sync done');");
    // Both records come in one batch, after the script
    EXPECT_EQ(page.eval("seen.join(' | ')"),
              "sync done | 2:true:true | childList +1 -0 prev=second next=null true | "
              "childList +0 -1 prev=null next=second true");
}

TEST(MutationObserver, SubtreeAttributesAndCharacterData) {
    Page page(kPage);
    page.eval("var seen = []; var mo = new MutationObserver(function (records) {"
              "  records.forEach(function (r) { seen.push(r.type + ':' + (r.target.id || r.target.nodeName) + ':' +"
              "    r.attributeName + ':' + r.oldValue); }); });"
              "mo.observe(document.body, { subtree: true, attributeOldValue: true, characterDataOldValue: true,"
              "                            attributeFilter: ['class', 'title'] });"
              "var second = document.getElementById('second');"
              "second.className = 'changed';"
              "second.setAttribute('data-x', '1');"      // filtered out
              "second.title = 'hello';"
              "second.firstChild.data = 'TWO';");
    EXPECT_EQ(page.eval("seen.join(' ')"),
              "attributes:second:class:note attributes:second:title:null characterData:#text:null:two");
    // Without subtree, a change below the target is not seen
    page.eval("seen = []; mo.disconnect(); mo.observe(document.body, { attributes: true });"
              "second.className = 'again'; document.body.setAttribute('data-y', '2');");
    EXPECT_EQ(page.eval("seen.join(' ')"), "attributes:BODY:data-y:null");
}

TEST(MutationObserver, TakeRecordsDisconnectAndErrors) {
    Page page(kPage);
    page.eval("var calls = 0; var mo = new MutationObserver(function () { calls++; });"
              "mo.observe(document.getElementById('list'), { childList: true });"
              "document.getElementById('list').appendChild(document.createElement('li'));"
              "var taken = mo.takeRecords();");
    // takeRecords() empties the queue, so the callback never runs
    EXPECT_EQ(page.eval("taken.length + ',' + taken[0].type + ',' + calls"), "1,childList,0");
    page.eval("mo.disconnect(); document.getElementById('list').appendChild(document.createElement('li'));");
    EXPECT_EQ(page.eval("calls"), "0");
    EXPECT_EQ(page.eval("(function () { try { mo.observe(document.body, {}); } catch (e) { return e.name; } })()"),
              "TypeError");
    EXPECT_EQ(page.eval("(function () { try { new MutationObserver(1); } catch (e) { return e.name; } })()"),
              "TypeError");
    // An error in a callback goes to the console and does not stop other observers
    page.eval("var ok = 0;"
              "new MutationObserver(function () { throw new Error('boom'); }).observe(document.body, { attributes: true });"
              "new MutationObserver(function () { ok++; }).observe(document.body, { attributes: true });"
              "document.body.setAttribute('x', '1');");
    EXPECT_EQ(page.eval("ok"), "1");
    bool reported = false;
    for (const auto& line : page.console) reported = reported || line.find("boom") != std::string::npos;
    EXPECT_TRUE(reported);
}

TEST(DomBindings, WindowIsTheGlobalObject) {
    Page page(kPage);
    // var and function declarations at script level are window properties
    EXPECT_EQ(page.eval("var answer = 42; function hello() { return 'hi'; }"
                        " [window.answer, window.hello(), 'answer' in window].join()"),
              "42,hi,true");
    // What a bundle puts on window is a bare global in later scripts (UMD, jQuery's $)
    page.eval("(function (root) { root.Lib = { v: 7 }; root.$ = function () { return 'jq'; }; })(this);");
    EXPECT_EQ(page.eval("Lib.v + ',' + $()"), "7,jq");
    EXPECT_EQ(page.eval("this === window && globalThis === window && self === window"), "true");
    // Assigning an undeclared name makes a window property
    EXPECT_EQ(page.eval("implicitGlobal = 5; window.implicitGlobal"), "5");
    // A script's var names exist (undefined) before it runs, even in a branch not taken
    EXPECT_EQ(page.eval("var before = typeof laterVar; if (false) { var laterVar = 1; } before + ',' + laterVar"),
              "undefined,undefined");
    // Reading an undeclared name throws; typeof does not
    EXPECT_EQ(page.eval("(function () { try { notDeclaredAnywhere; return 'no'; }"
                        " catch (e) { return e.name + ': ' + e.message; } })()"),
              "ReferenceError: notDeclaredAnywhere is not defined");
    EXPECT_EQ(page.eval("typeof notDeclaredAnywhere"), "undefined");
    // Built-ins and DOM interfaces are the same either way, and for-in skips them
    EXPECT_EQ(page.eval("[window.Array === Array, window.document === document,"
                        " window.HTMLElement === HTMLElement, window.setTimeout === setTimeout].join()"),
              "true,true,true,true");
    EXPECT_EQ(page.eval("var seen = []; for (var k in window) { if (k === 'Array' || k === 'JSON') seen.push(k); }"
                        " seen.length"),
              "0");
}

TEST(DomBindings, TopLevelLetConstAndClassAreNotWindowProperties) {
    Page page(kPage);
    // Globals every later script sees, but not properties of window
    page.eval("let counter = 1; const LIMIT = 3; class Widget { static make() { return new Widget(); } }");
    EXPECT_EQ(page.eval("[counter, LIMIT, typeof Widget, Widget.make() instanceof Widget].join()"),
              "1,3,function,true");
    EXPECT_EQ(page.eval("[window.counter, window.LIMIT, window.Widget, 'counter' in window,"
                        " Object.keys(window).indexOf('LIMIT')].join()"),
              ",,,false,-1");
    // Functions from an earlier script read and write them
    page.eval("function bump() { counter++; return counter; }");
    EXPECT_EQ(page.eval("bump(); bump() + ',' + counter"), "3,3");
    // A let shadows a window property of the same name without changing it
    page.eval("window.shadowed = 'prop'; let shadowed = 'let';");
    EXPECT_EQ(page.eval("shadowed + ',' + window.shadowed"), "let,prop");
    // const cannot be assigned; the binding keeps its value
    EXPECT_EQ(page.eval("(function () { try { LIMIT = 4; return 'no'; } catch (e) { return e.name; } })() + ',' + LIMIT"),
              "TypeError,3");
    // Before its declaration runs the name is in its temporal dead zone
    EXPECT_EQ(page.eval("function early() { try { return lateLet; } catch (e) { return e.name + ': ' + e.message; } }"
                        " var r = early(); let lateLet = 'ok'; r + ' / ' + early()"),
              "ReferenceError: Cannot access 'lateLet' before initialization / ok");
    // Declaring the same name again in a later script stops that script before it runs
    ScriptResult again = page.script->evaluate("var ranAgain = true; let counter = 9;", "again.js");
    EXPECT_FALSE(again.success);
    EXPECT_NE(again.error.find("SyntaxError"), std::string::npos) << again.error;
    EXPECT_EQ(page.eval("[counter, typeof ranAgain].join()"), "3,undefined");
    // Destructuring declares every name
    page.eval("const { first: firstName, rest = 'r' } = { first: 'f' }; let [one, , three] = [1, 2, 3];");
    EXPECT_EQ(page.eval("[firstName, rest, one, three, 'one' in window].join()"), "f,r,1,3,false");
}

TEST(DomBindings, AppendChildReachesTheDom) {
    Page page(kPage);
    uint64_t before = domMutationVersion();
    page.eval("var d = document.createElement('DIV'); d.id = 'made'; d.textContent = 'hello';"
              "document.body.appendChild(d); d.isConnected");
    DOMElement* made = page.byId("made");
    ASSERT_NE(made, nullptr);
    EXPECT_EQ(made->tagName(), "div");
    EXPECT_EQ(made->textContent(), "hello");
    EXPECT_EQ(made->parentNode(), page.doc->body());
    EXPECT_GT(domMutationVersion(), before);
    EXPECT_EQ(page.eval("document.getElementById('made') === d"), "true");
}

TEST(DomBindings, RemovedNodesStayUsable) {
    Page page(kPage);
    EXPECT_EQ(page.eval("var p = document.getElementById('first'); var r = p.parentNode.removeChild(p);"
                        "[r === p, p.parentNode, p.isConnected, p.textContent].join()"),
              "true,,false,one");
    EXPECT_EQ(page.byId("first"), nullptr);
    page.eval("document.getElementById('list').appendChild(p)");
    DOMElement* moved = page.byId("first");
    ASSERT_NE(moved, nullptr);
    EXPECT_EQ(moved->parentNode(), page.byId("list"));
    // remove() and moving an attached node from one parent to another
    page.eval("document.getElementById('second').remove(); document.getElementById('root').appendChild(p)");
    EXPECT_EQ(page.byId("second"), nullptr);
    EXPECT_EQ(page.byId("first")->parentNode(), page.byId("root"));
    EXPECT_EQ(page.byId("root")->childNodes().size(), 1u);
}

TEST(DomBindings, InsertBeforeReplaceChildAndFragments) {
    Page page(kPage);
    page.eval("var root = document.getElementById('root');"
              "var a = document.createElement('span'); a.id = 'a';"
              "root.insertBefore(a, document.getElementById('second'));"
              "var f = document.createDocumentFragment();"
              "for (var i = 0; i < 3; i++) { var li = document.createElement('li'); li.textContent = 'n' + i; f.appendChild(li); }"
              "document.getElementById('list').appendChild(f);"
              "var b = document.createElement('b'); b.id = 'b';"
              "root.replaceChild(b, document.getElementById('first'));");
    DOMElement* root = page.byId("root");
    ASSERT_EQ(root->childNodes().size(), 3u);
    EXPECT_EQ(static_cast<DOMElement*>(root->childNodes()[0].get())->id(), "b");
    EXPECT_EQ(static_cast<DOMElement*>(root->childNodes()[1].get())->id(), "a");
    EXPECT_EQ(static_cast<DOMElement*>(root->childNodes()[2].get())->id(), "second");
    EXPECT_EQ(page.byId("list")->childNodes().size(), 5u);
    EXPECT_EQ(page.eval("f.childNodes.length"), "0");
    EXPECT_EQ(page.eval("var old = document.querySelector('#first'); old"), "null");
}

TEST(DomBindings, HierarchyErrorsAreCatchable) {
    Page page(kPage);
    EXPECT_EQ(page.eval("var n; try { document.getElementById('first').appendChild(document.body) } catch (e) { n = e.name } n"),
              "HierarchyRequestError");
    EXPECT_EQ(page.eval("var n; try { document.body.removeChild(document.getElementById('first')) } catch (e) { n = e.name } n"),
              "NotFoundError");
    EXPECT_EQ(page.eval("var n; try { document.body.appendChild({}) } catch (e) { n = e.name } n"), "TypeError");
}

TEST(DomBindings, InnerHTMLParsesAndSerializes) {
    Page page(kPage);
    page.eval("document.getElementById('root').innerHTML = '<b class=\"k\">x</b>text<i>y</i>'");
    DOMElement* root = page.byId("root");
    ASSERT_EQ(root->childNodes().size(), 3u);
    EXPECT_EQ(static_cast<DOMElement*>(root->childNodes()[0].get())->tagName(), "b");
    EXPECT_EQ(root->childNodes()[1]->nodeValue(), "text");
    EXPECT_EQ(page.eval("document.getElementById('root').innerHTML"), "<b class=\"k\">x</b>text<i>y</i>");
    EXPECT_EQ(page.eval("document.querySelector('#root .k').textContent"), "x");
    // Rows parse inside a table body
    page.eval("var t = document.createElement('table'); document.body.appendChild(t);"
              "t.innerHTML = '<tr><td>1</td><td>2</td></tr>'");
    EXPECT_EQ(page.eval("t.querySelectorAll('td').length"), "2");
    page.eval("document.getElementById('list').insertAdjacentHTML('beforeend', '<li id=\"z\">z</li>')");
    ASSERT_NE(page.byId("z"), nullptr);
    EXPECT_EQ(page.byId("z")->parentNode(), page.byId("list"));
}

TEST(DomBindings, TextContentReplacesChildren) {
    Page page(kPage);
    EXPECT_EQ(page.eval("var root = document.getElementById('root'); var p = document.getElementById('first');"
                        "root.textContent = 'plain'; [root.childNodes.length, p.parentNode, p.textContent].join()"),
              "1,,one");
    EXPECT_EQ(page.byId("root")->textContent(), "plain");
    EXPECT_EQ(page.byId("first"), nullptr);
    // A single text child changes in place
    page.eval("root.textContent = 'again'");
    EXPECT_EQ(page.byId("root")->textContent(), "again");
    EXPECT_EQ(page.eval("var t = document.createTextNode('x'); t.data = 'yz'; [t.nodeValue, t.length].join()"), "yz,2");
}

TEST(DomBindings, AttributesClassListAndDataset) {
    Page page(kPage);
    page.eval("var r = document.getElementById('root'); r.classList.add('extra'); r.classList.remove('box');"
              "r.classList.toggle('main'); r.setAttribute('data-user-id', '7'); r.dataset.fooBar = 'baz';"
              "r.title = 'tip'; r.hidden = true");
    DOMElement* root = page.byId("root");
    EXPECT_EQ(root->getAttribute("class"), "extra");
    EXPECT_EQ(root->getAttribute("data-foo-bar"), "baz");
    EXPECT_EQ(root->getAttribute("title"), "tip");
    EXPECT_TRUE(root->hasAttribute("hidden"));
    EXPECT_EQ(page.eval("[r.classList.contains('extra'), r.classList.length, r.className, r.dataset.userId,"
                        " r.getAttribute('missing'), r.hasAttribute('data-user-id'), r.classList === r.classList].join()"),
              "true,1,extra,7,,true,true");
    page.eval("r.removeAttribute('hidden'); r.hidden = false");
    EXPECT_FALSE(root->hasAttribute("hidden"));
}

TEST(DomBindings, InlineStyle) {
    Page page(kPage);
    page.eval("var s = document.getElementById('root').style; s.backgroundColor = 'red'; s.setProperty('margin-top', '4px');"
              "s.width = '10px'; s.width = ''");
    std::string style = page.byId("root")->getAttribute("style");
    EXPECT_NE(style.find("background-color: red"), std::string::npos) << style;
    EXPECT_NE(style.find("margin-top: 4px"), std::string::npos) << style;
    EXPECT_EQ(style.find("width"), std::string::npos) << style;
    EXPECT_EQ(page.eval("[s.backgroundColor, s.getPropertyValue('margin-top'), s.color === '', 'opacity' in s].join()"),
              "red,4px,true,true");
    page.eval("s.cssText = 'color: blue'");
    EXPECT_EQ(page.byId("root")->getAttribute("style"), "color: blue");
}

TEST(DomBindings, Selectors) {
    Page page(kPage);
    EXPECT_EQ(page.eval("document.querySelectorAll('p').length"), "2");
    EXPECT_EQ(page.eval("document.querySelectorAll('#root > p.note, li.x').length"), "2");
    EXPECT_EQ(page.eval("document.querySelector('ul li:last-child').textContent"), "b");
    EXPECT_EQ(page.eval("document.getElementsByClassName('box main').length"), "1");
    EXPECT_EQ(page.eval("document.getElementsByTagName('LI').length"), "2");
    EXPECT_EQ(page.eval("var s = document.getElementById('second'); [s.matches('.note'), s.closest('div').id,"
                        " s.closest('ul')].join()"),
              "true,root,");
    // A C++ caller gets the same answers
    EXPECT_EQ(page.doc->querySelectorAll("#root p").size(), 2u);
    ASSERT_NE(page.doc->querySelector("li.x"), nullptr);
}

TEST(DomBindings, EventsCaptureTargetBubble) {
    Page page(kPage);
    page.eval("var log = [];"
              "window.addEventListener('click', function (e) { log.push('window-capture') }, true);"
              "document.addEventListener('click', function (e) { log.push('document') });"
              "document.getElementById('root').addEventListener('click', function (e) { log.push('root:' + e.eventPhase + ':' + (this === e.currentTarget)) });"
              "document.getElementById('first').addEventListener('click', function (e) { log.push('target:' + e.target.id) });"
              "window.addEventListener('click', function (e) { log.push('window') });");
    EXPECT_TRUE(page.script->dispatchMouseEvent(page.byId("first"), "click", 5, 6));
    EXPECT_EQ(page.eval("log.join(' ')"), "window-capture target:first root:3:true document window");

    // stopPropagation, preventDefault, once, removeEventListener, on* properties
    page.eval("log = []; var f = document.getElementById('first');"
              "function stop(e) { e.stopPropagation(); e.preventDefault(); log.push('stop') }"
              "f.addEventListener('click', stop); f.addEventListener('click', function () { log.push('once') }, { once: true });"
              "f.onclick = function (e) { log.push('onclick:' + e.clientX) };");
    EXPECT_FALSE(page.script->dispatchMouseEvent(page.byId("first"), "click", 5, 6));
    EXPECT_EQ(page.eval("log.join(' ')"), "window-capture target:first stop once onclick:5");
    page.eval("log = []; f.removeEventListener('click', stop)");
    EXPECT_TRUE(page.script->dispatchMouseEvent(page.byId("first"), "click", 1, 1));
    EXPECT_EQ(page.eval("log.join(' ')"), "window-capture target:first onclick:1 root:3:true document window");
}

TEST(DomBindings, SyntheticEventsAndClick) {
    Page page(kPage);
    EXPECT_EQ(page.eval("var got = []; var r = document.getElementById('root');"
                        "r.addEventListener('ping', function (e) { got.push(e.detail + ':' + e.bubbles + ':' + e.isTrusted) });"
                        "document.getElementById('first').dispatchEvent(new CustomEvent('ping', { bubbles: true, detail: 42 }));"
                        "var e = new Event('ping', { cancelable: true }); r.addEventListener('ping', function (ev) { ev.preventDefault() });"
                        "got.push(r.dispatchEvent(e)); got.push(e.defaultPrevented);"
                        "r.onclick = function () { got.push('clicked') }; r.click(); got.join()"),
              "42:true:false,undefined:false:false,false,true,clicked");
}

TEST(DomBindings, InlineHandlerAttributes) {
    Page page(R"(<html><body onload="window.loaded = (window.loaded || 0) + 1">
<button id="b" onclick="window.hits = (window.hits || 0) + 1; return false">go</button></body></html>)");
    EXPECT_FALSE(page.script->dispatchMouseEvent(page.byId("b"), "click", 0, 0));
    EXPECT_TRUE(page.console.empty()) << page.console.front();
    EXPECT_EQ(page.eval("window.hits"), "1");
    page.script->fireLoadEvent();
    EXPECT_EQ(page.eval("window.loaded"), "1");
}

TEST(DomBindings, LifecycleEvents) {
    Page page(kPage);
    page.eval("var seen = [document.readyState];"
              "document.addEventListener('DOMContentLoaded', function () { seen.push('dcl:' + document.readyState) });"
              "window.addEventListener('load', function (e) { seen.push('load:' + document.readyState + ':' + (e.target === window)) });"
              "window.onload = function () { seen.push('onload') };"
              "addEventListener('load', function () { seen.push('bare') });");
    page.script->fireDOMContentLoaded();
    page.script->fireLoadEvent();
    EXPECT_EQ(page.eval("seen.join(' ')"), "loading dcl:interactive load:complete:true bare onload");
}

TEST(DomBindings, ListenerErrorsGoToTheConsole) {
    Page page(kPage);
    page.eval("var after = false; document.body.addEventListener('click', function () { throw new Error('boom') });"
              "document.body.addEventListener('click', function () { after = true });");
    page.script->dispatchMouseEvent(page.doc->body(), "click", 0, 0);
    EXPECT_EQ(page.eval("after"), "true");
    bool reported = false;
    for (const auto& line : page.console) reported = reported || line.find("boom") != std::string::npos;
    EXPECT_TRUE(reported);
}

TEST(DomBindings, DetachedNodesWaitForRelease) {
    Page page(kPage);
    DOMBindings* bindings = page.script->domBindings();
    ASSERT_NE(bindings, nullptr);
    // Unreached children of a cleared element wait for the next relayout
    page.eval("document.getElementById('list').innerHTML = ''");
    EXPECT_EQ(bindings->detachedNodeCount(), 2u);
    page.script->releaseDetachedNodes();
    EXPECT_EQ(bindings->detachedNodeCount(), 0u);
    // A child the script holds stays with its wrapper, usable after release
    page.eval("var keep = document.getElementById('first'); document.getElementById('root').textContent = ''");
    page.script->releaseDetachedNodes();
    EXPECT_EQ(page.eval("keep.textContent + ':' + keep.isConnected"), "one:false");
}

TEST(DomBindings, DocumentDestroyedBeforeScripts) {
    // On navigation the browser replaces the document before the script context
    Page page(kPage);
    page.eval("var held = document.getElementById('first'); var made = document.createElement('div')");
    page.doc.reset();
    EXPECT_EQ(page.eval("String(held.textContent) + ':' + typeof made.textContent"), "undefined:string");
    page.script.reset();
}

TEST(DomBindings, CppQueriesAndFragmentParsing) {
    HTMLParser parser;
    auto nodes = parser.parseFragmentNodes("<li>a</li> <li>b</li>", "ul");
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(nodes[0]->nodeName(), "li");
    auto doc = parser.parse("<html><body><div class='a b'><span class='b'></span></div></body></html>");
    EXPECT_EQ(doc->getElementsByClassName("b").size(), 2u);
    EXPECT_EQ(doc->getElementsByClassName("a b").size(), 1u);
    DOMElement* div = doc->querySelector("div");
    ASSERT_NE(div, nullptr);
    div->setInnerHTML("<p>1</p><p>2</p>");
    EXPECT_EQ(div->querySelectorAll("p").size(), 2u);
    EXPECT_EQ(div->childNodes()[0]->ownerDocument(), doc.get());
}

// ---------------------------------------------------------------------------
// fetch() and XMLHttpRequest: requests go to the installed handler, the
// response settles on the main thread from processTimers()
// ---------------------------------------------------------------------------

namespace {
struct FakeNetwork {
    std::vector<FetchRequest> requests;
    std::vector<FetchDone> pending;   // answered later, like a worker thread would
    void install(ScriptContext& script) {
        script.setFetchHandler([this](const FetchRequest& request, FetchDone done) {
            requests.push_back(request);
            pending.push_back(std::move(done));
        });
    }
    void answer(size_t index, int status, const std::string& body,
                const std::string& contentType = "application/json") {
        FetchResponse response;
        response.status = status;
        response.statusText = status == 200 ? "OK" : "Error";
        response.url = requests[index].url;
        response.headers = {{"Content-Type", contentType}, {"X-Test", "1"}};
        response.body = body;
        FetchDone done = std::move(pending[index]);
        done(std::move(response));
    }
    void fail(size_t index, const std::string& error) {
        FetchResponse response;
        response.error = error;
        FetchDone done = std::move(pending[index]);
        done(std::move(response));
    }
};
}  // namespace

TEST(FetchBindings, FetchResolvesWithJsonOnTheTimerPump) {
    Page page(kPage);
    FakeNetwork net;
    net.install(*page.script);
    page.eval("var out = []; fetch('/api/items?x=1', {method: 'POST', headers: {'Content-Type': 'text/plain'}, body: 'ping'})"
              ".then(function(r) { out.push(r.ok, r.status, r.headers.get('x-test'), r.url); return r.json(); })"
              ".then(function(j) { out.push(j.a); console.log('done ' + out.join(',')); })"
              ".catch(function(e) { console.log('fail ' + e); })");
    ASSERT_EQ(net.requests.size(), 1u);
    EXPECT_EQ(net.requests[0].method, "POST");
    EXPECT_EQ(net.requests[0].url, "/api/items?x=1");
    EXPECT_EQ(net.requests[0].body, "ping");
    ASSERT_EQ(net.requests[0].headers.size(), 1u);
    EXPECT_EQ(net.requests[0].headers[0].second, "text/plain");
    EXPECT_EQ(page.script->pendingFetchCount(), 1);
    EXPECT_TRUE(page.console.empty()) << "nothing settles before the response arrives";

    net.answer(0, 200, "{\"a\": 42}");
    EXPECT_TRUE(page.console.empty()) << "the response is applied on the main thread, not in the callback";
    page.script->processTimers();
    ASSERT_EQ(page.console.size(), 1u);
    EXPECT_EQ(page.console[0], "log: done true,200,1,/api/items?x=1,42");
    EXPECT_EQ(page.script->pendingFetchCount(), 0);
}

TEST(FetchBindings, FetchTextErrorsAndFailures) {
    Page page(kPage);
    FakeNetwork net;
    net.install(*page.script);
    page.eval("fetch('/missing').then(function(r) { return r.text().then(function(t) { console.log(r.ok + ' ' + r.status + ' ' + t); }); });"
              "fetch('/down').catch(function(e) { console.log('caught ' + (e instanceof TypeError) + ' ' + e.message); });"
              "fetch('/twice').then(function(r) { r.text(); return r.text(); }).catch(function(e) { console.log('used ' + e.message); });");
    ASSERT_EQ(net.requests.size(), 3u);
    net.answer(0, 404, "not here", "text/plain");
    net.fail(1, "DNS resolution failed");
    net.answer(2, 200, "x", "text/plain");
    page.script->processTimers();
    ASSERT_EQ(page.console.size(), 3u);
    EXPECT_EQ(page.console[0], "log: false 404 not here");
    EXPECT_EQ(page.console[1], "log: caught true Failed to fetch: DNS resolution failed");
    EXPECT_EQ(page.console[2], "log: used Failed to execute 'text' on 'Response': body stream already read");
}

TEST(FetchBindings, XmlHttpRequestEventsAndHeaders) {
    Page page(kPage);
    FakeNetwork net;
    net.install(*page.script);
    page.eval("var states = []; var x = new XMLHttpRequest();"
              "x.onreadystatechange = function() { states.push(x.readyState); };"
              "x.addEventListener('load', function(e) { console.log('load ' + x.status + ' ' + x.responseText + ' ' + x.getResponseHeader('X-TEST') + ' ' + (e.target === x)); });"
              "x.onloadend = function() { console.log('end ' + states.join('')); };"
              "x.open('GET', '/data.txt'); x.setRequestHeader('Accept', 'text/plain'); x.send();");
    ASSERT_EQ(net.requests.size(), 1u);
    EXPECT_TRUE(net.requests[0].xhr);
    EXPECT_EQ(net.requests[0].url, "/data.txt");
    ASSERT_EQ(net.requests[0].headers.size(), 1u);
    EXPECT_EQ(net.requests[0].headers[0].first, "Accept");
    EXPECT_EQ(page.eval("x.readyState"), "1");

    net.answer(0, 200, "payload", "text/plain");
    page.script->processTimers();
    ASSERT_EQ(page.console.size(), 2u);
    EXPECT_EQ(page.console[0], "log: load 200 payload 1 true");
    EXPECT_EQ(page.console[1], "log: end 14");

    // responseType json, and a network failure fires error with status 0
    page.console.clear();
    page.eval("var y = new XMLHttpRequest(); y.responseType = 'json';"
              "y.onload = function() { console.log('json ' + y.response.k); };"
              "y.open('GET', '/j'); y.send();"
              "var z = new XMLHttpRequest(); z.onerror = function() { console.log('error ' + z.status + ' ' + z.readyState); };"
              "z.open('GET', '/bad'); z.send();");
    ASSERT_EQ(net.requests.size(), 3u);
    net.answer(1, 200, "{\"k\": \"v\"}");
    net.fail(2, "Connection refused");
    page.script->processTimers();
    ASSERT_EQ(page.console.size(), 2u);
    EXPECT_EQ(page.console[0], "log: json v");
    EXPECT_EQ(page.console[1], "log: error 0 4");
}

TEST(FetchBindings, AbortedXhrAndClosedPageDropResponses) {
    Page page(kPage);
    FakeNetwork net;
    net.install(*page.script);
    page.eval("var a = new XMLHttpRequest(); a.onload = function() { console.log('should not run'); };"
              "a.open('GET', '/slow'); a.send(); a.abort();");
    ASSERT_EQ(net.requests.size(), 1u);
    net.answer(0, 200, "late");
    page.script->processTimers();
    EXPECT_TRUE(page.console.empty());

    page.eval("fetch('/after-close').then(function() { console.log('should not run'); });");
    ASSERT_EQ(net.requests.size(), 2u);
    FetchDone done = std::move(net.pending[1]);
    page.script.reset();   // the tab closed while the request was in flight
    FetchResponse response;
    response.status = 200;
    done(std::move(response));   // must not touch the destroyed context
}

TEST(FetchBindings, WithoutAHandlerFetchRejects) {
    Page page(kPage);
    page.eval("fetch('/x').catch(function(e) { console.log('no net: ' + e.message); });");
    page.script->processTimers();
    ASSERT_EQ(page.console.size(), 1u);
    EXPECT_EQ(page.console[0], "log: no net: Failed to fetch: No network available to scripts");
}

// ----------------------------------------------------------------------------
// Web Storage
// ----------------------------------------------------------------------------

TEST(WebStorage, AreaOrderQuotaAndFileFormat) {
    StorageArea area(20);
    EXPECT_TRUE(area.setItem("b", "1"));
    EXPECT_TRUE(area.setItem("a", "2"));
    EXPECT_TRUE(area.setItem("b", "3"));   // keeps its place
    EXPECT_EQ(*area.key(0), "b");
    EXPECT_EQ(*area.key(1), "a");
    EXPECT_FALSE(area.key(2).has_value());
    EXPECT_EQ(area.usedBytes(), 4u);
    EXPECT_FALSE(area.setItem("big", std::string(17, 'x')));   // 4 + 3 + 17 > 20
    EXPECT_FALSE(area.getItem("big").has_value());
    EXPECT_TRUE(area.setItem("b", std::string(16, 'y')));      // replacing frees the old value
    area.removeItem("b");
    EXPECT_EQ(area.length(), 1u);
    EXPECT_EQ(area.usedBytes(), 2u);

    // Any bytes survive a round trip
    StorageArea odd;
    std::string weird = std::string("tab\there\nline 12:34 ") + std::string(1, '\0') + "end";
    odd.setItem(weird, weird + "!");
    odd.setItem("", "");
    odd.setItem("\xc3\xa9t\xc3\xa9", "\xf0\x9f\x98\x80");
    auto back = StorageArea::deserialize(odd.serialize());
    ASSERT_TRUE(back.has_value());
    EXPECT_FALSE(back->dirty);
    EXPECT_EQ(back->length(), 3u);
    EXPECT_EQ(*back->key(0), weird);
    EXPECT_EQ(*back->getItem(weird), weird + "!");
    EXPECT_EQ(*back->getItem(""), "");
    EXPECT_EQ(*back->getItem("\xc3\xa9t\xc3\xa9"), "\xf0\x9f\x98\x80");
    // Damaged files are refused, not half read
    std::string bytes = odd.serialize();
    EXPECT_FALSE(StorageArea::deserialize(bytes.substr(0, bytes.size() - 2)).has_value());
    EXPECT_FALSE(StorageArea::deserialize("k\tv\n").has_value());
}

TEST(WebStorage, Origins) {
    EXPECT_EQ(storageOriginOf("https://Example.COM/a/b?c#d"), "https://example.com");
    EXPECT_EQ(storageOriginOf("https://example.com:443/"), "https://example.com");
    EXPECT_EQ(storageOriginOf("http://example.com:80"), "http://example.com");
    EXPECT_EQ(storageOriginOf("http://example.com:8080/x"), "http://example.com:8080");
    EXPECT_EQ(storageOriginOf("https://user:pw@example.com/"), "https://example.com");
    EXPECT_EQ(storageOriginOf("http://[::1]:8000/"), "http://[::1]:8000");
    EXPECT_EQ(storageOriginOf("http://[::1]/"), "http://[::1]");
    EXPECT_EQ(storageOriginOf("file:///home/a/page.html"), "file://");
    EXPECT_EQ(storageOriginOf("zepra://start"), "zepra://start");
    EXPECT_EQ(storageOriginOf("about:blank"), "");
    EXPECT_EQ(storageOriginOf("data:text/html,hi"), "");
    EXPECT_EQ(storageOriginOf(""), "");
}

TEST(WebStorage, LocalStorageIsPerOriginAndSessionStorageIsPerPage) {
    resetLocalStorageForTesting();
    Page a(kPage), b(kPage), other(kPage);
    a.script->setPageUrl("https://site.test/one");
    b.script->setPageUrl("https://site.test:443/two");
    other.script->setPageUrl("https://other.test/");
    a.eval("localStorage.setItem('k', 1); localStorage.setItem('obj', {}); sessionStorage.setItem('s', 'a')");
    EXPECT_EQ(b.eval("localStorage.getItem('k') + '|' + localStorage.getItem('obj') + '|' + localStorage.length"),
              "1|[object Object]|2");
    EXPECT_EQ(b.eval("String(sessionStorage.getItem('s'))"), "null");
    EXPECT_EQ(other.eval("String(localStorage.getItem('k')) + localStorage.length"), "null0");
    EXPECT_EQ(b.eval("localStorage.key(0) + ',' + localStorage.key(1) + ',' + localStorage.key(2)"), "k,obj,null");
    b.eval("localStorage.removeItem('k')");
    EXPECT_EQ(a.eval("localStorage.length + localStorage.key(0)"), "1obj");
    EXPECT_EQ(a.eval("window.localStorage === localStorage"), "true");

    // Spec errors
    EXPECT_EQ(a.eval("var r = 'no'; try { localStorage.setItem('x') } catch (e) { r = e.name } r"), "TypeError");
    EXPECT_EQ(a.eval("r = 'no'; try { localStorage.setItem('big', 'x'.repeat(6 * 1024 * 1024)) }"
                     " catch (e) { r = e.name + ' ' + e.code + ' ' + (e instanceof Error) } r"),
              "QuotaExceededError 22 true");
    EXPECT_EQ(a.eval("String(localStorage.getItem('big'))"), "null");
    a.eval("localStorage.clear()");
    EXPECT_EQ(b.eval("localStorage.length"), "0");

    // A page without an origin still has a working storage, its own
    Page blank(kPage);
    blank.script->setPageUrl("about:blank");
    blank.eval("localStorage.setItem('mine', 'y')");
    EXPECT_EQ(blank.eval("localStorage.getItem('mine')"), "y");
    EXPECT_EQ(other.eval("String(localStorage.getItem('mine'))"), "null");
    resetLocalStorageForTesting();
}

TEST(WebStorage, LocalStorageIsKeptOnDisk) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / ("zepra-storage-test-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::remove_all(dir);
    setLocalStorageDirectory(dir.string());
    {
        Page page(kPage);
        page.script->setPageUrl("http://127.0.0.1:8000/app");
        page.eval("localStorage.setItem('theme', 'dark'); localStorage.setItem('multi\\nline', 'a\\tb')");
        flushLocalStorage(true);
    }
    EXPECT_TRUE(fs::exists(dir / "http%3A%2F%2F127.0.0.1%3A8000.localstorage"));
    // A fresh start (another browser run) reads it back
    resetLocalStorageForTesting();
    {
        Page page(kPage);
        page.script->setPageUrl("http://127.0.0.1:8000/other-page");
        EXPECT_EQ(page.eval("localStorage.getItem('theme') + '|' + JSON.stringify(localStorage.getItem('multi\\nline'))"),
                  "dark|\"a\\tb\"");
        page.eval("localStorage.removeItem('theme')");
        flushLocalStorage(true);
    }
    resetLocalStorageForTesting();
    {
        Page page(kPage);
        page.script->setPageUrl("http://127.0.0.1:8000/");
        EXPECT_EQ(page.eval("String(localStorage.getItem('theme')) + localStorage.length"), "null1");
    }
    // A damaged file starts the origin empty instead of failing
    { std::ofstream(dir / "https%3A%2F%2Fbroken.test.localstorage") << "garbage"; }
    {
        Page page(kPage);
        page.script->setPageUrl("https://broken.test/");
        EXPECT_EQ(page.eval("localStorage.length"), "0");
    }
    setLocalStorageDirectory("");
    fs::remove_all(dir);
}

// =============================================================================
// window.location and history
// =============================================================================

TEST(PageUrl, SplitsAndResolves) {
    PageUrl u = PageUrl::parse("HTTPS://User:pw@Example.COM:443/a/./b/../c?x=1#top");
    ASSERT_TRUE(u.valid);
    EXPECT_EQ(u.href(), "https://User:pw@example.com/a/c?x=1#top");
    EXPECT_EQ(u.origin(), "https://example.com");
    EXPECT_EQ(u.host(), "example.com");
    EXPECT_EQ(u.search(), "?x=1");
    EXPECT_EQ(u.hash(), "#top");
    EXPECT_EQ(PageUrl::parse("http://h:8080").href(), "http://h:8080/");
    EXPECT_EQ(PageUrl::parse("http://h:080/").port, "");
    EXPECT_EQ(PageUrl::parse("file:///home/me/page.html").href(), "file:///home/me/page.html");
    EXPECT_EQ(PageUrl::parse("file:///home/me/page.html").origin(), "null");
    EXPECT_EQ(PageUrl::parse("about:blank").pathname, "blank");
    EXPECT_FALSE(PageUrl::parse("C:/x").valid);
    EXPECT_FALSE(PageUrl::parse("http://").valid);

    const std::string base = "https://a.com/dir/page.html?q=1#f";
    EXPECT_EQ(PageUrl::resolve(base, "other.html").href(), "https://a.com/dir/other.html");
    EXPECT_EQ(PageUrl::resolve(base, "../up").href(), "https://a.com/up");
    EXPECT_EQ(PageUrl::resolve(base, "/root?z").href(), "https://a.com/root?z");
    EXPECT_EQ(PageUrl::resolve(base, "?n=2").href(), "https://a.com/dir/page.html?n=2");
    EXPECT_EQ(PageUrl::resolve(base, "#g").href(), "https://a.com/dir/page.html?q=1#g");
    EXPECT_EQ(PageUrl::resolve(base, "").href(), "https://a.com/dir/page.html?q=1");
    EXPECT_EQ(PageUrl::resolve(base, "//cdn.net/x.js").href(), "https://cdn.net/x.js");
    EXPECT_EQ(PageUrl::resolve(base, "http://b.org").href(), "http://b.org/");
    EXPECT_EQ(PageUrl::resolve(base, "  sub\\x  ").href(), "https://a.com/dir/sub/x");
    EXPECT_EQ(PageUrl::resolve(base, "https:same.html").href(), "https://a.com/dir/same.html");
    EXPECT_FALSE(PageUrl::resolve(base, "https://").valid);
    EXPECT_EQ(PageUrl::resolve("about:blank", "#x").href(), "about:blank#x");
    EXPECT_FALSE(PageUrl::resolve("about:blank", "page.html").valid);
}

struct LocationPage : Page {
    std::vector<NavigationRequest> navigations;
    explicit LocationPage(const std::string& url) : Page(kPage) {
        script->setPageUrl(url);
        script->setNavigationHandler([this](const NavigationRequest& r) { navigations.push_back(r); });
    }
    std::string last() const {
        if (navigations.empty()) return "<none>";
        const char* kinds[] = {"assign", "replace", "reload", "same-document"};
        return std::string(kinds[static_cast<int>(navigations.back().kind)]) + " " + navigations.back().url;
    }
};

TEST(Location, ReadsThePageUrl) {
    LocationPage page("https://shop.example.com:8443/items/list.html?page=2#reviews");
    EXPECT_EQ(page.eval("[location.href, location.protocol, location.host, location.hostname, location.port,"
                        " location.pathname, location.search, location.hash, location.origin].join(' | ')"),
              "https://shop.example.com:8443/items/list.html?page=2#reviews | https: | shop.example.com:8443 | "
              "shop.example.com | 8443 | /items/list.html | ?page=2 | #reviews | https://shop.example.com:8443");
    EXPECT_EQ(page.eval("[String(location), location.toString() === location.href, window.location === location,"
                        " document.location === location].join()"),
              "https://shop.example.com:8443/items/list.html?page=2#reviews,true,true,true");
    // Before the browser gives it one, the page is about:blank
    Page blank(kPage);
    EXPECT_EQ(blank.eval("location.href + ' ' + location.origin"), "about:blank null");
}

TEST(Location, AssigningAsksTheBrowserToNavigate) {
    LocationPage page("https://a.com/dir/page.html");
    page.eval("location.href = 'next.html?x=1'");
    EXPECT_EQ(page.last(), "assign https://a.com/dir/next.html?x=1");
    // The page keeps its URL until the browser loads the new one
    EXPECT_EQ(page.eval("location.pathname"), "/dir/page.html");
    page.eval("location.assign('/root')");
    EXPECT_EQ(page.last(), "assign https://a.com/root");
    page.eval("location.replace('https://b.org/elsewhere')");
    EXPECT_EQ(page.last(), "replace https://b.org/elsewhere");
    page.eval("location.reload()");
    EXPECT_EQ(page.last(), "reload https://a.com/dir/page.html");
    page.eval("window.location = '../up.html'");
    EXPECT_EQ(page.last(), "assign https://a.com/up.html");
    page.eval("location = 'bare.html'");
    EXPECT_EQ(page.last(), "assign https://a.com/dir/bare.html");
    page.eval("location.search = 'q=zepra'");
    EXPECT_EQ(page.last(), "assign https://a.com/dir/page.html?q=zepra");
    page.eval("location.pathname = 'p/../q'");
    EXPECT_EQ(page.last(), "assign https://a.com/q");
    page.eval("location.hostname = 'B.example'");
    EXPECT_EQ(page.last(), "assign https://b.example/dir/page.html");
    page.eval("location.port = '8080'");
    EXPECT_EQ(page.last(), "assign https://a.com:8080/dir/page.html");
    page.eval("location.protocol = 'http'");
    EXPECT_EQ(page.last(), "assign http://a.com/dir/page.html");
    EXPECT_EQ(page.eval("(function () { try { location.href = 'http://'; } catch (e) { return e.name; } })()"),
              "SyntaxError");
}

TEST(Location, FragmentChangesStayOnThePage) {
    LocationPage page("https://a.com/doc.html?v=1");
    page.eval("var seen = []; window.addEventListener('hashchange', function (e) {"
              "  seen.push(e.type + ' ' + e.oldURL + ' -> ' + e.newURL + ' ' + location.hash); });"
              "location.hash = 'part2';");
    EXPECT_EQ(page.last(), "same-document https://a.com/doc.html?v=1#part2");
    EXPECT_EQ(page.eval("location.hash + ' ' + location.href"), "#part2 https://a.com/doc.html?v=1#part2");
    // hashchange comes later, as a task
    EXPECT_EQ(page.eval("seen.length"), "0");
    page.script->processTimers();
    EXPECT_EQ(page.eval("seen.join()"),
              "hashchange https://a.com/doc.html?v=1 -> https://a.com/doc.html?v=1#part2 #part2");
    // A link-style assignment that only changes the fragment does not load either
    page.eval("location.href = '#part3'");
    EXPECT_EQ(page.last(), "same-document https://a.com/doc.html?v=1#part3");
    // The same fragment again changes nothing
    size_t count = page.navigations.size();
    page.eval("location.hash = '#part3'");
    EXPECT_EQ(page.navigations.size(), count);
    // Leaving the fragment for another query loads
    page.eval("location.href = '?v=2#part3'");
    EXPECT_EQ(page.last(), "assign https://a.com/doc.html?v=2#part3");
}

TEST(History, PushAndReplaceStateChangeTheUrl) {
    LocationPage page("https://app.test/");
    page.eval("history.pushState({ view: 'inbox' }, '', '/mail/inbox')");
    EXPECT_EQ(page.last(), "same-document https://app.test/mail/inbox");
    EXPECT_EQ(page.eval("[location.pathname, history.state.view, history.length].join()"), "/mail/inbox,inbox,2");
    page.eval("history.replaceState(null, '', '?unread=1')");
    EXPECT_EQ(page.eval("[location.href, history.state, history.length].join()"),
              "https://app.test/mail/inbox?unread=1,,2");
    // Only the state, no URL
    page.eval("history.replaceState({ n: 1 }, '')");
    EXPECT_EQ(page.eval("history.state.n + ' ' + location.search"), "1 ?unread=1");
    EXPECT_EQ(page.eval("(function () { try { history.pushState(null, '', 'https://evil.test/'); }"
                        " catch (e) { return e.name; } })()"),
              "SecurityError");
    EXPECT_EQ(page.eval("location.host"), "app.test");
}

// ============================================================================
// <script> elements: prepare / execute (script_elements.cpp)
// ============================================================================

namespace {

/// A ScriptFetcher that answers when the test says so
struct HeldFetches {
    std::vector<std::pair<std::string, ScriptFetchDone>> pending;
    std::map<std::string, std::string> files;   // url suffix -> body (missing: 404)

    ScriptFetcher fetcher() {
        return [this](const std::string& url, ScriptFetchDone done) { pending.emplace_back(url, std::move(done)); };
    }
    /// Answers the request whose URL ends in `suffix`
    void answer(const std::string& suffix) {
        for (auto it = pending.begin(); it != pending.end(); ++it) {
            if (it->first.size() < suffix.size() || it->first.compare(it->first.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
            ScriptFetchResult result;
            auto file = files.find(suffix);
            result.ok = file != files.end();
            if (result.ok) result.body = file->second;
            else result.error = "HTTP 404";
            ScriptFetchDone done = std::move(it->second);
            pending.erase(it);
            done(std::move(result));
            return;
        }
        ADD_FAILURE() << "no request for " << suffix;
    }
};

}  // namespace

TEST(ScriptElements, ScriptInsertedInlineRunsInsideAppendChild) {
    Page page("<html><head></head><body></body></html>");
    page.script->setPageUrl("https://example.test/index.html");
    EXPECT_EQ(page.eval(R"JS(
        var log = [];
        var s = document.createElement('script');
        s.id = 'inl';
        s.textContent = "log.push('run:' + document.currentScript.id)";
        log.push('before'); document.head.appendChild(s); log.push('after');
        log.push('cs:' + document.currentScript);
        log.join())JS"), "before,run:inl,after,cs:null");
    // Already started: moving it does not run it again
    EXPECT_EQ(page.eval("document.body.appendChild(s); log.length"), "4");
}

TEST(ScriptElements, ScriptInsertedExternalRunsAfterTheInsertingScript) {
    Page page("<html><head></head><body></body></html>");
    page.script->setPageUrl("https://example.test/dir/index.html");
    HeldFetches net;
    net.files["/dir/a.js"] = "log.push('a:' + (document.currentScript && document.currentScript.id))";
    page.script->setScriptFetcher(net.fetcher());
    EXPECT_EQ(page.eval(R"JS(
        var log = [];
        var a = document.createElement('script');
        a.id = 'A'; a.src = 'a.js';
        a.onload = function (e) { log.push('onload:' + (e.target === a)); };
        a.addEventListener('load', function () { log.push('listener'); });
        document.head.appendChild(a);
        log.push('appended async=' + a.async + ' src=' + a.src);
        log.join())JS"), "appended async=true src=https://example.test/dir/a.js");
    ASSERT_EQ(net.pending.size(), 1u);
    EXPECT_EQ(net.pending[0].first, "https://example.test/dir/a.js");
    net.answer("/dir/a.js");
    // The answer only lands in the inbox; the script runs as a task of its own
    EXPECT_EQ(page.eval("log.length"), "1");
    page.script->processTimers();
    EXPECT_EQ(page.eval("log.join()"), "appended async=true src=https://example.test/dir/a.js,a:A,listener,onload:true");
    EXPECT_EQ(page.script->pendingScriptCount(), 0u);
}

TEST(ScriptElements, FailedFetchFiresErrorAndAsyncFalseKeepsOrder) {
    Page page("<html><head></head><body></body></html>");
    page.script->setPageUrl("https://example.test/");
    HeldFetches net;
    net.files["/1.js"] = "log.push('1')";
    net.files["/2.js"] = "log.push('2')";
    page.script->setScriptFetcher(net.fetcher());
    page.eval(R"JS(
        var log = [];
        function add(src, ordered) {
            var s = document.createElement('script');
            if (ordered) s.async = false;
            s.src = src;
            s.onload = function () { log.push('load ' + src); };
            s.onerror = function (e) { log.push('error ' + src + ' ' + e.type); };
            document.body.appendChild(s);
        }
        add('1.js', true); add('2.js', true); add('missing.js', false);
        var e = document.createElement('script'); e.setAttribute('src', '');
        e.onerror = function () { log.push('empty src error'); };
        document.body.appendChild(e);
        0)JS");
    // The second arrives first: it still waits for the first
    net.answer("/2.js");
    page.script->processTimers();
    EXPECT_EQ(page.eval("log.join()"), "empty src error");
    net.answer("/missing.js");
    net.answer("/1.js");
    page.script->processTimers();
    EXPECT_EQ(page.eval("log.join()"), "empty src error,error missing.js error,1,load 1.js,2,load 2.js");
}

TEST(ScriptElements, TypesInnerHTMLAndLateText) {
    Page page("<html><head></head><body></body></html>");
    EXPECT_EQ(page.eval(R"JS(
        var log = [];
        function dyn(type, code) {
            var s = document.createElement('script');
            if (type) s.type = type;
            s.text = code;
            document.body.appendChild(s);
            return s;
        }
        dyn('application/json', "log.push('json')");
        dyn('text/plain', "log.push('plain')");
        dyn(' TEXT/JavaScript ', "log.push('js')");
        var nm = document.createElement('script'); nm.setAttribute('nomodule', ''); nm.text = "log.push('nomodule')";
        document.body.appendChild(nm);
        var d = document.createElement('div');
        d.innerHTML = '<script>log.push("innerHTML")<\/script>';
        document.body.appendChild(d);
        var late = document.createElement('script');
        document.body.appendChild(late);
        late.textContent = "log.push('late')";
        late.textContent = "log.push('late again')";
        var detached = document.createElement('div');
        var ds = document.createElement('script'); ds.text = "log.push('connected')";
        detached.appendChild(ds);
        log.push('|');
        document.body.appendChild(detached);
        log.push(document.scripts.length);
        log.join())JS"), "js,late,|,connected,7");
}

TEST(ScriptElements, ParserScriptsDeferAndSelfRemoval) {
    Page page(R"HTML(<html><head>
        <script>var log = []; document.addEventListener('DOMContentLoaded', function () { log.push('DCL'); });</script>
        <script defer src="d.js"></script>
        <script id="self">var me = document.currentScript; me.parentNode.removeChild(me); log.push('self:' + me.id);</script>
        <script id="next">log.push('next:' + document.currentScript.id + ':' + document.currentScript.async);</script>
        </head><body><script src="b.js" onload="log.push('b loaded')"></script><script>log.push('body')</script></body></html>)HTML");
    page.script->setPageUrl("https://example.test/");
    page.script->setScriptFetcher([](const std::string& url, ScriptFetchDone done) {
        ScriptFetchResult result;
        result.ok = true;
        result.body = "log.push('" + url.substr(url.rfind('/') + 1) + "')";
        done(std::move(result));
    });
    page.script->runParserScripts(nullptr);
    page.script->runDeferredScripts();
    page.script->fireDOMContentLoaded();
    EXPECT_EQ(page.eval("log.join()"), "self:self,next:next:false,b.js,b loaded,body,d.js,DCL");
}
