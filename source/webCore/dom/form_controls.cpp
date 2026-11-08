// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file form_controls.cpp
 * @brief Form control state, focusability, form owners and form data.
 */

#include "dom/form_controls.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>

namespace Zepra::WebCore {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string tagOf(const DOMElement* e) { return e ? lower(e->tagName()) : std::string(); }

DOMElement* asElement(DOMNode* node) {
    return node && node->nodeType() == NodeType::Element ? static_cast<DOMElement*>(node) : nullptr;
}

/// The top of the tree `node` is in (the document when connected)
DOMNode* rootOf(const DOMNode* node) {
    DOMNode* n = const_cast<DOMNode*>(node);
    while (n && n->parentNode()) n = n->parentNode();
    return n;
}

void forEachElement(DOMNode* root, const std::function<void(DOMElement*)>& visit) {
    if (!root) return;
    for (const auto& child : root->childNodes()) {
        if (DOMElement* e = asElement(child.get())) visit(e);
        forEachElement(child.get(), visit);
    }
}

bool isListedElement(const DOMElement* e) {
    static const char* const kListed[] = {"button", "fieldset", "input", "object", "output", "select", "textarea"};
    std::string tag = tagOf(e);
    return std::find_if(std::begin(kListed), std::end(kListed), [&](const char* t) { return tag == t; }) != std::end(kListed);
}

/// The option a single-choice <select> shows (null when none)
DOMElement* selectedOption(const DOMElement* select) {
    long index = selectedIndex(select);
    if (index < 0) return nullptr;
    return selectOptions(select)[static_cast<size_t>(index)];
}

std::string optionValue(const DOMElement* option) {
    return option->hasAttribute("value") ? option->getAttribute("value") : optionText(option);
}

}  // namespace

std::vector<DOMElement*> selectOptions(const DOMElement* select) {
    std::vector<DOMElement*> options;
    if (!select) return options;
    // Children, and children of child <optgroup>s (HTML: the list of options)
    for (const auto& child : select->childNodes()) {
        DOMElement* e = asElement(child.get());
        if (!e) continue;
        if (tagOf(e) == "option") {
            options.push_back(e);
        } else if (tagOf(e) == "optgroup") {
            for (const auto& grandchild : e->childNodes()) {
                DOMElement* option = asElement(grandchild.get());
                if (option && tagOf(option) == "option") options.push_back(option);
            }
        }
    }
    return options;
}

DOMElement* optionOwner(const DOMElement* option) {
    DOMNode* parent = option ? option->parentNode() : nullptr;
    DOMElement* e = asElement(parent);
    if (e && tagOf(e) == "optgroup") e = asElement(e->parentNode());
    return e && tagOf(e) == "select" ? e : nullptr;
}

bool isSingleSelect(const DOMElement* select) {
    return select && tagOf(select) == "select" && !select->hasAttribute("multiple");
}

bool optionSelected(const DOMElement* option) {
    if (!option) return false;
    return option->checkedDirty() ? option->checked() : option->hasAttribute("selected");
}

bool isOptionDisabled(const DOMElement* option) {
    if (!option) return true;
    if (option->hasAttribute("disabled")) return true;
    DOMElement* parent = asElement(option->parentNode());
    return parent && tagOf(parent) == "optgroup" && parent->hasAttribute("disabled");
}

std::string optionText(const DOMElement* option) {
    // White space stripped and collapsed (HTML: the option's text)
    std::string text, raw = option ? option->textContent() : "";
    for (char c : raw) {
        bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
        if (space) {
            if (!text.empty() && text.back() != ' ') text += ' ';
        } else {
            text += c;
        }
    }
    if (!text.empty() && text.back() == ' ') text.pop_back();
    return text;
}

std::string optionLabel(const DOMElement* option) {
    if (option && option->hasAttribute("label") && !option->getAttribute("label").empty())
        return option->getAttribute("label");
    return optionText(option);
}

long selectedIndex(const DOMElement* select) {
    std::vector<DOMElement*> options = selectOptions(select);
    bool single = isSingleSelect(select);
    long found = -1;
    bool touched = false;
    for (size_t i = 0; i < options.size(); i++) {
        touched = touched || options[i]->checkedDirty();
        if (!optionSelected(options[i])) continue;
        // Several selected attributes in a single select: the last one wins
        found = static_cast<long>(i);
        if (!single) return found;
    }
    // Nothing selected yet: a drop-down shows its first enabled option
    if (found < 0 && single && !touched) {
        for (size_t i = 0; i < options.size(); i++) {
            if (!isOptionDisabled(options[i])) return static_cast<long>(i);
        }
    }
    return found;
}

void setOptionSelected(DOMElement* option, bool selected) {
    if (!option) return;
    DOMElement* select = optionOwner(option);
    if (selected && isSingleSelect(select)) {
        for (DOMElement* other : selectOptions(select)) other->setDirtyChecked(other == option);
        return;
    }
    // Remember every option's state, so none falls back to its attribute
    if (select) {
        for (DOMElement* other : selectOptions(select)) other->setDirtyChecked(optionSelected(other));
    }
    option->setDirtyChecked(selected);
    // A drop-down without a selected option shows its first enabled one
    if (!selected && isSingleSelect(select) && selectedIndex(select) < 0) {
        for (DOMElement* other : selectOptions(select)) {
            if (!isOptionDisabled(other)) {
                other->setDirtyChecked(true);
                break;
            }
        }
    }
}

void setSelectedIndex(DOMElement* select, long index) {
    std::vector<DOMElement*> options = selectOptions(select);
    for (size_t i = 0; i < options.size(); i++) options[i]->setDirtyChecked(static_cast<long>(i) == index);
}

std::string inputType(const DOMElement* element) {
    std::string type = element ? lower(element->getAttribute("type")) : std::string();
    return type.empty() ? "text" : type;
}

bool isTextControl(const DOMElement* element) {
    std::string tag = tagOf(element);
    if (tag == "textarea") return true;
    if (tag != "input") return false;
    static const char* const kNotText[] = {"checkbox", "radio", "submit", "button", "reset", "image",
                                           "file", "hidden", "range", "color"};
    std::string type = inputType(element);
    return std::none_of(std::begin(kNotText), std::end(kNotText), [&](const char* t) { return type == t; });
}

bool isTextInput(const DOMElement* element) { return tagOf(element) == "input" && isTextControl(element); }

bool isCheckable(const DOMElement* element) {
    if (tagOf(element) != "input") return false;
    std::string type = inputType(element);
    return type == "checkbox" || type == "radio";
}

bool isSubmitButton(const DOMElement* element) {
    std::string tag = tagOf(element);
    if (tag == "button") {
        std::string type = lower(element->getAttribute("type"));
        return type.empty() || type == "submit";
    }
    if (tag != "input") return false;
    std::string type = inputType(element);
    return type == "submit" || type == "image";
}

bool isResetButton(const DOMElement* element) {
    std::string tag = tagOf(element);
    return (tag == "button" || tag == "input") && lower(element->getAttribute("type")) == "reset";
}

bool isDisabledControl(const DOMElement* element) {
    if (!element) return false;
    if (isListedElement(element) && element->hasAttribute("disabled")) return true;
    for (DOMNode* p = element->parentNode(); p; p = p->parentNode()) {
        DOMElement* e = asElement(p);
        if (e && tagOf(e) == "fieldset" && e->hasAttribute("disabled")) return true;
    }
    return false;
}

bool isFocusable(const DOMElement* element) {
    if (!element) return false;
    std::string tag = tagOf(element);
    if (tag == "input" || tag == "textarea" || tag == "select" || tag == "button") {
        return !isDisabledControl(element) && !(tag == "input" && inputType(element) == "hidden");
    }
    if ((tag == "a" || tag == "area") && element->hasAttribute("href")) return true;
    if (element->hasAttribute("tabindex")) return true;
    if (element->hasAttribute("contenteditable")) return lower(element->getAttribute("contenteditable")) != "false";
    return false;
}

DOMElement* focusableSelfOrAncestor(DOMElement* element) {
    for (DOMNode* n = element; n; n = n->parentNode()) {
        DOMElement* e = asElement(n);
        if (e && isFocusable(e)) return e;
    }
    return nullptr;
}

std::string formControlValue(const DOMElement* element) {
    if (!element) return "";
    std::string tag = tagOf(element);
    if (element->valueDirty() && tag != "select") return element->dirtyValue();
    if (tag == "textarea") return element->textContent();
    if (tag == "select") {
        DOMElement* option = selectedOption(element);
        return option ? optionValue(option) : "";
    }
    if (tag == "option") return optionValue(element);
    if (isCheckable(element) && !element->hasAttribute("value")) return "on";
    return element->getAttribute("value");
}

void setFormControlValue(DOMElement* element, const std::string& value) {
    if (!element) return;
    bool changed = formControlValue(element) != value;
    element->setDirtyValue(value);
    // A new value moves the caret to its end (HTML: the value setter)
    if (changed) element->clearSelection();
}

bool supportsTextSelection(const DOMElement* element) {
    if (!element) return false;
    std::string tag = tagOf(element);
    if (tag == "textarea") return true;
    if (tag != "input") return false;
    std::string type = inputType(element);
    return type == "text" || type == "search" || type == "url" || type == "tel" || type == "password";
}

TextSelection formControlSelection(const DOMElement* element) {
    TextSelection selection;
    if (!element) return selection;
    std::string value = formControlValue(element);
    if (!element->selectionSet()) {
        selection.start = selection.end = value.size();
        return selection;
    }
    selection.start = characterBoundary(value, std::min(element->selectionStart(), value.size()));
    selection.end = characterBoundary(value, std::min(element->selectionEnd(), value.size()));
    selection.backward = element->selectionBackward() && selection.start != selection.end;
    return selection;
}

void setFormControlSelection(DOMElement* element, size_t start, size_t end, bool backward) {
    if (!element) return;
    if (end < start) std::swap(start, end);
    std::string value = formControlValue(element);
    start = characterBoundary(value, std::min(start, value.size()));
    end = characterBoundary(value, std::min(end, value.size()));
    element->setSelection(start, end, backward && start != end);
}

void setFormControlCaret(DOMElement* element, size_t anchor, size_t caret) {
    setFormControlSelection(element, std::min(anchor, caret), std::max(anchor, caret), caret < anchor);
}

void replaceFormControlText(DOMElement* element, size_t start, size_t end, const std::string& text) {
    if (!element) return;
    std::string value = formControlValue(element);
    start = std::min(start, value.size());
    end = std::min(std::max(start, end), value.size());
    value.replace(start, end - start, text);
    element->setDirtyValue(value);
    size_t caret = start + text.size();
    element->setSelection(caret, caret, false);
}

bool formControlChecked(const DOMElement* element) {
    if (!element) return false;
    return element->checkedDirty() ? element->checked() : element->hasAttribute("checked");
}

void setFormControlChecked(DOMElement* element, bool checked) {
    if (!element) return;
    if (checked && tagOf(element) == "input" && inputType(element) == "radio") {
        for (DOMElement* other : radioGroup(element)) {
            if (other != element && formControlChecked(other)) other->setDirtyChecked(false);
        }
    }
    element->setDirtyChecked(checked);
}

DOMElement* formOwner(const DOMElement* control) {
    if (!control) return nullptr;
    if (control->hasAttribute("form")) {
        // form="id": the form with that id in the same tree, or none
        const std::string id = control->getAttribute("form");
        DOMElement* found = nullptr;
        forEachElement(rootOf(control), [&](DOMElement* e) {
            if (!found && e->getAttribute("id") == id) found = e;
        });
        return found && tagOf(found) == "form" ? found : nullptr;
    }
    for (DOMNode* p = control->parentNode(); p; p = p->parentNode()) {
        DOMElement* e = asElement(p);
        if (e && tagOf(e) == "form") return e;
    }
    return nullptr;
}

std::vector<DOMElement*> formControls(const DOMElement* form) {
    std::vector<DOMElement*> controls;
    if (!form) return controls;
    forEachElement(rootOf(form), [&](DOMElement* e) {
        if (isListedElement(e) && formOwner(e) == form) controls.push_back(e);
    });
    return controls;
}

DOMElement* defaultButton(const DOMElement* form) {
    for (DOMElement* e : formControls(form)) {
        if (isSubmitButton(e)) return e;
    }
    return nullptr;
}

std::vector<DOMElement*> radioGroup(const DOMElement* radio) {
    std::vector<DOMElement*> group;
    if (!radio) return group;
    const std::string name = radio->getAttribute("name");
    if (name.empty()) {
        group.push_back(const_cast<DOMElement*>(radio));
        return group;
    }
    const DOMElement* owner = formOwner(radio);
    forEachElement(rootOf(radio), [&](DOMElement* e) {
        if (tagOf(e) == "input" && inputType(e) == "radio" && e->getAttribute("name") == name && formOwner(e) == owner) {
            group.push_back(e);
        }
    });
    return group;
}

void resetFormControls(const DOMElement* form) {
    for (DOMElement* e : formControls(form)) {
        e->resetControlState();
        // A select goes back to its options' selected attributes
        if (tagOf(e) == "select") {
            for (DOMElement* option : selectOptions(e)) option->resetControlState();
        }
    }
}

std::vector<std::pair<std::string, std::string>> formData(const DOMElement* form, const DOMElement* submitter,
                                                          std::vector<bool>* files) {
    std::vector<std::pair<std::string, std::string>> data;
    // Marks every pair added so far that has no mark yet
    auto mark = [&](bool file) {
        if (files) files->resize(data.size(), file);
    };
    for (DOMElement* e : formControls(form)) {
        std::string tag = tagOf(e);
        std::string name = e->getAttribute("name");
        if (isDisabledControl(e) || tag == "fieldset" || tag == "object" || tag == "output") continue;
        bool button = tag == "button" || (tag == "input" && (isSubmitButton(e) || isResetButton(e) || inputType(e) == "button"));
        if (button && e != submitter) continue;
        if (isCheckable(e) && !formControlChecked(e)) continue;
        if (tag == "input" && inputType(e) == "image") {
            // The click point; we have no point for a keyboard submission
            std::string prefix = name.empty() ? "" : name + ".";
            data.emplace_back(prefix + "x", "0");
            data.emplace_back(prefix + "y", "0");
            continue;
        }
        if (name.empty()) continue;
        if (tag == "input" && inputType(e) == "file") {
            mark(false);
            data.emplace_back(name, "");
            mark(true);
            continue;
        }
        if (tag == "select") {
            // Each selected option that is not disabled
            if (isSingleSelect(e)) {
                DOMElement* option = selectedOption(e);
                if (option && !isOptionDisabled(option)) data.emplace_back(name, optionValue(option));
            } else {
                for (DOMElement* option : selectOptions(e)) {
                    if (optionSelected(option) && !isOptionDisabled(option)) data.emplace_back(name, optionValue(option));
                }
            }
            continue;
        }
        std::string value = formControlValue(e);
        if (tag == "textarea") {
            // Line breaks go as CR LF
            std::string normalized;
            for (size_t i = 0; i < value.size(); i++) {
                char c = value[i];
                if (c == '\r') {
                    normalized += "\r\n";
                    if (i + 1 < value.size() && value[i + 1] == '\n') i++;
                } else if (c == '\n') {
                    normalized += "\r\n";
                } else {
                    normalized += c;
                }
            }
            value = normalized;
        }
        data.emplace_back(name, value);
    }
    mark(false);
    return data;
}

std::string urlEncodeFormData(const std::vector<std::pair<std::string, std::string>>& data) {
    static const char kHex[] = "0123456789ABCDEF";
    auto encode = [](const std::string& s) {
        std::string out;
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '*' || c == '-' || c == '.' || c == '_') {
                out += static_cast<char>(c);
            } else if (c == ' ') {
                out += '+';
            } else {
                out += '%';
                out += kHex[c >> 4];
                out += kHex[c & 15];
            }
        }
        return out;
    };
    std::string out;
    for (const auto& [name, value] : data) {
        if (!out.empty()) out += '&';
        out += encode(name) + "=" + encode(value);
    }
    return out;
}

std::string multipartFormData(const std::vector<std::pair<std::string, std::string>>& data,
                              const std::vector<bool>& files, const std::string& boundary) {
    auto escapeName = [](const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == '"') out += "%22";
            else if (c == '\r') out += "%0D";
            else if (c == '\n') out += "%0A";
            else out += c;
        }
        return out;
    };
    std::string out;
    for (size_t i = 0; i < data.size(); i++) {
        out += "--" + boundary + "\r\n";
        out += "Content-Disposition: form-data; name=\"" + escapeName(data[i].first) + "\"";
        if (i < files.size() && files[i]) {
            out += "; filename=\"\"\r\nContent-Type: application/octet-stream\r\n\r\n\r\n";
            continue;
        }
        out += "\r\n\r\n" + data[i].second + "\r\n";
    }
    out += "--" + boundary + "--\r\n";
    return out;
}

std::string plainTextFormData(const std::vector<std::pair<std::string, std::string>>& data) {
    std::string out;
    for (const auto& [name, value] : data) out += name + "=" + value + "\r\n";
    return out;
}

FormSubmission planFormSubmission(const DOMElement* form, const DOMElement* submitter) {
    FormSubmission plan;
    if (!form) return plan;
    auto pick = [&](const char* submitterAttr, const char* formAttr) {
        if (submitter && submitter->hasAttribute(submitterAttr)) return submitter->getAttribute(submitterAttr);
        return form->getAttribute(formAttr);
    };
    plan.action = pick("formaction", "action");
    std::string method = lower(pick("formmethod", "method"));
    plan.method = (method == "post" || method == "dialog") ? method : "get";
    plan.target = pick("formtarget", "target");
    std::string enctype = lower(pick("formenctype", "enctype"));
    if (plan.method != "post" || (enctype != "multipart/form-data" && enctype != "text/plain")) {
        enctype = "application/x-www-form-urlencoded";
    }
    plan.enctype = enctype;
    std::vector<bool> files;
    auto data = formData(form, submitter, &files);
    if (enctype == "multipart/form-data") {
        // A boundary that does not occur in the data
        std::string all;
        for (const auto& [name, value] : data) all += name + value;
        uint64_t n = 0x5a657072u ^ (all.size() * 0x9E3779B97F4A7C15ull);
        std::string boundary;
        do {
            static const char kHex[] = "0123456789abcdef";
            boundary = "----ZepraFormBoundary";
            for (int i = 0; i < 16; i++) boundary += kHex[(n >> (i * 4)) & 15];
            n = n * 6364136223846793005ull + 1442695040888963407ull;
        } while (all.find(boundary) != std::string::npos);
        plan.body = multipartFormData(data, files, boundary);
        plan.contentType = "multipart/form-data; boundary=" + boundary;
    } else if (enctype == "text/plain") {
        plan.body = plainTextFormData(data);
        plan.contentType = "text/plain";
    } else {
        plan.body = urlEncodeFormData(data);
        plan.contentType = enctype;
    }
    return plan;
}

std::string formSubmissionUrl(const std::string& actionUrl, const std::string& body) {
    std::string base = actionUrl.substr(0, actionUrl.find('#'));
    base = base.substr(0, base.find('?'));
    return base + "?" + body;
}

bool eraseLastCharacter(std::string& text) {
    if (text.empty()) return false;
    size_t i = text.size() - 1;
    // Step back over UTF-8 continuation bytes (10xxxxxx)
    while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) i--;
    text.erase(i);
    return true;
}

namespace {
bool continuationByte(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }
}  // namespace

size_t characterBoundary(const std::string& text, size_t offset) {
    offset = std::min(offset, text.size());
    while (offset > 0 && offset < text.size() && continuationByte(text[offset])) offset--;
    return offset;
}

size_t previousCharacter(const std::string& text, size_t offset) {
    offset = characterBoundary(text, offset);
    if (offset == 0) return 0;
    offset--;
    while (offset > 0 && continuationByte(text[offset])) offset--;
    return offset;
}

size_t nextCharacter(const std::string& text, size_t offset) {
    offset = characterBoundary(text, offset);
    if (offset >= text.size()) return text.size();
    offset++;
    while (offset < text.size() && continuationByte(text[offset])) offset++;
    return offset;
}

} // namespace Zepra::WebCore
