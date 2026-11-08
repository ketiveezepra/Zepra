// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file user_input.cpp
 * @brief Keys, typing and clicks reaching the page: the event sequence a
 *        browser fires for each, and the default actions that follow when no
 *        listener canceled them (ScriptContext's user input methods).
 */

#include "scripting/script_context.hpp"
#include "scripting/dom_bindings.hpp"
#include "dom/form_controls.hpp"

#include "runtime/execution/vm.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <vector>

namespace Zepra::WebCore {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

/// Characters a key or paste may put into a control: no control characters,
/// and no line breaks outside a textarea
std::string typeable(const std::string& text, bool multiline) {
    std::string out;
    for (char c : text) {
        unsigned char u = static_cast<unsigned char>(c);
        if (c == '\n' && multiline) out += c;
        else if (u >= 0x20 && u != 0x7F) out += c;
    }
    return out;
}

size_t characterCount(const std::string& text) {
    return static_cast<size_t>(std::count_if(text.begin(), text.end(), [](char c) {
        return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    }));
}

bool editable(const DOMElement* e) {
    return e && isTextControl(e) && !e->hasAttribute("readonly") && !isDisabledControl(e);
}

/// Keys a focused text control keeps for itself instead of the page
/// (scrolling) or the browser (history)
bool controlKeepsKey(const KeyEventInit& key) {
    if (key.ctrlKey || key.metaKey) return false;
    static const char* const kPassThrough[] = {"Tab", "Escape", "F1", "F2", "F3", "F4", "F5", "F6",
                                               "F7", "F8", "F9", "F10", "F11", "F12"};
    return std::none_of(std::begin(kPassThrough), std::end(kPassThrough), [&](const char* k) { return key.key == k; });
}

bool isButtonLike(const DOMElement* e) {
    if (!e) return false;
    std::string tag = lower(e->tagName());
    return tag == "button" || isCheckable(e) || (tag == "input" && (isSubmitButton(e) || isResetButton(e) ||
                                                                    inputType(e) == "button"));
}

/// Types `text` into the focused text control in place of its selection:
/// beforeinput, the new value, input. False when nothing was typed (no
/// editable control, canceled, full).
bool typeInto(DOMBindings& bindings, const std::string& text, const std::string& inputType) {
    DOMElement* focused = bindings.focusedElement();
    if (!editable(focused)) return false;
    std::string data = typeable(text, lower(focused->tagName()) == "textarea");
    if (data.empty()) return false;
    // maxlength counts characters; the selection makes room as it goes
    std::string value = formControlValue(focused);
    TextSelection selection = formControlSelection(focused);
    if (focused->hasAttribute("maxlength")) {
        long max = std::strtol(focused->getAttribute("maxlength").c_str(), nullptr, 10);
        if (max >= 0) {
            size_t used = characterCount(value) -
                          characterCount(value.substr(selection.start, selection.end - selection.start));
            size_t room = static_cast<size_t>(max) > used ? static_cast<size_t>(max) - used : 0;
            std::string fitted;
            for (size_t i = 0, n = 0; i < data.size(); i++) {
                bool lead = (static_cast<unsigned char>(data[i]) & 0xC0) != 0x80;
                if (lead && n++ == room) break;
                fitted += data[i];
            }
            data = fitted;
            if (data.empty()) return false;
        }
    }
    std::string eventData = inputType == "insertLineBreak" ? "" : data;
    if (!bindings.fireInputEvent(focused, "beforeinput", inputType, eventData)) return false;
    // A beforeinput listener may have moved focus, or the selection
    if (bindings.focusedElement() != focused) return false;
    selection = formControlSelection(focused);
    replaceFormControlText(focused, selection.start, selection.end, data);
    bindings.fireInputEvent(focused, "input", inputType, eventData);
    return true;
}

bool wordCharacter(char c) {
    unsigned char u = static_cast<unsigned char>(c);
    return u >= 0x80 || std::isalnum(u) || c == '_';
}

/// Where Ctrl+Left lands: the start of the word before `offset`
size_t previousWordStart(const std::string& text, size_t offset) {
    while (offset > 0 && !wordCharacter(text[offset - 1])) offset--;
    while (offset > 0 && wordCharacter(text[offset - 1])) offset--;
    return characterBoundary(text, offset);
}

/// Where Ctrl+Right lands: the end of the word after `offset`
size_t nextWordEnd(const std::string& text, size_t offset) {
    while (offset < text.size() && !wordCharacter(text[offset])) offset++;
    while (offset < text.size() && wordCharacter(text[offset])) offset++;
    return offset;
}

size_t lineStart(const std::string& text, size_t offset) {
    size_t newline = offset == 0 ? std::string::npos : text.rfind('\n', offset - 1);
    return newline == std::string::npos ? 0 : newline + 1;
}

size_t lineEnd(const std::string& text, size_t offset) {
    size_t newline = text.find('\n', offset);
    return newline == std::string::npos ? text.size() : newline;
}

/// The offset `characters` characters into the line starting at `start`,
/// or the line's end when it is shorter
size_t intoLine(const std::string& text, size_t start, size_t characters) {
    size_t end = lineEnd(text, start), offset = start;
    while (characters-- > 0 && offset < end) offset = nextCharacter(text, offset);
    return offset;
}

/// Where a caret-moving key takes the caret in a text control's value, or
/// npos for a key that does not move it
size_t movedCaret(const std::string& value, const TextSelection& selection, const KeyEventInit& key,
                  bool multiline) {
    size_t caret = selection.caret();
    bool collapse = !key.shiftKey && !selection.collapsed();
    if (key.key == "ArrowLeft") {
        if (collapse) return selection.start;
        return key.ctrlKey ? previousWordStart(value, caret) : previousCharacter(value, caret);
    }
    if (key.key == "ArrowRight") {
        if (collapse) return selection.end;
        return key.ctrlKey ? nextWordEnd(value, caret) : nextCharacter(value, caret);
    }
    if (key.key == "Home") return multiline && !key.ctrlKey ? lineStart(value, caret) : 0;
    if (key.key == "End") return multiline && !key.ctrlKey ? lineEnd(value, caret) : value.size();
    if (key.key == "ArrowUp" || key.key == "ArrowDown") {
        bool up = key.key == "ArrowUp";
        // One line: to the start or the end, as Chrome and Firefox do
        if (!multiline) return up ? 0 : value.size();
        size_t start = lineStart(value, caret);
        size_t column = characterCount(value.substr(start, caret - start));
        if (up) return start == 0 ? 0 : intoLine(value, lineStart(value, start - 1), column);
        size_t end = lineEnd(value, caret);
        return end == value.size() ? value.size() : intoLine(value, end + 1, column);
    }
    return std::string::npos;
}

/// Backspace / Delete: removes the selection, else the character (Ctrl:
/// the word) before or after the caret, between beforeinput and input
void deleteText(DOMBindings& bindings, DOMElement* field, bool forward, bool word) {
    std::string value = formControlValue(field);
    TextSelection selection = formControlSelection(field);
    size_t from = selection.start, to = selection.end;
    if (selection.collapsed()) {
        if (forward) to = word ? nextWordEnd(value, to) : nextCharacter(value, to);
        else from = word ? previousWordStart(value, from) : previousCharacter(value, from);
    }
    if (from == to) return;
    std::string inputType = std::string(word && selection.collapsed() ? "deleteWord" : "deleteContent") +
                            (forward ? "Forward" : "Backward");
    if (!bindings.fireInputEvent(field, "beforeinput", inputType, "")) return;
    if (bindings.focusedElement() != field) return;
    replaceFormControlText(field, from, to, "");
    bindings.fireInputEvent(field, "input", inputType, "");
}

/// The option a key moves a closed drop-down to: the next or previous
/// enabled one (arrows), the first or last (Home, End, Page keys), or the
/// next whose label starts with the typed character. -1 when the key is the
/// select's but there is nowhere to go; -2 when the key is not the select's.
long optionForKey(const DOMElement* select, const KeyEventInit& key, const std::string& text) {
    std::vector<DOMElement*> options = selectOptions(select);
    long count = static_cast<long>(options.size());
    long current = selectedIndex(select);
    auto enabled = [&](long i) { return i >= 0 && i < count && !isOptionDisabled(options[i]); };
    auto step = [&](long from, long direction) {
        for (long i = from + direction; i >= 0 && i < count; i += direction) {
            if (enabled(i)) return i;
        }
        return -1L;
    };
    if (key.key == "ArrowDown" || key.key == "ArrowRight") return step(current, 1);
    if (key.key == "ArrowUp" || key.key == "ArrowLeft") return current < 0 ? step(count, -1) : step(current, -1);
    if (key.key == "Home" || key.key == "PageUp") return step(-1, 1);
    if (key.key == "End" || key.key == "PageDown") return step(count, -1);
    if (text.empty() || key.altKey || static_cast<unsigned char>(text[0]) < 0x20) return -2;
    // Type-ahead: the next option (after the current one, wrapping) whose
    // label starts with the character
    std::string typed = lower(text);
    for (long k = 1; k <= count; k++) {
        long i = ((current < 0 ? -1 : current) + k) % count;
        if (!enabled(i)) continue;
        std::string label = lower(optionLabel(options[i]));
        if (label.compare(0, typed.size(), typed) == 0) return i;
    }
    return -1;
}

}  // namespace

DOMElement* ScriptContext::focusedElement() const {
    return domBindings_ ? domBindings_->focusedElement() : nullptr;
}

void ScriptContext::focusElement(DOMElement* element) {
    if (!vm_ || !domBindings_) return;
    runTask([&] { domBindings_->setFocus(element); }, "focus listener");
}

void ScriptContext::setSubmitHandler(SubmitHandler handler) { submitHandler_ = std::move(handler); }

bool ScriptContext::pressElement(DOMElement* target, double clientX, double clientY) {
    if (!vm_ || !domBindings_ || !target) return true;
    mouseDown(target, clientX, clientY);
    return !mouseUp(target, clientX, clientY).canceled;
}

bool ScriptContext::mouseDown(DOMElement* target, double clientX, double clientY) {
    if (!vm_ || !domBindings_) return true;
    domBindings_->setPressedElement(nullptr);
    endDrag();
    if (!target) return true;
    bool notCanceled = true;
    runTask([&] {
        MouseEventInit init;
        init.buttons = 1;
        init.detail = 1;
        notCanceled = domBindings_->fireMouseEvent(target, "mousedown", clientX, clientY, init);
        // Focus moves on the press, before the click; a canceled mousedown
        // leaves it where it was
        if (notCanceled) domBindings_->setFocus(focusableSelfOrAncestor(target));
    }, "event listener");
    // The mousedown gave the element a wrapper, which keeps it alive if a
    // listener removes it before the release
    if (domBindings_->isConnected(target)) domBindings_->setPressedElement(target);
    // A press on a draggable element that nobody canceled may become a drag
    if (notCanceled) noteDragPress(target, clientX, clientY);
    return notCanceled;
}

MouseUpResult ScriptContext::mouseUp(DOMElement* target, double clientX, double clientY) {
    MouseUpResult result;
    if (!vm_ || !domBindings_) return result;
    DOMElement* pressed = domBindings_->pressedElement();
    domBindings_->setPressedElement(nullptr);
    // A drag ends with drop/dragend instead of mouseup and click
    if (drag_ && dragRelease(target, clientX, clientY)) return result;
    if (!target) return result;
    runTask([&] {
        MouseEventInit init;
        init.detail = 1;
        domBindings_->fireMouseEvent(target, "mouseup", clientX, clientY, init);
        // The click goes to the nearest element holding both where the
        // button went down and where it came up
        if (!pressed || !domBindings_->isConnected(pressed) || !domBindings_->isConnected(target)) return;
        DOMElement* clickTarget = nullptr;
        for (DOMNode* n = pressed; n && !clickTarget; n = n->parentNode()) {
            if (n->contains(target) && n->nodeType() == NodeType::Element) clickTarget = static_cast<DOMElement*>(n);
        }
        if (!clickTarget) return;
        result.clicked = true;
        result.canceled = !domBindings_->click(clickTarget, clientX, clientY);
    }, "event listener");
    return result;
}

void ScriptContext::mouseMove(DOMElement* target, double clientX, double clientY, bool buttonDown) {
    if (!vm_ || !domBindings_) return;
    if (drag_ && dragMove(target, clientX, clientY, buttonDown)) return;
    DOMElement* old = domBindings_->pointerElement();
    if (old && !domBindings_->isConnected(old)) old = nullptr;
    if (target && !domBindings_->isConnected(target)) target = nullptr;
    if (old == target && !target) return;
    runTask([&] {
        MouseEventInit init;
        init.buttons = buttonDown ? 1 : 0;
        if (old != target) {
            auto contains = [](DOMNode* a, DOMNode* b) { return a && b && a->contains(b); };
            if (old) {
                MouseEventInit out = init;
                out.relatedTarget = target;
                domBindings_->fireMouseEvent(old, "mouseout", clientX, clientY, out);
                // mouseleave at the old element and each ancestor the pointer left, innermost first
                out.bubbles = out.cancelable = false;
                for (DOMNode* n = old; n && n->nodeType() == NodeType::Element && !contains(n, target); n = n->parentNode()) {
                    domBindings_->fireMouseEvent(static_cast<DOMElement*>(n), "mouseleave", clientX, clientY, out);
                }
            }
            if (target) {
                MouseEventInit over = init;
                over.relatedTarget = old;
                domBindings_->fireMouseEvent(target, "mouseover", clientX, clientY, over);
                // mouseenter at each element the pointer entered, outermost first
                std::vector<DOMElement*> entered;
                for (DOMNode* n = target; n && n->nodeType() == NodeType::Element && !contains(n, old); n = n->parentNode()) {
                    entered.push_back(static_cast<DOMElement*>(n));
                }
                over.bubbles = over.cancelable = false;
                for (auto it = entered.rbegin(); it != entered.rend(); ++it) {
                    domBindings_->fireMouseEvent(*it, "mouseenter", clientX, clientY, over);
                }
            }
        }
        if (target) domBindings_->fireMouseEvent(target, "mousemove", clientX, clientY, init);
    }, "event listener");
    domBindings_->setPointerElement(target && domBindings_->isConnected(target) ? target : nullptr);
}

KeyResult ScriptContext::keyDown(const KeyEventInit& key, const std::string& text) {
    KeyResult result;
    if (!vm_ || !domBindings_ || !document_) return result;
    runTask([&] {
        DOMElement* focused = domBindings_->focusedElement();
        DOMElement* target = focused ? focused : document_->body();
        if (!target) target = document_->documentElement();
        if (!domBindings_->fireKeyboardEvent(target, "keydown", key)) {
            result.canceled = true;
            return;
        }
        // keypress: keys that make a character, and Enter
        bool character = !text.empty() && !key.ctrlKey && !key.metaKey;
        if ((character || key.key == "Enter") && !domBindings_->fireKeyboardEvent(target, "keypress", key)) {
            result.canceled = true;
            return;
        }
        // A listener may have moved focus
        focused = domBindings_->focusedElement();
        if (!focused) return;

        if (isTextControl(focused)) {
            result.consumed = controlKeepsKey(key);
            bool textarea = lower(focused->tagName()) == "textarea";
            // The caret moves in read-only fields too
            if (!key.altKey && !key.metaKey) {
                std::string value = formControlValue(focused);
                TextSelection selection = formControlSelection(focused);
                size_t caret = movedCaret(value, selection, key, textarea);
                if (caret != std::string::npos) {
                    result.consumed = true;
                    setFormControlCaret(focused, key.shiftKey ? selection.anchor() : caret, caret);
                    return;
                }
                if (key.ctrlKey && (key.key == "a" || key.key == "A")) {
                    result.consumed = true;
                    setFormControlSelection(focused, 0, value.size());
                    return;
                }
            }
            if (!editable(focused) || key.metaKey) return;
            if (key.key == "Backspace" || key.key == "Delete") {
                result.consumed = true;
                deleteText(*domBindings_, focused, key.key == "Delete", key.ctrlKey);
                return;
            }
            if (key.ctrlKey) return;
            if (character) {
                typeInto(*domBindings_, text, "insertText");
            } else if (key.key == "Enter" && textarea) {
                typeInto(*domBindings_, "\n", "insertLineBreak");
            } else if (key.key == "Enter") {
                // Implicit submission: the form's default button is clicked;
                // without one, a form with a single text field submits itself
                domBindings_->commitFocusedValue();
                DOMElement* form = formOwner(focused);
                if (!form) return;
                if (DOMElement* button = defaultButton(form)) {
                    if (!isDisabledControl(button)) domBindings_->click(button, 0, 0);
                    return;
                }
                std::vector<DOMElement*> controls = formControls(form);
                long fields = std::count_if(controls.begin(), controls.end(), [](DOMElement* e) { return isTextInput(e); });
                if (fields <= 1) domBindings_->requestSubmit(form, nullptr);
            }
            return;
        }
        if (isSingleSelect(focused) && !isDisabledControl(focused) && !key.ctrlKey && !key.metaKey) {
            if (key.key == " " || key.key == "F4" || (key.altKey && (key.key == "ArrowDown" || key.key == "ArrowUp"))) {
                result.consumed = result.openPicker = true;
                return;
            }
            long next = optionForKey(focused, key, text);
            if (next != -2) {
                result.consumed = true;
                if (next >= 0) domBindings_->pickOption(focused, next);
            }
            return;
        }
        // Enter clicks a focused button; Space clicks buttons, checkboxes and radios
        if (isButtonLike(focused) && !key.ctrlKey && !key.metaKey && !key.altKey &&
            ((key.key == "Enter" && !isCheckable(focused)) || key.key == " ")) {
            result.consumed = true;
            domBindings_->click(focused, 0, 0);
        }
    }, "event listener");
    return result;
}

void ScriptContext::keyUp(const KeyEventInit& key) {
    if (!vm_ || !domBindings_ || !document_) return;
    runTask([&] {
        DOMElement* focused = domBindings_->focusedElement();
        DOMElement* target = focused ? focused : document_->body();
        if (!target) target = document_->documentElement();
        domBindings_->fireKeyboardEvent(target, "keyup", key);
    }, "event listener");
}

bool ScriptContext::chooseOption(DOMElement* select, long index) {
    if (!vm_ || !domBindings_) return false;
    bool changed = false;
    runTask([&] { changed = domBindings_->pickOption(select, index); }, "event listener");
    return changed;
}

bool ScriptContext::insertText(const std::string& text, const std::string& inputType) {
    if (!vm_ || !domBindings_) return false;
    bool inserted = false;
    runTask([&] { inserted = typeInto(*domBindings_, text, inputType); }, "event listener");
    return inserted;
}

} // namespace Zepra::WebCore
