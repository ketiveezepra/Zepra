// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file page_keys.cpp
 * @brief NXRender key codes to DOM KeyboardEvent members (UI Events
 *        KeyboardEvent key/code values, legacy keyCode on a US layout).
 */

#include "page_keys.hpp"

#include <cctype>

namespace ZepraBrowser {

using NXRender::KeyCode;
using Zepra::WebCore::KeyEventInit;

namespace {

struct KeyInfo {
    const char* key;       // named key value, or the unshifted character
    const char* shifted;   // the character with Shift (null: same as key)
    const char* code;
    int keyCode;
    int location;
};

/// Keys other than letters, digits and function keys
bool lookup(KeyCode k, KeyInfo& out) {
    switch (k) {
    case KeyCode::Left:         out = {"ArrowLeft", nullptr, "ArrowLeft", 37, 0}; return true;
    case KeyCode::Right:        out = {"ArrowRight", nullptr, "ArrowRight", 39, 0}; return true;
    case KeyCode::Up:           out = {"ArrowUp", nullptr, "ArrowUp", 38, 0}; return true;
    case KeyCode::Down:         out = {"ArrowDown", nullptr, "ArrowDown", 40, 0}; return true;
    case KeyCode::Home:         out = {"Home", nullptr, "Home", 36, 0}; return true;
    case KeyCode::End:          out = {"End", nullptr, "End", 35, 0}; return true;
    case KeyCode::PageUp:       out = {"PageUp", nullptr, "PageUp", 33, 0}; return true;
    case KeyCode::PageDown:     out = {"PageDown", nullptr, "PageDown", 34, 0}; return true;
    case KeyCode::Backspace:    out = {"Backspace", nullptr, "Backspace", 8, 0}; return true;
    case KeyCode::Delete:       out = {"Delete", nullptr, "Delete", 46, 0}; return true;
    case KeyCode::Insert:       out = {"Insert", nullptr, "Insert", 45, 0}; return true;
    case KeyCode::Enter:        out = {"Enter", nullptr, "Enter", 13, 0}; return true;
    case KeyCode::Tab:          out = {"Tab", nullptr, "Tab", 9, 0}; return true;
    case KeyCode::Escape:       out = {"Escape", nullptr, "Escape", 27, 0}; return true;
    case KeyCode::Space:        out = {" ", nullptr, "Space", 32, 0}; return true;
    case KeyCode::Shift:        out = {"Shift", nullptr, "ShiftLeft", 16, 1}; return true;
    case KeyCode::Ctrl:         out = {"Control", nullptr, "ControlLeft", 17, 1}; return true;
    case KeyCode::Alt:          out = {"Alt", nullptr, "AltLeft", 18, 1}; return true;
    case KeyCode::Meta:         out = {"Meta", nullptr, "MetaLeft", 91, 1}; return true;
    case KeyCode::CapsLock:     out = {"CapsLock", nullptr, "CapsLock", 20, 0}; return true;
    case KeyCode::NumLock:      out = {"NumLock", nullptr, "NumLock", 144, 0}; return true;
    case KeyCode::ScrollLock:   out = {"ScrollLock", nullptr, "ScrollLock", 145, 0}; return true;
    case KeyCode::Comma:        out = {",", "<", "Comma", 188, 0}; return true;
    case KeyCode::Period:       out = {".", ">", "Period", 190, 0}; return true;
    case KeyCode::Slash:        out = {"/", "?", "Slash", 191, 0}; return true;
    case KeyCode::Backslash:    out = {"\\", "|", "Backslash", 220, 0}; return true;
    case KeyCode::Semicolon:    out = {";", ":", "Semicolon", 186, 0}; return true;
    case KeyCode::Quote:        out = {"'", "\"", "Quote", 222, 0}; return true;
    case KeyCode::LeftBracket:  out = {"[", "{", "BracketLeft", 219, 0}; return true;
    case KeyCode::RightBracket: out = {"]", "}", "BracketRight", 221, 0}; return true;
    case KeyCode::Minus:        out = {"-", "_", "Minus", 189, 0}; return true;
    case KeyCode::Equals:       out = {"=", "+", "Equal", 187, 0}; return true;
    case KeyCode::Backtick:     out = {"`", "~", "Backquote", 192, 0}; return true;
    case KeyCode::Plus:         out = {"+", nullptr, "NumpadAdd", 107, 3}; return true;
    default: return false;
    }
}

}  // namespace

KeyEventInit domKeyFor(KeyCode key, const std::string& text, const NXRender::Modifiers& modifiers, bool repeat) {
    KeyEventInit init;
    init.repeat = repeat;
    init.altKey = modifiers.alt;
    init.ctrlKey = modifiers.ctrl;
    init.shiftKey = modifiers.shift;
    init.metaKey = modifiers.meta;

    const int k = static_cast<int>(key);
    if (key >= KeyCode::A && key <= KeyCode::Z) {
        char letter = static_cast<char>('a' + (k - static_cast<int>(KeyCode::A)));
        char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
        init.code = std::string("Key") + upper;
        init.keyCode = upper;
        init.key = std::string(1, modifiers.shift ? upper : letter);
    } else if (key >= KeyCode::Num0 && key <= KeyCode::Num9) {
        static const char* const kShiftedDigits[] = {")", "!", "@", "#", "$", "%", "^", "&", "*", "("};
        int digit = k - static_cast<int>(KeyCode::Num0);
        init.code = "Digit" + std::to_string(digit);
        init.keyCode = '0' + digit;
        init.key = modifiers.shift ? kShiftedDigits[digit] : std::string(1, static_cast<char>('0' + digit));
    } else if (key >= KeyCode::F1 && key <= KeyCode::F12) {
        int n = k - static_cast<int>(KeyCode::F1) + 1;
        init.key = init.code = "F" + std::to_string(n);
        init.keyCode = 111 + n;
    } else if (KeyInfo info{}; lookup(key, info)) {
        init.key = (modifiers.shift && info.shifted) ? info.shifted : info.key;
        init.code = info.code;
        init.keyCode = info.keyCode;
        init.location = info.location;
    } else {
        init.key = "Unidentified";
    }
    // The character the platform typed is the key's value (its case, its
    // layout); keys that type nothing keep their names
    if (!text.empty() && (static_cast<unsigned char>(text[0]) >= 0x20)) init.key = text;
    return init;
}

} // namespace ZepraBrowser
