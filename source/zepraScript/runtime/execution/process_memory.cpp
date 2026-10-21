// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file process_memory.cpp
 * @brief Resident memory of the current process (for ResourceMonitor)
 */

#include "runtime/execution/Sandbox.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <psapi.h>
#elif defined(__APPLE__)
#  include <mach/mach.h>
#elif defined(__linux__)
#  include <cstdio>
#  include <unistd.h>
#endif

namespace Zepra::Runtime {

size_t processResidentBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return pmc.WorkingSetSize;
    }
    return 0;
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return static_cast<size_t>(info.resident_size);
    }
    return 0;
#elif defined(__linux__)
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long totalPages = 0;
    unsigned long residentPages = 0;
    int fields = std::fscanf(f, "%lu %lu", &totalPages, &residentPages);
    std::fclose(f);
    return fields == 2 ? residentPages * static_cast<size_t>(sysconf(_SC_PAGESIZE)) : 0;
#else
    return 0;
#endif
}

} // namespace Zepra::Runtime
