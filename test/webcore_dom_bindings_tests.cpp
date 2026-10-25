// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * webcore_dom_bindings_tests.cpp - Page scripts change the real DOM: live
 * wrappers (one object per node), every mutation reaching the C++ tree,
 * DOM events, and the lifetime of nodes scripts remove. No window.
 */

#include <gtest/gtest.h>

#include "browser/dom.hpp"
#include "html/html_parser.hpp"
#include "scripting/dom_bindings.hpp"
#include "scripting/script_context.hpp"

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
