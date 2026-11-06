// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * devtools_console_input_tests.cpp - The DevTools Console's input and output
 * filter apart from drawing: the multi-line editor, the unfinished-input
 * check behind Enter, the autocomplete query and ranking, the text and level
 * filter, and the names source links show.
 */

#include <gtest/gtest.h>

#include "devtools/console_input.hpp"
#include "scripting/console_format.hpp"

#include <string>
#include <vector>

using namespace ZepraBrowser::DevTools;
using Zepra::WebCore::ConsoleMessage;

namespace {

ConsoleMessage message(const std::string& level, const std::string& text, const std::string& source = "") {
    ConsoleMessage m;
    m.level = level;
    m.text = text;
    m.source = source;
    return m;
}

std::vector<std::string> path(std::initializer_list<const char*> parts) {
    return std::vector<std::string>(parts.begin(), parts.end());
}

} // namespace

// ---------------------------------------------------------------------------
// Editor
// ---------------------------------------------------------------------------

TEST(ConsoleEditor, InsertsAtTheCaret) {
    ConsoleEditor e;
    e.insert("ac");
    e.left();
    e.insert("b");
    EXPECT_EQ(e.text(), "abc");
    EXPECT_EQ(e.caret(), 2u);
    e.end();
    e.backspace();
    EXPECT_EQ(e.text(), "ab");
    e.home();
    e.deleteForward();
    EXPECT_EQ(e.text(), "b");
}

TEST(ConsoleEditor, KeepsCodePointsWhole) {
    ConsoleEditor e;
    e.insert("a\xC3\xA9");          // "aé"
    e.left();
    EXPECT_EQ(e.caret(), 1u);       // before the two-byte é, never inside it
    e.right();
    e.backspace();
    EXPECT_EQ(e.text(), "a");
    e.setText("\xE2\x82\xAC");      // "€"
    e.setCaret(2);
    EXPECT_EQ(e.caret(), 0u);
    e.end();
    e.backspace();
    EXPECT_TRUE(e.empty());
}

TEST(ConsoleEditor, PasteNormalisesLineEnds) {
    ConsoleEditor e;
    e.insert("a\r\nb\rc");
    EXPECT_EQ(e.text(), "a\nb\nc");
    EXPECT_EQ(e.lineCount(), 3u);
    EXPECT_EQ(e.caretLine(), 2u);
}

TEST(ConsoleEditor, UpAndDownKeepTheColumn) {
    ConsoleEditor e;
    e.setText("abcd\nx\nwxyz");
    e.moveTo(2, 3);
    EXPECT_EQ(e.caretLinePrefix(), "wxy");
    EXPECT_TRUE(e.up());                  // the short middle line: its end
    EXPECT_EQ(e.caretLine(), 1u);
    EXPECT_EQ(e.caretLinePrefix(), "x");
    EXPECT_TRUE(e.up());
    EXPECT_EQ(e.caretLinePrefix(), "a");
    EXPECT_FALSE(e.up());                 // first line: the console walks its history
    e.moveTo(1, 1);
    EXPECT_TRUE(e.down());
    EXPECT_EQ(e.caretLinePrefix(), "w");
    EXPECT_FALSE(e.down());               // last line
}

TEST(ConsoleEditor, ReplacesTheTypedPrefix) {
    ConsoleEditor e;
    e.setText("document.bo");
    e.replaceBeforeCaret(9, "body");
    EXPECT_EQ(e.text(), "document.body");
    EXPECT_EQ(e.caret(), e.text().size());
    EXPECT_EQ(e.lines(), path({"document.body"}));
}

// ---------------------------------------------------------------------------
// Unfinished input
// ---------------------------------------------------------------------------

TEST(ConsoleInput, OpenBracketsAreUnfinished) {
    EXPECT_TRUE(isIncompleteInput("function f() {"));
    EXPECT_TRUE(isIncompleteInput("foo("));
    EXPECT_TRUE(isIncompleteInput("[1, 2,"));
    EXPECT_TRUE(isIncompleteInput("if (a) { b(["));
    EXPECT_FALSE(isIncompleteInput("function f() { return [1]; }"));
    EXPECT_FALSE(isIncompleteInput("1 + 2"));
    EXPECT_FALSE(isIncompleteInput(""));
}

TEST(ConsoleInput, StringsCommentsAndTemplates) {
    EXPECT_FALSE(isIncompleteInput("'{'"));              // a bracket in a string does not count
    EXPECT_FALSE(isIncompleteInput("// {"));
    EXPECT_FALSE(isIncompleteInput("/* ( */ 1"));
    EXPECT_TRUE(isIncompleteInput("/* still open"));
    EXPECT_TRUE(isIncompleteInput("`line one"));
    EXPECT_TRUE(isIncompleteInput("`a ${b"));
    EXPECT_FALSE(isIncompleteInput("`a ${ {x: 1}.x } b`"));
    EXPECT_TRUE(isIncompleteInput("'abc\\"));             // a backslash continues the string
    EXPECT_FALSE(isIncompleteInput("'abc"));               // unterminated: the engine reports it
}

TEST(ConsoleInput, StrayClosersRunAndReportTheError) {
    EXPECT_FALSE(isIncompleteInput("}"));
    EXPECT_FALSE(isIncompleteInput("(]"));
}

// ---------------------------------------------------------------------------
// Autocomplete
// ---------------------------------------------------------------------------

TEST(ConsoleCompletion, GlobalsAndProperties) {
    CompletionQuery q = completionQuery("doc", 3);
    ASSERT_TRUE(q.valid);
    EXPECT_TRUE(q.path.empty());
    EXPECT_EQ(q.prefix, "doc");
    EXPECT_EQ(q.replaceFrom, 0u);

    q = completionQuery("x = document.bo", 15);
    ASSERT_TRUE(q.valid);
    EXPECT_EQ(q.path, path({"document"}));
    EXPECT_EQ(q.prefix, "bo");
    EXPECT_EQ(q.replaceFrom, 13u);

    q = completionQuery("a.b.", 4);
    ASSERT_TRUE(q.valid);
    EXPECT_EQ(q.path, path({"a", "b"}));
    EXPECT_EQ(q.prefix, "");

    q = completionQuery("a?.b", 4);
    ASSERT_TRUE(q.valid);
    EXPECT_EQ(q.path, path({"a"}));
    EXPECT_EQ(q.prefix, "b");
}

TEST(ConsoleCompletion, SpreadStartsANewChain) {
    CompletionQuery q = completionQuery("[...doc", 7);
    ASSERT_TRUE(q.valid);
    EXPECT_TRUE(q.path.empty());
    EXPECT_EQ(q.prefix, "doc");
    q = completionQuery("f(...a.b", 8);
    ASSERT_TRUE(q.valid);
    EXPECT_EQ(q.path, path({"a"}));
}

TEST(ConsoleCompletion, OnlyWhereCodeIsTyped) {
    EXPECT_FALSE(completionQuery("'doc", 4).valid);
    EXPECT_FALSE(completionQuery("// doc", 6).valid);
    EXPECT_FALSE(completionQuery("`doc", 4).valid);
    EXPECT_TRUE(completionQuery("`${doc", 6).valid);    // a template substitution is code
    EXPECT_FALSE(completionQuery("f().x", 5).valid);     // would need a call
    EXPECT_FALSE(completionQuery("a[0].x", 6).valid);
    EXPECT_FALSE(completionQuery("1.5", 3).valid);
    EXPECT_FALSE(completionQuery("1.", 2).valid);
    EXPECT_FALSE(completionQuery("document", 3).valid);  // caret inside a word
}

TEST(ConsoleCompletion, RankingPutsExactCaseFirst) {
    std::vector<std::string> names = {"Document", "document", "documentElement", "dir", "doc", "zzz", "document"};
    std::vector<std::string> r = rankCompletions(names, "doc");
    EXPECT_EQ(r, path({"doc", "document", "documentElement", "Document"}));
    EXPECT_EQ(rankCompletions(names, "doc", 2), path({"doc", "document"}));
    EXPECT_TRUE(rankCompletions(names, "q").empty());
}

TEST(ConsoleCompletion, EngineInternalsStayHidden) {
    std::vector<std::string> names = {"__pageUrl__", "__document_ref__", "_private"};
    EXPECT_EQ(rankCompletions(names, "_"), path({"_private"}));
    EXPECT_EQ(rankCompletions(names, "__"), path({"__document_ref__", "__pageUrl__"}));
}

// ---------------------------------------------------------------------------
// Filter
// ---------------------------------------------------------------------------

TEST(ConsoleFilter, LevelsHaveTheirButtons) {
    EXPECT_EQ(filterLevel(message("error", "")), "error");
    EXPECT_EQ(filterLevel(message("warn", "")), "warn");
    EXPECT_EQ(filterLevel(message("info", "")), "info");
    EXPECT_EQ(filterLevel(message("debug", "")), "debug");
    EXPECT_EQ(filterLevel(message("log", "")), "log");
    EXPECT_EQ(filterLevel(message("trace", "")), "log");
    EXPECT_EQ(filterLevel(message("group", "")), "log");
    EXPECT_EQ(filterLevel(message("input", "")), "");
    EXPECT_EQ(filterLevel(message("result", "")), "");

    ConsoleFilter f;
    f.info = false;
    f.errors = false;
    ConsoleFilterMatcher m(f);
    EXPECT_FALSE(m.passes(message("info", "x")));
    EXPECT_FALSE(m.passes(message("error", "x")));
    EXPECT_TRUE(m.passes(message("log", "x")));
    EXPECT_TRUE(m.passes(message("input", "x")));   // what was typed always shows
    EXPECT_FALSE(m.hasText());
}

TEST(ConsoleFilter, TextIgnoresCaseAndMatchesTheSource) {
    ConsoleFilter f;
    f.text = "HELLO";
    ConsoleFilterMatcher m(f);
    EXPECT_TRUE(m.hasText());
    EXPECT_TRUE(m.passes(message("log", "say hello world")));
    EXPECT_FALSE(m.passes(message("log", "goodbye")));
    f.text = "app.js";
    EXPECT_TRUE(ConsoleFilterMatcher(f).passes(message("log", "x", "https://a.org/app.js:3")));
}

TEST(ConsoleFilter, MinusExcludes) {
    ConsoleFilter f;
    f.text = "-noise";
    ConsoleFilterMatcher m(f);
    EXPECT_FALSE(m.passes(message("log", "some NOISE here")));
    EXPECT_TRUE(m.passes(message("log", "signal")));
    f.text = "-";
    EXPECT_FALSE(ConsoleFilterMatcher(f).passes(message("log", "no dash")));   // a lone "-" is text
}

TEST(ConsoleFilter, RegularExpressions) {
    ConsoleFilter f;
    f.text = "/^id=\\d+$/";
    ConsoleFilterMatcher m(f);
    EXPECT_TRUE(m.passes(message("log", "id=42")));
    EXPECT_FALSE(m.passes(message("log", "ID=42")));
    f.text = "/^id=\\d+$/i";
    EXPECT_TRUE(ConsoleFilterMatcher(f).passes(message("log", "ID=42")));
    f.text = "-/err/";
    EXPECT_FALSE(ConsoleFilterMatcher(f).passes(message("log", "an err")));
    f.text = "/[/";                                   // not a valid expression: the text itself
    EXPECT_TRUE(ConsoleFilterMatcher(f).passes(message("log", "a /[/ b")));
    EXPECT_FALSE(ConsoleFilterMatcher(f).passes(message("log", "a b")));
}

// ---------------------------------------------------------------------------
// Source links
// ---------------------------------------------------------------------------

TEST(ConsoleSource, DisplayNames) {
    EXPECT_EQ(sourceDisplayName("https://a.org/js/app.js?v=2"), "app.js");
    EXPECT_EQ(sourceDisplayName("https://a.org/js/app.js#top"), "app.js");
    EXPECT_EQ(sourceDisplayName("file:///home/u/page.html"), "page.html");
    EXPECT_EQ(sourceDisplayName("https://a.org/docs/"), "docs");
    EXPECT_EQ(sourceDisplayName("<console>"), "<console>");
    EXPECT_EQ(sourceDisplayName("app.js"), "app.js");
}
