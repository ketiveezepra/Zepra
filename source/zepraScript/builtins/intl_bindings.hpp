#pragma once
// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file intl_bindings.hpp
 * @brief The script-facing Intl namespace object
 */

#include "runtime/objects/object.hpp"

namespace Zepra::Builtins {

// Builds the global `Intl` object (constructors inherit Object.prototype)
Runtime::Object* createIntlObject(Runtime::Object* objectPrototype);

} // namespace Zepra::Builtins
