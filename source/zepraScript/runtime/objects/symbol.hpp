#pragma once

// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — symbol.hpp — Symbol registry with all ES2025 well-known symbols

#include "config.hpp"
#include "value.hpp"
#include <string>
#include <optional>
#include <unordered_map>
#include <atomic>
#include <mutex>

namespace Zepra::Runtime {

class SymbolRegistry {
public:
    static SymbolRegistry& instance();

    Value createSymbol(const std::string& description = "");

    // Global registry (Symbol.for / Symbol.keyFor).
    Value symbolFor(const std::string& key);
    std::string symbolKeyFor(Value symbol);

    // Well-known symbols — ES2025 complete.
    Value getIterator() const { return iterator_; }
    Value getAsyncIterator() const { return asyncIterator_; }
    Value getToStringTag() const { return toStringTag_; }
    Value getHasInstance() const { return hasInstance_; }
    Value getIsConcatSpreadable() const { return isConcatSpreadable_; }
    Value getUnscopables() const { return unscopables_; }
    Value getToPrimitive() const { return toPrimitive_; }
    Value getSpecies() const { return species_; }
    Value getMatch() const { return match_; }
    Value getMatchAll() const { return matchAll_; }
    Value getReplace() const { return replace_; }
    Value getSearch() const { return search_; }
    Value getSplit() const { return split_; }
    Value getDispose() const { return dispose_; }
    Value getAsyncDispose() const { return asyncDispose_; }
    Value getMetadata() const { return metadata_; }

    std::string getDescription(uint32_t id) const;
    bool isWellKnown(uint32_t id) const;

    // Reverse lookup table for Symbol.keyFor.
    void buildReverseLookup();

private:
    SymbolRegistry();

    Value createWellKnown(const char* desc);

    std::atomic<uint32_t> nextId_{1};
    mutable std::mutex mutex_;
    std::unordered_map<uint32_t, std::string> descriptions_;
    std::unordered_map<std::string, Value> globalRegistry_;
    std::unordered_map<uint32_t, std::string> reverseRegistry_;

    // Well-known symbols.
    Value iterator_;
    Value asyncIterator_;
    Value toStringTag_;
    Value hasInstance_;
    Value isConcatSpreadable_;
    Value unscopables_;
    Value toPrimitive_;
    Value species_;
    Value match_;
    Value matchAll_;
    Value replace_;
    Value search_;
    Value split_;
    Value dispose_;
    Value asyncDispose_;
    Value metadata_;
};

/**
 * The description a symbol was created with ("iterator" symbols report
 * "Symbol.iterator"), or std::nullopt when it has none.
 */
std::optional<std::string> symbolDescription(uint32_t id);

/** Symbol(description): what String(symbol) and symbol.toString() give. */
std::string symbolDescriptiveString(uint32_t id);

} // namespace Zepra::Runtime
