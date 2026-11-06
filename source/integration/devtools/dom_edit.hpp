// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file dom_edit.hpp
 * @brief What the Inspector's markup view changes and finds: attribute edits,
 *        text edits, "Edit as HTML", delete and duplicate, with undo; plus
 *        "Copy CSS Selector", "Copy outerHTML" and the markup search.
 *
 * Node lifetime. A node the Inspector takes out of the tree is never freed
 * while something may still point at it: layout boxes (until the browser
 * lays the page out again), the script wrapper of the node, or the panel
 * itself. Every edit goes through an EditHistory, which owns the subtrees it
 * removed for as long as the edit can be undone. When an edit can no longer
 * be undone (it fell off the end of the history, undo() put the nodes back
 * and took the inserted ones out, or clear()), its subtrees are handed on:
 *  - to the detached sink when one is set. The browser sets it to
 *    DOMBindings::adoptDetachedNode(), so they live exactly like nodes a
 *    script removed: kept by their wrapper while a script can reach them,
 *    otherwise freed by ScriptContext::releaseDetachedNodes(), which the
 *    browser calls right after its next relayout;
 *  - otherwise to the history's own detached list, freed by
 *    releaseDetached(). Call it only when no layout box can point at them
 *    any more (after a relayout of their page, or once that page is gone).
 *
 * Every edit changes the tree through DOMNode/DOMElement/DOMText methods,
 * each of which bumps domMutationVersion(), so the browser restyles and lays
 * the page out again on its next frame.
 */
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Zepra::WebCore {
class DOMNode;
class DOMElement;
class DOMDocument;
class DOMText;
}

namespace ZepraBrowser::DevTools {

/// One attribute as typed in the markup view
using AttributeText = std::pair<std::string, std::string>;

/// The attributes in what the user typed in an attribute field, in order, as
/// the HTML tokenizer reads a start tag: `a="1" b c='x' d=4` gives a=1, b="",
/// c=x, d=4. Names are lowercased, character references in values decoded
/// (&amp; -> &), repeated names keep the first value, an unterminated quote
/// runs to the end. Empty or blank input gives no attributes.
std::vector<AttributeText> parseAttributeText(const std::string& text);

/// Whether the Inspector refuses structural edits of a node (delete,
/// duplicate, Edit as HTML): the document, its <html>, <head> and <body>,
/// and nodes that are not in a tree. Attributes and text can still be edited.
bool isProtectedNode(const Zepra::WebCore::DOMNode* node);

/**
 * @brief The Inspector's edits of one document, and their undo.
 *
 * Edits made on a node of another document, or a node the history keeps
 * detached, are refused (structural edits) or still made (attributes, text).
 * Pointers returned are to nodes in the tree when returned; like any node,
 * they can be removed later by a script or another edit.
 */
class EditHistory {
public:
    /// Receives a removed subtree the history no longer needs (see the file comment)
    using DetachedSink = std::function<void(std::unique_ptr<Zepra::WebCore::DOMNode>)>;

    /// `limit` is how many edits undo() can take back (at least 1)
    explicit EditHistory(Zepra::WebCore::DOMDocument* document = nullptr, size_t limit = 50);
    ~EditHistory();
    EditHistory(const EditHistory&) = delete;
    EditHistory& operator=(const EditHistory&) = delete;

    Zepra::WebCore::DOMDocument* document() const { return document_; }
    /// Starts over on another document (or the same one after a reload). The
    /// old edits' subtrees go to the detached list, never to the sink (it may
    /// belong to the old page); call releaseDetached() once that page is gone.
    void setDocument(Zepra::WebCore::DOMDocument* document);
    /// Where subtrees go once no edit needs them; empty = the detached list
    void setDetachedSink(DetachedSink sink) { sink_ = std::move(sink); }

    // ---- Attributes --------------------------------------------------------

    /// Sets (or adds) one attribute
    bool setAttribute(Zepra::WebCore::DOMElement* element, const std::string& name, const std::string& value);
    /// Renames an attribute, keeping its value; false when it is missing or the
    /// new name is empty. A rename onto an existing name replaces that attribute.
    bool renameAttribute(Zepra::WebCore::DOMElement* element, const std::string& oldName,
                         const std::string& newName);
    /// Removes one attribute; false when the element does not have it
    bool removeAttribute(Zepra::WebCore::DOMElement* element, const std::string& name);
    /// Firefox's attribute field: attribute `oldName` (empty when adding a new
    /// one) is replaced by whatever was typed, parsed with parseAttributeText:
    /// several attributes, one, or none (which removes it). One undo step.
    bool editAttribute(Zepra::WebCore::DOMElement* element, const std::string& oldName,
                       const std::string& typedText);

    // ---- Text --------------------------------------------------------------

    /// Sets the data of a text or comment node; false for any other node
    bool setText(Zepra::WebCore::DOMNode* node, const std::string& text);

    // ---- Structure ---------------------------------------------------------

    /// Edit as HTML (outerHTML replace): the node is replaced by the nodes
    /// parsed from `html` in the context of its parent. Returns the first new
    /// node, or null when `html` gave no nodes (the node is then removed).
    /// Refused (null, nothing changed) for protected nodes and for a node
    /// whose parent is the document; use setInnerHTML for <body> and <head>.
    Zepra::WebCore::DOMNode* replaceWithHTML(Zepra::WebCore::DOMNode* node, const std::string& html);
    /// Edit as HTML of an element's content (what the panel offers for <body>
    /// and <head>): its children are replaced by the nodes parsed from `html`.
    /// Refused for <html> and for elements not in a tree.
    bool setInnerHTML(Zepra::WebCore::DOMElement* element, const std::string& html);
    /// Deletes a node (refused for protected nodes)
    bool removeNode(Zepra::WebCore::DOMNode* node);
    /// Duplicate Node: a deep clone inserted right after the node; returns the
    /// clone (null when refused: protected, or its parent is the document)
    Zepra::WebCore::DOMNode* duplicateNode(Zepra::WebCore::DOMNode* node);

    // ---- Undo --------------------------------------------------------------

    bool canUndo() const { return !records_.empty(); }
    size_t undoCount() const { return records_.size(); }
    /// Takes back the last edit and returns the node it brought back or
    /// touched: the element of an attribute edit, the text node, the first
    /// node put back, or for an undone duplicate the original. Null when there
    /// is no edit, or when the page changed the tree since so that it cannot be
    /// put back (that edit is then dropped).
    Zepra::WebCore::DOMNode* undo();
    /// Forgets every edit (their subtrees are handed on as described above)
    void clear();

    // ---- Detached nodes ----------------------------------------------------

    /// Whether a node is inside a subtree the history keeps out of the tree
    /// (an undoable removal, or waiting in the detached list)
    bool keepsDetached(const Zepra::WebCore::DOMNode* node) const;
    /// Frees the detached list (see the file comment for when it is safe)
    void releaseDetached();
    size_t detachedCount() const { return detached_.size(); }

private:
    struct Record;

    Zepra::WebCore::DOMDocument* document_ = nullptr;
    size_t limit_ = 50;
    DetachedSink sink_;
    std::vector<std::unique_ptr<Record>> records_;   ///< oldest first
    std::vector<std::unique_ptr<Zepra::WebCore::DOMNode>> detached_;

    void push(std::unique_ptr<Record> record);
    /// Hands a subtree on (sink or detached list)
    void discard(std::unique_ptr<Zepra::WebCore::DOMNode> root, bool useSink = true);
    void discardRecord(Record& record, bool useSink = true);
    bool attributeEdit(Zepra::WebCore::DOMElement* element,
                       const std::function<bool(Zepra::WebCore::DOMElement*)>& change);
    /// Whether a node is alive: in the document's tree or kept by the history
    bool isLive(const Zepra::WebCore::DOMNode* node) const;
};

// ---- The panel's edits -----------------------------------------------------
//
// The markup view edits through one process-wide EditHistory. It follows the
// document of the node being edited: an edit in another document starts it
// over (resetEditHistory with no sink). The panel must still call
// resetEditHistory(page.document, ...) as soon as it sees a new document,
// before undoLastEdit() or any edit: until then the history may refer to a
// document that was freed.

/// The process-wide history the functions below edit through
EditHistory& sharedEditHistory();
/// Starts the shared history over on `document`. Frees the subtrees an earlier
/// reset (or releaseDetachedEditNodes) left waiting, and leaves the old
/// document's undoable subtrees waiting in their place: they are freed by the
/// next reset, so a layout tree built before this one never sees them freed.
/// `sink` (the new page's DOMBindings::adoptDetachedNode) receives subtrees
/// no edit needs any more; without one they wait for the next reset or
/// releaseDetachedEditNodes().
void resetEditHistory(Zepra::WebCore::DOMDocument* document, EditHistory::DetachedSink sink = {});
/// Frees the subtrees waiting in the shared history; call only after a relayout
void releaseDetachedEditNodes();

/// EditHistory::editAttribute: `oldName` ("" = add) replaced by what was typed
void replaceAttribute(Zepra::WebCore::DOMElement* el, const std::string& oldName, const std::string& typedText);
/// EditHistory::setText
void setText(Zepra::WebCore::DOMText* text, const std::string& data);
/// EditHistory::replaceWithHTML: the first new node, or null (removed, or refused)
Zepra::WebCore::DOMNode* replaceWithHtml(Zepra::WebCore::DOMNode* node, const std::string& html);
/// EditHistory::removeNode: false when refused (html, head, body, not in a tree)
bool removeNode(Zepra::WebCore::DOMNode* node);
/// EditHistory::duplicateNode: the clone, or null when refused
Zepra::WebCore::DOMNode* duplicateNode(Zepra::WebCore::DOMNode* node);
/// Undoes the last edit that can still be undone (stale ones are skipped);
/// the node it brought back or touched, or null when there is nothing to undo
Zepra::WebCore::DOMNode* undoLastEdit();

// ---- Copy ------------------------------------------------------------------

/// Copy CSS Selector: a short selector that matches only this element in its
/// document, as Firefox makes it: its id when unique, else its tag or classes
/// when unique, else the parent's selector + " > tag:nth-child(i)". Checked
/// with the engine's selector matcher. Empty for an element not in a document.
std::string uniqueSelector(Zepra::WebCore::DOMElement* element);

/// Copy outerHTML: an element's markup, a text node's escaped text (as is in
/// <script> and <style>), a comment's <!--data-->, the children of a document
/// or fragment. Attribute order follows the DOM's attribute map.
std::string outerHTMLOf(const Zepra::WebCore::DOMNode* node);
/// outerHTMLOf, under the panel's name
std::string outerHtmlOf(Zepra::WebCore::DOMNode* node);

// ---- Search ----------------------------------------------------------------

struct MarkupSearchOptions {
    bool text = true;         ///< text and comment nodes whose data contains the query
    bool tags = true;         ///< elements whose tag name is the query ("div", "<div", "<div>")
    bool attributes = true;   ///< elements with an attribute whose name or value contains the query
    bool selector = true;     ///< elements the query matches as a CSS selector, when it is one
    size_t maxResults = 0;    ///< 0 = no limit
};

/// The markup search: nodes under `root` (and `root` itself) in document
/// order, each once, that match the query (trimmed, case-insensitive for
/// text, tags and attributes). Text matches give the text or comment node.
/// One walk of the tree, plus one selector match of it when the query is a
/// selector the engine's matcher understands (see isSearchableSelector).
std::vector<Zepra::WebCore::DOMNode*> searchMarkup(Zepra::WebCore::DOMNode* root, const std::string& query,
                                                   const MarkupSearchOptions& options = {});

/// Whether a query is a selector list the engine's matcher handles exactly:
/// type, universal, #id, .class, [attr], [attr=value], the pseudo-classes it
/// implements (:nth-child only with a number, odd or even), and the
/// descendant, >, + and ~ combinators. Anything else ("a:foo", "div[",
/// "[a^=b]", "li:nth-child(2n+1)") is searched as text only.
bool isSearchableSelector(const std::string& query);

/// searchMarkup over a whole document with every kind of match
std::vector<Zepra::WebCore::DOMNode*> searchMarkup(Zepra::WebCore::DOMDocument* doc, const std::string& query);

} // namespace ZepraBrowser::DevTools
