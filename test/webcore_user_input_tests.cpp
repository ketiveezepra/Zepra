// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * webcore_user_input_tests.cpp - Keys, typing, focus and forms on the live
 * DOM: the live value and checkedness of controls, form data, keydown /
 * keypress / beforeinput / input / keyup, focus / blur / change, click
 * activation (checkboxes, labels, submit and reset buttons), implicit
 * submission, and the DOM key values the browser maps platform keys to.
 * No window.
 */

#include <gtest/gtest.h>

#include "browser/dom.hpp"
#include "css/css_engine.hpp"
#include "dom/form_controls.hpp"
#include "html/html_parser.hpp"
#include "scripting/dom_bindings.hpp"
#include "scripting/script_context.hpp"
#include "page_keys.hpp"

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
    std::vector<FormSubmission> submissions;

    explicit Page(const std::string& html) {
        // Focus, hover and press are process-wide: a test's freed elements
        // must not look focused to the next test's document (an allocator
        // can hand the same address to one of its elements)
        interactionState() = {};
        HTMLParser parser;
        doc = parser.parse(html);
        script = std::make_unique<ScriptContext>();
        script->setConsoleHandler([this](const std::string& level, const std::string& message) {
            console.push_back(level + ": " + message);
        });
        script->initialize(doc.get());
        script->setSubmitHandler([this](const FormSubmission& s) { submissions.push_back(s); });
    }

    std::string eval(const std::string& code) {
        ScriptResult r = script->evaluate(code, "test.js");
        EXPECT_TRUE(r.success) << r.error << "\n  in: " << code;
        return r.success ? r.value : "<error: " + r.error + ">";
    }

    DOMElement* byId(const std::string& id) { return doc->getElementById(id); }

    ~Page() { interactionState() = {}; }
};

KeyEventInit key(const std::string& k, const std::string& code, int keyCode) {
    KeyEventInit init;
    init.key = k;
    init.code = code;
    init.keyCode = keyCode;
    return init;
}

KeyResult typeKey(Page& page, char c) {
    std::string s(1, c);
    std::string upper(1, static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return page.script->keyDown(key(s, "Key" + upper, upper[0]), s);
}

const char* kForm = R"(<!DOCTYPE html><html><body>
<form id="f" action="/search?old=1#top">
  <input id="q" name="q" value="start">
  <input id="lang" name="lang" type="hidden" value="en">
  <input id="cb" name="safe" type="checkbox">
  <input id="r1" name="size" type="radio" value="s" checked>
  <input id="r2" name="size" type="radio" value="l">
  <input id="off" name="off" value="x" disabled>
  <textarea id="t" name="note">a</textarea>
  <select id="sel" name="sort"><option value="new">New</option><option selected>Top</option></select>
  <button id="go" name="go" value="1">Go</button>
  <button id="clear" type="reset">Clear</button>
  <label id="cblabel" for="cb">Safe search</label>
</form>
<input id="outside" name="o" form="f" value="out">
<div id="plain">text</div>
<button id="lone" disabled>Disabled</button>
</body></html>)";

/// Records the events that reach the listed elements (and the form, in the
/// bubble phase) as "type:id" in window.log
const char* kLogger = R"(
var log = [];
function rec(e) {
  var id = e.currentTarget.id || e.currentTarget.nodeName;
  log.push(e.type + ':' + (e.target.id || e.target.nodeName) + (e.currentTarget === e.target ? '' : '@' + id));
}
)";

}  // namespace

// ----------------------------------------------------------------------------
// Control state
// ----------------------------------------------------------------------------

TEST(FormControls, ValueIsLiveAndApartFromTheAttribute) {
    Page page(kForm);
    EXPECT_EQ(page.eval("var q = document.getElementById('q'); q.value + ' ' + q.defaultValue"), "start start");
    uint64_t before = domMutationVersion();
    EXPECT_EQ(page.eval("q.value = 'typed'; [q.value, q.defaultValue, q.getAttribute('value')].join()"),
              "typed,start,start");
    EXPECT_GT(domMutationVersion(), before) << "a script's value must be laid out again";
    EXPECT_EQ(formControlValue(page.byId("q")), "typed");
    // Changing the attribute later does not touch a value that was set
    EXPECT_EQ(page.eval("q.setAttribute('value', 'attr'); q.value + ' ' + q.defaultValue"), "typed attr");
    // A textarea's default is its text
    EXPECT_EQ(page.eval("var t = document.getElementById('t'); t.value = 'b'; t.value + t.defaultValue + t.textContent"), "baa");
    // A select's value is its selected option's
    EXPECT_EQ(page.eval("document.getElementById('sel').value"), "Top");
    // A clone keeps what was typed
    EXPECT_EQ(page.eval("q.cloneNode().value"), "typed");
}

TEST(FormControls, CheckedIsLiveAndRadiosExcludeEachOther) {
    Page page(kForm);
    EXPECT_EQ(page.eval("var cb = document.getElementById('cb'); cb.checked = true;"
                        "[cb.checked, cb.defaultChecked, cb.hasAttribute('checked'), cb.value].join()"),
              "true,false,false,on");
    EXPECT_TRUE(selectorMatches(page.byId("cb"), "input:checked"));
    EXPECT_EQ(page.eval("var r1 = document.getElementById('r1'), r2 = document.getElementById('r2');"
                        "r2.checked = true; [r1.checked, r2.checked].join()"),
              "false,true");
    EXPECT_FALSE(selectorMatches(page.byId("r1"), ":checked"));
    EXPECT_EQ(radioGroup(page.byId("r1")).size(), 2u);
}

TEST(FormControls, FormOwnerElementsAndReset) {
    Page page(kForm);
    page.eval(kLogger);
    EXPECT_EQ(page.eval("var f = document.getElementById('f');"
                        "[document.getElementById('q').form === f, document.getElementById('outside').form === f,"
                        " f.elements.length, f.length, f.elements[0].id, typeof document.getElementById('plain').form].join()"),
              "true,true,11,11,q,undefined");
    page.eval("var q = document.getElementById('q'), cb = document.getElementById('cb'),"
              "    r1 = document.getElementById('r1'), r2 = document.getElementById('r2');"
              "f.addEventListener('reset', rec); q.value = 'x'; cb.checked = true; r2.checked = true; f.reset();");
    EXPECT_EQ(page.eval("log.join()"), "reset:f");
    EXPECT_EQ(page.eval("[q.value, cb.checked, r1.checked, r2.checked].join()"), "start,false,true,false");
    // A canceled reset changes nothing
    page.eval("q.value = 'kept'; f.onreset = function() { return false; }; f.reset();");
    EXPECT_EQ(page.eval("q.value"), "kept");
}

// ----------------------------------------------------------------------------
// What a form submits
// ----------------------------------------------------------------------------

TEST(FormData, SuccessfulControlsInTreeOrder) {
    Page page(kForm);
    DOMElement* form = page.byId("f");
    auto data = formData(form, page.byId("go"));
    std::string joined;
    for (const auto& [n, v] : data) joined += n + "=" + v + ";";
    // The unchecked box, the disabled field and the reset button send nothing
    EXPECT_EQ(joined, "q=start;lang=en;size=s;note=a;sort=Top;go=1;o=out;");
    // Without a submitter no button is sent
    EXPECT_EQ(formData(form, nullptr).size(), 6u);

    setFormControlValue(page.byId("t"), "l1\nl2");
    setFormControlChecked(page.byId("cb"), true);
    EXPECT_EQ(urlEncodeFormData(formData(form, nullptr)),
              "q=start&lang=en&safe=on&size=s&note=l1%0D%0Al2&sort=Top&o=out");
}

TEST(FormData, EncodingAndTheSubmissionUrl) {
    EXPECT_EQ(urlEncodeFormData({{"q", "a b&c=d/\xc3\xa9"}, {"x*y", "-._~"}}), "q=a+b%26c%3Dd%2F%C3%A9&x*y=-._%7E");
    EXPECT_EQ(formSubmissionUrl("https://x.test/search?old=1#top", "q=1"), "https://x.test/search?q=1");
    EXPECT_EQ(formSubmissionUrl("https://x.test/", ""), "https://x.test/?");

    Page page(R"(<form id="f" action="/a" method="POST" target="_blank">
        <input name="n" value="v"><button id="b" formaction="/b" formmethod="get">B</button></form>)");
    FormSubmission own = planFormSubmission(page.byId("f"), nullptr);
    EXPECT_EQ(own.action, "/a");
    EXPECT_EQ(own.method, "post");
    EXPECT_EQ(own.target, "_blank");
    EXPECT_EQ(own.body, "n=v");
    FormSubmission viaButton = planFormSubmission(page.byId("f"), page.byId("b"));
    EXPECT_EQ(viaButton.action, "/b");
    EXPECT_EQ(viaButton.method, "get");
}

// ----------------------------------------------------------------------------
// Focus
// ----------------------------------------------------------------------------

TEST(Focus, FocusBlurFocusinFocusoutAndActiveElement) {
    Page page(kForm);
    page.eval(kLogger);
    page.eval("var q = document.getElementById('q'), t = document.getElementById('t'), f = document.getElementById('f');"
              "for (var el of [q, t]) for (var type of ['focus', 'blur', 'focusin', 'focusout']) el.addEventListener(type, rec);"
              "for (var type of ['focus', 'blur', 'focusin', 'focusout']) f.addEventListener(type, rec);"
              "q.addEventListener('blur', function(e) { log.push('active=' + document.activeElement.nodeName + ' related=' + e.relatedTarget.id); });"
              "t.addEventListener('focus', function() { log.push('active=' + document.activeElement.id); });");
    EXPECT_EQ(page.eval("document.activeElement === document.body"), "true");
    page.eval("q.focus()");
    // focus and blur stay on the element; focusin and focusout bubble
    EXPECT_EQ(page.eval("log.join()"), "focus:q,focusin:q,focusin:q@f");
    EXPECT_EQ(page.eval("document.activeElement === q"), "true");
    EXPECT_EQ(page.script->focusedElement(), page.byId("q"));
    EXPECT_TRUE(selectorMatches(page.byId("q"), ":focus"));
    EXPECT_TRUE(selectorMatches(page.byId("f"), ":focus-within"));

    page.eval("log = []; t.focus()");
    EXPECT_EQ(page.eval("log.join()"),
              "blur:q,active=BODY related=t,focusout:q,focusout:q@f,focus:t,active=t,focusin:t,focusin:t@f");
    page.eval("log = []; q.blur(); t.blur()");
    EXPECT_EQ(page.eval("log.join() + ' ' + (document.activeElement === document.body)"), "blur:t,focusout:t,focusout:t@f true");
    // Elements that cannot take focus ignore focus()
    page.eval("document.getElementById('plain').focus(); document.getElementById('lone').focus()");
    EXPECT_EQ(page.script->focusedElement(), nullptr);
    page.eval("document.getElementById('plain').tabIndex = 0; document.getElementById('plain').focus()");
    EXPECT_EQ(page.script->focusedElement(), page.byId("plain"));
}

TEST(Focus, ChangeFiresOnBlurOnlyForTheUsersEdits) {
    Page page(kForm);
    page.eval(kLogger);
    page.eval("var q = document.getElementById('q'); q.addEventListener('change', rec); q.focus();"
              "q.value = 'by script'; q.blur();");
    EXPECT_EQ(page.eval("log.join()"), "") << "a script's change fires no change event";
    page.script->focusElement(page.byId("q"));
    typeKey(page, 'x');
    page.script->focusElement(nullptr);
    EXPECT_EQ(page.eval("log.join() + ' ' + q.value"), "change:q by scriptx");
}

// ----------------------------------------------------------------------------
// Keys and typing
// ----------------------------------------------------------------------------

TEST(Keys, TypingFiresKeydownKeypressBeforeinputInputKeyup) {
    Page page(kForm);
    page.eval(kLogger);
    page.eval("var q = document.getElementById('q'); q.value = '';"
              "var seen = [];"
              "for (var type of ['keydown', 'keypress', 'keyup', 'beforeinput', 'input']) q.addEventListener(type, rec);"
              "q.addEventListener('keydown', function(e) { seen.push([e instanceof KeyboardEvent, e.isTrusted, e.key, e.code,"
              "  e.keyCode, e.which, e.charCode, e.shiftKey, e.getModifierState('Shift'), e.bubbles, e.cancelable].join()); });"
              "q.addEventListener('keypress', function(e) { seen.push([e.key, e.keyCode, e.which, e.charCode].join()); });"
              "q.addEventListener('input', function(e) { seen.push([e instanceof InputEvent, e.inputType, e.data, q.value].join()); });");
    page.script->focusElement(page.byId("q"));
    uint64_t before = domMutationVersion();
    KeyEventInit shiftA = key("A", "KeyA", 65);
    shiftA.shiftKey = true;
    KeyResult r = page.script->keyDown(shiftA, "A");
    page.script->keyUp(shiftA);
    EXPECT_FALSE(r.canceled);
    EXPECT_TRUE(r.consumed);
    EXPECT_EQ(page.eval("log.join()"), "keydown:q,keypress:q,beforeinput:q,input:q,keyup:q");
    EXPECT_EQ(page.eval("seen.join(' | ')"),
              "true,true,A,KeyA,65,65,0,true,true,true,true | A,65,65,65 | true,insertText,A,A");
    EXPECT_EQ(formControlValue(page.byId("q")), "A");
    EXPECT_EQ(domMutationVersion(), before) << "typing changes no attribute; the browser redraws the field itself";

    typeKey(page, 'b');
    page.script->insertText("\xc3\xa9");   // é, from an input method
    EXPECT_EQ(page.eval("q.value"), "Ab\xc3\xa9");
    page.eval("log = []");
    page.script->keyDown(key("Backspace", "Backspace", 8), "");
    EXPECT_EQ(page.eval("log.join() + ' ' + q.value"), "keydown:q,beforeinput:q,input:q Ab")
        << "Backspace has no keypress and removes a whole character";
    // Control characters never reach a field
    page.script->insertText("x\ty\x01");
    EXPECT_EQ(page.eval("q.value"), "Abxy");
}

TEST(Keys, CanceledKeysTypeNothing) {
    Page page(kForm);
    page.eval("var q = document.getElementById('q'); q.value = '';"
              "q.addEventListener('keydown', function(e) { if (e.key === 'x') e.preventDefault(); });"
              "q.addEventListener('keypress', function(e) { if (e.key === 'y') e.preventDefault(); });"
              "q.addEventListener('beforeinput', function(e) { if (e.data === 'z') e.preventDefault(); });");
    page.script->focusElement(page.byId("q"));
    EXPECT_TRUE(typeKey(page, 'x').canceled);
    EXPECT_TRUE(typeKey(page, 'y').canceled);
    EXPECT_FALSE(typeKey(page, 'z').canceled);
    typeKey(page, 'k');
    EXPECT_EQ(page.eval("q.value"), "k");
}

TEST(Keys, MaxlengthReadonlyAndListenersThatRewriteTheValue) {
    Page page(R"(<input id="a" maxlength="2"><input id="b" readonly value="ro">
                 <input id="c">)");
    page.script->focusElement(page.byId("a"));
    for (char c : std::string("abc")) typeKey(page, c);
    EXPECT_EQ(formControlValue(page.byId("a")), "ab");
    page.script->focusElement(page.byId("b"));
    KeyResult r = typeKey(page, 'z');
    EXPECT_TRUE(r.consumed) << "a read-only field still keeps its keys from scrolling the page";
    EXPECT_EQ(formControlValue(page.byId("b")), "ro");
    page.eval("var c = document.getElementById('c');"
              "c.addEventListener('input', function() { c.value = c.value.toUpperCase(); });");
    page.script->focusElement(page.byId("c"));
    typeKey(page, 'h');
    typeKey(page, 'i');
    EXPECT_EQ(formControlValue(page.byId("c")), "HI");
}

TEST(Keys, WithoutFocusKeysGoToTheBody) {
    Page page(kForm);
    page.eval("var keys = []; document.addEventListener('keydown', function(e) { keys.push(e.target.nodeName + ':' + e.key); });"
              "window.addEventListener('keyup', function(e) { keys.push('up:' + e.key); });");
    KeyResult r = page.script->keyDown(key(" ", "Space", 32), " ");
    page.script->keyUp(key(" ", "Space", 32));
    EXPECT_FALSE(r.consumed) << "Space on the page itself scrolls";
    EXPECT_EQ(page.eval("keys.join()"), "BODY: ,up: ");
    page.eval("document.body.onkeydown = function(e) { e.preventDefault(); }");
    EXPECT_TRUE(page.script->keyDown(key("PageDown", "PageDown", 34), "").canceled);
    // Typing with nothing focused changes nothing
    EXPECT_EQ(formControlValue(page.byId("q")), "start");
}

TEST(Keys, KeyboardEventConstructor) {
    Page page(kForm);
    EXPECT_EQ(page.eval("var e = new KeyboardEvent('keydown', { key: 'x', ctrlKey: true, bubbles: true });"
                        "[e.type, e.key, e.code, e.keyCode, e.ctrlKey, e.altKey, e.getModifierState('Control'),"
                        " e.bubbles, e.isTrusted, KeyboardEvent.DOM_KEY_LOCATION_NUMPAD].join()"),
              "keydown,x,,0,true,false,true,true,false,3");
    EXPECT_EQ(page.eval("var i = new InputEvent('input', { data: 'q', inputType: 'insertText' }); i.data + i.inputType"), "qinsertText");
    EXPECT_EQ(page.eval("new FocusEvent('focus').relatedTarget"), "null");
}

// ----------------------------------------------------------------------------
// Enter, clicks and submissions
// ----------------------------------------------------------------------------

TEST(Submit, EnterClicksTheDefaultButtonAndSubmits) {
    Page page(kForm);
    page.eval(kLogger);
    page.eval("var q = document.getElementById('q'), f = document.getElementById('f'), go = document.getElementById('go');"
              "q.addEventListener('change', rec); go.addEventListener('click', rec);"
              "f.addEventListener('submit', function(e) { log.push('submit:' + e.submitter.id + ':' + (e instanceof SubmitEvent) + ':' + e.cancelable); });");
    page.script->focusElement(page.byId("q"));
    typeKey(page, 's');
    page.script->keyDown(key("Enter", "Enter", 13), "");
    EXPECT_EQ(page.eval("log.join()"), "change:q,click:go,submit:go:true:true");
    ASSERT_EQ(page.submissions.size(), 1u);
    EXPECT_EQ(page.submissions[0].action, "/search?old=1#top");
    EXPECT_EQ(page.submissions[0].method, "get");
    EXPECT_EQ(page.submissions[0].body, "q=starts&lang=en&size=s&note=a&sort=Top&go=1&o=out");

    // A canceled submit goes nowhere
    page.eval("f.addEventListener('submit', function(e) { e.preventDefault(); })");
    page.script->keyDown(key("Enter", "Enter", 13), "");
    EXPECT_EQ(page.submissions.size(), 1u);
}

TEST(Submit, ImplicitSubmissionWithoutAButton) {
    Page one(R"(<form id="f"><input id="a" name="a"></form>)");
    one.script->focusElement(one.byId("a"));
    one.script->keyDown(key("Enter", "Enter", 13), "");
    EXPECT_EQ(one.submissions.size(), 1u) << "a form with one field submits on Enter";

    Page two(R"(<form id="f"><input id="a" name="a"><input id="b" name="b"></form>)");
    two.script->focusElement(two.byId("a"));
    two.script->keyDown(key("Enter", "Enter", 13), "");
    EXPECT_EQ(two.submissions.size(), 0u) << "two fields and no button: Enter does not submit";

    Page area(R"(<form id="f"><textarea id="t" name="t"></textarea><button>Go</button></form>)");
    area.script->focusElement(area.byId("t"));
    typeKey(area, 'a');
    area.script->keyDown(key("Enter", "Enter", 13), "");
    typeKey(area, 'b');
    EXPECT_EQ(formControlValue(area.byId("t")), "a\nb");
    EXPECT_EQ(area.submissions.size(), 0u) << "Enter in a textarea starts a line";
}

TEST(Submit, ScriptSubmissions) {
    Page page(kForm);
    page.eval(kLogger);
    page.eval("var f = document.getElementById('f'); f.addEventListener('submit', rec); f.submit();");
    EXPECT_EQ(page.eval("log.join()"), "") << "form.submit() fires no submit event";
    EXPECT_EQ(page.submissions.size(), 1u);
    page.eval("f.requestSubmit(document.getElementById('go'))");
    EXPECT_EQ(page.eval("log.join()"), "submit:f");
    EXPECT_EQ(page.submissions.size(), 2u);
    EXPECT_EQ(page.eval("var err = 'no'; try { f.requestSubmit(document.getElementById('q')); } catch (e) { err = e.name } err"), "TypeError");
}

TEST(Clicks, PressMovesFocusAndActivates) {
    Page page(kForm);
    page.eval(kLogger);
    page.eval("var cb = document.getElementById('cb');"
              "cb.addEventListener('click', function(e) { log.push('click sees ' + cb.checked); });"
              "for (var type of ['input', 'change']) cb.addEventListener(type, rec);");
    // A checkbox toggles before its click listeners run
    EXPECT_TRUE(page.script->pressElement(page.byId("cb"), 5, 5));
    EXPECT_EQ(page.eval("log.join() + ' ' + cb.checked"), "click sees true,input:cb,change:cb true");
    EXPECT_EQ(page.script->focusedElement(), page.byId("cb"));
    // A canceled click puts it back and fires no change
    page.eval("log = []; cb.onclick = function() { return false; }");
    EXPECT_FALSE(page.script->pressElement(page.byId("cb"), 5, 5));
    EXPECT_EQ(page.eval("log.join() + ' ' + cb.checked"), "click sees false true");
    page.eval("cb.onclick = null");
    // A label clicks its control
    page.eval("log = []");
    page.script->pressElement(page.byId("cblabel"), 5, 5);
    EXPECT_EQ(page.eval("cb.checked"), "false");
    EXPECT_EQ(page.eval("log.join()"), "click sees false,input:cb,change:cb");
    // Radios: the pressed one is checked, the group's other one not
    page.script->pressElement(page.byId("r2"), 5, 5);
    EXPECT_EQ(page.eval("[document.getElementById('r1').checked, document.getElementById('r2').checked].join()"), "false,true");

    // Pressing text that cannot take focus gives focus back to the page
    page.script->pressElement(page.byId("q"), 5, 5);
    EXPECT_EQ(page.script->focusedElement(), page.byId("q"));
    page.script->pressElement(page.byId("plain"), 5, 5);
    EXPECT_EQ(page.script->focusedElement(), nullptr);

    // Disabled buttons get no click
    page.eval("document.getElementById('lone').addEventListener('click', rec); log = []");
    page.script->pressElement(page.byId("lone"), 5, 5);
    EXPECT_EQ(page.eval("log.join()"), "");
}

TEST(Clicks, SubmitAndResetButtons) {
    Page page(kForm);
    page.eval("document.getElementById('q').value = 'changed'");
    page.script->pressElement(page.byId("go"), 5, 5);
    ASSERT_EQ(page.submissions.size(), 1u);
    EXPECT_NE(page.submissions[0].body.find("q=changed"), std::string::npos);
    page.script->pressElement(page.byId("clear"), 5, 5);
    EXPECT_EQ(formControlValue(page.byId("q")), "start");
    EXPECT_EQ(page.submissions.size(), 1u);
    // el.click() from a script activates too
    page.eval("document.getElementById('go').click()");
    EXPECT_EQ(page.submissions.size(), 2u);
}

TEST(Clicks, SpaceAndEnterOnFocusedButtons) {
    Page page(kForm);
    page.script->focusElement(page.byId("cb"));
    EXPECT_TRUE(page.script->keyDown(key(" ", "Space", 32), " ").consumed);
    EXPECT_TRUE(formControlChecked(page.byId("cb")));
    page.script->keyDown(key("Enter", "Enter", 13), "");
    EXPECT_TRUE(formControlChecked(page.byId("cb"))) << "Enter does not toggle a checkbox";
    page.script->focusElement(page.byId("go"));
    page.script->keyDown(key("Enter", "Enter", 13), "");
    EXPECT_EQ(page.submissions.size(), 1u);
}

TEST(Clicks, ListenerErrorsDoNotStopTheDefaultAction) {
    Page page(kForm);
    page.eval("document.getElementById('q').addEventListener('keydown', function() { throw new Error('boom'); })");
    page.script->focusElement(page.byId("q"));
    typeKey(page, 'z');
    EXPECT_EQ(formControlValue(page.byId("q")), "startz");
    ASSERT_FALSE(page.console.empty());
    EXPECT_NE(page.console.back().find("boom"), std::string::npos);
}

// ----------------------------------------------------------------------------
// Platform keys to DOM key values (the browser's mapping)
// ----------------------------------------------------------------------------

TEST(PageKeys, KeyCodeAndKeyValues) {
    using NXRender::KeyCode;
    NXRender::Modifiers none;
    NXRender::Modifiers shift;
    shift.shift = true;
    NXRender::Modifiers ctrl;
    ctrl.ctrl = true;

    KeyEventInit a = ZepraBrowser::domKeyFor(KeyCode::A, "a", none);
    EXPECT_EQ(a.key, "a");
    EXPECT_EQ(a.code, "KeyA");
    EXPECT_EQ(a.keyCode, 65);
    EXPECT_EQ(ZepraBrowser::domKeyFor(KeyCode::A, "A", shift).key, "A");
    // Ctrl+A types nothing; the key still says which letter
    KeyEventInit ctrlA = ZepraBrowser::domKeyFor(KeyCode::A, "", ctrl);
    EXPECT_EQ(ctrlA.key, "a");
    EXPECT_TRUE(ctrlA.ctrlKey);
    // Key up comes without text: Shift picks the shifted symbol
    EXPECT_EQ(ZepraBrowser::domKeyFor(KeyCode::Num1, "", shift).key, "!");
    EXPECT_EQ(ZepraBrowser::domKeyFor(KeyCode::Num1, "", none).code, "Digit1");
    EXPECT_EQ(ZepraBrowser::domKeyFor(KeyCode::Num1, "", none).keyCode, 49);
    EXPECT_EQ(ZepraBrowser::domKeyFor(KeyCode::Slash, "?", shift).key, "?");

    KeyEventInit enter = ZepraBrowser::domKeyFor(KeyCode::Enter, "", none);
    EXPECT_EQ(enter.key, "Enter");
    EXPECT_EQ(enter.keyCode, 13);
    KeyEventInit left = ZepraBrowser::domKeyFor(KeyCode::Left, "", none);
    EXPECT_EQ(left.key, "ArrowLeft");
    EXPECT_EQ(left.keyCode, 37);
    KeyEventInit space = ZepraBrowser::domKeyFor(KeyCode::Space, " ", none);
    EXPECT_EQ(space.key, " ");
    EXPECT_EQ(space.code, "Space");
    KeyEventInit f5 = ZepraBrowser::domKeyFor(KeyCode::F5, "", none);
    EXPECT_EQ(f5.key, "F5");
    EXPECT_EQ(f5.keyCode, 116);
    KeyEventInit shiftKey = ZepraBrowser::domKeyFor(KeyCode::Shift, "", shift);
    EXPECT_EQ(shiftKey.key, "Shift");
    EXPECT_EQ(shiftKey.location, 1);
    EXPECT_EQ(ZepraBrowser::domKeyFor(KeyCode::Unknown, "", none).key, "Unidentified");
}
