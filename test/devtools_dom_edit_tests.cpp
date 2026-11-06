// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file devtools_dom_edit_tests.cpp
 * @brief The Inspector's markup view: attribute, text and HTML edits and
 *        their undo, delete and duplicate, the nodes it refuses to touch,
 *        Copy CSS Selector, Copy outerHTML, the markup search, and the
 *        lifetime of the nodes it takes out of the tree.
 */

#include <gtest/gtest.h>

#include "browser/dom.hpp"
#include "css/css_engine.hpp"
#include "devtools/dom_edit.hpp"
#include "html/html_parser.hpp"
#include "scripting/dom_bindings.hpp"
#include "scripting/script_context.hpp"

#include <memory>
#include <string>
#include <vector>

// Globals the layout engine expects from the browser
int g_width = 1280;
int g_height = 800;

using namespace Zepra::WebCore;
using namespace ZepraBrowser::DevTools;

namespace {

const char* kHtml = R"(<!DOCTYPE html><html><head><title>t</title></head><body>
<div id="main" class="box"><p id="p1" class="note">Hello World</p><p class="note">second</p><!-- a comment --></div>
<ul class="list"><li class="item">one</li><li class="item">two</li><li class="item special">three</li></ul>
<ul class="list"><li class="item">four</li><li class="item">five</li></ul>
<a href="https://example.com/page" data-x="Needle">link</a>
<span id="dup">a</span><span id="dup">b</span>
<section><i class="md:p-4">x</i><i>y</i></section>
</body></html>)";

std::unique_ptr<DOMDocument> parse(const char* html = kHtml) {
    HTMLParser parser;
    return parser.parse(html);
}

std::vector<DOMElement*> elementChildren(DOMNode* node) {
    std::vector<DOMElement*> out;
    for (const auto& child : node->childNodes()) {
        if (child->nodeType() == NodeType::Element) out.push_back(static_cast<DOMElement*>(child.get()));
    }
    return out;
}

std::string tags(const std::vector<DOMElement*>& elements) {
    std::string out;
    for (auto* e : elements) out += (out.empty() ? "" : ",") + e->tagName();
    return out;
}

/// The single element a selector matches in the document, or null when it matches 0 or 2+
DOMElement* onlyMatch(DOMDocument* doc, const std::string& selector) {
    auto all = doc->querySelectorAll(selector);
    return all.size() == 1 ? all[0] : nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// parseAttributeText
// ---------------------------------------------------------------------------

TEST(DomEditAttributeText, QuotedUnquotedAndValueless) {
    auto attrs = parseAttributeText("a=\"1\" b c='x' d=4");
    ASSERT_EQ(attrs.size(), 4u);
    EXPECT_EQ(attrs[0], AttributeText("a", "1"));
    EXPECT_EQ(attrs[1], AttributeText("b", ""));
    EXPECT_EQ(attrs[2], AttributeText("c", "x"));
    EXPECT_EQ(attrs[3], AttributeText("d", "4"));
}

TEST(DomEditAttributeText, EmptyAndBlankInput) {
    EXPECT_TRUE(parseAttributeText("").empty());
    EXPECT_TRUE(parseAttributeText("   \t ").empty());
}

TEST(DomEditAttributeText, QuotesSpacesAndEntities) {
    auto attrs = parseAttributeText("title=\"it's a 'test'\" alt='say \"hi\"' href=\"a?b=1&amp;c=2\"");
    ASSERT_EQ(attrs.size(), 3u);
    EXPECT_EQ(attrs[0].second, "it's a 'test'");
    EXPECT_EQ(attrs[1].second, "say \"hi\"");
    EXPECT_EQ(attrs[2].second, "a?b=1&c=2");
}

TEST(DomEditAttributeText, NamesLowercasedFirstDuplicateWinsSpacesAroundEquals) {
    auto attrs = parseAttributeText("  DATA-Foo = bar  data-foo=baz CLASS=\"x y\" ");
    ASSERT_EQ(attrs.size(), 2u);
    EXPECT_EQ(attrs[0], AttributeText("data-foo", "bar"));
    EXPECT_EQ(attrs[1], AttributeText("class", "x y"));
}

TEST(DomEditAttributeText, UnterminatedQuoteRunsToTheEnd) {
    auto attrs = parseAttributeText("a=\"open value");
    ASSERT_EQ(attrs.size(), 1u);
    EXPECT_EQ(attrs[0], AttributeText("a", "open value"));
    auto single = parseAttributeText("b c='x y");
    ASSERT_EQ(single.size(), 2u);
    EXPECT_EQ(single[1], AttributeText("c", "x y"));
}

TEST(DomEditAttributeText, EmptyValueAndSelfClosingSlash) {
    auto attrs = parseAttributeText("a=\"\" b='' /");
    ASSERT_EQ(attrs.size(), 2u);
    EXPECT_EQ(attrs[0], AttributeText("a", ""));
    EXPECT_EQ(attrs[1], AttributeText("b", ""));
}

// ---------------------------------------------------------------------------
// Attribute edits
// ---------------------------------------------------------------------------

TEST(DomEditAttributes, SetRenameRemove) {
    auto doc = parse();
    EditHistory history(doc.get());
    DOMElement* p = doc->getElementById("p1");

    uint64_t v = domMutationVersion();
    EXPECT_TRUE(history.setAttribute(p, "title", "tip"));
    EXPECT_EQ(p->getAttribute("title"), "tip");
    EXPECT_GT(domMutationVersion(), v);

    v = domMutationVersion();
    EXPECT_TRUE(history.renameAttribute(p, "title", "data-tip"));
    EXPECT_FALSE(p->hasAttribute("title"));
    EXPECT_EQ(p->getAttribute("data-tip"), "tip");
    EXPECT_GT(domMutationVersion(), v);

    EXPECT_FALSE(history.renameAttribute(p, "missing", "x"));
    EXPECT_FALSE(history.renameAttribute(p, "data-tip", ""));

    v = domMutationVersion();
    EXPECT_TRUE(history.removeAttribute(p, "data-tip"));
    EXPECT_FALSE(p->hasAttribute("data-tip"));
    EXPECT_GT(domMutationVersion(), v);
    EXPECT_FALSE(history.removeAttribute(p, "data-tip"));
    EXPECT_EQ(history.undoCount(), 3u);
}

TEST(DomEditAttributes, EditReplacesWithWhateverWasTyped) {
    auto doc = parse();
    EditHistory history(doc.get());
    DOMElement* p = doc->getElementById("p1");

    // One attribute typed into the class field: becomes several
    EXPECT_TRUE(history.editAttribute(p, "class", "class=\"note big\" title=hi hidden"));
    EXPECT_EQ(p->getAttribute("class"), "note big");
    EXPECT_EQ(p->getAttribute("title"), "hi");
    EXPECT_TRUE(p->hasAttribute("hidden"));

    // Typed a different name: the old one goes
    EXPECT_TRUE(history.editAttribute(p, "title", "aria-label=\"Hi there\""));
    EXPECT_FALSE(p->hasAttribute("title"));
    EXPECT_EQ(p->getAttribute("aria-label"), "Hi there");

    // Cleared the field: the attribute is removed
    EXPECT_TRUE(history.editAttribute(p, "hidden", "  "));
    EXPECT_FALSE(p->hasAttribute("hidden"));

    // New attribute field (no old name)
    EXPECT_TRUE(history.editAttribute(p, "", "data-new=1"));
    EXPECT_EQ(p->getAttribute("data-new"), "1");
    // Nothing typed into a new field: nothing to do
    EXPECT_FALSE(history.editAttribute(p, "", ""));

    // One undo per edit, back to the parsed attributes
    while (history.canUndo()) EXPECT_TRUE(history.undo());
    EXPECT_EQ(p->getAttribute("class"), "note");
    EXPECT_EQ(p->getAttribute("id"), "p1");
    EXPECT_EQ(p->attributes().size(), 2u);
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

TEST(DomEditText, SetsTextAndCommentData) {
    auto doc = parse();
    EditHistory history(doc.get());
    DOMElement* p = doc->getElementById("p1");
    DOMNode* text = p->firstChild();
    ASSERT_EQ(text->nodeType(), NodeType::Text);

    uint64_t v = domMutationVersion();
    EXPECT_TRUE(history.setText(text, "Bye"));
    EXPECT_EQ(p->textContent(), "Bye");
    EXPECT_GT(domMutationVersion(), v);

    DOMNode* comment = doc->getElementById("main")->lastChild();
    ASSERT_EQ(comment->nodeType(), NodeType::Comment);
    EXPECT_TRUE(history.setText(comment, " edited "));
    EXPECT_EQ(comment->nodeValue(), " edited ");

    EXPECT_FALSE(history.setText(p, "elements are not text"));

    EXPECT_TRUE(history.undo());
    EXPECT_EQ(comment->nodeValue(), " a comment ");
    EXPECT_TRUE(history.undo());
    EXPECT_EQ(p->textContent(), "Hello World");
}

// ---------------------------------------------------------------------------
// Edit as HTML
// ---------------------------------------------------------------------------

TEST(DomEditHTML, ReplaceWithSeveralTopLevelNodes) {
    auto doc = parse();
    EditHistory history(doc.get());
    DOMElement* main = doc->getElementById("main");
    DOMElement* p1 = doc->getElementById("p1");

    uint64_t v = domMutationVersion();
    DOMNode* first = history.replaceWithHTML(p1, "<h2 class=\"t\">A</h2>text<em>B</em>");
    ASSERT_NE(first, nullptr);
    EXPECT_GT(domMutationVersion(), v);
    EXPECT_EQ(first->nodeName(), "h2");
    EXPECT_EQ(first->parentNode(), main);
    EXPECT_EQ(main->firstChild(), first);
    EXPECT_EQ(first->nextSibling()->nodeValue(), "text");
    EXPECT_EQ(first->nextSibling()->nextSibling()->nodeName(), "em");
    EXPECT_EQ(tags(elementChildren(main)), "h2,em,p");
    EXPECT_EQ(doc->getElementById("p1"), nullptr);
    // The new nodes belong to the page's document
    EXPECT_EQ(first->ownerDocument(), doc.get());
    EXPECT_EQ(first->firstChild()->ownerDocument(), doc.get());

    // The replaced node is kept, not freed
    EXPECT_TRUE(history.keepsDetached(p1));
    EXPECT_EQ(p1->getAttribute("id"), "p1");
    EXPECT_EQ(p1->parentNode(), nullptr);

    EXPECT_TRUE(history.undo());
    EXPECT_EQ(doc->getElementById("p1"), p1);
    EXPECT_EQ(main->firstChild(), p1);
    EXPECT_EQ(tags(elementChildren(main)), "p,p");
    // The inserted nodes are not freed by undo either: they wait in the detached list
    EXPECT_EQ(history.detachedCount(), 3u);
    EXPECT_TRUE(history.keepsDetached(first));
}

TEST(DomEditHTML, EmptyHTMLRemovesTheNode) {
    auto doc = parse();
    EditHistory history(doc.get());
    DOMElement* main = doc->getElementById("main");
    DOMElement* p1 = doc->getElementById("p1");
    EXPECT_EQ(history.replaceWithHTML(p1, ""), nullptr);
    EXPECT_EQ(doc->getElementById("p1"), nullptr);
    EXPECT_EQ(elementChildren(main).size(), 1u);
    EXPECT_TRUE(history.canUndo());
    EXPECT_TRUE(history.undo());
    EXPECT_EQ(main->firstChild(), p1);
}

TEST(DomEditHTML, ReplacesTextNodesAndParsesInTheParentsContext) {
    auto doc = parse();
    EditHistory history(doc.get());
    DOMElement* li = doc->querySelector("li");
    DOMNode* text = li->firstChild();
    DOMNode* first = history.replaceWithHTML(text, "<b>bold</b>");
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(li->innerHTML(), "<b>bold</b>");

    // A row edited as HTML is parsed as table content, not dropped
    auto tdoc = parse("<!DOCTYPE html><html><body><table><tbody><tr id=r><td>1</td></tr></tbody></table></body></html>");
    EditHistory thistory(tdoc.get());
    DOMNode* row = thistory.replaceWithHTML(tdoc->getElementById("r"), "<tr id=n><td>2</td></tr>");
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->nodeName(), "tr");
    EXPECT_EQ(row->parentNode()->nodeName(), "tbody");
}

TEST(DomEditHTML, RefusesHtmlHeadBodyButEditsBodyContent) {
    auto doc = parse();
    EditHistory history(doc.get());
    uint64_t v = domMutationVersion();
    EXPECT_EQ(history.replaceWithHTML(doc->documentElement(), "<html></html>"), nullptr);
    EXPECT_EQ(history.replaceWithHTML(doc->head(), "<head></head>"), nullptr);
    EXPECT_EQ(history.replaceWithHTML(doc->body(), "<body></body>"), nullptr);
    EXPECT_EQ(domMutationVersion(), v);
    EXPECT_FALSE(history.canUndo());
    EXPECT_NE(doc->body(), nullptr);

    // Body is edited by its content
    DOMElement* body = doc->body();
    DOMElement* main = doc->getElementById("main");
    EXPECT_TRUE(history.setInnerHTML(body, "<p id=only>new page</p>"));
    EXPECT_EQ(doc->body(), body);
    EXPECT_EQ(tags(elementChildren(body)), "p");
    EXPECT_GT(domMutationVersion(), v);
    EXPECT_TRUE(history.keepsDetached(main));
    EXPECT_FALSE(history.setInnerHTML(doc->documentElement(), "<body></body>"));

    EXPECT_TRUE(history.undo());
    EXPECT_EQ(doc->getElementById("main"), main);
    EXPECT_EQ(doc->getElementById("only"), nullptr);
}

// ---------------------------------------------------------------------------
// Delete and duplicate
// ---------------------------------------------------------------------------

TEST(DomEditStructure, DeleteKeepsTheNodeAliveAndUndoPutsItBack) {
    auto doc = parse();
    EditHistory history(doc.get());
    DOMElement* main = doc->getElementById("main");
    DOMElement* p1 = doc->getElementById("p1");
    DOMNode* text = p1->firstChild();

    uint64_t v = domMutationVersion();
    EXPECT_TRUE(history.removeNode(p1));
    EXPECT_GT(domMutationVersion(), v);
    EXPECT_EQ(doc->getElementById("p1"), nullptr);
    EXPECT_EQ(p1->parentNode(), nullptr);
    // Still a valid node with its subtree: the layout and the panel may still point at it
    EXPECT_EQ(p1->tagName(), "p");
    EXPECT_EQ(text->nodeValue(), "Hello World");
    EXPECT_TRUE(history.keepsDetached(text));

    EXPECT_TRUE(history.undo());
    EXPECT_EQ(main->firstChild(), p1);
    EXPECT_EQ(p1->parentNode(), main);
    EXPECT_FALSE(history.keepsDetached(p1));
    EXPECT_FALSE(history.canUndo());
}

TEST(DomEditStructure, RefusesProtectedNodes) {
    auto doc = parse();
    EditHistory history(doc.get());
    uint64_t v = domMutationVersion();
    EXPECT_FALSE(history.removeNode(doc->documentElement()));
    EXPECT_FALSE(history.removeNode(doc->head()));
    EXPECT_FALSE(history.removeNode(doc->body()));
    EXPECT_FALSE(history.removeNode(doc.get()));
    EXPECT_FALSE(history.removeNode(nullptr));
    EXPECT_EQ(history.duplicateNode(doc->body()), nullptr);
    EXPECT_EQ(history.duplicateNode(doc->documentElement()), nullptr);
    EXPECT_EQ(domMutationVersion(), v);
    EXPECT_FALSE(history.canUndo());

    EXPECT_TRUE(isProtectedNode(doc->body()));
    EXPECT_FALSE(isProtectedNode(doc->getElementById("main")));

    // A node of another document is not edited through this history
    auto other = parse();
    EXPECT_FALSE(history.removeNode(other->getElementById("main")));
    EXPECT_NE(other->getElementById("main"), nullptr);
}

TEST(DomEditStructure, DuplicateGoesRightAfterTheNode) {
    auto doc = parse();
    EditHistory history(doc.get());
    DOMElement* main = doc->getElementById("main");
    DOMElement* p1 = doc->getElementById("p1");
    DOMNode* second = p1->nextSibling();

    uint64_t v = domMutationVersion();
    DOMNode* clone = history.duplicateNode(p1);
    ASSERT_NE(clone, nullptr);
    EXPECT_GT(domMutationVersion(), v);
    EXPECT_NE(clone, p1);
    EXPECT_EQ(p1->nextSibling(), clone);
    EXPECT_EQ(clone->nextSibling(), second);
    EXPECT_EQ(outerHTMLOf(clone), outerHTMLOf(p1));
    EXPECT_EQ(clone->firstChild()->nodeValue(), "Hello World");   // deep
    EXPECT_EQ(clone->ownerDocument(), doc.get());

    // The last child: the clone becomes the last child
    DOMNode* last = main->lastChild();
    DOMNode* lastClone = history.duplicateNode(last);
    ASSERT_NE(lastClone, nullptr);
    EXPECT_EQ(main->lastChild(), lastClone);
    EXPECT_EQ(lastClone->nodeType(), NodeType::Comment);

    EXPECT_TRUE(history.undo());
    EXPECT_EQ(main->lastChild(), last);
    EXPECT_TRUE(history.undo());
    EXPECT_EQ(p1->nextSibling(), second);
    EXPECT_EQ(elementChildren(main).size(), 2u);
}

// ---------------------------------------------------------------------------
// Undo and node lifetime
// ---------------------------------------------------------------------------

TEST(DomEditUndo, LimitHandsOldSubtreesToTheSink) {
    auto doc = parse();
    std::vector<std::unique_ptr<DOMNode>> sunk;
    EditHistory history(doc.get(), 2);
    history.setDetachedSink([&](std::unique_ptr<DOMNode> n) { sunk.push_back(std::move(n)); });

    auto items = doc->querySelectorAll("li");
    ASSERT_EQ(items.size(), 5u);
    EXPECT_TRUE(history.removeNode(items[0]));
    EXPECT_TRUE(history.removeNode(items[1]));
    EXPECT_TRUE(sunk.empty());
    EXPECT_TRUE(history.removeNode(items[2]));
    // The oldest edit can no longer be undone: its node went to the sink, alive
    EXPECT_EQ(history.undoCount(), 2u);
    ASSERT_EQ(sunk.size(), 1u);
    EXPECT_EQ(sunk[0].get(), items[0]);
    EXPECT_EQ(items[0]->textContent(), "one");

    EXPECT_TRUE(history.undo());
    EXPECT_TRUE(history.undo());
    EXPECT_FALSE(history.undo());
    EXPECT_EQ(doc->querySelectorAll("li").size(), 4u);
}

TEST(DomEditUndo, DetachedListIsFreedOnlyOnRelease) {
    auto doc = parse();
    EditHistory history(doc.get(), 1);
    DOMElement* hovered = doc->querySelector("li");
    interactionState().hovered = hovered;
    EXPECT_TRUE(history.removeNode(hovered->parentNode()));
    EXPECT_TRUE(history.removeNode(doc->getElementById("main")));
    // The first edit fell off: no sink, so it waits in the detached list
    EXPECT_EQ(history.detachedCount(), 1u);
    EXPECT_TRUE(history.keepsDetached(hovered));
    EXPECT_EQ(hovered->textContent(), "one");
    EXPECT_EQ(interactionState().hovered, hovered);

    history.releaseDetached();
    EXPECT_EQ(history.detachedCount(), 0u);
    // Hover state does not point into what was freed
    EXPECT_EQ(interactionState().hovered, nullptr);
}

TEST(DomEditUndo, SkipsEditsWhoseNodesThePageRemoved) {
    auto doc = parse();
    EditHistory history(doc.get());
    DOMElement* p1 = doc->getElementById("p1");
    DOMElement* main = doc->getElementById("main");
    EXPECT_TRUE(history.setAttribute(p1, "title", "x"));
    // The page itself removes the element (and frees it)
    std::unique_ptr<DOMNode> gone = main->removeChild(p1);
    gone.reset();
    EXPECT_FALSE(history.undo());   // the stale pointer is never followed
    EXPECT_FALSE(history.canUndo());
}

TEST(DomEditUndo, ScriptWrappersKeepNodesTheHistoryHandsToTheBindings) {
    auto doc = parse();
    auto script = std::make_unique<ScriptContext>();
    script->initialize(doc.get());
    DOMBindings* bindings = script->domBindings();
    ASSERT_NE(bindings, nullptr);

    EditHistory history(doc.get(), 1);
    history.setDetachedSink([bindings](std::unique_ptr<DOMNode> n) { bindings->adoptDetachedNode(std::move(n)); });

    ASSERT_TRUE(script->evaluate("window.kept = document.getElementById('p1'); 1", "t.js").success);
    DOMElement* p1 = doc->getElementById("p1");
    EXPECT_TRUE(history.removeNode(p1));
    EXPECT_TRUE(history.removeNode(doc->querySelector("ul")));   // drops the first edit to the bindings
    script->releaseDetachedNodes();
    // The script still reaches the deleted node: its wrapper keeps it
    ScriptResult r = script->evaluate("var k = window.kept; k.id + ':' + k.textContent + ':' + (k.parentNode === null)", "t.js");
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.value, "p1:Hello World:true");

    // A subtree no script reaches is freed by the browser's release after relayout
    history.clear();
    EXPECT_EQ(bindings->detachedNodeCount(), 1u);
    script->releaseDetachedNodes();
    EXPECT_EQ(bindings->detachedNodeCount(), 0u);
}

// ---------------------------------------------------------------------------
// Copy CSS Selector
// ---------------------------------------------------------------------------

TEST(DomEditSelector, UniqueSelectors) {
    auto doc = parse();
    DOMElement* p1 = doc->getElementById("p1");
    EXPECT_EQ(uniqueSelector(p1), "#p1");
    EXPECT_EQ(uniqueSelector(doc->body()), "body");
    EXPECT_EQ(uniqueSelector(doc->documentElement()), "html");
    EXPECT_EQ(uniqueSelector(doc->querySelector("a")), "a");
    EXPECT_EQ(uniqueSelector(doc->querySelector(".special")), ".special");
    EXPECT_EQ(uniqueSelector(doc->querySelector("i")), ".md\\:p-4");
    EXPECT_EQ(onlyMatch(doc.get(), ".md\\:p-4"), doc->querySelector("i"));

    // Every element gets a selector that matches it alone, with repeated classes and ids
    std::vector<DOMElement*> all = doc->querySelectorAll("*");
    ASSERT_GT(all.size(), 15u);
    for (DOMElement* e : all) {
        std::string sel = uniqueSelector(e);
        ASSERT_FALSE(sel.empty()) << e->outerHTML();
        EXPECT_EQ(onlyMatch(doc.get(), sel), e) << sel << " for " << e->outerHTML();
    }
}

TEST(DomEditSelector, RepeatedClassesUseNthChildUnderAUniqueAncestor) {
    auto doc = parse();
    auto items = doc->querySelectorAll("li.item");
    ASSERT_EQ(items.size(), 5u);
    // Second list, second item: neither class nor tag nor parent class is unique
    std::string sel = uniqueSelector(items[4]);
    EXPECT_EQ(sel, "body > ul:nth-child(3) > li:nth-child(2)");
    EXPECT_EQ(onlyMatch(doc.get(), sel), items[4]);

    // Repeated ids are not used
    auto spans = doc->querySelectorAll("span");
    ASSERT_EQ(spans.size(), 2u);
    std::string s0 = uniqueSelector(spans[0]);
    std::string s1 = uniqueSelector(spans[1]);
    EXPECT_EQ(s0.find('#'), std::string::npos);
    EXPECT_NE(s0, s1);
    EXPECT_EQ(onlyMatch(doc.get(), s1), spans[1]);
}

TEST(DomEditSelector, OddIdsAndDetachedElements) {
    auto doc = parse("<!DOCTYPE html><html><body><div id=\"a.b\">x</div><div id=\"9lives\">y</div><div>z</div></body></html>");
    auto divs = doc->querySelectorAll("div");
    ASSERT_EQ(divs.size(), 3u);
    EXPECT_EQ(uniqueSelector(divs[0]), "[id=\"a.b\"]");
    EXPECT_EQ(uniqueSelector(divs[1]), "[id=\"9lives\"]");
    for (DOMElement* d : divs) EXPECT_EQ(onlyMatch(doc.get(), uniqueSelector(d)), d);

    DOMElement* body = doc->body();
    std::unique_ptr<DOMNode> detached = body->removeChild(divs[2]);
    EXPECT_EQ(uniqueSelector(static_cast<DOMElement*>(detached.get())), "");
    EXPECT_EQ(uniqueSelector(nullptr), "");
}

// ---------------------------------------------------------------------------
// Copy outerHTML
// ---------------------------------------------------------------------------

TEST(DomEditOuterHTML, ElementsTextAndComments) {
    auto doc = parse("<!DOCTYPE html><html><head><style>a > b { }</style></head><body>"
                     "<p id=x>a &lt; b &amp; c</p><!--note--><br></body></html>");
    DOMElement* p = doc->getElementById("x");
    EXPECT_EQ(outerHTMLOf(p), "<p id=\"x\">a &lt; b &amp; c</p>");
    EXPECT_EQ(outerHTMLOf(p->firstChild()), "a &lt; b &amp; c");
    EXPECT_EQ(outerHTMLOf(p->nextSibling()), "<!--note-->");
    EXPECT_EQ(outerHTMLOf(doc->querySelector("br")), "<br>");
    // Style text is raw text
    EXPECT_EQ(outerHTMLOf(doc->querySelector("style")->firstChild()), "a > b { }");
    EXPECT_EQ(outerHTMLOf(nullptr), "");
}

// ---------------------------------------------------------------------------
// Markup search
// ---------------------------------------------------------------------------

TEST(DomEditSearch, TextIsCaseInsensitiveAndGivesTheTextNode) {
    auto doc = parse();
    auto found = searchMarkup(doc.get(), "hello world");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0]->nodeType(), NodeType::Text);
    EXPECT_EQ(found[0]->parentNode(), doc->getElementById("p1"));

    auto comment = searchMarkup(doc.get(), "A COMMENT");
    ASSERT_EQ(comment.size(), 1u);
    EXPECT_EQ(comment[0]->nodeType(), NodeType::Comment);

    EXPECT_TRUE(searchMarkup(doc.get(), "").empty());
    EXPECT_TRUE(searchMarkup(doc.get(), "   ").empty());
    EXPECT_TRUE(searchMarkup(doc.get(), "zzz-not-there").empty());
}

TEST(DomEditSearch, TagNames) {
    auto doc = parse();
    MarkupSearchOptions tagsOnly;
    tagsOnly.text = tagsOnly.attributes = tagsOnly.selector = false;
    for (const char* q : {"li", "LI", "<li", "<li>"}) {
        auto found = searchMarkup(doc.get(), q, tagsOnly);
        ASSERT_EQ(found.size(), 5u) << q;
        for (auto* n : found) EXPECT_EQ(n->nodeName(), "li") << q;
    }
    // Every kind of match together: the <li>s, the <ul class="list">s and the "link" text, in order
    auto all = searchMarkup(doc.get(), "li");
    ASSERT_EQ(all.size(), 8u);
    EXPECT_EQ(all[0]->nodeName(), "ul");
    EXPECT_EQ(all[1]->nodeName(), "li");
    EXPECT_EQ(all[7]->nodeValue(), "link");
}

TEST(DomEditSearch, AttributeNamesAndValues) {
    auto doc = parse();
    auto byValue = searchMarkup(doc.get(), "example.com");
    ASSERT_EQ(byValue.size(), 1u);
    EXPECT_EQ(byValue[0]->nodeName(), "a");

    auto byName = searchMarkup(doc.get(), "data-x");
    ASSERT_EQ(byName.size(), 1u);
    EXPECT_EQ(byName[0]->nodeName(), "a");

    // "needle" is in an attribute value (any case)
    auto mixed = searchMarkup(doc.get(), "NEEDLE");
    ASSERT_EQ(mixed.size(), 1u);
    EXPECT_EQ(mixed[0]->nodeName(), "a");
}

TEST(DomEditSearch, SelectorsInDocumentOrderWithoutDuplicates) {
    auto doc = parse();
    // ".item" matches as a selector and as a class attribute value: each once, in order
    auto items = searchMarkup(doc.get(), "ul > li.item");
    ASSERT_EQ(items.size(), 5u);
    auto expected = doc->querySelectorAll("li");
    for (size_t i = 0; i < items.size(); i++) EXPECT_EQ(items[i], expected[i]);

    auto special = searchMarkup(doc.get(), "li:nth-child(3)");
    ASSERT_EQ(special.size(), 1u);
    EXPECT_EQ(special[0], doc->querySelector(".special"));

    // A selector and a text match together, in document order
    auto both = searchMarkup(doc.get(), "#main, a");
    ASSERT_EQ(both.size(), 2u);
    EXPECT_EQ(both[0], doc->getElementById("main"));
    EXPECT_EQ(both[1]->nodeName(), "a");

    MarkupSearchOptions limited;
    limited.maxResults = 2;
    EXPECT_EQ(searchMarkup(doc.get(), ".item", limited).size(), 2u);
}

TEST(DomEditSearch, InvalidSelectorsFallBackToText) {
    EXPECT_TRUE(isSearchableSelector("div.box > p#x, a[href], li:nth-child(2) ~ li, :not(.a)"));
    EXPECT_TRUE(isSearchableSelector(".md\\:p-4"));
    EXPECT_FALSE(isSearchableSelector("div["));
    EXPECT_FALSE(isSearchableSelector("a:foo"));
    EXPECT_FALSE(isSearchableSelector("[a^=b]"));
    EXPECT_FALSE(isSearchableSelector("li:nth-child(2n+1)"));
    EXPECT_FALSE(isSearchableSelector("price: $5"));
    EXPECT_FALSE(isSearchableSelector("1abc"));
    EXPECT_FALSE(isSearchableSelector(""));

    auto doc = parse("<!DOCTYPE html><html><body><p>price: $5</p><a>no</a><p>a:foo here</p></body></html>");
    auto price = searchMarkup(doc.get(), "price: $5");
    ASSERT_EQ(price.size(), 1u);
    EXPECT_EQ(price[0]->nodeType(), NodeType::Text);
    // "a:foo" would match every <a> if handed to the matcher; it is text only
    auto foo = searchMarkup(doc.get(), "a:foo");
    ASSERT_EQ(foo.size(), 1u);
    EXPECT_EQ(foo[0]->nodeValue(), "a:foo here");
    EXPECT_TRUE(searchMarkup(doc.get(), "div[").empty());
}

TEST(DomEditSearch, LargePageIsOneWalk) {
    std::string html = "<!DOCTYPE html><html><body>";
    for (int i = 0; i < 3000; i++) {
        html += "<div class=\"row r" + std::to_string(i) + "\"><span>cell " + std::to_string(i) + "</span></div>";
    }
    html += "</body></html>";
    HTMLParser parser;
    auto doc = parser.parse(html);
    auto found = searchMarkup(doc.get(), "cell 2999");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(searchMarkup(doc.get(), ".r17").size(), 1u);
    EXPECT_EQ(searchMarkup(doc.get(), "span").size(), 3000u);
}

// ---------------------------------------------------------------------------
// The panel's functions (process-wide history)
// ---------------------------------------------------------------------------

TEST(DomEditShared, PanelFunctionsEditAndUndoThroughOneHistory) {
    auto doc = parse();
    resetEditHistory(doc.get());
    EXPECT_EQ(undoLastEdit(), nullptr);

    DOMElement* p1 = doc->getElementById("p1");
    DOMElement* main = doc->getElementById("main");
    replaceAttribute(p1, "class", "class=big title=t");
    EXPECT_EQ(p1->getAttribute("class"), "big");
    EXPECT_EQ(p1->getAttribute("title"), "t");
    replaceAttribute(p1, "title", "");
    EXPECT_FALSE(p1->hasAttribute("title"));

    auto* text = static_cast<DOMText*>(p1->firstChild());
    setText(text, "changed");
    EXPECT_EQ(text->data(), "changed");

    DOMNode* clone = duplicateNode(p1);
    ASSERT_NE(clone, nullptr);
    EXPECT_EQ(p1->nextSibling(), clone);
    EXPECT_FALSE(removeNode(doc->body()));
    EXPECT_TRUE(removeNode(clone));
    EXPECT_EQ(outerHtmlOf(clone).rfind("<p ", 0), 0u);   // still alive: kept by the history

    DOMNode* first = replaceWithHtml(p1, "<h3>x</h3><h4>y</h4>");
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(tags(elementChildren(main)), "h3,h4,p");
    EXPECT_EQ(searchMarkup(doc.get(), "h4").size(), 1u);

    // Undo, newest first, each returning the node it brought back or touched
    EXPECT_EQ(undoLastEdit(), p1);       // replace
    EXPECT_EQ(tags(elementChildren(main)), "p,p");
    EXPECT_EQ(undoLastEdit(), clone);    // remove
    EXPECT_EQ(undoLastEdit(), p1);       // duplicate: back to the original
    EXPECT_EQ(elementChildren(main).size(), 2u);
    EXPECT_EQ(undoLastEdit(), text);     // text
    EXPECT_EQ(text->data(), "Hello World");
    EXPECT_EQ(undoLastEdit(), p1);       // attribute removal
    EXPECT_EQ(undoLastEdit(), p1);       // attribute edit
    EXPECT_EQ(p1->getAttribute("class"), "note");
    EXPECT_FALSE(p1->hasAttribute("title"));
    EXPECT_EQ(undoLastEdit(), nullptr);
    releaseDetachedEditNodes();
}

TEST(DomEditShared, EditingAnotherDocumentStartsOver) {
    auto a = parse();
    auto b = parse();
    resetEditHistory(a.get());
    EXPECT_TRUE(removeNode(a->getElementById("p1")));
    EXPECT_EQ(sharedEditHistory().document(), a.get());
    // An edit in another document: the history follows it, the old edit is gone
    EXPECT_TRUE(removeNode(b->getElementById("p1")));
    EXPECT_EQ(sharedEditHistory().document(), b.get());
    EXPECT_EQ(sharedEditHistory().undoCount(), 1u);
    EXPECT_NE(undoLastEdit(), nullptr);
    EXPECT_NE(b->getElementById("p1"), nullptr);
    EXPECT_EQ(a->getElementById("p1"), nullptr);
    // The panel resets on a new document before the old one goes away
    resetEditHistory(nullptr);
    a.reset();
    EXPECT_EQ(undoLastEdit(), nullptr);
    resetEditHistory(nullptr);   // frees what the earlier resets left waiting
    EXPECT_EQ(sharedEditHistory().detachedCount(), 0u);
}
