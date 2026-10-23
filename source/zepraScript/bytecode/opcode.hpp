#pragma once

/**
 * @file opcode.hpp
 * @brief Bytecode instruction definitions
 */

#include "../config.hpp"
#include <string>

namespace Zepra::Bytecode {

/**
 * @brief Bytecode opcodes
 *
 * Operands that index the constant pool (names, functions, module paths)
 * are 16-bit, big-endian; OP_CONSTANT is the one-byte short form.
 */
enum class Opcode : uint8 {
    // Stack manipulation
    OP_NOP = 0x00,
    OP_POP,
    OP_DUP,
    OP_SWAP,
    
    // Constants
    OP_CONSTANT,        // Push constant from pool
    OP_CONSTANT_LONG,   // Push constant (wide index)
    OP_NIL,            // Push null
    OP_TRUE,           // Push true
    OP_FALSE,          // Push false
    OP_ZERO,           // Push 0
    OP_ONE,            // Push 1
    
    // Arithmetic
    OP_ADD,
    OP_SUBTRACT,
    OP_MULTIPLY,
    OP_DIVIDE,
    OP_MODULO,
    OP_POWER,
    OP_NEGATE,
    OP_INCREMENT,
    OP_DECREMENT,
    
    // Bitwise
    OP_BITWISE_AND,
    OP_BITWISE_OR,
    OP_BITWISE_XOR,
    OP_BITWISE_NOT,
    OP_LEFT_SHIFT,
    OP_RIGHT_SHIFT,
    OP_UNSIGNED_RIGHT_SHIFT,
    
    // Comparison
    OP_EQUAL,
    OP_STRICT_EQUAL,
    OP_NOT_EQUAL,
    OP_STRICT_NOT_EQUAL,
    OP_LESS,
    OP_LESS_EQUAL,
    OP_GREATER,
    OP_GREATER_EQUAL,
    
    // Logical
    OP_NOT,
    OP_AND,            // Short-circuit and (jumps)
    OP_OR,             // Short-circuit or (jumps)
    OP_NULLISH,        // Nullish coalescing (??)
    
    // Type operations
    OP_TYPEOF,
    OP_INSTANCEOF,
    OP_IN,
    
    // Variables
    OP_GET_LOCAL,
    OP_SET_LOCAL,
    OP_GET_GLOBAL,
    OP_SET_GLOBAL,
    OP_DEFINE_GLOBAL,
    OP_GET_UPVALUE,
    OP_SET_UPVALUE,
    OP_CLOSE_UPVALUE,
    
    // Properties
    OP_GET_PROPERTY,
    OP_SET_PROPERTY,
    OP_GET_ELEMENT,
    OP_SET_ELEMENT,
    OP_DELETE_PROPERTY,
    
    // Objects & Arrays
    OP_CREATE_OBJECT,
    OP_CREATE_ARRAY,   // u16 count: [e0..en-1] -> [array]
    OP_INIT_PROPERTY,
    OP_INIT_ELEMENT,   // [array, value] -> [array] (appends)
    OP_SPREAD,         // [array, iterable] -> [array] (appends every item)
    
    // Control flow
    OP_JUMP,
    OP_JUMP_IF_FALSE,
    OP_JUMP_IF_TRUE,
    OP_JUMP_IF_NIL,
    OP_LOOP,           // Backwards jump
    
    // Switch support
    OP_SWITCH,
    OP_CASE,
    
    // Functions
    OP_CALL,
    OP_CALL_METHOD,
    OP_RETURN,
    OP_CLOSURE,
    
    // Construction
    OP_NEW,
    
    // Classes (ES6)
    OP_INHERIT,         // Set up prototype chain for class extends
    OP_DEFINE_METHOD,   // Add method to prototype
    OP_DEFINE_STATIC,   // Add static method to class
    OP_DEFINE_GETTER,   // Define getter property
    OP_DEFINE_SETTER,   // Define setter property
    OP_SUPER_CALL,      // Call super constructor
    OP_SUPER_GET,       // Get property from super prototype
    
    // Exception handling
    OP_THROW,
    OP_TRY_BEGIN,
    OP_TRY_END,
    OP_CATCH,
    OP_FINALLY,
    
    // Iterators
    OP_GET_ITERATOR,   // [iterable] -> [iterator]
    OP_ITERATOR_NEXT,  // u16 exit offset: [iterator] -> [value], or [] and jump when done
    OP_FOR_IN,
    OP_FOR_OF,
    
    // Generators/Async
    OP_YIELD,
    OP_AWAIT,
    
    // Debug
    OP_DEBUGGER,
    OP_LINE,           // Source line info
    
    // Modules
    OP_IMPORT,         // Load module, push exports object
    OP_EXPORT,         // Register exported value
    OP_IMPORT_BINDING, // Bind imported name to local
    
    // Appended so existing opcode numbers do not change
    OP_DUP2,           // [a, b]       -> [a, b, a, b]
    OP_DUP_X1,         // [a, b]       -> [b, a, b]
    OP_DUP_X2,         // [a, b, c]    -> [c, a, b, c]
    OP_PICK,           // u8 n: copies the value n below the top to the top
    OP_TO_NUMBER,      // Unary +
    OP_DELETE_ELEMENT, // [obj, key] -> [bool]
    OP_CALL_WITH_THIS, // u8 argc: [this, fn, args...] -> [result]
    OP_CALL_SPREAD,    // [this, fn, argsArray] -> [result]
    OP_NEW_SPREAD,     // [ctor, argsArray] -> [object]
    OP_SUPER_APPLY,    // [argsArray, this, superclass] -> [result]
    OP_OBJECT_REST,    // u8 n: [source, key1..keyn] -> [copy of source without those keys]
    OP_GET_CALLEE,     // Pushes the running function (a named function expression's own name)

    // End marker
    OP_END
};

/**
 * @brief Get the name of an opcode
 */
const char* opcodeName(Opcode op);

/**
 * @brief Get the number of operand bytes for an opcode
 */
int opcodeOperandCount(Opcode op);

/**
 * @brief Get stack effect of an opcode (how it changes stack size)
 */
int opcodeStackEffect(Opcode op);

} // namespace Zepra::Bytecode
