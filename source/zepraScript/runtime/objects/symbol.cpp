// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — symbol.cpp — Symbol registry with all ES2025 well-known symbols

#include "runtime/objects/symbol.hpp"

namespace Zepra::Runtime {

SymbolRegistry& SymbolRegistry::instance() {
    static SymbolRegistry instance;
    return instance;
}

Value SymbolRegistry::createWellKnown(const char* desc) {
    uint32_t id = nextId_++;
    Value sym = Value::symbol(id);
    descriptions_[id] = desc;
    return sym;
}

SymbolRegistry::SymbolRegistry() {
    iterator_            = createWellKnown("Symbol.iterator");
    asyncIterator_       = createWellKnown("Symbol.asyncIterator");
    toStringTag_         = createWellKnown("Symbol.toStringTag");
    hasInstance_          = createWellKnown("Symbol.hasInstance");
    isConcatSpreadable_  = createWellKnown("Symbol.isConcatSpreadable");
    unscopables_         = createWellKnown("Symbol.unscopables");
    toPrimitive_         = createWellKnown("Symbol.toPrimitive");
    species_             = createWellKnown("Symbol.species");
    match_               = createWellKnown("Symbol.match");
    matchAll_            = createWellKnown("Symbol.matchAll");
    replace_             = createWellKnown("Symbol.replace");
    search_              = createWellKnown("Symbol.search");
    split_               = createWellKnown("Symbol.split");
    dispose_             = createWellKnown("Symbol.dispose");
    asyncDispose_        = createWellKnown("Symbol.asyncDispose");
    metadata_            = createWellKnown("Symbol.metadata");
}

Value SymbolRegistry::createSymbol(const std::string& description) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t id = nextId_++;
    Value symbol = Value::symbol(id);
    if (!description.empty()) {
        descriptions_[id] = description;
    }
    return symbol;
}

Value SymbolRegistry::symbolFor(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = globalRegistry_.find(key);
    if (it != globalRegistry_.end()) {
        return it->second;
    }

    uint32_t id = nextId_++;
    Value symbol = Value::symbol(id);
    descriptions_[id] = key;
    globalRegistry_[key] = symbol;
    reverseRegistry_[id] = key;
    return symbol;
}

std::string SymbolRegistry::symbolKeyFor(Value symbol) {
    if (!symbol.isSymbol()) return "";

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = reverseRegistry_.find(symbol.asSymbol());
    if (it != reverseRegistry_.end()) {
        return it->second;
    }
    return "";
}

std::string SymbolRegistry::getDescription(uint32_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = descriptions_.find(id);
    return it != descriptions_.end() ? it->second : "";
}

bool SymbolRegistry::isWellKnown(uint32_t id) const {
    // Well-known symbols have IDs 1-16 (created in constructor).
    return id >= 1 && id <= 16;
}

void SymbolRegistry::buildReverseLookup() {
    std::lock_guard<std::mutex> lock(mutex_);
    reverseRegistry_.clear();
    for (auto& [key, val] : globalRegistry_) {
        reverseRegistry_[val.asSymbol()] = key;
    }
}

std::optional<std::string> symbolDescription(uint32_t id) {
    // The well-known symbols the engine uses carry fixed ids (see well_known_symbols.hpp)
    static const char* wellKnown[] = {
        "Symbol.iterator", "Symbol.toPrimitive", "Symbol.hasInstance", "Symbol.toStringTag",
        "Symbol.species", "Symbol.isConcatSpreadable", "Symbol.unscopables", "Symbol.asyncIterator",
        "Symbol.match", "Symbol.replace", "Symbol.search", "Symbol.split"};
    if (id >= 0xFFFF0001 && id <= 0xFFFF000C) return std::string(wellKnown[id - 0xFFFF0001]);

    std::string description = SymbolRegistry::instance().getDescription(id);
    if (description.empty()) return std::nullopt;
    return description;
}

std::string symbolDescriptiveString(uint32_t id) {
    return "Symbol(" + symbolDescription(id).value_or("") + ")";
}

} // namespace Zepra::Runtime
