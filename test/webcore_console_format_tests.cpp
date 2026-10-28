// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * webcore_console_format_tests.cpp - Script values as the DevTools Console
 * shows them: Firefox-style previews of primitives, arrays, objects,
 * functions, errors, DOM nodes, Maps and Sets; depth and length limits;
 * cycles; the console methods; and the console helpers ($0, $, $$, $_).
 */

#include <gtest/gtest.h>

#include "browser/dom.hpp"
#include "html/html_parser.hpp"
#include "scripting/console_format.hpp"
#include "scripting/script_context.hpp"

#include <memory>
#include <string>
#include <vector>

// Viewport size the layout engine reads (zepra_browser.cpp defines these)
int g_width = 1280;
int g_height = 800;

using namespace Zepra::WebCore;
using Kind = ValuePreview::Kind;

namespace {

struct Page {
    std::unique_ptr<DOMDocument> doc;
    std::unique_ptr<ScriptContext> script;
    std::vector<ConsoleMessage> console;

    explicit Page(const std::string& html = kPage) {
        HTMLParser parser;
        doc = parser.parse(html);
        script = std::make_unique<ScriptContext>();
        script->setConsoleMessageHandler([this](const ConsoleMessage& m) { console.push_back(m); });
        script->initialize(doc.get());
    }

    /// The preview of an expression's value, as console input shows it (strings quoted)
    ValuePreview eval(const std::string& code, DOMNode* inspected = nullptr) {
        ConsoleMessage m = script->evaluateConsole(code, inspected);
        EXPECT_EQ(m.level, "result") << m.text << "\n  in: " << code;
        if (m.values.empty()) return ValuePreview{};
        return m.values[0];
    }

    /// The preview text of an expression
    std::string text(const std::string& code) { return eval(code).text; }

    /// The last console message after running code
    const ConsoleMessage& logged(const std::string& code) {
        ScriptResult r = script->evaluate(code, "test.js");
        EXPECT_TRUE(r.success) << r.error << "\n  in: " << code;
        EXPECT_FALSE(console.empty()) << "nothing logged by: " << code;
        static ConsoleMessage none;
        return console.empty() ? none : console.back();
    }

    static constexpr const char* kPage = R"(<!DOCTYPE html><html><head><title>Console</title></head><body>
<div id="card" class="box demo" data-ticks="3"><p id="first">one</p><p class="note">two</p></div>
</body></html>)";
};

const ValuePreview* childNamed(const ValuePreview& p, const std::string& key) {
    for (const auto& c : p.children) if (c.key == key) return &c;
    return nullptr;
}

std::string keys(const ValuePreview& p) {
    std::string s;
    for (const auto& c : p.children) s += (s.empty() ? "" : ",") + c.key;
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

TEST(ConsoleFormat, Primitives) {
    Page page;
    EXPECT_EQ(page.text("undefined"), "undefined");
    EXPECT_EQ(page.text("null"), "null");
    EXPECT_EQ(page.text("true"), "true");
    EXPECT_EQ(page.text("42"), "42");
    EXPECT_EQ(page.text("-0"), "-0");
    EXPECT_EQ(page.text("1.5e21"), "1.5e+21");
    EXPECT_EQ(page.text("0/0"), "NaN");
    EXPECT_EQ(page.text("-1/0"), "-Infinity");
    EXPECT_EQ(page.text("'hi'"), "\"hi\"");
    EXPECT_EQ(page.text("'a\"b\\nc'"), "\"a\\\"b\\nc\"");
    EXPECT_EQ(page.text("Symbol('tag')"), "Symbol(tag)");
    EXPECT_EQ(page.text("Symbol()"), "Symbol()");
    EXPECT_EQ(page.text("Symbol.iterator"), "Symbol(Symbol.iterator)");
    EXPECT_EQ(page.eval("'hi'").kind, Kind::String);
    EXPECT_EQ(page.eval("1").kind, Kind::Number);
    EXPECT_FALSE(page.eval("1").expandable);
}

TEST(ConsoleFormat, StringsQuotedInInputNotInLogs) {
    Page page;
    EXPECT_EQ(page.text("'text'"), "\"text\"");
    const ConsoleMessage& m = page.logged("console.log('text', 1, 'more')");
    EXPECT_EQ(m.level, "log");
    EXPECT_EQ(m.text, "text 1 more");
    ASSERT_EQ(m.values.size(), 3u);
    EXPECT_EQ(m.values[0].text, "text");
    EXPECT_EQ(m.values[2].text, "more");
    // Inside an object a string is quoted either way
    EXPECT_EQ(page.logged("console.log({ s: 'x' })").text, "Object { s: \"x\" }");
}

TEST(ConsoleFormat, LongStringsAreCut) {
    Page page;
    ValuePreview p = page.eval("'x'.repeat(600)");
    EXPECT_LT(p.text.size(), 520u);
    EXPECT_NE(p.text.find("…"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Arrays and objects
// ---------------------------------------------------------------------------

TEST(ConsoleFormat, Arrays) {
    Page page;
    ValuePreview p = page.eval("[1, 'two', true]");
    EXPECT_EQ(p.kind, Kind::Array);
    EXPECT_EQ(p.text, "Array(3) [ 1, \"two\", true ]");
    EXPECT_TRUE(p.expandable);
    EXPECT_EQ(keys(p), "0,1,2,length");
    EXPECT_EQ(p.children[1].text, "\"two\"");
    EXPECT_EQ(p.children[1].kind, Kind::String);
    EXPECT_EQ(p.children[3].text, "3");
    EXPECT_EQ(page.text("[]"), "Array []");
    EXPECT_EQ(page.text("[1, [2, 3], { a: 1 }]"), "Array(3) [ 1, (2) […], {…} ]");
    EXPECT_EQ(page.text("[[]]"), "Array(1) [ [] ]");
}

TEST(ConsoleFormat, ArrayHolesAndLongArrays) {
    Page page;
    EXPECT_EQ(page.text("[1, , , 4]"), "Array(4) [ 1, <2 empty slots>, 4 ]");
    ValuePreview p = page.eval("Array.from({ length: 150 }, (_, i) => i)");
    EXPECT_EQ(p.text, "Array(150) [ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, … ]");
    // 100 children, then "… 50 more", then length
    ASSERT_EQ(p.children.size(), 102u);
    EXPECT_EQ(p.children[100].kind, Kind::More);
    EXPECT_EQ(p.children[100].text, "… 50 more");
    EXPECT_EQ(p.children[101].key, "length");
}

TEST(ConsoleFormat, Objects) {
    Page page;
    ValuePreview p = page.eval("({ a: 1, b: [1, 2], c: 'x', d: null })");
    EXPECT_EQ(p.kind, Kind::Object);
    EXPECT_EQ(p.text, "Object { a: 1, b: (2) […], c: \"x\", d: null }");
    EXPECT_EQ(keys(p), "a,b,c,d");
    const ValuePreview* b = childNamed(p, "b");
    ASSERT_TRUE(b);
    EXPECT_EQ(b->text, "Array(2) [ 1, 2 ]");
    EXPECT_TRUE(b->expandable);
    EXPECT_EQ(keys(*b), "0,1,length");
    EXPECT_EQ(page.text("({})"), "Object {}");
    EXPECT_EQ(page.text("({ nested: { deeper: { x: 1 } } })"), "Object { nested: {…} }");
}

TEST(ConsoleFormat, ClassInstancesShowTheirClass) {
    Page page;
    page.script->evaluate("class Point { constructor() { this.x = 1; this.y = 2; } }", "test.js");
    EXPECT_EQ(page.text("new Point()"), "Point { x: 1, y: 2 }");
    EXPECT_EQ(page.text("[new Point()]"), "Array(1) [ Point ]");
    EXPECT_EQ(page.text("Object.create(null)"), "Object {}");
}

TEST(ConsoleFormat, GettersAreShownNotRun) {
    Page page;
    page.script->evaluate("var ran = 0; var o = { get g() { ran++; return 1; }, set s(v) {}, plain: 2 };", "test.js");
    ValuePreview p = page.eval("o");
    EXPECT_EQ(p.text, "Object { g: Getter, s: Setter, plain: 2 }");
    const ValuePreview* g = childNamed(p, "g");
    ASSERT_TRUE(g);
    EXPECT_EQ(g->kind, Kind::Getter);
    EXPECT_EQ(g->text, "Getter");
    EXPECT_FALSE(g->expandable);
    EXPECT_EQ(page.text("ran"), "0");
}

TEST(ConsoleFormat, DepthLimitAndCycles) {
    Page page;
    ValuePreview p = page.eval("({ l1: { l2: { l3: { l4: { l5: 1 } } } } })");
    const ValuePreview* l1 = childNamed(p, "l1");
    ASSERT_TRUE(l1);
    const ValuePreview* l2 = childNamed(*l1, "l2");
    ASSERT_TRUE(l2);
    const ValuePreview* l3 = childNamed(*l2, "l3");
    ASSERT_TRUE(l3);
    // Materialized three levels down; the next level keeps its preview, but holds nothing
    EXPECT_EQ(l3->text, "Object { l4: {…} }");
    EXPECT_TRUE(l3->expandable);
    EXPECT_TRUE(l3->children.empty());

    page.script->evaluate("var cyc = { name: 'c', list: [] }; cyc.self = cyc; cyc.list.push(cyc);", "test.js");
    ValuePreview c = page.eval("cyc");
    EXPECT_EQ(c.text, "Object { name: \"c\", list: (1) […], self: {…} }");
    const ValuePreview* self = childNamed(c, "self");
    ASSERT_TRUE(self);
    EXPECT_TRUE(self->cyclic);
    EXPECT_FALSE(self->expandable);
    EXPECT_EQ(self->text, "{…} (circular)");
    const ValuePreview* list = childNamed(c, "list");
    ASSERT_TRUE(list);
    ASSERT_FALSE(list->children.empty());
    EXPECT_TRUE(list->children[0].cyclic);
    // The same object twice without a cycle is not circular
    EXPECT_EQ(page.text("(function () { var o = { a: 1 }; return [o, o]; })()"), "Array(2) [ {…}, {…} ]");
}

// ---------------------------------------------------------------------------
// Functions, errors, built-ins
// ---------------------------------------------------------------------------

TEST(ConsoleFormat, Functions) {
    Page page;
    EXPECT_EQ(page.text("function foo(a, b) {}"), "undefined");
    EXPECT_EQ(page.text("foo"), "function foo()");
    EXPECT_EQ(page.text("(function () {})"), "function ()");
    EXPECT_EQ(page.text("async function af() {}; af"), "async function af()");
    EXPECT_EQ(page.text("console.log"), "function log()");
    ValuePreview p = page.eval("foo");
    EXPECT_EQ(p.kind, Kind::Function);
    EXPECT_EQ(childNamed(p, "name")->text, "\"foo\"");
    EXPECT_EQ(childNamed(p, "length")->text, "2");
    EXPECT_EQ(page.text("({ f: foo })"), "Object { f: function foo() }");
}

TEST(ConsoleFormat, Errors) {
    Page page;
    ValuePreview p = page.eval("new TypeError('bad thing')");
    EXPECT_EQ(p.kind, Kind::Error);
    EXPECT_EQ(p.text.substr(0, 20), "TypeError: bad thing");
    EXPECT_TRUE(p.expandable);
    const ValuePreview* message = childNamed(p, "message");
    ASSERT_TRUE(message);
    EXPECT_EQ(message->text, "\"bad thing\"");
    EXPECT_TRUE(childNamed(p, "stack"));
    EXPECT_EQ(page.text("[new Error('x')]"), "Array(1) [ Error: x ]");
    // A thrown error, caught in a script, keeps its stack lines under the header
    page.script->evaluate("function thrower() { throw new RangeError('out'); }\n"
                          "var caught; try { thrower(); } catch (e) { caught = e; }", "test.js");
    ValuePreview c = page.eval("caught");
    EXPECT_EQ(c.text.substr(0, 15), "RangeError: out");
    EXPECT_NE(c.text.find("\n    at thrower"), std::string::npos) << c.text;
}

TEST(ConsoleFormat, MapsAndSets) {
    Page page;
    ValuePreview m = page.eval("new Map([['a', 1], ['b', { x: 1 }]])");
    EXPECT_EQ(m.kind, Kind::Map);
    EXPECT_EQ(m.text, "Map(2) { \"a\" → 1, \"b\" → {…} }");
    EXPECT_EQ(keys(m), "0,1,size");
    EXPECT_EQ(m.children[0].kind, Kind::Entry);
    EXPECT_EQ(m.children[0].text, "\"a\" → 1");
    EXPECT_EQ(keys(m.children[0]), "<key>,<value>");
    EXPECT_EQ(m.children[0].children[1].text, "1");
    EXPECT_EQ(m.children[2].text, "2");
    EXPECT_EQ(page.text("new Map()"), "Map(0)");

    ValuePreview s = page.eval("new Set([1, 'two', [3]])");
    EXPECT_EQ(s.kind, Kind::Set);
    EXPECT_EQ(s.text, "Set(3) [ 1, \"two\", (1) […] ]");
    EXPECT_EQ(keys(s), "0,1,2,size");
    EXPECT_EQ(s.children[1].text, "\"two\"");
    EXPECT_EQ(page.text("({ m: new Map([[1, 2]]), s: new Set() })"), "Object { m: Map(1), s: Set(0) }");
}

TEST(ConsoleFormat, OtherBuiltIns) {
    Page page;
    // (the engine's toISOString prints the local time; the preview takes it as is)
    EXPECT_EQ(page.text("new Date(0)").substr(0, 16), "Date 1970-01-01T");
    EXPECT_FALSE(page.eval("new Date(0)").expandable);
    EXPECT_EQ(page.text("/a+b/gi"), "/a+b/gi");
    EXPECT_FALSE(page.eval("/x/").expandable);
    EXPECT_EQ(page.text("Promise.resolve(3)"), "Promise { <state>: \"fulfilled\", <value>: 3 }");
    EXPECT_EQ(page.text("new Promise(function () {})"), "Promise { <state>: \"pending\" }");
}

// ---------------------------------------------------------------------------
// DOM nodes
// ---------------------------------------------------------------------------

TEST(ConsoleFormat, DomNodes) {
    Page page;
    ValuePreview p = page.eval("document.getElementById('card')");
    EXPECT_EQ(p.kind, Kind::Node);
    EXPECT_EQ(p.text, "<div id=\"card\" class=\"box demo\" data-ticks=\"3\">");
    EXPECT_TRUE(p.expandable);
    EXPECT_EQ(childNamed(p, "tagName")->text, "\"DIV\"");
    EXPECT_EQ(childNamed(p, "id")->text, "\"card\"");
    EXPECT_EQ(childNamed(p, "className")->text, "\"box demo\"");
    const ValuePreview* attrs = childNamed(p, "attributes");
    ASSERT_TRUE(attrs);
    EXPECT_EQ(attrs->text, "NamedNodeMap(3)");
    EXPECT_EQ(keys(*attrs), "id,class,data-ticks");
    const ValuePreview* kids = childNamed(p, "childNodes");
    ASSERT_TRUE(kids);
    EXPECT_EQ(kids->text, "NodeList(2)");
    EXPECT_EQ(kids->children[0].text, "<p id=\"first\">");
    EXPECT_EQ(childNamed(p, "textContent")->text, "\"onetwo\"");

    EXPECT_EQ(page.text("document.body"), "<body>");
    EXPECT_EQ(page.text("document"), "HTMLDocument");
    EXPECT_EQ(page.text("document.getElementById('first').firstChild"), "#text \"one\"");
    EXPECT_FALSE(page.eval("document.getElementById('first').firstChild").expandable);
    EXPECT_EQ(page.text("document.createComment(' note ')"), "<!-- note -->");
    // Nested, a node is its selector
    EXPECT_EQ(page.text("[document.getElementById('card'), document.body]"), "Array(2) [ <div#card.box.demo>, <body> ]");
    EXPECT_EQ(page.text("({ el: document.querySelector('.note') })"), "Object { el: <p.note> }");
}

TEST(ConsoleFormat, NodeLists) {
    Page page;
    ValuePreview p = page.eval("document.querySelectorAll('p')");
    EXPECT_EQ(p.kind, Kind::Array);
    EXPECT_EQ(p.text, "Array(2) [ <p#first>, <p.note> ]");
    ASSERT_GE(p.children.size(), 2u);
    EXPECT_EQ(p.children[0].kind, Kind::Node);
    EXPECT_EQ(p.children[0].text, "<p id=\"first\">");
}

// ---------------------------------------------------------------------------
// The console object
// ---------------------------------------------------------------------------

TEST(ConsoleFormat, ConsoleMethodsAndLevels) {
    Page page;
    EXPECT_EQ(page.logged("console.info('i')").level, "info");
    EXPECT_EQ(page.logged("console.debug('d')").level, "debug");
    EXPECT_EQ(page.logged("console.warn('w', 1)").text, "w 1");
    EXPECT_EQ(page.logged("console.warn('w', 1)").level, "warn");
    EXPECT_EQ(page.logged("console.error(new Error('e'))").text.substr(0, 8), "Error: e");
    const ConsoleMessage& dir = page.logged("console.dir({ a: 1 })");
    EXPECT_TRUE(dir.values[0].expanded);
    EXPECT_EQ(page.logged("console.table([{ a: 1 }])").text, "Array(1) [ {…} ]");
    const ConsoleMessage& trace = page.logged("function tr() { console.trace('here'); } tr();");
    EXPECT_EQ(trace.level, "trace");
    EXPECT_EQ(trace.text.substr(0, 20), "console.trace() here");
    EXPECT_NE(trace.text.find("at tr"), std::string::npos) << trace.text;
}

TEST(ConsoleFormat, FormatSubstitutions) {
    Page page;
    EXPECT_EQ(page.logged("console.log('%s is %d years, %f tall', 'Ann', 41.9, 1.5, 'extra')").text,
              "Ann is 41 years, 1.5 tall extra");
    EXPECT_EQ(page.logged("console.log('%o and %O', [1], { a: 1 })").text, "Array(1) [ 1 ] and Object { a: 1 }");
    EXPECT_EQ(page.logged("console.log('%cstyled', 'color: red')").text, "styled");
    EXPECT_EQ(page.logged("console.log('100%', 'done')").text, "100% done");
    EXPECT_EQ(page.logged("console.log('%s')").text, "%s");
}

TEST(ConsoleFormat, AssertCountTimeGroup) {
    Page page;
    size_t before = page.console.size();
    page.script->evaluate("console.assert(true, 'not shown')", "test.js");
    EXPECT_EQ(page.console.size(), before);
    const ConsoleMessage& failed = page.logged("console.assert(1 > 2, 'math', 3)");
    EXPECT_EQ(failed.level, "error");
    EXPECT_EQ(failed.text, "Assertion failed: math 3");
    EXPECT_EQ(page.logged("console.assert(false)").text, "Assertion failed");

    EXPECT_EQ(page.logged("console.count()").text, "default: 1");
    EXPECT_EQ(page.logged("console.count('x'); console.count('x')").text, "x: 2");
    EXPECT_EQ(page.logged("console.count()").text, "default: 2");
    EXPECT_EQ(page.logged("console.countReset('x'); console.count('x')").text, "x: 1");

    const ConsoleMessage& t = page.logged("console.time('t'); console.timeEnd('t')");
    EXPECT_EQ(t.text.substr(0, 3), "t: ");
    EXPECT_NE(t.text.find("ms - timer ended"), std::string::npos) << t.text;
    EXPECT_EQ(page.logged("console.timeEnd('nope')").level, "warn");

    page.script->evaluate("console.group('G'); console.log('in'); console.group(); console.log('deeper');"
                          " console.groupEnd(); console.groupEnd(); console.log('out')", "test.js");
    size_t n = page.console.size();
    ASSERT_GE(n, 5u);
    EXPECT_EQ(page.console[n - 5].level, "group");
    EXPECT_EQ(page.console[n - 5].text, "G");
    EXPECT_EQ(page.console[n - 5].depth, 0);
    EXPECT_EQ(page.console[n - 4].text, "in");
    EXPECT_EQ(page.console[n - 4].depth, 1);
    EXPECT_EQ(page.console[n - 3].level, "group");
    EXPECT_EQ(page.console[n - 3].text, "console.group");
    EXPECT_EQ(page.console[n - 3].depth, 1);
    EXPECT_EQ(page.console[n - 2].text, "deeper");
    EXPECT_EQ(page.console[n - 2].depth, 2);
    EXPECT_EQ(page.console[n - 1].text, "out");
    EXPECT_EQ(page.console[n - 1].depth, 0);
    EXPECT_EQ(page.logged("console.clear()").level, "clear");
}

TEST(ConsoleFormat, TextHandlerStillGetsPlainText) {
    HTMLParser parser;
    auto doc = parser.parse("<p>x</p>");
    ScriptContext script;
    std::vector<std::string> lines;
    script.setConsoleHandler([&](const std::string& level, const std::string& message) {
        lines.push_back(level + ": " + message);
    });
    script.initialize(doc.get());
    script.evaluate("console.log('a', { b: 1 }); console.warn('w'); console.clear(); console.error('e')", "test.js");
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], "log: a Object { b: 1 }");
    EXPECT_EQ(lines[1], "warn: w");
    EXPECT_EQ(lines[2], "error: e");
}

// ---------------------------------------------------------------------------
// Console input: helpers and errors
// ---------------------------------------------------------------------------

TEST(ConsoleFormat, ConsoleHelpers) {
    Page page;
    DOMElement* card = page.doc->getElementById("card");
    EXPECT_EQ(page.eval("$0", card).text, "<div id=\"card\" class=\"box demo\" data-ticks=\"3\">");
    EXPECT_EQ(page.eval("$0 === document.getElementById('card')", card).text, "true");
    EXPECT_EQ(page.eval("$0").text, "undefined");
    EXPECT_EQ(page.text("$('#first')"), "<p id=\"first\">");
    EXPECT_EQ(page.text("$('.nope')"), "null");
    EXPECT_EQ(page.text("$$('p')"), "Array(2) [ <p#first>, <p.note> ]");
    EXPECT_EQ(page.text("$$('p').length"), "2");
    EXPECT_EQ(page.text("$_"), "2");
    EXPECT_EQ(page.text("1 + 2"), "3");
    EXPECT_EQ(page.text("$_ * 2"), "6");
    EXPECT_EQ(page.text("copy('x')"), "undefined");
    ConsoleMessage cleared = page.script->evaluateConsole("clear()");
    EXPECT_EQ(cleared.level, "result");
    ASSERT_FALSE(page.console.empty());
    EXPECT_EQ(page.console.back().level, "clear");
}

TEST(ConsoleFormat, PageGlobalsWinOverHelpers) {
    Page page;
    page.script->evaluate("var $ = function () { return 'jquery'; };", "test.js");
    EXPECT_EQ(page.text("$('#first')"), "\"jquery\"");
    EXPECT_EQ(page.text("$$('p').length"), "2");
}

TEST(ConsoleFormat, InputErrors) {
    Page page;
    size_t before = page.console.size();
    ConsoleMessage m = page.script->evaluateConsole("null.x");
    EXPECT_EQ(m.level, "error");
    EXPECT_EQ(m.text.substr(0, 10), "TypeError:") << m.text;
    ASSERT_EQ(m.values.size(), 1u);
    EXPECT_EQ(m.values[0].kind, Kind::Error);
    EXPECT_TRUE(m.values[0].expandable);
    // An error from console input is reported once, by the result, not through the handler
    EXPECT_EQ(page.console.size(), before);

    ConsoleMessage thrown = page.script->evaluateConsole("throw new RangeError('r')");
    EXPECT_EQ(thrown.text.substr(0, 13), "RangeError: r");
    ConsoleMessage plain = page.script->evaluateConsole("throw 'just a string'");
    EXPECT_EQ(plain.level, "error");
    EXPECT_EQ(plain.text, "\"just a string\"");

    ConsoleMessage syntax = page.script->evaluateConsole("let x = ;");
    EXPECT_EQ(syntax.level, "error");
    EXPECT_EQ(syntax.text.substr(0, 13), "SyntaxError: ");
    EXPECT_EQ(syntax.text.find(":1:"), std::string::npos) << syntax.text;   // the position is in `source`
    EXPECT_EQ(syntax.source.substr(0, 12), "<console>:1:") << syntax.source;
    // Page scripts still report their errors through the handler
    page.script->evaluate("undefinedFunction()", "test.js");
    ASSERT_GT(page.console.size(), before);
    EXPECT_EQ(page.console.back().level, "error");
}
