// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
#pragma once

/**
 * @file list_iterator.hpp
 * @brief Iterator objects over a fixed list of values
 *
 * What String.prototype.matchAll, Map.prototype.entries, Set.prototype.values
 * and friends return: an object with next() that is also iterable, so both
 * `for (x of it)` and manual `it.next()` work.
 */

#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"
#include <vector>

namespace Zepra::Builtins {

using Runtime::Object;
using Runtime::Value;

class ListIterator : public Object {
public:
    explicit ListIterator(std::vector<Value> items) : items_(std::move(items)) {}

    /** The next value, false once the list is exhausted. */
    bool next(Value& out) {
        if (index_ >= items_.size()) return false;
        out = items_[index_++];
        return true;
    }

    const std::vector<Value>& items() const { return items_; }

private:
    std::vector<Value> items_;
    size_t index_ = 0;
};

class IteratorBuiltin {
public:
    /** The prototype shared by list iterators: next() and [Symbol.iterator]() returning `this`. */
    static Object* createIteratorPrototype();

    /** A new iterator over `items`, inheriting from the running VM's iterator prototype. */
    static Value makeListIterator(std::vector<Value> items);
};

} // namespace Zepra::Builtins
