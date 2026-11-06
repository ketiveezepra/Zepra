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
    bool notCanceled = true;
    runTask([&] {
        // Focus moves on the press, before the click
        DOMElement* focusable = focusableSelfOrAncestor(target);
        domBindings_->setFocus(focusable);
        notCanceled = domBindings_->click(target, clientX, clientY);
    }, "event listener");
    return notCanceled;
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
