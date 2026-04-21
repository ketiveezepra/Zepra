// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "core/TaskRunner.h"

namespace Zepra {

static thread_local EventLoopRunner* tl_eventLoop = nullptr;

EventLoopRunner* GetCurrentEventLoop() {
    return tl_eventLoop;
}

void SetCurrentEventLoop(EventLoopRunner* loop) {
    tl_eventLoop = loop;
}

} // namespace Zepra
