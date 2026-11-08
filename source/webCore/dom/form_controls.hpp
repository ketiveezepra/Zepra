// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file form_controls.hpp
 * @brief Form controls as the HTML spec describes them, on the plain DOM:
 *        the live value and checkedness of <input>/<textarea>, which elements
 *        take focus, a control's form owner, and what a form submits.
 *
 * Page scripts (dom_bindings.cpp), user input (user_input.cpp) and the
 * browser's layout all read a control through these, so what the user typed,
 * what a script set and what is drawn are the same value.
 */

#pragma once

#include "browser/dom.hpp"

#include <string>
#include <utility>
#include <vector>

namespace Zepra::WebCore {

/// <input> type, lowercase; "text" when missing (the attribute's default)
std::string inputType(const DOMElement* element);
/// <textarea>, or an <input> whose value is text the user types
/// (text, search, password, email, url, tel, number, date, ...)
bool isTextControl(const DOMElement* element);
/// A single-line text <input> (Enter submits its form)
bool isTextInput(const DOMElement* element);
/// <input type=checkbox|radio>
bool isCheckable(const DOMElement* element);
/// <button> (type submit, or no type) and <input type=submit|image>
bool isSubmitButton(const DOMElement* element);
/// <button type=reset> and <input type=reset>
bool isResetButton(const DOMElement* element);
/// The control or one of its <fieldset> ancestors is disabled
bool isDisabledControl(const DOMElement* element);
/// Takes focus when clicked or through focus(): enabled form controls,
/// links with an href, and elements with a tabindex or contenteditable
bool isFocusable(const DOMElement* element);
/// The element itself when focusable, else its nearest focusable ancestor
DOMElement* focusableSelfOrAncestor(DOMElement* element);

/// What the control holds: the value typed or set by a script, else the
/// value attribute (a textarea: its text; a select: its selected option's
/// value; a checkbox or radio without a value attribute: "on")
std::string formControlValue(const DOMElement* element);
/// Sets the value the control holds without touching its attributes; a new
/// value puts the caret at its end. The caller decides whether the page must
/// be laid out again.
void setFormControlValue(DOMElement* element, const std::string& value);
/// A control whose text a user or script can select: a <textarea>, or an
/// <input> of type text, search, url, tel or password (selectionStart is
/// null on the others)
bool supportsTextSelection(const DOMElement* element);

/// A text control's selection, as byte offsets into its value (which is
/// also how page scripts index strings); start <= end, and the caret is
/// the end the user moves (start when `backward`)
struct TextSelection {
    size_t start = 0;
    size_t end = 0;
    bool backward = false;
    size_t caret() const { return backward ? start : end; }
    size_t anchor() const { return backward ? end : start; }
    bool collapsed() const { return start == end; }
};
/// The selection, kept inside the value and on character boundaries; the
/// end of the value until something placed it
TextSelection formControlSelection(const DOMElement* element);
/// Selects [start, end) (swapped when end < start), clamped as above
void setFormControlSelection(DOMElement* element, size_t start, size_t end, bool backward = false);
/// Selects from `anchor` to `caret`, whichever comes first
void setFormControlCaret(DOMElement* element, size_t anchor, size_t caret);
/// Puts `text` in place of [start, end) of the value and leaves the caret
/// after it (no events: the caller fires them)
void replaceFormControlText(DOMElement* element, size_t start, size_t end, const std::string& text);

/// A checkbox's or radio's checkedness (the checked attribute until a click
/// or a script set it)
bool formControlChecked(const DOMElement* element);
/// Sets checkedness; checking a radio unchecks the others of its group
/// (same name, same form owner)
void setFormControlChecked(DOMElement* element, bool checked);

/// The <form> a control belongs to: the one its form attribute names, else
/// the nearest <form> ancestor; null when there is none
DOMElement* formOwner(const DOMElement* control);
/// The form's controls in tree order (form.elements): button, fieldset,
/// input, object, output, select, textarea
std::vector<DOMElement*> formControls(const DOMElement* form);
/// The first submit button of the form (Enter in a text field clicks it)
DOMElement* defaultButton(const DOMElement* form);
/// The radios of the same group as `radio`, itself included
std::vector<DOMElement*> radioGroup(const DOMElement* radio);
/// Puts every control of the form back to its attributes
void resetFormControls(const DOMElement* form);

/// The name/value pairs a submission sends, in tree order: enabled controls
/// with a name; checkboxes and radios only when checked; buttons only when
/// they are the submitter. When `files` is given it gets, for each pair,
/// whether it came from a file input (sent with no file: we have no picker)
std::vector<std::pair<std::string, std::string>> formData(const DOMElement* form, const DOMElement* submitter,
                                                          std::vector<bool>* files = nullptr);
/// application/x-www-form-urlencoded: "q=a+b&lang=en"
std::string urlEncodeFormData(const std::vector<std::pair<std::string, std::string>>& data);
/// multipart/form-data: a part per pair between `boundary` lines; names are
/// escaped as browsers do (" CR LF as %22 %0D %0A), `files[i]` marks a file
/// part (an empty filename and application/octet-stream)
std::string multipartFormData(const std::vector<std::pair<std::string, std::string>>& data,
                              const std::vector<bool>& files, const std::string& boundary);
/// text/plain: "name=value" lines ending in CR LF, nothing escaped
std::string plainTextFormData(const std::vector<std::pair<std::string, std::string>>& data);

/// Where and how a form goes: the submitter's formaction / formmethod /
/// formtarget win over the form's attributes
struct FormSubmission {
    std::string action;   // as written; empty means the document's own URL
    std::string method;   // "get", "post" or "dialog"
    std::string target;   // "_blank", a frame name, or empty
    /// "application/x-www-form-urlencoded", "multipart/form-data" or
    /// "text/plain" (formenctype / enctype; GET always url-encodes)
    std::string enctype;
    std::string body;          // the form data in that encoding
    std::string contentType;   // the Content-Type a POST sends (the boundary included)
};
FormSubmission planFormSubmission(const DOMElement* form, const DOMElement* submitter);
/// The URL a GET submission navigates to: `actionUrl` (already resolved)
/// without its query and fragment, then "?" and the encoded data
std::string formSubmissionUrl(const std::string& actionUrl, const std::string& body);

/// Removes the last character (UTF-8 sequence) of `text`; false when empty
bool eraseLastCharacter(std::string& text);
/// The offset of the character boundary before / after `offset` in `text`
/// (a UTF-8 sequence is one step); stays put at either end
size_t previousCharacter(const std::string& text, size_t offset);
size_t nextCharacter(const std::string& text, size_t offset);
/// `offset` moved back to the start of the character it falls inside
size_t characterBoundary(const std::string& text, size_t offset);

} // namespace Zepra::WebCore
