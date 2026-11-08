// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file custom_properties.cpp
 * @brief Custom property resolution and var() substitution.
 */

#include "css/custom_properties.hpp"

#include <cctype>
#include <functional>
#include <set>
#include <vector>

namespace Zepra::WebCore {

namespace {

/// Deepest nesting of var() fallbacks followed before giving up
constexpr int kMaxDepth = 64;

/// A value may grow by substitution; stop runaway growth ("--a: var(--b) var(--b)" chains)
constexpr size_t kMaxValueLength = 1 << 20;

using Lookup = std::function<const std::string*(const std::string&)>;

bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
           static_cast<unsigned char>(c) >= 0x80;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// True when "var(" starts at i, as a whole function name
bool varAt(const std::string& s, size_t i) {
    if (i + 4 > s.size()) return false;
    if (std::tolower(static_cast<unsigned char>(s[i])) != 'v' ||
        std::tolower(static_cast<unsigned char>(s[i + 1])) != 'a' ||
        std::tolower(static_cast<unsigned char>(s[i + 2])) != 'r' || s[i + 3] != '(') {
        return false;
    }
    return i == 0 || !isIdentChar(s[i - 1]);
}

/// Index past the string literal that starts at i (a quote), or npos if unclosed
size_t skipString(const std::string& s, size_t i) {
    char quote = s[i++];
    while (i < s.size()) {
        if (s[i] == '\\') { i += 2; continue; }
        if (s[i] == quote) return i + 1;
        if (s[i] == '\n') return std::string::npos;
        i++;
    }
    return std::string::npos;
}

bool substitute(const std::string& value, const Lookup& lookup, std::string& out, int depth) {
    if (depth > kMaxDepth) return false;
    size_t i = 0;
    while (i < value.size()) {
        char c = value[i];
        if (c == '"' || c == '\'') {
            size_t end = skipString(value, i);
            if (end == std::string::npos) return false;
            out.append(value, i, end - i);
            i = end;
            continue;
        }
        if (c == '/' && i + 1 < value.size() && value[i + 1] == '*') {
            size_t end = value.find("*/", i + 2);
            i = end == std::string::npos ? value.size() : end + 2;
            out += ' ';
            continue;
        }
        if (!varAt(value, i)) {
            out += c;
            i++;
            continue;
        }
        // var( name [, fallback] )
        size_t argsStart = i + 4;
        size_t comma = std::string::npos;
        size_t j = argsStart;
        int parens = 1;
        while (j < value.size()) {
            char d = value[j];
            if (d == '"' || d == '\'') {
                j = skipString(value, j);
                if (j == std::string::npos) return false;
                continue;
            }
            if (d == '(' || d == '[' || d == '{') parens++;
            else if (d == ')' || d == ']' || d == '}') {
                if (--parens == 0) break;
            } else if (d == ',' && parens == 1 && comma == std::string::npos) {
                comma = j;
            }
            j++;
        }
        if (j >= value.size()) return false;  // unclosed var(
        size_t nameEnd = comma == std::string::npos ? j : comma;
        std::string name = trim(value.substr(argsStart, nameEnd - argsStart));
        if (!isCustomPropertyName(name)) return false;

        if (const std::string* found = lookup(name)) {
            out += *found;
        } else if (comma != std::string::npos) {
            std::string fallback;
            if (!substitute(value.substr(comma + 1, j - comma - 1), lookup, fallback, depth + 1)) return false;
            out += trim(fallback);
        } else {
            return false;
        }
        if (out.size() > kMaxValueLength) return false;
        i = j + 1;
    }
    return true;
}

}  // namespace

bool isCustomPropertyName(const std::string& name) {
    return name.size() > 2 && name[0] == '-' && name[1] == '-';
}

bool containsVarFunction(const std::string& value) {
    for (size_t i = value.find_first_of("vV"); i != std::string::npos; i = value.find_first_of("vV", i + 1)) {
        if (varAt(value, i)) return true;
    }
    return false;
}

bool substituteVarFunctions(const std::string& value, const CustomPropertyMap& vars, std::string& out) {
    out.clear();
    Lookup lookup = [&vars](const std::string& name) -> const std::string* {
        auto it = vars.find(name);
        return it != vars.end() ? &it->second : nullptr;
    };
    if (!substitute(value, lookup, out, 0)) return false;
    out = trim(out);
    return true;
}

std::shared_ptr<const CustomPropertyMap> resolveCustomProperties(
    const std::shared_ptr<const CustomPropertyMap>& inherited,
    const CustomPropertyMap& declared,
    const RegisteredPropertyMap* registered) {
    if (registered && registered->empty()) registered = nullptr;
    if (declared.empty() && !registered) return inherited;

    auto parentValue = [&](const std::string& name) -> const std::string* {
        if (!inherited) return nullptr;
        auto it = inherited->find(name);
        return it != inherited->end() ? &it->second : nullptr;
    };
    auto registration = [&](const std::string& name) -> const RegisteredCustomProperty* {
        if (!registered) return nullptr;
        auto it = registered->find(name);
        return it != registered->end() ? &it->second : nullptr;
    };

    // What the element starts from before its own declarations: the parent's
    // values, except that registered properties which don't inherit (or have
    // no value yet) take their initial value
    std::shared_ptr<const CustomPropertyMap> base = inherited;
    if (registered) {
        bool changes = false;
        for (const auto& [name, reg] : *registered) {
            const std::string* parent = parentValue(name);
            bool wantsInitial = !reg.inherits || !parent;
            if (!wantsInitial) continue;
            if (reg.hasInitialValue ? (!parent || *parent != reg.initialValue) : parent != nullptr) {
                changes = true;
                break;
            }
        }
        if (changes) {
            auto copy = std::make_shared<CustomPropertyMap>(inherited ? *inherited : CustomPropertyMap());
            for (const auto& [name, reg] : *registered) {
                if (reg.inherits && parentValue(name)) continue;
                if (reg.hasInitialValue) (*copy)[name] = reg.initialValue;
                else copy->erase(name);
            }
            base = copy;
        }
    }
    if (declared.empty()) return base;

    auto baseValue = [&](const std::string& name) -> const std::string* {
        if (!base) return nullptr;
        auto it = base->find(name);
        return it != base->end() ? &it->second : nullptr;
    };
    // The value a registered property falls back to: unset
    auto unsetValue = [&](const std::string& name, const RegisteredCustomProperty& reg) -> const std::string* {
        if (reg.inherits) {
            if (const std::string* parent = parentValue(name)) return parent;
        }
        return reg.hasInitialValue ? &reg.initialValue : nullptr;
    };

    enum class State { Pending, Resolving, Done };
    struct Entry {
        std::string raw;
        State state = State::Pending;
        bool inCycle = false;
        bool valid = false;
        std::string value;
    };
    std::map<std::string, Entry> entries;
    for (const auto& [name, raw] : declared) entries[name].raw = trim(raw);

    std::vector<std::string> stack;
    std::function<const std::string*(const std::string&)> resolve =
        [&](const std::string& name) -> const std::string* {
        auto it = entries.find(name);
        if (it == entries.end()) return baseValue(name);
        Entry& e = it->second;
        if (e.state == State::Resolving) {
            // A dependency cycle: everything on it is invalid
            for (size_t k = stack.size(); k-- > 0;) {
                entries[stack[k]].inCycle = true;
                if (stack[k] == name) break;
            }
            return nullptr;
        }
        if (e.state == State::Pending) {
            e.state = State::Resolving;
            stack.push_back(name);
            const RegisteredCustomProperty* reg = registration(name);
            std::string keyword = lower(e.raw);
            auto take = [&](const std::string* v) {
                e.valid = v != nullptr;
                if (v) e.value = *v;
            };
            if (keyword == "initial") {
                take(reg && reg->hasInitialValue ? &reg->initialValue : nullptr);
            } else if (keyword == "inherit") {
                take(parentValue(name));
            } else if (keyword == "unset" || keyword == "revert" || keyword == "revert-layer") {
                take(reg ? unsetValue(name, *reg) : parentValue(name));
            } else if (containsVarFunction(e.raw)) {
                std::string out;
                e.valid = substitute(e.raw, resolve, out, 0);
                if (e.valid) e.value = trim(out);
            } else {
                e.valid = true;
                e.value = e.raw;
            }
            stack.pop_back();
            e.state = State::Done;
        }
        return e.valid && !e.inCycle ? &e.value : nullptr;
    };

    auto result = std::make_shared<CustomPropertyMap>(base ? *base : CustomPropertyMap());
    for (auto& [name, entry] : entries) resolve(name);
    for (auto& [name, entry] : entries) {
        if (entry.valid && !entry.inCycle) {
            (*result)[name] = entry.value;
        } else if (const RegisteredCustomProperty* reg = registration(name); reg && unsetValue(name, *reg)) {
            (*result)[name] = *unsetValue(name, *reg);  // invalid at computed-value time: unset
        } else {
            result->erase(name);
        }
    }
    return result;
}

}  // namespace Zepra::WebCore
