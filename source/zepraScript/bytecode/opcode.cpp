// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file opcode.cpp
 * @brief Opcode metadata: names, operand counts, stack effects
 */

#include "bytecode/opcode.hpp"

namespace Zepra::Bytecode {

const char* opcodeName(Opcode op) {
    switch (op) {
        case Opcode::OP_NOP: return "NOP";
        case Opcode::OP_POP: return "POP";
        case Opcode::OP_DUP: return "DUP";
        case Opcode::OP_SWAP: return "SWAP";
        case Opcode::OP_DUP2: return "DUP2";
        case Opcode::OP_DUP_X1: return "DUP_X1";
        case Opcode::OP_DUP_X2: return "DUP_X2";
        case Opcode::OP_PICK: return "PICK";
        case Opcode::OP_TO_NUMBER: return "TO_NUMBER";
        case Opcode::OP_DELETE_ELEMENT: return "DELETE_ELEMENT";
        case Opcode::OP_CALL_WITH_THIS: return "CALL_WITH_THIS";
        case Opcode::OP_CALL_SPREAD: return "CALL_SPREAD";
        case Opcode::OP_NEW_SPREAD: return "NEW_SPREAD";
        case Opcode::OP_SUPER_APPLY: return "SUPER_APPLY";
        case Opcode::OP_OBJECT_REST: return "OBJECT_REST";
        case Opcode::OP_GET_CALLEE: return "GET_CALLEE";
        case Opcode::OP_CONSTANT: return "CONSTANT";
        case Opcode::OP_CONSTANT_LONG: return "CONSTANT_LONG";
        case Opcode::OP_NIL: return "NIL";
        case Opcode::OP_TRUE: return "TRUE";
        case Opcode::OP_FALSE: return "FALSE";
        case Opcode::OP_ZERO: return "ZERO";
        case Opcode::OP_ONE: return "ONE";
        case Opcode::OP_ADD: return "ADD";
        case Opcode::OP_SUBTRACT: return "SUBTRACT";
        case Opcode::OP_MULTIPLY: return "MULTIPLY";
        case Opcode::OP_DIVIDE: return "DIVIDE";
        case Opcode::OP_MODULO: return "MODULO";
        case Opcode::OP_POWER: return "POWER";
        case Opcode::OP_NEGATE: return "NEGATE";
        case Opcode::OP_INCREMENT: return "INCREMENT";
        case Opcode::OP_DECREMENT: return "DECREMENT";
        case Opcode::OP_BITWISE_AND: return "BITWISE_AND";
        case Opcode::OP_BITWISE_OR: return "BITWISE_OR";
        case Opcode::OP_BITWISE_XOR: return "BITWISE_XOR";
        case Opcode::OP_BITWISE_NOT: return "BITWISE_NOT";
        case Opcode::OP_LEFT_SHIFT: return "LEFT_SHIFT";
        case Opcode::OP_RIGHT_SHIFT: return "RIGHT_SHIFT";
        case Opcode::OP_UNSIGNED_RIGHT_SHIFT: return "UNSIGNED_RIGHT_SHIFT";
        case Opcode::OP_EQUAL: return "EQUAL";
        case Opcode::OP_STRICT_EQUAL: return "STRICT_EQUAL";
        case Opcode::OP_NOT_EQUAL: return "NOT_EQUAL";
        case Opcode::OP_STRICT_NOT_EQUAL: return "STRICT_NOT_EQUAL";
        case Opcode::OP_LESS: return "LESS";
        case Opcode::OP_LESS_EQUAL: return "LESS_EQUAL";
        case Opcode::OP_GREATER: return "GREATER";
        case Opcode::OP_GREATER_EQUAL: return "GREATER_EQUAL";
        case Opcode::OP_NOT: return "NOT";
        case Opcode::OP_AND: return "AND";
        case Opcode::OP_OR: return "OR";
        case Opcode::OP_NULLISH: return "NULLISH";
        case Opcode::OP_TYPEOF: return "TYPEOF";
        case Opcode::OP_INSTANCEOF: return "INSTANCEOF";
        case Opcode::OP_IN: return "IN";
        case Opcode::OP_GET_LOCAL: return "GET_LOCAL";
        case Opcode::OP_SET_LOCAL: return "SET_LOCAL";
        case Opcode::OP_GET_GLOBAL: return "GET_GLOBAL";
        case Opcode::OP_SET_GLOBAL: return "SET_GLOBAL";
        case Opcode::OP_DEFINE_GLOBAL: return "DEFINE_GLOBAL";
        case Opcode::OP_GET_UPVALUE: return "GET_UPVALUE";
        case Opcode::OP_SET_UPVALUE: return "SET_UPVALUE";
        case Opcode::OP_CLOSE_UPVALUE: return "CLOSE_UPVALUE";
        case Opcode::OP_GET_PROPERTY: return "GET_PROPERTY";
        case Opcode::OP_SET_PROPERTY: return "SET_PROPERTY";
        case Opcode::OP_GET_ELEMENT: return "GET_ELEMENT";
        case Opcode::OP_SET_ELEMENT: return "SET_ELEMENT";
        case Opcode::OP_DELETE_PROPERTY: return "DELETE_PROPERTY";
        case Opcode::OP_CREATE_OBJECT: return "CREATE_OBJECT";
        case Opcode::OP_CREATE_ARRAY: return "CREATE_ARRAY";
        case Opcode::OP_INIT_PROPERTY: return "INIT_PROPERTY";
        case Opcode::OP_INIT_ELEMENT: return "INIT_ELEMENT";
        case Opcode::OP_SPREAD: return "SPREAD";
        case Opcode::OP_JUMP: return "JUMP";
        case Opcode::OP_JUMP_IF_FALSE: return "JUMP_IF_FALSE";
        case Opcode::OP_JUMP_IF_TRUE: return "JUMP_IF_TRUE";
        case Opcode::OP_JUMP_IF_NIL: return "JUMP_IF_NIL";
        case Opcode::OP_LOOP: return "LOOP";
        case Opcode::OP_SWITCH: return "SWITCH";
        case Opcode::OP_CASE: return "CASE";
        case Opcode::OP_CALL: return "CALL";
        case Opcode::OP_CALL_METHOD: return "CALL_METHOD";
        case Opcode::OP_RETURN: return "RETURN";
        case Opcode::OP_CLOSURE: return "CLOSURE";
        case Opcode::OP_NEW: return "NEW";
        case Opcode::OP_INHERIT: return "INHERIT";
        case Opcode::OP_DEFINE_METHOD: return "DEFINE_METHOD";
        case Opcode::OP_DEFINE_STATIC: return "DEFINE_STATIC";
        case Opcode::OP_DEFINE_GETTER: return "DEFINE_GETTER";
        case Opcode::OP_DEFINE_SETTER: return "DEFINE_SETTER";
        case Opcode::OP_SUPER_CALL: return "SUPER_CALL";
        case Opcode::OP_SUPER_GET: return "SUPER_GET";
        case Opcode::OP_THROW: return "THROW";
        case Opcode::OP_TRY_BEGIN: return "TRY_BEGIN";
        case Opcode::OP_TRY_END: return "TRY_END";
        case Opcode::OP_CATCH: return "CATCH";
        case Opcode::OP_FINALLY: return "FINALLY";
        case Opcode::OP_GET_ITERATOR: return "GET_ITERATOR";
        case Opcode::OP_ITERATOR_NEXT: return "ITERATOR_NEXT";
        case Opcode::OP_FOR_IN: return "FOR_IN";
        case Opcode::OP_FOR_OF: return "FOR_OF";
        case Opcode::OP_YIELD: return "YIELD";
        case Opcode::OP_AWAIT: return "AWAIT";
        case Opcode::OP_DEBUGGER: return "DEBUGGER";
        case Opcode::OP_LINE: return "LINE";
        case Opcode::OP_IMPORT: return "IMPORT";
        case Opcode::OP_EXPORT: return "EXPORT";
        case Opcode::OP_IMPORT_BINDING: return "IMPORT_BINDING";
        case Opcode::OP_END: return "END";
        default: return "UNKNOWN";
    }
}

int opcodeOperandCount(Opcode op) {
    switch (op) {
        // 1-byte operand: argument count, depth or flag
        case Opcode::OP_CONSTANT:
        case Opcode::OP_CALL:
        case Opcode::OP_NEW:
        case Opcode::OP_SUPER_CALL:
        case Opcode::OP_YIELD:
        case Opcode::OP_PICK:
        case Opcode::OP_CALL_WITH_THIS:
        case Opcode::OP_OBJECT_REST:
            return 1;
        // 2-byte operand: constant-pool index, local/upvalue slot, count or
        // jump offset
        case Opcode::OP_CONSTANT_LONG:
        case Opcode::OP_GET_LOCAL:
        case Opcode::OP_SET_LOCAL:
        case Opcode::OP_GET_UPVALUE:
        case Opcode::OP_SET_UPVALUE:
        case Opcode::OP_GET_GLOBAL:
        case Opcode::OP_SET_GLOBAL:
        case Opcode::OP_DEFINE_GLOBAL:
        case Opcode::OP_GET_PROPERTY:
        case Opcode::OP_SET_PROPERTY:
        case Opcode::OP_DELETE_PROPERTY:
        case Opcode::OP_CREATE_ARRAY:
        case Opcode::OP_DEFINE_METHOD:
        case Opcode::OP_DEFINE_STATIC:
        case Opcode::OP_DEFINE_GETTER:
        case Opcode::OP_DEFINE_SETTER:
        case Opcode::OP_SUPER_GET:
        case Opcode::OP_IMPORT:
        case Opcode::OP_EXPORT:
        case Opcode::OP_IMPORT_BINDING:
        case Opcode::OP_JUMP:
        case Opcode::OP_JUMP_IF_FALSE:
        case Opcode::OP_JUMP_IF_TRUE:
        case Opcode::OP_JUMP_IF_NIL:
        case Opcode::OP_LOOP:
        case Opcode::OP_AND:
        case Opcode::OP_OR:
        case Opcode::OP_NULLISH:
        case Opcode::OP_CASE:
        case Opcode::OP_TRY_BEGIN:
        case Opcode::OP_ITERATOR_NEXT:
        case Opcode::OP_LINE:
            return 2;
        // Method name index (2 bytes) + argument count (1 byte)
        case Opcode::OP_CALL_METHOD:
            return 3;
        // Function index (2 bytes) + upvalue count (2 bytes), then three bytes
        // (isLocal, 16-bit index) per upvalue
        case Opcode::OP_CLOSURE:
            return 4;
        default:
            return 0;
    }
}

int opcodeStackEffect(Opcode op) {
    // Net change in stack height. Calls, OP_CREATE_ARRAY and OP_OBJECT_REST
    // depend on their operands and report 0 here.
    switch (op) {
        case Opcode::OP_DUP2:
            return 2;
        case Opcode::OP_CONSTANT:
        case Opcode::OP_CONSTANT_LONG:
        case Opcode::OP_NIL:
        case Opcode::OP_TRUE:
        case Opcode::OP_FALSE:
        case Opcode::OP_ZERO:
        case Opcode::OP_ONE:
        case Opcode::OP_DUP:
        case Opcode::OP_DUP_X1:
        case Opcode::OP_DUP_X2:
        case Opcode::OP_PICK:
        case Opcode::OP_GET_LOCAL:
        case Opcode::OP_GET_GLOBAL:
        case Opcode::OP_GET_UPVALUE:
        case Opcode::OP_CREATE_OBJECT:
        case Opcode::OP_CLOSURE:
        case Opcode::OP_IMPORT:
        case Opcode::OP_CATCH:
        case Opcode::OP_GET_CALLEE:
            return 1;
        case Opcode::OP_POP:
        case Opcode::OP_DEFINE_GLOBAL:
        case Opcode::OP_CLOSE_UPVALUE:
        case Opcode::OP_THROW:
        case Opcode::OP_EXPORT:
        case Opcode::OP_RETURN:
        case Opcode::OP_INIT_ELEMENT:
        case Opcode::OP_SPREAD:
        case Opcode::OP_INHERIT:
        case Opcode::OP_DEFINE_METHOD:
        case Opcode::OP_DEFINE_STATIC:
        case Opcode::OP_DEFINE_GETTER:
        case Opcode::OP_DEFINE_SETTER:
        case Opcode::OP_ADD:
        case Opcode::OP_SUBTRACT:
        case Opcode::OP_MULTIPLY:
        case Opcode::OP_DIVIDE:
        case Opcode::OP_MODULO:
        case Opcode::OP_POWER:
        case Opcode::OP_BITWISE_AND:
        case Opcode::OP_BITWISE_OR:
        case Opcode::OP_BITWISE_XOR:
        case Opcode::OP_LEFT_SHIFT:
        case Opcode::OP_RIGHT_SHIFT:
        case Opcode::OP_UNSIGNED_RIGHT_SHIFT:
        case Opcode::OP_EQUAL:
        case Opcode::OP_STRICT_EQUAL:
        case Opcode::OP_NOT_EQUAL:
        case Opcode::OP_STRICT_NOT_EQUAL:
        case Opcode::OP_LESS:
        case Opcode::OP_LESS_EQUAL:
        case Opcode::OP_GREATER:
        case Opcode::OP_GREATER_EQUAL:
        case Opcode::OP_INSTANCEOF:
        case Opcode::OP_IN:
        case Opcode::OP_GET_ELEMENT:
        case Opcode::OP_SET_PROPERTY:
        case Opcode::OP_DELETE_ELEMENT:
        case Opcode::OP_NEW_SPREAD:
            return -1;
        case Opcode::OP_INIT_PROPERTY:
        case Opcode::OP_SET_ELEMENT:
        case Opcode::OP_CALL_SPREAD:
        case Opcode::OP_SUPER_APPLY:
            return -2;
        // SET_LOCAL/SET_GLOBAL/SET_UPVALUE store and keep the value: net 0.
        // ITERATOR_NEXT replaces the iterator with a value, or drops it and
        // jumps when done.
        default:
            return 0;
    }
}

} // namespace Zepra::Bytecode
