// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file console_format.cpp
 * @brief Script values as the DevTools Console shows them (see the header)
 */
#include "scripting/console_format.hpp"
#include "scripting/dom_bindings.hpp"
#include "browser/dom.hpp"

#include "builtins/date.hpp"
#include "builtins/map.hpp"
#include "builtins/regexp.hpp"
#include "builtins/set.hpp"
#include "runtime/async/promise.hpp"
#include "runtime/builtins_api/bigint_object.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/map.hpp"
#include "runtime/objects/number_conversion.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/set.hpp"
#include "runtime/objects/symbol.hpp"
#include "runtime/objects/value.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace Zepra::WebCore {

using namespace Zepra::Runtime;
using Kind = ValuePreview::Kind;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    return s;
}

// The built-ins make Builtins::MapObject / SetObject; host code may make Runtime::Map / Set
std::vector<std::pair<Value, Value>> mapEntries(const Object* o) {
    if (const auto* m = dynamic_cast<const Zepra::Builtins::MapObject*>(o)) return m->entries();
    if (const auto* m = dynamic_cast<const Map*>(o)) return m->entries();
    return {};
}

std::vector<Value> setValues(const Object* o) {
    if (const auto* s = dynamic_cast<const Zepra::Builtins::SetObject*>(o)) return s->values();
    if (const auto* s = dynamic_cast<const Set*>(o)) return s->values();
    return {};
}

std::string quoted(const std::string& s, size_t maxLength) {
    std::string out = "\"";
    size_t n = 0;
    for (unsigned char c : s) {
        if (n++ >= maxLength) { out += "…"; break; }
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += (char)c; break;
        }
    }
    return out + "\"";
}

std::string cut(const std::string& s, size_t maxLength) {
    if (s.size() <= maxLength) return s;
    size_t end = maxLength;
    while (end > 0 && ((unsigned char)s[end] & 0xC0) == 0x80) end--;   // keep UTF-8 whole
    return s.substr(0, end) + "…";
}

std::string numberText(double d) {
    if (d == 0 && std::signbit(d)) return "-0";
    return NumberConversion::toString(d);
}

/// The name a class instance is shown with: its constructor's name, "Object" otherwise
std::string className(const Object* obj) {
    for (const Object* proto = obj->prototype(); proto; proto = proto->prototype()) {
        auto desc = proto->getOwnPropertyDescriptor("constructor");
        if (!desc) continue;
        if (desc->value.isObject() && desc->value.asObject()->isFunction()) {
            const std::string& name = static_cast<const Function*>(desc->value.asObject())->name();
            return name.empty() ? "Object" : name;
        }
        break;
    }
    return "Object";
}

std::string nodeSelector(const DOMElement* el) {
    std::string s = lower(el->tagName());
    std::string id = el->getAttribute("id");
    if (!id.empty()) s += "#" + id;
    std::string cls = el->getAttribute("class");
    size_t pos = 0;
    while (pos < cls.size()) {
        while (pos < cls.size() && std::isspace((unsigned char)cls[pos])) pos++;
        size_t start = pos;
        while (pos < cls.size() && !std::isspace((unsigned char)cls[pos])) pos++;
        if (pos > start) s += "." + cls.substr(start, pos - start);
    }
    return s;
}

/// id and class first, the rest by name
std::vector<std::pair<std::string, std::string>> orderedAttributes(const DOMElement* el) {
    std::vector<std::pair<std::string, std::string>> attrs(el->attributes().begin(), el->attributes().end());
    std::sort(attrs.begin(), attrs.end(), [](const auto& a, const auto& b) {
        auto rank = [](const std::string& n) { return n == "id" ? 0 : n == "class" ? 1 : 2; };
        if (rank(a.first) != rank(b.first)) return rank(a.first) < rank(b.first);
        return a.first < b.first;
    });
    return attrs;
}

std::string collapse(const std::string& in) {
    std::string out;
    bool space = true;
    for (unsigned char c : in) {
        if (std::isspace(c)) { if (!space) out += ' '; space = true; }
        else { out += (char)c; space = false; }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

class Formatter {
public:
    explicit Formatter(const FormatOptions& o) : opt_(o) {}

    ValuePreview format(const Value& v, bool quoteStrings) {
        ValuePreview p;
        fill(p, v, 0, quoteStrings);
        return p;
    }

private:
    const FormatOptions& opt_;
    std::vector<const Object*> path_;   // objects being expanded, root first
    size_t made_ = 0;

    bool onPath(const Object* o) const { return std::find(path_.begin(), path_.end(), o) != path_.end(); }

    // ---- preview text ------------------------------------------------------

    /// The short form of a value inside another's preview: {…}, (2) […], "x"
    std::string nested(const Value& v) {
        if (!v.isObject()) return primitiveText(v, true);
        const Object* o = v.asObject();
        if (DOMNode* node = DOMBindings::nodeOf(o)) return nestedNodeText(node);
        switch (o->objectType()) {
            case ObjectType::Array: {
                size_t n = o->length();
                return n == 0 ? "[]" : "(" + std::to_string(n) + ") […]";
            }
            case ObjectType::Function: case ObjectType::BoundFunction: return functionText(o);
            case ObjectType::Error: return errorHeader(o);
            case ObjectType::Map: return "Map(" + std::to_string(mapEntries(o).size()) + ")";
            case ObjectType::Set: return "Set(" + std::to_string(setValues(o).size()) + ")";
            case ObjectType::RegExp: return regexpText(o);
            case ObjectType::Promise: return "Promise";
            case ObjectType::BigInt: return v.toString();
            default:
                if (dynamic_cast<const Zepra::Builtins::DateObject*>(o)) return dateText(o);
                return className(o) == "Object" ? "{…}" : className(o);
        }
    }

    std::string primitiveText(const Value& v, bool quote) {
        if (v.isUndefined()) return "undefined";
        if (v.isNull()) return "null";
        if (v.isBoolean()) return v.asBoolean() ? "true" : "false";
        if (v.isNumber()) return numberText(v.asNumber());
        if (v.isString()) return quote ? quoted(v.asString()->value(), opt_.maxStringLength)
                                       : cut(v.asString()->value(), opt_.maxStringLength);
        if (v.isSymbol()) {
            auto d = symbolDescription(v.asSymbol());
            return "Symbol(" + (d ? *d : std::string()) + ")";
        }
        if (v.isBigInt()) return v.asBigInt()->toString() + "n";
        return v.toString();
    }

    std::string functionText(const Object* o) {
        const auto* fn = static_cast<const Function*>(o);
        if (fn->isBound()) return "function bound " + (fn->boundTarget() ? fn->boundTarget()->name() : std::string()) + "()";
        std::string head = fn->isAsync() ? "async function" : "function";
        if (fn->isGenerator()) head += "*";
        return head + " " + fn->name() + "()";
    }

    std::string errorHeader(const Object* o) {
        Value name = o->get("name"), message = o->get("message");
        std::string head = name.isString() ? name.asString()->value() : "Error";
        if (message.isString() && !message.asString()->value().empty()) head += ": " + message.asString()->value();
        return head;
    }

    std::string dateText(const Object* o) {
        const auto* d = dynamic_cast<const Zepra::Builtins::DateObject*>(o);
        if (!d) return "Date";
        if (std::isnan(d->getTime())) return "Date Invalid Date";
        return "Date " + d->toISOString();
    }

    std::string regexpText(const Object* o) {
        const auto* r = dynamic_cast<const Zepra::Builtins::RegExpObject*>(o);
        return r ? "/" + r->pattern() + "/" + r->flags() : "RegExp";
    }

    std::string nestedNodeText(const DOMNode* node) {
        switch (node->nodeType()) {
            case NodeType::Element: return "<" + nodeSelector(static_cast<const DOMElement*>(node)) + ">";
            case NodeType::Text: return "#text";
            case NodeType::Comment: return "<!-- -->";
            case NodeType::Document: return "HTMLDocument";
            default: return "DocumentFragment";
        }
    }

    std::string nodeText(const DOMNode* node) {
        switch (node->nodeType()) {
            case NodeType::Element: {
                const auto* el = static_cast<const DOMElement*>(node);
                std::string s = "<" + lower(el->tagName());
                for (const auto& [name, value] : orderedAttributes(el)) s += " " + name + "=" + quoted(value, 80);
                return s + ">";
            }
            case NodeType::Text: return "#text " + quoted(static_cast<const DOMText*>(node)->data(), 80);
            case NodeType::Comment: return "<!-- " + cut(collapse(static_cast<const DOMComment*>(node)->data()), 80) + " -->";
            case NodeType::Document: return "HTMLDocument";
            default: return "DocumentFragment";
        }
    }

    /// "a: 1, b: \"x\"" over the first maxPreviewItems own properties
    std::string propertiesPreview(const Object* o, const std::vector<std::string>& names) {
        std::string s;
        size_t shown = 0;
        for (const std::string& name : names) {
            if (shown == opt_.maxPreviewItems) { s += ", …"; break; }
            auto desc = o->getOwnPropertyDescriptor(name);
            if (!desc) continue;
            if (shown++) s += ", ";
            s += name + ": " + (desc->isAccessorDescriptor() ? accessorText(*desc) : nested(desc->value));
        }
        return s;
    }

    static std::string accessorText(const PropertyDescriptor& d) {
        bool g = !d.getter.isUndefined(), st = !d.setter.isUndefined();
        return g && st ? "Getter & Setter" : g ? "Getter" : "Setter";
    }

    // ---- the tree ----------------------------------------------------------

    ValuePreview& child(ValuePreview& parent, const std::string& key) {
        parent.children.emplace_back();
        parent.children.back().key = key;
        made_++;
        return parent.children.back();
    }

    void property(ValuePreview& parent, const std::string& key, const Object* o, int depth) {
        auto desc = o->getOwnPropertyDescriptor(key);
        if (!desc) return;
        ValuePreview& c = child(parent, key);
        if (desc->isAccessorDescriptor()) {
            c.kind = Kind::Getter;
            c.text = accessorText(*desc);
        } else {
            fill(c, desc->value, depth, true);
        }
    }

    void fill(ValuePreview& p, const Value& v, int depth, bool quoteStrings) {
        if (!v.isObject() || v.isBigInt()) {
            p.kind = v.isUndefined() ? Kind::Undefined : v.isNull() ? Kind::Null : v.isBoolean() ? Kind::Boolean
                   : v.isNumber() ? Kind::Number : v.isString() ? Kind::String : v.isSymbol() ? Kind::Symbol
                   : v.isBigInt() ? Kind::BigInt : Kind::Undefined;
            p.text = primitiveText(v, quoteStrings);
            return;
        }
        const Object* o = v.asObject();
        p.kind = kindOf(o);
        p.expandable = true;
        if (onPath(o)) {
            p.cyclic = true;
            p.expandable = false;
            p.text = nested(v) + " (circular)";
            return;
        }
        path_.push_back(o);
        // Children only while there is room: deeper rows keep their preview
        // and stay marked expandable, but hold nothing
        bool materialize = depth < opt_.maxDepth && made_ < opt_.maxNodes;
        switch (p.kind) {
            case Kind::Node: fillNode(p, DOMBindings::nodeOf(o), depth, materialize); break;
            case Kind::Array: fillArray(p, o, depth, materialize); break;
            case Kind::Function: fillFunction(p, o, depth, materialize); break;
            case Kind::Error: fillError(p, o, depth, materialize); break;
            case Kind::Map: fillMap(p, o, depth, materialize); break;
            case Kind::Set: fillSet(p, o, depth, materialize); break;
            case Kind::Date: p.text = dateText(o); p.expandable = false; break;
            case Kind::RegExp: p.text = regexpText(o); p.expandable = false; break;
            case Kind::Promise: fillPromise(p, o, depth, materialize); break;
            default: fillObject(p, o, depth, materialize); break;
        }
        path_.pop_back();
    }

    Kind kindOf(const Object* o) {
        if (DOMBindings::nodeOf(o)) return Kind::Node;
        if (dynamic_cast<const Zepra::Builtins::DateObject*>(o)) return Kind::Date;   // an Ordinary object
        switch (o->objectType()) {
            case ObjectType::Array: return Kind::Array;
            case ObjectType::Function: case ObjectType::BoundFunction: return Kind::Function;
            case ObjectType::Error: return Kind::Error;
            case ObjectType::Map: return Kind::Map;
            case ObjectType::Set: return Kind::Set;
            case ObjectType::Date: return Kind::Date;
            case ObjectType::RegExp: return Kind::RegExp;
            case ObjectType::Promise: return Kind::Promise;
            default: return Kind::Object;
        }
    }

    /// Own named properties (not indices)
    static std::vector<std::string> namedProperties(const Object* o) {
        std::vector<std::string> names;
        for (std::string& n : o->getOwnPropertyNames()) {
            if (!n.empty() && std::isdigit((unsigned char)n[0])) continue;
            names.push_back(std::move(n));
        }
        return names;
    }

    void properties(ValuePreview& p, const Object* o, const std::vector<std::string>& names, int depth) {
        size_t n = 0;
        for (const std::string& name : names) {
            if (n++ == opt_.maxChildren) {
                ValuePreview& more = child(p, "");
                more.kind = Kind::More;
                more.text = "… " + std::to_string(names.size() - opt_.maxChildren) + " more";
                break;
            }
            property(p, name, o, depth + 1);
        }
    }

    void fillObject(ValuePreview& p, const Object* o, int depth, bool materialize) {
        std::vector<std::string> names = o->getOwnPropertyNames();
        std::string name = className(o);
        p.text = name + " { " + propertiesPreview(o, names) + " }";
        if (names.empty()) p.text = name + " {}";
        if (materialize) properties(p, o, names, depth);
    }

    void fillArray(ValuePreview& p, const Object* o, int depth, bool materialize) {
        size_t n = o->length();
        std::string items;
        size_t shown = 0, holes = 0;
        auto flushHoles = [&]() {
            if (!holes) return;
            if (shown++) items += ", ";
            items += "<" + std::to_string(holes) + " empty slot" + (holes > 1 ? "s" : "") + ">";
            holes = 0;
        };
        for (size_t i = 0; i < n; i++) {
            if (shown >= opt_.maxPreviewItems) { items += ", …"; holes = 0; break; }
            if (!o->has((uint32_t)i)) { holes++; continue; }
            flushHoles();
            if (shown++) items += ", ";
            items += nested(o->get((uint32_t)i));
        }
        flushHoles();
        p.text = n == 0 ? "Array []" : "Array(" + std::to_string(n) + ") [ " + items + " ]";
        if (!materialize) return;
        for (size_t i = 0; i < n; i++) {
            if (i == opt_.maxChildren) {
                ValuePreview& more = child(p, "");
                more.kind = Kind::More;
                more.text = "… " + std::to_string(n - i) + " more";
                break;
            }
            if (!o->has((uint32_t)i)) continue;
            fill(child(p, std::to_string(i)), o->get((uint32_t)i), depth + 1, true);
        }
        properties(p, o, namedProperties(o), depth);
        ValuePreview& len = child(p, "length");
        len.kind = Kind::Number;
        len.text = std::to_string(n);
    }

    void fillFunction(ValuePreview& p, const Object* o, int depth, bool materialize) {
        p.text = functionText(o);
        if (!materialize) return;
        const auto* fn = static_cast<const Function*>(o);
        ValuePreview& name = child(p, "name");
        name.kind = Kind::String;
        name.text = quoted(fn->name(), opt_.maxStringLength);
        ValuePreview& len = child(p, "length");
        len.kind = Kind::Number;
        len.text = std::to_string(fn->arity());
        std::vector<std::string> names;
        for (std::string& n : namedProperties(o)) if (n != "name" && n != "length" && n != "prototype") names.push_back(std::move(n));
        properties(p, o, names, depth);
    }

    void fillError(ValuePreview& p, const Object* o, int depth, bool materialize) {
        p.text = errorHeader(o);
        Value stack = o->get("stack");
        std::string frames;
        if (stack.isString()) {
            // The stack starts with the header line; the frames follow
            const std::string& s = stack.asString()->value();
            size_t nl = s.find('\n');
            if (nl != std::string::npos) frames = s.substr(nl + 1);
            while (!frames.empty() && (frames.back() == '\n' || frames.back() == ' ')) frames.pop_back();
        }
        if (!frames.empty()) p.text += "\n" + frames;
        if (!materialize) return;
        std::vector<std::string> names;
        for (std::string& n : namedProperties(o)) if (n != "stack") names.push_back(std::move(n));
        if (std::find(names.begin(), names.end(), "message") == names.end() && o->get("message").isString()) {
            ValuePreview& m = child(p, "message");
            m.kind = Kind::String;
            m.text = quoted(o->get("message").asString()->value(), opt_.maxStringLength);
        }
        properties(p, o, names, depth);
        if (stack.isString()) {
            ValuePreview& st = child(p, "stack");
            st.kind = Kind::String;
            st.text = quoted(stack.asString()->value(), opt_.maxStringLength);
        }
    }

    void fillMap(ValuePreview& p, const Object* o, int depth, bool materialize) {
        auto entries = mapEntries(o);
        std::string items;
        for (size_t i = 0; i < entries.size(); i++) {
            if (i == opt_.maxPreviewItems) { items += ", …"; break; }
            if (i) items += ", ";
            items += nested(entries[i].first) + " → " + nested(entries[i].second);
        }
        p.text = "Map(" + std::to_string(entries.size()) + ")" + (entries.empty() ? "" : " { " + items + " }");
        if (!materialize) return;
        for (size_t i = 0; i < entries.size(); i++) {
            if (i == opt_.maxChildren) {
                ValuePreview& more = child(p, "");
                more.kind = Kind::More;
                more.text = "… " + std::to_string(entries.size() - i) + " more";
                break;
            }
            ValuePreview& e = child(p, std::to_string(i));
            e.kind = Kind::Entry;
            e.text = nested(entries[i].first) + " → " + nested(entries[i].second);
            e.expandable = true;
            if (depth + 1 < opt_.maxDepth) {
                fill(child(e, "<key>"), entries[i].first, depth + 2, true);
                fill(child(e, "<value>"), entries[i].second, depth + 2, true);
            }
        }
        ValuePreview& size = child(p, "size");
        size.kind = Kind::Number;
        size.text = std::to_string(entries.size());
    }

    void fillSet(ValuePreview& p, const Object* o, int depth, bool materialize) {
        auto values = setValues(o);
        std::string items;
        for (size_t i = 0; i < values.size(); i++) {
            if (i == opt_.maxPreviewItems) { items += ", …"; break; }
            if (i) items += ", ";
            items += nested(values[i]);
        }
        p.text = "Set(" + std::to_string(values.size()) + ")" + (values.empty() ? "" : " [ " + items + " ]");
        if (!materialize) return;
        for (size_t i = 0; i < values.size(); i++) {
            if (i == opt_.maxChildren) {
                ValuePreview& more = child(p, "");
                more.kind = Kind::More;
                more.text = "… " + std::to_string(values.size() - i) + " more";
                break;
            }
            fill(child(p, std::to_string(i)), values[i], depth + 1, true);
        }
        ValuePreview& size = child(p, "size");
        size.kind = Kind::Number;
        size.text = std::to_string(values.size());
    }

    void fillPromise(ValuePreview& p, const Object* o, int depth, bool materialize) {
        const auto* pr = static_cast<const Promise*>(o);
        const char* state = pr->state() == PromiseState::Pending ? "pending"
                          : pr->state() == PromiseState::Fulfilled ? "fulfilled" : "rejected";
        p.text = std::string("Promise { <state>: \"") + state + "\"";
        if (pr->state() == PromiseState::Fulfilled) p.text += ", <value>: " + nested(pr->result());
        else if (pr->state() == PromiseState::Rejected) p.text += ", <reason>: " + nested(pr->result());
        p.text += " }";
        if (!materialize) return;
        ValuePreview& st = child(p, "<state>");
        st.kind = Kind::String;
        st.text = std::string("\"") + state + "\"";
        if (pr->state() == PromiseState::Fulfilled) fill(child(p, "<value>"), pr->result(), depth + 1, true);
        else if (pr->state() == PromiseState::Rejected) fill(child(p, "<reason>"), pr->result(), depth + 1, true);
    }

    void fillNode(ValuePreview& p, DOMNode* node, int depth, bool materialize) {
        if (!node) { p.text = "<destroyed node>"; p.expandable = false; return; }
        p.text = nodeText(node);
        if (node->nodeType() == NodeType::Text || node->nodeType() == NodeType::Comment) {
            p.expandable = false;
            return;
        }
        if (!materialize) return;
        auto str = [&](const std::string& key, const std::string& value) {
            ValuePreview& c = child(p, key);
            c.kind = Kind::String;
            c.text = quoted(value, opt_.maxStringLength);
        };
        if (node->nodeType() == NodeType::Element) {
            const auto* el = static_cast<const DOMElement*>(node);
            str("tagName", upper(el->tagName()));
            str("id", el->getAttribute("id"));
            str("className", el->getAttribute("class"));
            ValuePreview& attrs = child(p, "attributes");
            attrs.kind = Kind::Object;
            auto ordered = orderedAttributes(el);
            attrs.text = "NamedNodeMap(" + std::to_string(ordered.size()) + ")";
            attrs.expandable = !ordered.empty();
            for (const auto& [name, value] : ordered) {
                ValuePreview& a = child(attrs, name);
                a.kind = Kind::String;
                a.text = quoted(value, opt_.maxStringLength);
            }
        }
        ValuePreview& kids = child(p, "childNodes");
        kids.kind = Kind::Array;
        const auto& children = node->childNodes();
        kids.text = "NodeList(" + std::to_string(children.size()) + ")";
        kids.expandable = !children.empty();
        if (depth + 1 < opt_.maxDepth) {
            for (size_t i = 0; i < children.size(); i++) {
                if (i == opt_.maxChildren) {
                    ValuePreview& more = child(kids, "");
                    more.kind = Kind::More;
                    more.text = "… " + std::to_string(children.size() - i) + " more";
                    break;
                }
                ValuePreview& c = child(kids, std::to_string(i));
                c.kind = Kind::Node;
                c.text = nodeText(children[i].get());
                c.expandable = false;
            }
        }
        if (node->nodeType() == NodeType::Element)
            str("textContent", cut(collapse(static_cast<const DOMElement*>(node)->textContent()), 200));
    }
};

// console.log("%s is %d", a, b): the substitutions of the first argument
bool applyFormat(const std::vector<Value>& args, const FormatOptions& opt, std::vector<ValuePreview>& out) {
    if (args.empty() || !args[0].isString()) return false;
    const std::string& fmt = args[0].asString()->value();
    if (fmt.find('%') == std::string::npos) return false;
    Formatter f(opt);
    std::string text;
    size_t next = 1;
    bool used = false;
    for (size_t i = 0; i < fmt.size(); i++) {
        char c = fmt[i];
        if (c != '%' || i + 1 >= fmt.size()) { text += c; continue; }
        char spec = fmt[i + 1];
        if (spec == '%') { text += '%'; i++; continue; }
        if (spec != 's' && spec != 'd' && spec != 'i' && spec != 'f' && spec != 'o' && spec != 'O' && spec != 'c') {
            text += c;
            continue;
        }
        i++;
        if (spec == 'c') { if (next < args.size()) next++; used = true; continue; }   // styles: ignored
        if (next >= args.size()) { text += c; text += spec; continue; }
        used = true;
        const Value& v = args[next++];
        switch (spec) {
            case 's': text += v.isString() ? v.asString()->value() : f.format(v, false).text; break;
            case 'd': case 'i': {
                double d = v.isObject() ? std::nan("") : v.toNumber();
                text += std::isnan(d) ? "NaN" : numberText(std::trunc(d));
                break;
            }
            case 'f': text += numberText(v.isObject() ? std::nan("") : v.toNumber()); break;
            default: text += f.format(v, true).text; break;
        }
    }
    if (!used) return false;
    ValuePreview first;
    first.kind = Kind::String;
    first.text = text;
    out.push_back(std::move(first));
    for (; next < args.size(); next++) out.push_back(f.format(args[next], false));
    return true;
}

} // namespace

ValuePreview formatValue(const Value& value, const FormatOptions& options) {
    Formatter f(options);
    return f.format(value, options.quoteStrings);
}

std::vector<ValuePreview> formatArguments(const std::vector<Value>& args, const FormatOptions& options) {
    std::vector<ValuePreview> out;
    if (applyFormat(args, options, out)) return out;
    Formatter f(options);
    out.reserve(args.size());
    for (const Value& v : args) out.push_back(f.format(v, false));
    return out;
}

std::string previewText(const std::vector<ValuePreview>& values) {
    std::string s;
    for (size_t i = 0; i < values.size(); i++) {
        if (i) s += ' ';
        s += values[i].text;
    }
    return s;
}

} // namespace Zepra::WebCore
