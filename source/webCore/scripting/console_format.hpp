// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file console_format.hpp
 * @brief Script values as the DevTools Console shows them: a one-line
 *        preview per value, as Firefox prints it, plus the children a
 *        row reveals when expanded.
 *
 * A preview is a snapshot taken when the value is logged (the console
 * holds no reference into the script heap), materialized to a limited
 * depth and width, with cycles marked. The same formatting gives the
 * text form for the terminal.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Zepra::Runtime { class Value; class Object; }

namespace Zepra::WebCore {

class DOMNode;

/// One value (or one property of one) as the console shows it
struct ValuePreview {
    enum class Kind : uint8_t {
        Undefined, Null, Boolean, Number, BigInt, String, Symbol,
        Function, Array, Object, Error, Node, Map, Set, Entry, Date, RegExp, Promise,
        Getter,     ///< an accessor property: "Getter", never invoked
        More        ///< "… N more" when the children were cut
    };
    Kind kind = Kind::Undefined;
    std::string key;          ///< property name, index or entry index when a child; "" at the root
    std::string text;         ///< the preview line: 'Array(3) [ 1, 2, 3 ]', '"x"', '<div id="a">'
    bool expandable = false;  ///< has children (materialized, or cut off by the depth limit)
    bool cyclic = false;      ///< the same object as one of its ancestors
    bool expanded = false;    ///< panel state: the children are shown
    std::vector<ValuePreview> children;
};

struct FormatOptions {
    int maxDepth = 3;             ///< levels of children materialized below the root
    size_t maxChildren = 100;     ///< children per object before "… N more"
    size_t maxPreviewItems = 10;  ///< items inside the "{ ... }" / "[ ... ]" preview text
    size_t maxStringLength = 500; ///< longer strings are cut with "…"
    size_t maxNodes = 4000;       ///< total previews per call; beyond it rows stay expandable but empty
    bool quoteStrings = true;     ///< a top-level string as "x" (false: as console.log prints it)
};

/// Formats a value as the console shows it. DOM node wrappers are recognised
/// through DOMBindings; getters are shown, not run.
ValuePreview formatValue(const Zepra::Runtime::Value& value, const FormatOptions& options = {});

/// Formats console.log-style arguments: the first argument's %s/%d/%i/%f/%o/%O/%c
/// substitutions are applied, strings are not quoted
std::vector<ValuePreview> formatArguments(const std::vector<Zepra::Runtime::Value>& args,
                                          const FormatOptions& options = {});

/// The terminal form of a list of previews: their texts joined by spaces
std::string previewText(const std::vector<ValuePreview>& values);

/// One console entry, as the browser keeps it per tab and the panel draws it
struct ConsoleMessage {
    /// log, info, debug, warn, error, trace, input, result, group, clear
    std::string level;
    std::string text;                  ///< the terminal form
    std::vector<ValuePreview> values;  ///< structured arguments (may be empty: text only)
    int depth = 0;                     ///< console.group nesting
    std::string source;                ///< "file:line:column" when the engine knows it
    uint64_t id = 0;                   ///< numbered by the panel once drawn (0 until then)
};

} // namespace Zepra::WebCore
