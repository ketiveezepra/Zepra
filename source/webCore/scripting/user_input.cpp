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

/// Types `text` into the focused text control: beforeinput, the new value,
/// input. False when nothing was typed (no editable control, canceled, full).
bool typeInto(DOMBindings& bindings, const std::string& text, const std::string& inputType) {
    DOMElement* focused = bindings.focusedElement();
    if (!editable(focused)) return false;
    std::string data = typeable(text, lower(focused->tagName()) == "textarea");
    if (data.empty()) return false;
    // maxlength counts characters
    std::string value = formControlValue(focused);
    if (focused->hasAttribute("maxlength")) {
        long max = std::strtol(focused->getAttribute("maxlength").c_str(), nullptr, 10);
        if (max >= 0) {
            size_t used = characterCount(value);
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
    // A beforeinput listener may have moved focus
    if (bindings.focusedElement() != focused) return false;
    setFormControlValue(focused, formControlValue(focused) + data);
    bindings.fireInputEvent(focused, "input", inputType, eventData);
    return true;
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
    return notCanceled;
}

MouseUpResult ScriptContext::mouseUp(DOMElement* target, double clientX, double clientY) {
    MouseUpResult result;
    if (!vm_ || !domBindings_) return result;
    DOMElement* pressed = domBindings_->pressedElement();
    domBindings_->setPressedElement(nullptr);
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
            if (!editable(focused) || key.ctrlKey || key.metaKey) return;
            bool textarea = lower(focused->tagName()) == "textarea";
            if (character) {
                typeInto(*domBindings_, text, "insertText");
            } else if (key.key == "Backspace") {
                std::string value = formControlValue(focused);
                if (value.empty() || !domBindings_->fireInputEvent(focused, "beforeinput", "deleteContentBackward", "")) return;
                eraseLastCharacter(value);
                setFormControlValue(focused, value);
                domBindings_->fireInputEvent(focused, "input", "deleteContentBackward", "");
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

bool ScriptContext::insertText(const std::string& text, const std::string& inputType) {
    if (!vm_ || !domBindings_) return false;
    bool inserted = false;
    runTask([&] { inserted = typeInto(*domBindings_, text, inputType); }, "event listener");
    return inserted;
}

} // namespace Zepra::WebCore
