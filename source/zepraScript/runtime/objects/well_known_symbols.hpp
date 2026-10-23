#pragma once

/**
 * @file well_known_symbols.hpp
 * @brief Well-known Symbol IDs (ES2024 §6.1.5.1)
 *
 * Reserved symbol IDs in the 0xFFFF0000+ range.
 * User-created symbols start from 1 and increment.
 */

#include <cstdint>
#include <string>

namespace Zepra::Runtime {

namespace WellKnownSymbols {
    constexpr uint32_t Iterator      = 0xFFFF0001;
    constexpr uint32_t ToPrimitive   = 0xFFFF0002;
    constexpr uint32_t HasInstance    = 0xFFFF0003;
    constexpr uint32_t ToStringTag   = 0xFFFF0004;
    constexpr uint32_t Species       = 0xFFFF0005;
    constexpr uint32_t IsConcatSpreadable = 0xFFFF0006;
    constexpr uint32_t Unscopables   = 0xFFFF0007;
    constexpr uint32_t AsyncIterator = 0xFFFF0008;
    constexpr uint32_t Match         = 0xFFFF0009;
    constexpr uint32_t Replace       = 0xFFFF000A;
    constexpr uint32_t Search        = 0xFFFF000B;
    constexpr uint32_t Split         = 0xFFFF000C;
}

/**
 * @brief Property key under which a symbol-keyed property is stored
 *
 * Objects have no separate symbol table yet, so symbol keys share the string
 * key space under this form: distinct per symbol, and never an array index.
 */
inline std::string symbolPropertyKey(uint32_t symbolId) {
    return "@@symbol:" + std::to_string(symbolId);
}

} // namespace Zepra::Runtime
