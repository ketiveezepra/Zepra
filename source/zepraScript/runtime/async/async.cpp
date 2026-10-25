// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file async.cpp
 * @brief Async/await runtime support
 *
 * Implements:
 * - AsyncExecutionContext: state machine for async function calls
 * - AsyncFunction: Function subclass that returns Promises
 * - AwaitHandler: suspend/resume on await expressions
 * - MicrotaskQueue: process promise reactions after each task
 */

#include "runtime/async/async.hpp"
#include <algorithm>
#include "runtime/async/async_function.hpp"
#include "runtime/async/promise.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"
#include "runtime/execution/vm.hpp"
#include <cassert>
#include <stdexcept>

namespace Zepra::Runtime {

// =============================================================================
// AsyncContext (simple state tracker from async.hpp)
// =============================================================================

AsyncContext::AsyncContext(Promise* promise)
    : promise_(promise), state_(0) {}

// NOTE: AsyncExecutionContext, AsyncFunction, and AwaitHandler are
// defined in async_function.cpp — do NOT duplicate them here.

// =============================================================================
// MicrotaskQueue
// =============================================================================

MicrotaskQueue& MicrotaskQueue::instance() {
    static MicrotaskQueue queue;
    return queue;
}

void MicrotaskQueue::enqueue(std::function<void()> task) {
    // Inside a running script the job belongs to that VM's own queue
    if (VM* vm = VM::current()) {
        vm->enqueueJob(std::move(task));
        return;
    }
    queue_.push_back(std::move(task));
}

void MicrotaskQueue::process() {
    // Process until empty — microtasks can enqueue more microtasks
    while (!queue_.empty()) {
        auto task = std::move(queue_.front());
        queue_.erase(queue_.begin());
        task();
    }
}

void MicrotaskQueue::drain() {
    process();
}

bool MicrotaskQueue::isEmpty() const {
    return queue_.empty();
}

} // namespace Zepra::Runtime
