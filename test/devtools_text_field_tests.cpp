// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file devtools_text_field_tests.cpp
 * @brief The DevTools text field: caret and selection movement over UTF-8,
 *        word movement, editing keys, clipboard results, single-line and
 *        multiline behaviour, and invalid input.
 */

#include <gtest/gtest.h>

#include "devtools/text_field.hpp"

#include <string>
#include <vector>

using namespace ZepraBrowser::DevTools;
using NXRender::KeyCode;
using R = TextField::Result;

namespace {

/// A field with the caret at the end and nothing selected
TextField field(const std::string& text, bool multiline = false) {
    TextField f(text, multiline);
    f.moveCaretTo(text.size());
    return f;
}

R key(TextField& f, KeyCode k, bool ctrl = false, bool shift = false) {
    return f.onKey(k, "", ctrl, shift);
}

R type(TextField& f, const std::string& text) {
    return f.onKey(KeyCode::Unknown, text, false, false);
}

/// Whether the bytes are well-formed UTF-8 (lead and continuation bytes line up)
bool validUtf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char b = (unsigned char)s[i];
        size_t len = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 0;
        if (!len || i + len > s.size()) return false;
        for (size_t k = 1; k < len; ++k) {
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        }
        i += len;
    }
    return true;
}

} // namespace

// ---- construction and setText ----

TEST(DevToolsTextField, ConstructorSelectsAll) {
    TextField f("hello");
    EXPECT_EQ(f.text(), "hello");
    EXPECT_FALSE(f.multiline());
    EXPECT_TRUE(f.hasSelection());
    EXPECT_EQ(f.selectionStart(), 0u);
    EXPECT_EQ(f.selectionEnd(), 5u);
    EXPECT_EQ(f.caret(), 5u);
    EXPECT_EQ(f.selectedText(), "hello");
}

TEST(DevToolsTextField, DefaultIsEmpty) {
    TextField f;
    EXPECT_EQ(f.text(), "");
    EXPECT_EQ(f.caret(), 0u);
    EXPECT_FALSE(f.hasSelection());
    EXPECT_EQ(key(f, KeyCode::Backspace), R::Moved);
    EXPECT_EQ(key(f, KeyCode::Delete), R::Moved);
    EXPECT_EQ(key(f, KeyCode::Left), R::Moved);
    EXPECT_EQ(f.lines(), std::vector<std::string>{""});
}

TEST(DevToolsTextField, SetTextWithoutSelectingPutsCaretAtEnd) {
    TextField f;
    f.setText("abc", false);
    EXPECT_FALSE(f.hasSelection());
    EXPECT_EQ(f.caret(), 3u);
    EXPECT_EQ(f.selectionStart(), 3u);
    EXPECT_EQ(f.selectionEnd(), 3u);
    EXPECT_EQ(f.selectedText(), "");
    f.setText("xy");
    EXPECT_EQ(f.selectedText(), "xy");
}

// ---- Left / Right ----

TEST(DevToolsTextField, LeftRightMoveByCodePoint) {
    TextField f = field("h\xC3\xA9llo");   // héllo
    EXPECT_EQ(key(f, KeyCode::Home), R::Moved);
    EXPECT_EQ(f.caret(), 0u);
    key(f, KeyCode::Right);
    EXPECT_EQ(f.caret(), 1u);
    key(f, KeyCode::Right);
    EXPECT_EQ(f.caret(), 3u);   // over the two bytes of é
    key(f, KeyCode::Left);
    EXPECT_EQ(f.caret(), 1u);
    key(f, KeyCode::Left);
    key(f, KeyCode::Left);      // stays at the start
    EXPECT_EQ(f.caret(), 0u);
}

TEST(DevToolsTextField, LeftRightOverCjkAndEmoji) {
    const std::string text = "\xE4\xBD\xA0\xE5\xA5\xBD\xF0\x9F\x98\x80!";   // 你好😀!
    TextField f = field(text);
    key(f, KeyCode::Left);
    EXPECT_EQ(f.caret(), 10u);
    key(f, KeyCode::Left);
    EXPECT_EQ(f.caret(), 6u);   // over the 4 bytes of the emoji
    key(f, KeyCode::Left);
    EXPECT_EQ(f.caret(), 3u);
    key(f, KeyCode::Left);
    EXPECT_EQ(f.caret(), 0u);
    key(f, KeyCode::Right);
    key(f, KeyCode::Right);
    key(f, KeyCode::Right);
    EXPECT_EQ(f.caret(), 10u);
    key(f, KeyCode::Right);
    key(f, KeyCode::Right);
    EXPECT_EQ(f.caret(), 11u);
}

TEST(DevToolsTextField, ShiftExtendsSelection) {
    TextField f = field("h\xC3\xA9llo");
    key(f, KeyCode::Home);
    key(f, KeyCode::Right, false, true);
    key(f, KeyCode::Right, false, true);
    EXPECT_TRUE(f.hasSelection());
    EXPECT_EQ(f.selectedText(), "h\xC3\xA9");
    key(f, KeyCode::Left, false, true);
    EXPECT_EQ(f.selectedText(), "h");
    // Back past the anchor: the selection flips to the other side
    key(f, KeyCode::End);
    key(f, KeyCode::Left, false, true);
    key(f, KeyCode::Left, false, true);
    EXPECT_EQ(f.selectedText(), "lo");
    EXPECT_EQ(f.caret(), 4u);
    EXPECT_EQ(f.selectionStart(), 4u);
    EXPECT_EQ(f.selectionEnd(), 6u);
}

TEST(DevToolsTextField, LeftRightCollapseSelection) {
    TextField f("abcdef");          // all selected
    key(f, KeyCode::Left);
    EXPECT_FALSE(f.hasSelection());
    EXPECT_EQ(f.caret(), 0u);
    f.selectAll();
    key(f, KeyCode::Right);
    EXPECT_FALSE(f.hasSelection());
    EXPECT_EQ(f.caret(), 6u);
}

// ---- words ----

TEST(DevToolsTextField, CtrlLeftRightMoveByWord) {
    TextField f = field("margin: 10px  auto;");
    key(f, KeyCode::Left, true);
    EXPECT_EQ(f.caret(), 14u);   // start of "auto"
    key(f, KeyCode::Left, true);
    EXPECT_EQ(f.caret(), 8u);    // start of "10px"
    key(f, KeyCode::Left, true);
    EXPECT_EQ(f.caret(), 0u);
    key(f, KeyCode::Left, true);
    EXPECT_EQ(f.caret(), 0u);
    key(f, KeyCode::Right, true);
    EXPECT_EQ(f.caret(), 6u);    // end of "margin"
    key(f, KeyCode::Right, true);
    EXPECT_EQ(f.caret(), 12u);   // end of "10px"
    key(f, KeyCode::Right, true);
    EXPECT_EQ(f.caret(), 18u);   // end of "auto"
    key(f, KeyCode::Right, true);
    EXPECT_EQ(f.caret(), 19u);   // the trailing ';'
}

TEST(DevToolsTextField, CtrlShiftSelectsWords) {
    TextField f = field("font-family: Noto Sans");
    key(f, KeyCode::Left, true, true);
    key(f, KeyCode::Left, true, true);
    EXPECT_EQ(f.selectedText(), "Noto Sans");
}

TEST(DevToolsTextField, WordsInMultibyteText) {
    // "héllo wörld": the accented letters belong to the words
    TextField f = field("h\xC3\xA9llo w\xC3\xB6rld");
    key(f, KeyCode::Left, true);
    EXPECT_EQ(f.caret(), 7u);
    key(f, KeyCode::Left, true);
    EXPECT_EQ(f.caret(), 0u);
    key(f, KeyCode::Right, true);
    EXPECT_EQ(f.caret(), 6u);
    // CJK punctuation separates words: 你好。世界
    TextField c = field("\xE4\xBD\xA0\xE5\xA5\xBD\xE3\x80\x82\xE4\xB8\x96\xE7\x95\x8C");
    key(c, KeyCode::Left, true);
    EXPECT_EQ(c.caret(), 9u);
    key(c, KeyCode::Left, true);
    EXPECT_EQ(c.caret(), 0u);
}

TEST(DevToolsTextField, CtrlBackspaceAndDeleteRemoveAWord) {
    TextField f = field("solid 1px red");
    EXPECT_EQ(key(f, KeyCode::Backspace, true), R::Changed);
    EXPECT_EQ(f.text(), "solid 1px ");
    EXPECT_EQ(key(f, KeyCode::Backspace, true), R::Changed);
    EXPECT_EQ(f.text(), "solid ");
    key(f, KeyCode::Home);
    EXPECT_EQ(key(f, KeyCode::Delete, true), R::Changed);
    EXPECT_EQ(f.text(), " ");
    EXPECT_EQ(f.caret(), 0u);
}

TEST(DevToolsTextField, SelectWordAt) {
    TextField f = field("color: rgb(1, 2, 3)");
    f.selectWordAt(9);
    EXPECT_EQ(f.selectedText(), "rgb");
    f.selectWordAt(5);
    EXPECT_EQ(f.selectedText(), ": ");
    f.selectWordAt(100);
    EXPECT_EQ(f.selectedText(), ")");
    TextField e;
    e.selectWordAt(3);
    EXPECT_FALSE(e.hasSelection());
}

// ---- Home / End ----

TEST(DevToolsTextField, HomeEndSingleLine) {
    TextField f = field("abc def");
    key(f, KeyCode::Home);
    EXPECT_EQ(f.caret(), 0u);
    key(f, KeyCode::End, false, true);
    EXPECT_EQ(f.selectedText(), "abc def");
    key(f, KeyCode::Home, true);
    EXPECT_EQ(f.caret(), 0u);
    EXPECT_FALSE(f.hasSelection());
    key(f, KeyCode::End, true);
    EXPECT_EQ(f.caret(), 7u);
}

TEST(DevToolsTextField, HomeEndMultilineUseTheLine) {
    TextField f = field("one\ntwo two\nthree", true);
    f.moveCaretTo(6);           // inside "two two"
    key(f, KeyCode::Home);
    EXPECT_EQ(f.caret(), 4u);
    key(f, KeyCode::End);
    EXPECT_EQ(f.caret(), 11u);
    key(f, KeyCode::Home, true, true);   // Ctrl+Shift+Home: to the start of the text
    EXPECT_EQ(f.caret(), 0u);
    EXPECT_EQ(f.selectedText(), "one\ntwo two");
    key(f, KeyCode::End, true);
    EXPECT_EQ(f.caret(), f.text().size());
}

// ---- Up / Down ----

TEST(DevToolsTextField, UpDownKeepTheColumnAndClampIt) {
    TextField f = field("abcdef\nab\nabcdefgh", true);
    f.moveCaretTo(5);           // column 5 of line 0
    EXPECT_EQ(f.caretLine(), 0u);
    EXPECT_EQ(f.caretColumn(), 5u);
    EXPECT_EQ(key(f, KeyCode::Down), R::Moved);
    EXPECT_EQ(f.caretLine(), 1u);
    EXPECT_EQ(f.caretColumn(), 2u);   // clamped to the short line
    key(f, KeyCode::Down);
    EXPECT_EQ(f.caretLine(), 2u);
    EXPECT_EQ(f.caretColumn(), 5u);   // the column comes back
    key(f, KeyCode::Up);
    key(f, KeyCode::Up);
    EXPECT_EQ(f.caret(), 5u);
    key(f, KeyCode::Up);              // first line: to the start
    EXPECT_EQ(f.caret(), 0u);
    f.moveCaretTo(12);
    key(f, KeyCode::Down);            // last line: to the end
    EXPECT_EQ(f.caret(), f.text().size());
}

TEST(DevToolsTextField, UpDownCountColumnsInCodePoints) {
    // Line 0 "héllo", line 1 "你好世界": column 3 is after "hél" and after "你好世"
    TextField f = field("h\xC3\xA9llo\n\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C", true);
    f.moveCaretTo(4);
    key(f, KeyCode::Down);
    EXPECT_EQ(f.caretLine(), 1u);
    EXPECT_EQ(f.caretColumn(), 9u);
    key(f, KeyCode::Up);
    EXPECT_EQ(f.caret(), 4u);
}

TEST(DevToolsTextField, ShiftUpDownSelectLines) {
    TextField f = field("ab\ncd", true);
    f.moveCaretTo(1);
    key(f, KeyCode::Down, false, true);
    EXPECT_EQ(f.selectedText(), "b\nc");
}

TEST(DevToolsTextField, UpDownInSingleLineAreThePanels) {
    TextField f = field("10px");
    EXPECT_EQ(key(f, KeyCode::Up), R::Ignored);
    EXPECT_EQ(key(f, KeyCode::Down), R::Ignored);
    EXPECT_EQ(f.caret(), 4u);
}

TEST(DevToolsTextField, LinesAndLineStarts) {
    TextField f = field("a\n\xC3\xA9t\xC3\xA9\n", true);
    EXPECT_EQ(f.lines(), (std::vector<std::string>{"a", "\xC3\xA9t\xC3\xA9", ""}));
    EXPECT_EQ(f.lineCount(), 3u);
    EXPECT_EQ(f.lineStart(0), 0u);
    EXPECT_EQ(f.lineStart(1), 2u);
    EXPECT_EQ(f.lineStart(2), 8u);
    EXPECT_EQ(f.lineStart(9), 8u);   // past the last line
    EXPECT_EQ(f.caretLine(), 2u);
    EXPECT_EQ(f.caretColumn(), 0u);
    f.moveCaretTo(5);
    EXPECT_EQ(f.caretLine(), 1u);
    EXPECT_EQ(f.caretColumn(), 3u);
}

TEST(DevToolsTextField, SingleLineIsOneLineEvenWithNewlines) {
    TextField f("a\nb");
    EXPECT_EQ(f.lines(), std::vector<std::string>{"a\nb"});
    EXPECT_EQ(f.lineCount(), 1u);
    EXPECT_EQ(f.caretLine(), 0u);
    EXPECT_EQ(f.caretColumn(), 3u);
    f.setMultiline(true);
    EXPECT_EQ(f.lineCount(), 2u);
    EXPECT_EQ(f.caretLine(), 1u);
}

// ---- Backspace / Delete ----

TEST(DevToolsTextField, BackspaceDeleteRemoveOneCodePoint) {
    TextField f = field("a\xC3\xA9\xF0\x9F\x98\x80z");   // aé😀z
    f.moveCaretTo(7);           // before z
    EXPECT_EQ(key(f, KeyCode::Backspace), R::Changed);
    EXPECT_EQ(f.text(), "a\xC3\xA9z");
    EXPECT_EQ(f.caret(), 3u);
    EXPECT_EQ(key(f, KeyCode::Backspace), R::Changed);
    EXPECT_EQ(f.text(), "az");
    key(f, KeyCode::Home);
    EXPECT_EQ(key(f, KeyCode::Backspace), R::Moved);   // nothing before the caret
    EXPECT_EQ(key(f, KeyCode::Delete), R::Changed);
    EXPECT_EQ(f.text(), "z");
    key(f, KeyCode::End);
    EXPECT_EQ(key(f, KeyCode::Delete), R::Moved);      // nothing after it
}

TEST(DevToolsTextField, DeleteRemovesCjkCodePoint) {
    TextField f = field("\xE4\xBD\xA0\xE5\xA5\xBD");
    key(f, KeyCode::Home);
    key(f, KeyCode::Delete);
    EXPECT_EQ(f.text(), "\xE5\xA5\xBD");
}

TEST(DevToolsTextField, BackspaceDeleteRemoveTheSelection) {
    TextField f = field("abcdef");
    f.moveCaretTo(1);
    f.moveCaretTo(4, true);
    EXPECT_EQ(key(f, KeyCode::Backspace), R::Changed);
    EXPECT_EQ(f.text(), "aef");
    EXPECT_EQ(f.caret(), 1u);
    f.moveCaretTo(0);
    f.moveCaretTo(2, true);
    EXPECT_EQ(key(f, KeyCode::Delete, true), R::Changed);   // Ctrl+Delete also only takes the selection
    EXPECT_EQ(f.text(), "f");
}

// ---- typing and insert ----

TEST(DevToolsTextField, TypingInsertsAtTheCaret) {
    TextField f = field("ac");
    f.moveCaretTo(1);
    EXPECT_EQ(f.onKey(KeyCode::B, "b", false, false), R::Changed);
    EXPECT_EQ(f.text(), "abc");
    EXPECT_EQ(f.caret(), 2u);
    EXPECT_EQ(type(f, "\xC3\xA9"), R::Changed);   // text without a key (IME)
    EXPECT_EQ(f.text(), "ab\xC3\xA9" "c");
    EXPECT_EQ(f.caret(), 4u);
    EXPECT_EQ(f.onKey(KeyCode::Space, "", false, false), R::Changed);
    EXPECT_EQ(f.text(), "ab\xC3\xA9 c");
    EXPECT_EQ(f.onKey(KeyCode::Space, " ", false, false), R::Changed);   // a single space, not two
    EXPECT_EQ(f.text(), "ab\xC3\xA9  c");
}

TEST(DevToolsTextField, TypingReplacesTheSelection) {
    TextField f("old value");   // all selected, as when editing starts
    EXPECT_EQ(type(f, "n"), R::Changed);
    EXPECT_EQ(f.text(), "n");
    EXPECT_FALSE(f.hasSelection());
    f.setText("10px solid red");
    f.moveCaretTo(5);
    f.moveCaretTo(10, true);
    f.insert("\xF0\x9F\x98\x80");
    EXPECT_EQ(f.text(), "10px \xF0\x9F\x98\x80 red");
    EXPECT_EQ(f.caret(), 9u);
}

TEST(DevToolsTextField, TextIsIgnoredWithCtrl) {
    TextField f = field("abc");
    EXPECT_EQ(f.onKey(KeyCode::B, "b", true, false), R::Ignored);
    EXPECT_EQ(f.text(), "abc");
}

TEST(DevToolsTextField, UnusedKeysAreNone) {
    TextField f = field("abc");
    EXPECT_EQ(key(f, KeyCode::F5), R::Ignored);
    EXPECT_EQ(key(f, KeyCode::PageUp), R::Ignored);
    EXPECT_EQ(key(f, KeyCode::Shift), R::Ignored);
    EXPECT_EQ(key(f, KeyCode::Tab, true), R::Ignored);
    EXPECT_EQ(f.text(), "abc");
}

TEST(DevToolsTextField, SingleLinePasteTurnsNewlinesIntoSpaces) {
    TextField f = field("");
    f.insert("a\nb\r\nc\rd\te");
    EXPECT_EQ(f.text(), "a b c d e");
    EXPECT_EQ(f.lineCount(), 1u);
}

TEST(DevToolsTextField, MultilinePasteNormalisesNewlines) {
    TextField f = field("", true);
    f.insert("a\r\nb\rc\n\td");
    EXPECT_EQ(f.text(), "a\nb\nc\n\td");
    EXPECT_EQ(f.lineCount(), 4u);
    EXPECT_EQ(f.caretLine(), 3u);
}

TEST(DevToolsTextField, InsertDropsControlCharacters) {
    TextField f = field("x");
    f.insert(std::string("a\x01\x1B\x7F" "b\xC2\x85" "c", 8));
    EXPECT_EQ(f.text(), "xabc");
    // Only control characters: nothing changes, the selection stays
    f.selectAll();
    EXPECT_EQ(type(f, "\x08"), R::Ignored);
    EXPECT_EQ(f.text(), "xabc");
    EXPECT_TRUE(f.hasSelection());
    f.insert("");
    EXPECT_EQ(f.text(), "xabc");
}

// ---- Enter / Escape / Tab ----

TEST(DevToolsTextField, EnterCommitsSingleLine) {
    TextField f = field("abc");
    EXPECT_EQ(key(f, KeyCode::Enter), R::Commit);
    EXPECT_EQ(key(f, KeyCode::Enter, false, true), R::Commit);
    EXPECT_EQ(f.onKey(KeyCode::Enter, "\r", false, false), R::Commit);
    EXPECT_EQ(f.text(), "abc");
}

TEST(DevToolsTextField, EnterInsertsNewlineInMultiline) {
    TextField f = field("ab", true);
    f.moveCaretTo(1);
    EXPECT_EQ(f.onKey(KeyCode::Enter, "\r", false, false), R::Changed);
    EXPECT_EQ(f.text(), "a\nb");
    EXPECT_EQ(f.caretLine(), 1u);
    EXPECT_EQ(f.caretColumn(), 0u);
    EXPECT_EQ(key(f, KeyCode::Enter, true), R::Commit);
    EXPECT_EQ(f.text(), "a\nb");
}

TEST(DevToolsTextField, EscapeCancelsTabMoves) {
    TextField f = field("abc", true);
    EXPECT_EQ(key(f, KeyCode::Escape), R::Cancel);
    EXPECT_EQ(f.onKey(KeyCode::Tab, "\t", false, false), R::Next);
    EXPECT_EQ(key(f, KeyCode::Tab, false, true), R::Previous);
    EXPECT_EQ(f.text(), "abc");
}

// ---- select all and the clipboard ----

TEST(DevToolsTextField, CtrlASelectsAll) {
    TextField f = field("h\xC3\xA9llo", true);
    EXPECT_EQ(key(f, KeyCode::A, true), R::Moved);
    EXPECT_EQ(f.selectionStart(), 0u);
    EXPECT_EQ(f.selectionEnd(), f.text().size());
    EXPECT_EQ(f.selectedText(), f.text());
}

TEST(DevToolsTextField, CopyLeavesTheText) {
    TextField f = field("hello world");
    f.moveCaretTo(6);
    f.moveCaretTo(11, true);
    EXPECT_EQ(key(f, KeyCode::C, true), R::Copy);
    EXPECT_EQ(f.selectedText(), "world");
    EXPECT_EQ(f.text(), "hello world");
    EXPECT_TRUE(f.hasSelection());
    EXPECT_EQ(key(f, KeyCode::Insert, true), R::Copy);
    // No selection: still Copy, with nothing to put on the clipboard
    f.moveCaretTo(0);
    EXPECT_EQ(key(f, KeyCode::C, true), R::Copy);
    EXPECT_EQ(f.selectedText(), "");
}

TEST(DevToolsTextField, CutRemovesTheSelectionAndKeepsIt) {
    TextField f = field("h\xC3\xA9llo world");
    f.moveCaretTo(0);
    f.moveCaretTo(6, true);
    EXPECT_EQ(key(f, KeyCode::X, true), R::Cut);
    EXPECT_EQ(f.lastCut(), "h\xC3\xA9llo");
    EXPECT_EQ(f.text(), " world");
    EXPECT_EQ(f.caret(), 0u);
    EXPECT_FALSE(f.hasSelection());
    // Shift+Delete cuts too
    f.moveCaretTo(6, true);
    EXPECT_EQ(key(f, KeyCode::Delete, false, true), R::Cut);
    EXPECT_EQ(f.lastCut(), " world");
    EXPECT_EQ(f.text(), "");
    // Nothing selected: Cut with nothing cut
    TextField g = field("abc");
    EXPECT_EQ(key(g, KeyCode::X, true), R::Cut);
    EXPECT_EQ(g.lastCut(), "");
    EXPECT_EQ(g.text(), "abc");
}

TEST(DevToolsTextField, PasteIsTheCallers) {
    TextField f = field("abc");
    EXPECT_EQ(key(f, KeyCode::V, true), R::Paste);
    EXPECT_EQ(key(f, KeyCode::Insert, false, true), R::Paste);
    EXPECT_EQ(f.text(), "abc");
}

// ---- moveCaretTo ----

TEST(DevToolsTextField, MoveCaretToClampsAndSnaps) {
    TextField f = field("a\xF0\x9F\x98\x80" "b");
    f.moveCaretTo(100);
    EXPECT_EQ(f.caret(), 6u);
    f.moveCaretTo(3);           // inside the emoji: back to its start
    EXPECT_EQ(f.caret(), 1u);
    f.moveCaretTo(4, true);
    EXPECT_EQ(f.caret(), 1u);
    EXPECT_FALSE(f.hasSelection());
    f.moveCaretTo(6, true);
    EXPECT_EQ(f.selectedText(), "\xF0\x9F\x98\x80" "b");
}

// ---- invalid UTF-8 ----

TEST(DevToolsTextField, InvalidBytesBecomeReplacementCharacters) {
    TextField f(std::string("a\xFF" "b\xC3", 4));
    EXPECT_EQ(f.text(), "a\xEF\xBF\xBD" "b\xEF\xBF\xBD");
    // Overlong, surrogate and truncated sequences
    f.setText(std::string("\xC0\xAF|\xED\xA0\x80|\xE4\xBD", 9));
    EXPECT_TRUE(validUtf8(f.text()));
    EXPECT_EQ(f.text().find('|'), 6u);   // two replacements for the overlong pair
    f.insert(std::string("\x80\x80x", 3));
    EXPECT_TRUE(validUtf8(f.text()));
}

TEST(DevToolsTextField, InvalidTextSurvivesEveryKey) {
    const std::string bad("\xE4\xBD\xFF\x80\n\xF0\x9F\x98 z\xC3", 11);
    const KeyCode keys[] = {KeyCode::Left, KeyCode::Right, KeyCode::Home, KeyCode::End,
                            KeyCode::Up, KeyCode::Down, KeyCode::Backspace, KeyCode::Delete};
    for (bool multiline : {false, true}) {
        TextField f(bad, multiline);
        for (int round = 0; round < 6; ++round) {
            for (KeyCode k : keys) {
                for (int mods = 0; mods < 4; ++mods) {
                    f.onKey(k, "", (mods & 1) != 0, (mods & 2) != 0);
                    ASSERT_TRUE(validUtf8(f.text()));
                    ASSERT_LE(f.caret(), f.text().size());
                    ASSERT_TRUE(validUtf8(f.text().substr(0, f.caret())));
                }
            }
            f.insert(bad);
            f.selectWordAt(round * 3);
        }
    }
}
