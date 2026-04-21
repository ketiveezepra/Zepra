# ZepraScript Bytecode Specification

> **Engine:** ZepraScript 1.2.0
> **Last Updated:** 2026-04-18

---

## 1. Encoding Format

ZepraScript bytecode is a flat byte stream. Each instruction consists of a 1-byte opcode
followed by zero or more operand bytes. All multi-byte operands are little-endian.

| Field | Size | Description |
|---|---|---|
| Opcode | 1 byte | Instruction identifier (0x00–0xFF) |
| Operands | 0–3 bytes | Varies by instruction |

Constants are stored separately in a constant pool associated with each `BytecodeChunk`.
Opcodes that reference constants use an index into this pool.

---

## 2. Opcode Table

### Stack Manipulation (0x00–0x03)

| Hex | Mnemonic | Operands | Stack Effect | Description |
|---|---|---|---|---|
| 0x00 | `OP_NOP` | — | 0 | No operation |
| 0x01 | `OP_POP` | — | -1 | Discard TOS |
| 0x02 | `OP_DUP` | — | +1 | Duplicate TOS |
| 0x03 | `OP_SWAP` | — | 0 | Swap TOS and TOS-1 |

### Constants (0x04–0x0A)

| Hex | Mnemonic | Operands | Stack Effect | Description |
|---|---|---|---|---|
| 0x04 | `OP_CONSTANT` | index:u8 | +1 | Push constant from pool |
| 0x05 | `OP_CONSTANT_LONG` | index:u16 | +1 | Push constant (wide index) |
| 0x06 | `OP_NIL` | — | +1 | Push `undefined` |
| 0x07 | `OP_TRUE` | — | +1 | Push `true` |
| 0x08 | `OP_FALSE` | — | +1 | Push `false` |
| 0x09 | `OP_ZERO` | — | +1 | Push numeric `0` |
| 0x0A | `OP_ONE` | — | +1 | Push numeric `1` |

### Arithmetic (0x0B–0x13)

| Hex | Mnemonic | Operands | Stack Effect | Description |
|---|---|---|---|---|
| 0x0B | `OP_ADD` | — | -1 | TOS-1 + TOS (also string concat) |
| 0x0C | `OP_SUBTRACT` | — | -1 | TOS-1 - TOS |
| 0x0D | `OP_MULTIPLY` | — | -1 | TOS-1 * TOS |
| 0x0E | `OP_DIVIDE` | — | -1 | TOS-1 / TOS |
| 0x0F | `OP_MODULO` | — | -1 | TOS-1 % TOS |
| 0x10 | `OP_POWER` | — | -1 | TOS-1 ** TOS |
| 0x11 | `OP_NEGATE` | — | 0 | -TOS |
| 0x12 | `OP_INCREMENT` | — | 0 | TOS + 1 |
| 0x13 | `OP_DECREMENT` | — | 0 | TOS - 1 |

### Bitwise (0x14–0x1A)

| Hex | Mnemonic | Operands | Stack Effect | Description |
|---|---|---|---|---|
| 0x14 | `OP_BITWISE_AND` | — | -1 | TOS-1 & TOS |
| 0x15 | `OP_BITWISE_OR` | — | -1 | TOS-1 \| TOS |
| 0x16 | `OP_BITWISE_XOR` | — | -1 | TOS-1 ^ TOS |
| 0x17 | `OP_BITWISE_NOT` | — | 0 | ~TOS |
| 0x18 | `OP_LEFT_SHIFT` | — | -1 | TOS-1 << TOS |
| 0x19 | `OP_RIGHT_SHIFT` | — | -1 | TOS-1 >> TOS (signed) |
| 0x1A | `OP_UNSIGNED_RIGHT_SHIFT` | — | -1 | TOS-1 >>> TOS |

### Comparison (0x1B–0x22)

| Hex | Mnemonic | Operands | Stack Effect | Description |
|---|---|---|---|---|
| 0x1B | `OP_EQUAL` | — | -1 | TOS-1 == TOS (abstract) |
| 0x1C | `OP_STRICT_EQUAL` | — | -1 | TOS-1 === TOS |
| 0x1D | `OP_NOT_EQUAL` | — | -1 | TOS-1 != TOS |
| 0x1E | `OP_STRICT_NOT_EQUAL` | — | -1 | TOS-1 !== TOS |
| 0x1F | `OP_LESS` | — | -1 | TOS-1 < TOS |
| 0x20 | `OP_LESS_EQUAL` | — | -1 | TOS-1 <= TOS |
| 0x21 | `OP_GREATER` | — | -1 | TOS-1 > TOS |
| 0x22 | `OP_GREATER_EQUAL` | — | -1 | TOS-1 >= TOS |

### Logical (0x23–0x26)

| Hex | Mnemonic | Operands | Stack Effect | Description |
|---|---|---|---|---|
| 0x23 | `OP_NOT` | — | 0 | !TOS |
| 0x24 | `OP_AND` | offset:u16 | 0 | Short-circuit `&&` (jump if falsy) |
| 0x25 | `OP_OR` | offset:u16 | 0 | Short-circuit `\|\|` (jump if truthy) |
| 0x26 | `OP_NULLISH` | offset:u16 | 0 | `??` (jump if non-null/undefined) |

### Type Operations (0x27–0x29)

| Hex | Mnemonic | Operands | Stack Effect | Description |
|---|---|---|---|---|
| 0x27 | `OP_TYPEOF` | — | 0 | Push typeof string |
| 0x28 | `OP_INSTANCEOF` | — | -1 | TOS-1 instanceof TOS |
| 0x29 | `OP_IN` | — | -1 | TOS-1 in TOS |

### Variables (0x2A–0x31)

| Hex | Mnemonic | Operands | Stack Effect | Description |
|---|---|---|---|---|
| 0x2A | `OP_GET_LOCAL` | slot:u8 | +1 | Push local variable |
| 0x2B | `OP_SET_LOCAL` | slot:u8 | 0 | Store into local |
| 0x2C | `OP_GET_GLOBAL` | name:u8 | +1 | Push global variable |
| 0x2D | `OP_SET_GLOBAL` | name:u8 | 0 | Store into global |
| 0x2E | `OP_DEFINE_GLOBAL` | name:u8 | -1 | Define new global |
| 0x2F | `OP_GET_UPVALUE` | index:u8 | +1 | Push captured variable |
| 0x30 | `OP_SET_UPVALUE` | index:u8 | 0 | Store into captured variable |
| 0x31 | `OP_CLOSE_UPVALUE` | — | -1 | Close over upvalue on stack |

### Properties (0x32–0x36)

| Hex | Mnemonic | Operands | Stack Effect | Description |
|---|---|---|---|---|
| 0x32 | `OP_GET_PROPERTY` | name:u8 | 0 | obj[name] — IC fast path on shape match |
| 0x33 | `OP_SET_PROPERTY` | name:u8 | -1 | obj[name] = val — triggers write barrier |
| 0x34 | `OP_GET_ELEMENT` | — | -1 | obj[TOS] |
| 0x35 | `OP_SET_ELEMENT` | — | -2 | obj[TOS-1] = TOS — triggers write barrier |
| 0x36 | `OP_DELETE_PROPERTY` | — | -1 | delete obj[TOS] |

### Objects & Arrays (0x37–0x3B)

| Hex | Mnemonic | Operands | Stack Effect | Description |
|---|---|---|---|---|
| 0x37 | `OP_CREATE_OBJECT` | — | +1 | Push new empty object |
| 0x38 | `OP_CREATE_ARRAY` | count:u8 | varies | Push new array with count elements from stack |
| 0x39 | `OP_INIT_PROPERTY` | name:u8 | -1 | Initialize named property on TOS object |
| 0x3A | `OP_INIT_ELEMENT` | — | -2 | Initialize indexed element |
| 0x3B | `OP_SPREAD` | — | varies | Spread iterable onto stack |

### Control Flow (0x3C–0x40)

| Hex | Mnemonic | Operands | Stack Effect | Description |
|---|---|---|---|---|
| 0x3C | `OP_JUMP` | offset:u16 | 0 | Unconditional forward jump |
| 0x3D | `OP_JUMP_IF_FALSE` | offset:u16 | -1 | Jump if TOS is falsy |
| 0x3E | `OP_JUMP_IF_TRUE` | offset:u16 | -1 | Jump if TOS is truthy |
| 0x3F | `OP_JUMP_IF_NIL` | offset:u16 | 0 | Jump if TOS is null/undefined |
| 0x40 | `OP_LOOP` | offset:u16 | 0 | Backwards jump (loop) |

### Switch (0x41–0x42)

| Hex | Mnemonic | Operands | Stack Effect | Description |
|---|---|---|---|---|
| 0x41 | `OP_SWITCH` | count:u16 | 0 | Begin switch dispatch |
| 0x42 | `OP_CASE` | offset:u16 | -1 | Match case value, jump if equal |

### Functions (0x43–0x46)

| Hex | Mnemonic | Operands | Stack Effect | Description |
|---|---|---|---|---|
| 0x43 | `OP_CALL` | argc:u8 | -argc | Call function with argc arguments |
| 0x44 | `OP_CALL_METHOD` | argc:u8, name:u8 | -argc | Call method |
| 0x45 | `OP_RETURN` | — | varies | Return from current function |
| 0x46 | `OP_CLOSURE` | func:u8, upvals... | +1 | Create closure (upvalue count + descriptors follow) |

### Construction (0x47)

| Hex | Mnemonic | Operands | Stack Effect | Description |
|---|---|---|---|---|
| 0x47 | `OP_NEW` | argc:u8 | -argc | Construct new instance |

### Classes (0x48–0x4E)

| Hex | Mnemonic | Operands | Stack Effect | Description |
|---|---|---|---|---|
| 0x48 | `OP_INHERIT` | — | -1 | Set up prototype chain |
| 0x49 | `OP_DEFINE_METHOD` | name:u8 | -1 | Add method to prototype |
| 0x4A | `OP_DEFINE_STATIC` | name:u8 | -1 | Add static method |
| 0x4B | `OP_DEFINE_GETTER` | name:u8 | -1 | Define getter |
| 0x4C | `OP_DEFINE_SETTER` | name:u8 | -1 | Define setter |
| 0x4D | `OP_SUPER_CALL` | argc:u8 | -argc | Call super constructor |
| 0x4E | `OP_SUPER_GET` | name:u8 | +1 | Get property from super |

### Exception Handling (0x4F–0x53)

| Hex | Mnemonic | Operands | Stack Effect | Description |
|---|---|---|---|---|
| 0x4F | `OP_THROW` | — | -1 | Throw TOS as exception (captures stack trace) |
| 0x50 | `OP_TRY_BEGIN` | catch:u16, finally:u16 | 0 | Enter try block |
| 0x51 | `OP_TRY_END` | — | 0 | Exit try block |
| 0x52 | `OP_CATCH` | slot:u8 | +1 | Bind caught exception to local |
| 0x53 | `OP_FINALLY` | — | 0 | Enter finally block |

### Iterators (0x54–0x57)

| Hex | Mnemonic | Operands | Stack Effect | Description |
|---|---|---|---|---|
| 0x54 | `OP_GET_ITERATOR` | — | 0 | Get Symbol.iterator from TOS |
| 0x55 | `OP_ITERATOR_NEXT` | — | +1 | Advance iterator, push {value, done} |
| 0x56 | `OP_FOR_IN` | — | +1 | Create key array for for-in |
| 0x57 | `OP_FOR_OF` | — | varies | Iterate Array, String, or generic iterable |

### Generators / Async (0x58–0x59)

| Hex | Mnemonic | Operands | Stack Effect | Description |
|---|---|---|---|---|
| 0x58 | `OP_YIELD` | — | 0 | Yield value, suspend generator |
| 0x59 | `OP_AWAIT` | — | 0 | Await promise, drain microtask queue |

### Debug (0x5A–0x5B)

| Hex | Mnemonic | Operands | Stack Effect | Description |
|---|---|---|---|---|
| 0x5A | `OP_DEBUGGER` | — | 0 | Trigger debug breakpoint |
| 0x5B | `OP_LINE` | line:u16 | 0 | Record source line for debug/stack traces |

### Modules (0x5C–0x5E)

| Hex | Mnemonic | Operands | Stack Effect | Description |
|---|---|---|---|---|
| 0x5C | `OP_IMPORT` | specifier:u8 | +1 | Load module, push exports |
| 0x5D | `OP_EXPORT` | name:u8 | -1 | Register exported value |
| 0x5E | `OP_IMPORT_BINDING` | name:u8, slot:u8 | 0 | Bind imported name to local |

### End (0x5F)

| Hex | Mnemonic | Operands | Stack Effect | Description |
|---|---|---|---|---|
| 0x5F | `OP_END` | — | 0 | End of bytecode stream |

---

## 3. Value Representation

ZepraScript uses **NaN-boxing** on 64-bit platforms. All values are stored as 64-bit doubles with
special NaN payloads encoding non-double types:

```
Double:   standard IEEE 754 double (not a NaN)
Pointer:  0x7FF8_xxxx_xxxx_xxxx  (quiet NaN + 48-bit pointer)
Integer:  0x7FFC_xxxx_xxxx_xxxx  (quiet NaN + 32-bit int)
Boolean:  0x7FFE_0000_0000_000x  (0 = false, 1 = true)
Nil:      0x7FFE_0000_0000_0002
```

---

## 4. Constant Pool

Each `BytecodeChunk` carries a `std::vector<Value>` constant pool. Constants include:
- String literals (interned)
- Numeric literals
- Function objects (for closures)
- Regular expression objects

Constant index operands are u8 (256 max) for `OP_CONSTANT` and u16 for `OP_CONSTANT_LONG`.

---

## 5. GC Integration Points

- `OP_SET_PROPERTY` and `OP_SET_ELEMENT` trigger **write barriers** for cross-generation refs
- The VM dispatch loop calls `maybeCollect()` every ~1K instructions (GC safe-point)
- `OP_CLOSURE` allocates on the GC heap and registers upvalue roots

---
