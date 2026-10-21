// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file zepra-repl.cpp
 * @brief ZepraScript interactive REPL (Read-Eval-Print-Loop)
 */

#include <iostream>
#include <algorithm>
#include <string>
#include <sstream>
#include <cmath>
#include <chrono>

#include "config.hpp"
#include "frontend/source_code.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "frontend/token.hpp"
#include "bytecode/bytecode_generator.hpp"
#include "runtime/objects/value.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/async/promise.hpp"
#include "runtime/execution/vm.hpp"
#include "runtime/execution/context.hpp"
#include "runtime/execution/global_object.hpp"
#include "builtins/console.hpp"
#include "builtins/math.hpp"
#include "builtins/json.hpp"
#include "builtins/array.hpp"
#include "builtins/object_builtins.hpp"
#include "builtins/string.hpp"

using namespace Zepra;

void printBanner() {
    std::cout << "\n";
    std::cout << "+=================================================+\n";
    std::cout << "|          ZepraScript JavaScript Engine          |\n";
    std::cout << "|               Version " << ZEPRA_VERSION << "                    |\n";
    std::cout << "+=================================================+\n";
    std::cout << "\n";
    std::cout << "Type JavaScript code to evaluate.\n";
    std::cout << "Type '.help' for commands, '.exit' to quit.\n";
    std::cout << "\n";
}

void printHelp() {
    std::cout << "\nREPL Commands:\n";
    std::cout << "  .help     Show this help message\n";
    std::cout << "  .exit     Exit the REPL\n";
    std::cout << "  .clear    Clear the screen\n";
    std::cout << "  .tokens   Tokenize input (debug)\n";
    std::cout << "  .ast      Parse and show AST (debug)\n";
    std::cout << "  .bytecode Show bytecode (debug)\n";
    std::cout << "\n";
}

/**
 * @brief Set up global environment with builtins
 */
void setupGlobals(Runtime::VM& vm) {
    // Register console object
    Runtime::Object* consoleObj = Builtins::Console::createConsoleObject(nullptr);
    vm.setGlobal("console", Runtime::Value::object(consoleObj));
    
    // Register Math object
    Runtime::Object* mathObj = Builtins::MathBuiltin::createMathObject();
    vm.setGlobal("Math", Runtime::Value::object(mathObj));
    
    // Register JSON object
    Runtime::Object* jsonObj = Builtins::JSONBuiltin::createJSONObject(nullptr);
    vm.setGlobal("JSON", Runtime::Value::object(jsonObj));
    
    // Register undefined, NaN, Infinity
    vm.setGlobal("undefined", Runtime::Value::undefined());
    vm.setGlobal("NaN", Runtime::Value::number(std::nan("")));
    vm.setGlobal("Infinity", Runtime::Value::number(INFINITY));
    
    // Array constructor
    Runtime::Object* arrayPrototype = Builtins::ArrayBuiltin::createArrayPrototype(nullptr);
    Runtime::Function* arrayConstructor = Runtime::createNativeFunction("Array",
        [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
            if (args.empty()) {
                return Runtime::Value::object(new Runtime::Array({}));
            }
            if (args.size() == 1 && args[0].isNumber()) {
                size_t len = static_cast<size_t>(args[0].asNumber());
                std::vector<Runtime::Value> elements(len, Runtime::Value::undefined());
                return Runtime::Value::object(new Runtime::Array(std::move(elements)));
            }
            std::vector<Runtime::Value> elements;
            for (const auto& arg : args) elements.push_back(arg);
            return Runtime::Value::object(new Runtime::Array(std::move(elements)));
        }, 1);
    arrayConstructor->set("isArray", Runtime::Value::object(new Runtime::Function("isArray", Builtins::ArrayBuiltin::isArray, 1)));
    arrayConstructor->set("from", Runtime::Value::object(new Runtime::Function("from", Builtins::ArrayBuiltin::from, 1)));
    arrayConstructor->set("of", Runtime::Value::object(new Runtime::Function("of", Builtins::ArrayBuiltin::of, 0)));
    arrayConstructor->set("prototype", Runtime::Value::object(arrayPrototype));
    vm.setGlobal("Array", Runtime::Value::object(arrayConstructor));
    
    // Object constructor and static methods
    Runtime::Function* objectConstructor = Runtime::createNativeFunction("Object",
        [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
            if (args.empty() || args[0].isNull() || args[0].isUndefined()) {
                return Runtime::Value::object(new Runtime::Object());
            }
            return args[0];
        }, 1);
    objectConstructor->set("keys", Runtime::Value::object(new Runtime::Function("keys", Builtins::ObjectBuiltin::keys, 1)));
    objectConstructor->set("values", Runtime::Value::object(new Runtime::Function("values", Builtins::ObjectBuiltin::values, 1)));
    objectConstructor->set("entries", Runtime::Value::object(new Runtime::Function("entries", Builtins::ObjectBuiltin::entries, 1)));
    objectConstructor->set("assign", Runtime::Value::object(new Runtime::Function("assign", Builtins::ObjectBuiltin::assign, 2)));
    objectConstructor->set("freeze", Runtime::Value::object(new Runtime::Function("freeze", Builtins::ObjectBuiltin::freeze, 1)));
    objectConstructor->set("create", Runtime::Value::object(new Runtime::Function("create", Builtins::ObjectBuiltin::create, 2)));
    vm.setGlobal("Object", Runtime::Value::object(objectConstructor));
    
    // Global functions
    vm.setGlobal("isNaN", Runtime::Value::object(
        Runtime::createNativeFunction("isNaN",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                if (args.empty()) return Runtime::Value::boolean(true);
                return Runtime::Value::boolean(std::isnan(args[0].toNumber()));
            }, 1)));
    
    vm.setGlobal("isFinite", Runtime::Value::object(
        Runtime::createNativeFunction("isFinite",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                if (args.empty()) return Runtime::Value::boolean(false);
                return Runtime::Value::boolean(std::isfinite(args[0].toNumber()));
            }, 1)));
    
    vm.setGlobal("parseInt", Runtime::Value::object(
        Runtime::createNativeFunction("parseInt",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                if (args.empty()) return Runtime::Value::number(std::nan(""));
                std::string str = args[0].toString();
                int radix = args.size() > 1 ? static_cast<int>(args[1].toNumber()) : 10;
                try {
                    return Runtime::Value::number(static_cast<double>(std::stoll(str, nullptr, radix)));
                } catch (...) {
                    return Runtime::Value::number(std::nan(""));
                }
            }, 2)));
    
    vm.setGlobal("parseFloat", Runtime::Value::object(
        Runtime::createNativeFunction("parseFloat",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                if (args.empty()) return Runtime::Value::number(std::nan(""));
                try {
                    return Runtime::Value::number(std::stod(args[0].toString()));
                } catch (...) {
                    return Runtime::Value::number(std::nan(""));
                }
            }, 1)));
    
    // Boolean constructor
    vm.setGlobal("Boolean", Runtime::Value::object(
        Runtime::createNativeFunction("Boolean",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                bool val = !args.empty() && args[0].toBoolean();
                return Runtime::Value::boolean(val);
            }, 1)));
    
    // Number constructor
    Runtime::Function* numberConstructor = Runtime::createNativeFunction("Number",
        [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
            double val = args.empty() ? 0 : args[0].toNumber();
            return Runtime::Value::number(val);
        }, 1);
    numberConstructor->set("isNaN", Runtime::Value::object(
        Runtime::createNativeFunction("isNaN",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                if (args.empty() || !args[0].isNumber()) return Runtime::Value::boolean(false);
                return Runtime::Value::boolean(std::isnan(args[0].asNumber()));
            }, 1)));
    numberConstructor->set("isFinite", Runtime::Value::object(
        Runtime::createNativeFunction("isFinite",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                if (args.empty() || !args[0].isNumber()) return Runtime::Value::boolean(false);
                return Runtime::Value::boolean(std::isfinite(args[0].asNumber()));
            }, 1)));
    numberConstructor->set("isInteger", Runtime::Value::object(
        Runtime::createNativeFunction("isInteger",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                if (args.empty() || !args[0].isNumber()) return Runtime::Value::boolean(false);
                double n = args[0].asNumber();
                return Runtime::Value::boolean(std::isfinite(n) && std::floor(n) == n);
            }, 1)));
    numberConstructor->set("MAX_VALUE", Runtime::Value::number(1.7976931348623157e+308));
    numberConstructor->set("MIN_VALUE", Runtime::Value::number(5e-324));
    numberConstructor->set("POSITIVE_INFINITY", Runtime::Value::number(INFINITY));
    numberConstructor->set("NEGATIVE_INFINITY", Runtime::Value::number(-INFINITY));
    numberConstructor->set("NaN", Runtime::Value::number(std::nan("")));
    vm.setGlobal("Number", Runtime::Value::object(numberConstructor));
    
    // String constructor
    Runtime::Function* stringConstructor = Runtime::createNativeFunction("String",
        [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
            std::string val = args.empty() ? "" : args[0].toString();
            return Runtime::Value::string(new Runtime::String(val));
        }, 1);
    stringConstructor->set("fromCharCode", Runtime::Value::object(
        Runtime::createNativeFunction("fromCharCode",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                std::string result;
                for (const auto& arg : args) {
                    int code = static_cast<int>(arg.toNumber());
                    if (code >= 0 && code <= 0x10FFFF) {
                        result += static_cast<char>(code);
                    }
                }
                return Runtime::Value::string(new Runtime::String(result));
            }, 1)));
    vm.setGlobal("String", Runtime::Value::object(stringConstructor));
    
    // Function constructor (limited - eval security)
    vm.setGlobal("Function", Runtime::Value::object(
        Runtime::createNativeFunction("Function",
            [](Runtime::Context*, const std::vector<Runtime::Value>&) {
                // For security, we return an empty function
                return Runtime::Value::object(Runtime::createNativeFunction("anonymous",
                    [](Runtime::Context*, const std::vector<Runtime::Value>&) {
                        return Runtime::Value::undefined();
                    }, 0));
            }, 1)));
    
    // encodeURI / decodeURI
    vm.setGlobal("encodeURI", Runtime::Value::object(
        Runtime::createNativeFunction("encodeURI",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                if (args.empty()) return Runtime::Value::string(new Runtime::String("undefined"));
                std::string s = args[0].toString();
                std::string result;
                for (char c : s) {
                    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' ||
                        c == '!' || c == '\'' || c == '(' || c == ')' || c == '*' ||
                        c == ';' || c == ',' || c == '/' || c == '?' || c == ':' ||
                        c == '@' || c == '&' || c == '=' || c == '+' || c == '$' || c == '#') {
                        result += c;
                    } else {
                        char buf[4];
                        std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
                        result += buf;
                    }
                }
                return Runtime::Value::string(new Runtime::String(result));
            }, 1)));
    
    vm.setGlobal("decodeURI", Runtime::Value::object(
        Runtime::createNativeFunction("decodeURI",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                if (args.empty()) return Runtime::Value::string(new Runtime::String("undefined"));
                std::string s = args[0].toString();
                std::string result;
                for (size_t i = 0; i < s.size(); i++) {
                    if (s[i] == '%' && i + 2 < s.size()) {
                        int val = 0;
                        if (std::sscanf(s.c_str() + i + 1, "%2x", &val) == 1) {
                            result += static_cast<char>(val);
                            i += 2;
                            continue;
                        }
                    }
                    result += s[i];
                }
                return Runtime::Value::string(new Runtime::String(result));
            }, 1)));
    
    vm.setGlobal("encodeURIComponent", Runtime::Value::object(
        Runtime::createNativeFunction("encodeURIComponent",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                if (args.empty()) return Runtime::Value::string(new Runtime::String("undefined"));
                std::string s = args[0].toString();
                std::string result;
                for (char c : s) {
                    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' ||
                        c == '!' || c == '\'' || c == '(' || c == ')' || c == '*') {
                        result += c;
                    } else {
                        char buf[4];
                        std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
                        result += buf;
                    }
                }
                return Runtime::Value::string(new Runtime::String(result));
            }, 1)));
    
    vm.setGlobal("decodeURIComponent", Runtime::Value::object(
        Runtime::createNativeFunction("decodeURIComponent",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                if (args.empty()) return Runtime::Value::string(new Runtime::String("undefined"));
                std::string s = args[0].toString();
                std::string result;
                for (size_t i = 0; i < s.size(); i++) {
                    if (s[i] == '%' && i + 2 < s.size()) {
                        int val = 0;
                        if (std::sscanf(s.c_str() + i + 1, "%2x", &val) == 1) {
                            result += static_cast<char>(val);
                            i += 2;
                            continue;
                        }
                    }
                    result += s[i];
                }
                return Runtime::Value::string(new Runtime::String(result));
            }, 1)));
    
    // Error constructor
    vm.setGlobal("Error", Runtime::Value::object(
        Runtime::createNativeFunction("Error",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                Runtime::Object* err = new Runtime::Object();
                err->set("name", Runtime::Value::string(new Runtime::String("Error")));
                err->set("message", Runtime::Value::string(new Runtime::String(
                    args.empty() ? "" : args[0].toString())));
                return Runtime::Value::object(err);
            }, 1)));
    
    // TypeError constructor
    vm.setGlobal("TypeError", Runtime::Value::object(
        Runtime::createNativeFunction("TypeError",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                Runtime::Object* err = new Runtime::Object();
                err->set("name", Runtime::Value::string(new Runtime::String("TypeError")));
                err->set("message", Runtime::Value::string(new Runtime::String(
                    args.empty() ? "" : args[0].toString())));
                return Runtime::Value::object(err);
            }, 1)));
    
    // Date constructor (basic)
    vm.setGlobal("Date", Runtime::Value::object(
        Runtime::createNativeFunction("Date",
            [](Runtime::Context*, const std::vector<Runtime::Value>&) {
                Runtime::Object* date = new Runtime::Object();
                auto now = std::chrono::system_clock::now();
                auto epoch = now.time_since_epoch();
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();
                date->set("timestamp", Runtime::Value::number(static_cast<double>(ms)));
                return Runtime::Value::object(date);
            }, 0)));
    
    // RegExp constructor (basic)
    vm.setGlobal("RegExp", Runtime::Value::object(
        Runtime::createNativeFunction("RegExp",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                Runtime::Object* re = new Runtime::Object();
                re->set("source", Runtime::Value::string(new Runtime::String(
                    args.empty() ? "" : args[0].toString())));
                re->set("flags", Runtime::Value::string(new Runtime::String(
                    args.size() > 1 ? args[1].toString() : "")));
                return Runtime::Value::object(re);
            }, 2)));
    
    // Symbol constructor (basic)
    static int symbolCounter = 0;
    vm.setGlobal("Symbol", Runtime::Value::object(
        Runtime::createNativeFunction("Symbol",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                std::string desc = args.empty() ? "" : args[0].toString();
                Runtime::Object* sym = new Runtime::Object();
                sym->set("description", Runtime::Value::string(new Runtime::String(desc)));
                sym->set("__id__", Runtime::Value::number(++symbolCounter));
                return Runtime::Value::object(sym);
            }, 1)));
    
    // Map constructor (basic)
    vm.setGlobal("Map", Runtime::Value::object(
        Runtime::createNativeFunction("Map",
            [](Runtime::Context*, const std::vector<Runtime::Value>&) {
                Runtime::Object* map = new Runtime::Object();
                map->set("size", Runtime::Value::number(0));
                return Runtime::Value::object(map);
            }, 0)));
    
    // Set constructor (basic)
    vm.setGlobal("Set", Runtime::Value::object(
        Runtime::createNativeFunction("Set",
            [](Runtime::Context*, const std::vector<Runtime::Value>&) {
                Runtime::Object* set = new Runtime::Object();
                set->set("size", Runtime::Value::number(0));
                return Runtime::Value::object(set);
            }, 0)));
    
    // WeakMap constructor (basic)
    vm.setGlobal("WeakMap", Runtime::Value::object(
        Runtime::createNativeFunction("WeakMap",
            [](Runtime::Context*, const std::vector<Runtime::Value>&) {
                return Runtime::Value::object(new Runtime::Object());
            }, 0)));
    
    // WeakSet constructor (basic)
    vm.setGlobal("WeakSet", Runtime::Value::object(
        Runtime::createNativeFunction("WeakSet",
            [](Runtime::Context*, const std::vector<Runtime::Value>&) {
                return Runtime::Value::object(new Runtime::Object());
            }, 0)));
    
    // Promise constructor with static methods
    Runtime::Function* promiseCtor = Runtime::createNativeFunction("Promise",
        [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
            Runtime::Promise* promise = new Runtime::Promise();
            if (!args.empty() && args[0].isObject() && args[0].asObject()->isFunction()) {
                // Create resolve/reject callbacks for executor
                auto* resolveFn = Runtime::createNativeFunction("resolve",
                    [promise](Runtime::Context*, const std::vector<Runtime::Value>& resolveArgs) {
                        if (!resolveArgs.empty()) {
                            promise->resolve(resolveArgs[0]);
                        } else {
                            promise->resolve(Runtime::Value::undefined());
                        }
                        return Runtime::Value::undefined();
                    }, 1);
                auto* rejectFn = Runtime::createNativeFunction("reject",
                    [promise](Runtime::Context*, const std::vector<Runtime::Value>& rejectArgs) {
                        if (!rejectArgs.empty()) {
                            promise->reject(rejectArgs[0]);
                        } else {
                            promise->reject(Runtime::Value::undefined());
                        }
                        return Runtime::Value::undefined();
                    }, 1);
                // Execute the executor with (resolve, reject)
                auto* executor = dynamic_cast<Runtime::Function*>(args[0].asObject());
                if (executor) {
                    std::vector<Runtime::Value> executorArgs = {
                        Runtime::Value::object(resolveFn),
                        Runtime::Value::object(rejectFn)
                    };
                    Runtime::FunctionCallInfo info(nullptr, Runtime::Value::undefined(), executorArgs);
                    executor->builtinFunction()(info);
                }
            }
            return Runtime::Value::object(promise);
        }, 1);
    
    // Add Promise.resolve static method
    promiseCtor->set("resolve", Runtime::Value::object(
        Runtime::createNativeFunction("resolve",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                Runtime::Value value = args.empty() ? Runtime::Value::undefined() : args[0];
                return Runtime::Value::object(Runtime::Promise::resolved(value));
            }, 1)));
    
    // Add Promise.reject static method
    promiseCtor->set("reject", Runtime::Value::object(
        Runtime::createNativeFunction("reject",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                Runtime::Value reason = args.empty() ? Runtime::Value::undefined() : args[0];
                return Runtime::Value::object(Runtime::Promise::rejected(reason));
            }, 1)));
    
    // Add Promise.all static method
    promiseCtor->set("all", Runtime::Value::object(
        Runtime::createNativeFunction("all",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                // Basic implementation - return resolved promise for now
                return Runtime::Value::object(Runtime::Promise::resolved(Runtime::Value::object(new Runtime::Array())));
            }, 1)));
    
    // Add Promise.race static method
    promiseCtor->set("race", Runtime::Value::object(
        Runtime::createNativeFunction("race",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                // Basic implementation
                return Runtime::Value::object(new Runtime::Promise());
            }, 1)));
    
    vm.setGlobal("Promise", Runtime::Value::object(promiseCtor));
    
    // Proxy constructor (basic)
    vm.setGlobal("Proxy", Runtime::Value::object(
        Runtime::createNativeFunction("Proxy",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                if (args.empty()) return Runtime::Value::object(new Runtime::Object());
                return args[0];  // Return target for now
            }, 2)));
    
    // Reflect object
    Runtime::Object* reflectObj = new Runtime::Object();
    reflectObj->set("get", Runtime::Value::object(
        Runtime::createNativeFunction("get",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                if (args.size() < 2 || !args[0].isObject()) return Runtime::Value::undefined();
                return args[0].asObject()->get(args[1].toString());
            }, 2)));
    reflectObj->set("set", Runtime::Value::object(
        Runtime::createNativeFunction("set",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                if (args.size() < 3 || !args[0].isObject()) return Runtime::Value::boolean(false);
                args[0].asObject()->set(args[1].toString(), args[2]);
                return Runtime::Value::boolean(true);
            }, 3)));
    reflectObj->set("has", Runtime::Value::object(
        Runtime::createNativeFunction("has",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                if (args.size() < 2 || !args[0].isObject()) return Runtime::Value::boolean(false);
                return Runtime::Value::boolean(!args[0].asObject()->get(args[1].toString()).isUndefined());
            }, 2)));
    vm.setGlobal("Reflect", Runtime::Value::object(reflectObj));
    
    // BigInt constructor (basic)
    vm.setGlobal("BigInt", Runtime::Value::object(
        Runtime::createNativeFunction("BigInt",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                if (args.empty()) return Runtime::Value::number(0);
                return Runtime::Value::number(std::floor(args[0].toNumber()));
            }, 1)));
    
    // ArrayBuffer constructor
    vm.setGlobal("ArrayBuffer", Runtime::Value::object(
        Runtime::createNativeFunction("ArrayBuffer",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                Runtime::Object* ab = new Runtime::Object();
                size_t size = args.empty() ? 0 : static_cast<size_t>(args[0].toNumber());
                ab->set("byteLength", Runtime::Value::number(static_cast<double>(size)));
                return Runtime::Value::object(ab);
            }, 1)));
    
    // DataView constructor
    vm.setGlobal("DataView", Runtime::Value::object(
        Runtime::createNativeFunction("DataView",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                Runtime::Object* dv = new Runtime::Object();
                if (!args.empty() && args[0].isObject()) {
                    dv->set("buffer", args[0]);
                }
                return Runtime::Value::object(dv);
            }, 1)));
    
    // Uint8Array constructor
    vm.setGlobal("Uint8Array", Runtime::Value::object(
        Runtime::createNativeFunction("Uint8Array",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                Runtime::Object* arr = new Runtime::Object();
                size_t len = args.empty() ? 0 : static_cast<size_t>(args[0].toNumber());
                arr->set("length", Runtime::Value::number(static_cast<double>(len)));
                arr->set("BYTES_PER_ELEMENT", Runtime::Value::number(1));
                return Runtime::Value::object(arr);
            }, 1)));
    
    // Int32Array constructor
    vm.setGlobal("Int32Array", Runtime::Value::object(
        Runtime::createNativeFunction("Int32Array",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                Runtime::Object* arr = new Runtime::Object();
                size_t len = args.empty() ? 0 : static_cast<size_t>(args[0].toNumber());
                arr->set("length", Runtime::Value::number(static_cast<double>(len)));
                arr->set("BYTES_PER_ELEMENT", Runtime::Value::number(4));
                return Runtime::Value::object(arr);
            }, 1)));
    
    // Float64Array constructor
    vm.setGlobal("Float64Array", Runtime::Value::object(
        Runtime::createNativeFunction("Float64Array",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                Runtime::Object* arr = new Runtime::Object();
                size_t len = args.empty() ? 0 : static_cast<size_t>(args[0].toNumber());
                arr->set("length", Runtime::Value::number(static_cast<double>(len)));
                arr->set("BYTES_PER_ELEMENT", Runtime::Value::number(8));
                return Runtime::Value::object(arr);
            }, 1)));
    
    // setTimeout (simplified - stores in pending list, no actual async)
    static int timerId = 0;
    vm.setGlobal("setTimeout", Runtime::Value::object(
        Runtime::createNativeFunction("setTimeout",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                // Return a timer ID (actual async would need event loop)
                return Runtime::Value::number(++timerId);
            }, 2)));
    
    // setInterval (simplified)
    vm.setGlobal("setInterval", Runtime::Value::object(
        Runtime::createNativeFunction("setInterval",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                return Runtime::Value::number(++timerId);
            }, 2)));
    
    // clearTimeout
    vm.setGlobal("clearTimeout", Runtime::Value::object(
        Runtime::createNativeFunction("clearTimeout",
            [](Runtime::Context*, const std::vector<Runtime::Value>&) {
                return Runtime::Value::undefined();
            }, 1)));
    
    // clearInterval
    vm.setGlobal("clearInterval", Runtime::Value::object(
        Runtime::createNativeFunction("clearInterval",
            [](Runtime::Context*, const std::vector<Runtime::Value>&) {
                return Runtime::Value::undefined();
            }, 1)));
    
    // URL constructor
    vm.setGlobal("URL", Runtime::Value::object(
        Runtime::createNativeFunction("URL",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                Runtime::Object* url = new Runtime::Object();
                if (!args.empty()) {
                    std::string urlStr = args[0].toString();
                    url->set("href", Runtime::Value::string(new Runtime::String(urlStr)));
                    // Parse basic URL components
                    size_t protocolEnd = urlStr.find("://");
                    if (protocolEnd != std::string::npos) {
                        url->set("protocol", Runtime::Value::string(new Runtime::String(urlStr.substr(0, protocolEnd + 1))));
                        size_t hostStart = protocolEnd + 3;
                        size_t pathStart = urlStr.find('/', hostStart);
                        if (pathStart != std::string::npos) {
                            url->set("host", Runtime::Value::string(new Runtime::String(urlStr.substr(hostStart, pathStart - hostStart))));
                            url->set("pathname", Runtime::Value::string(new Runtime::String(urlStr.substr(pathStart))));
                        } else {
                            url->set("host", Runtime::Value::string(new Runtime::String(urlStr.substr(hostStart))));
                            url->set("pathname", Runtime::Value::string(new Runtime::String("/")));
                        }
                    }
                }
                return Runtime::Value::object(url);
            }, 1)));
    
    // TextEncoder
    vm.setGlobal("TextEncoder", Runtime::Value::object(
        Runtime::createNativeFunction("TextEncoder",
            [](Runtime::Context*, const std::vector<Runtime::Value>&) {
                Runtime::Object* encoder = new Runtime::Object();
                encoder->set("encoding", Runtime::Value::string(new Runtime::String("utf-8")));
                encoder->set("encode", Runtime::Value::object(
                    Runtime::createNativeFunction("encode",
                        [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                            std::string str = args.empty() ? "" : args[0].toString();
                            Runtime::Object* arr = new Runtime::Object();
                            arr->set("length", Runtime::Value::number(static_cast<double>(str.size())));
                            arr->set("byteLength", Runtime::Value::number(static_cast<double>(str.size())));
                            return Runtime::Value::object(arr);
                        }, 1)));
                return Runtime::Value::object(encoder);
            }, 0)));
    
    // TextDecoder
    vm.setGlobal("TextDecoder", Runtime::Value::object(
        Runtime::createNativeFunction("TextDecoder",
            [](Runtime::Context*, const std::vector<Runtime::Value>&) {
                Runtime::Object* decoder = new Runtime::Object();
                decoder->set("encoding", Runtime::Value::string(new Runtime::String("utf-8")));
                decoder->set("decode", Runtime::Value::object(
                    Runtime::createNativeFunction("decode",
                        [](Runtime::Context*, const std::vector<Runtime::Value>&) {
                            return Runtime::Value::string(new Runtime::String(""));
                        }, 1)));
                return Runtime::Value::object(decoder);
            }, 0)));
    
    // Blob constructor
    vm.setGlobal("Blob", Runtime::Value::object(
        Runtime::createNativeFunction("Blob",
            [](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                Runtime::Object* blob = new Runtime::Object();
                blob->set("size", Runtime::Value::number(0));
                blob->set("type", Runtime::Value::string(new Runtime::String(
                    args.size() > 1 && args[1].isObject() ? 
                        args[1].asObject()->get("type").toString() : "")));
                return Runtime::Value::object(blob);
            }, 2)));
    
    // FormData constructor
    vm.setGlobal("FormData", Runtime::Value::object(
        Runtime::createNativeFunction("FormData",
            [](Runtime::Context*, const std::vector<Runtime::Value>&) {
                Runtime::Object* fd = new Runtime::Object();
                fd->set("append", Runtime::Value::object(
                    Runtime::createNativeFunction("append",
                        [](Runtime::Context*, const std::vector<Runtime::Value>&) {
                            return Runtime::Value::undefined();
                        }, 2)));
                return Runtime::Value::object(fd);
            }, 0)));
    
    // AbortController
    vm.setGlobal("AbortController", Runtime::Value::object(
        Runtime::createNativeFunction("AbortController",
            [](Runtime::Context*, const std::vector<Runtime::Value>&) {
                Runtime::Object* ac = new Runtime::Object();
                Runtime::Object* signal = new Runtime::Object();
                signal->set("aborted", Runtime::Value::boolean(false));
                ac->set("signal", Runtime::Value::object(signal));
                ac->set("abort", Runtime::Value::object(
                    Runtime::createNativeFunction("abort",
                        [](Runtime::Context*, const std::vector<Runtime::Value>&) {
                            return Runtime::Value::undefined();
                        }, 0)));
                return Runtime::Value::object(ac);
            }, 0)));
    
    // fetch (simplified - returns resolved promise-like object)
    vm.setGlobal("fetch", Runtime::Value::object(
        Runtime::createNativeFunction("fetch",
            [](Runtime::Context*, const std::vector<Runtime::Value>&) {
                Runtime::Object* response = new Runtime::Object();
                response->set("ok", Runtime::Value::boolean(true));
                response->set("status", Runtime::Value::number(200));
                response->set("json", Runtime::Value::object(
                    Runtime::createNativeFunction("json",
                        [](Runtime::Context*, const std::vector<Runtime::Value>&) {
                            return Runtime::Value::object(new Runtime::Object());
                        }, 0)));
                response->set("text", Runtime::Value::object(
                    Runtime::createNativeFunction("text",
                        [](Runtime::Context*, const std::vector<Runtime::Value>&) {
                            return Runtime::Value::string(new Runtime::String(""));
                        }, 0)));
                // Return promise-like with then
                Runtime::Object* promise = new Runtime::Object();
                promise->set("then", Runtime::Value::object(
                    Runtime::createNativeFunction("then",
                        [response](Runtime::Context*, const std::vector<Runtime::Value>& args) {
                            if (!args.empty() && args[0].isObject() && args[0].asObject()->isFunction()) {
                                Runtime::Function* fn = static_cast<Runtime::Function*>(args[0].asObject());
                                std::vector<Runtime::Value> callArgs = {Runtime::Value::object(response)};
                                return fn->call(nullptr, Runtime::Value::undefined(), callArgs);
                            }
                            return Runtime::Value::object(response);
                        }, 1)));
                return Runtime::Value::object(promise);
            }, 1)));
}

/**
 * @brief Tokenize and display tokens
 */
void tokenize(const std::string& code) {
    auto source = Frontend::SourceCode::fromString(code, "<repl>");
    Frontend::Lexer lexer(source.get());
    
    std::cout << "Tokens:\n";
    while (!lexer.isEof()) {
        Frontend::Token token = lexer.nextToken();
        std::cout << "  [" << Frontend::Token::typeName(token.type) << "]";
        if (!token.value.empty()) {
            std::cout << " '" << token.value << "'";
        }
        if (token.type == Frontend::TokenType::Number) {
            std::cout << " = " << token.numericValue;
        }
        std::cout << " @ " << token.start.line << ":" << token.start.column;
        std::cout << "\n";
        
        if (token.type == Frontend::TokenType::EndOfFile ||
            token.type == Frontend::TokenType::Error) {
            break;
        }
    }
    
    if (lexer.hasErrors()) {
        std::cout << "\nErrors:\n";
        for (const auto& err : lexer.errors()) {
            std::cout << "  " << err << "\n";
        }
    }
}

/**
 * @brief Evaluate JavaScript code using full pipeline
 */
void evaluate(const std::string& code, Runtime::VM& vm) {
    try {
        auto t0 = std::chrono::steady_clock::now();
        
        // 1. Parse source code to AST
        auto source = Frontend::SourceCode::fromString(code, "<repl>");
        Frontend::Parser parser(source.get());
        
        auto program = parser.parseProgram();
        
        auto t1 = std::chrono::steady_clock::now();
        fprintf(stderr, "[phase] parse: %ldms, stmts=%zu\n",
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
            program ? program->body().size() : 0);
        
        if (parser.hasErrors()) {
            for (const auto& err : parser.errors()) {
                std::cerr << "Parse error: " << err << "\n";
            }
            return;
        }
        
        // 2. Compile AST to bytecode
        Bytecode::BytecodeGenerator generator;
        auto chunk = generator.compile(program.get());
        
        auto t2 = std::chrono::steady_clock::now();
        fprintf(stderr, "[phase] compile: %ldms, bytecode=%zu bytes\n",
            std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count(),
            chunk ? chunk->code().size() : 0);
        
        if (generator.hasErrors()) {
            for (const auto& err : generator.errors()) {
                std::cerr << "Compile error: " << err << "\n";
            }
            return;
        }
        
        // 3. Execute bytecode
        Runtime::ExecutionResult result = vm.execute(chunk.get());
        
        auto t3 = std::chrono::steady_clock::now();
        fprintf(stderr, "[phase] execute: %ldms\n",
            std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count());
        
        if (result.status == Runtime::ExecutionResult::Status::Error) {
            std::cerr << "Runtime error: " << result.error << "\n";
            return;
        }
        
        // 4. Print result
        if (!result.value.isUndefined()) {
            std::cout << result.value.toString() << "\n";
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
    }
}

int main(int argc, char* argv[]) {
    // Create VM
    Runtime::VM vm(nullptr);
    setupGlobals(vm);

    // The engine's standard library replaces the REPL's minimal stand-ins
    // above (the REPL keeps its own console)
    Runtime::Context context(&vm);
    auto* globalObject = new Runtime::GlobalObject(&context);
    globalObject->initialize();
    globalObject->installInto(vm, {"console"});
    
    // Check for file argument
    if (argc > 1) {
        std::string filename = argv[1];
        auto source = Frontend::SourceCode::fromFile(filename);
        if (!source) {
            std::cerr << "Error: Could not read file '" << filename << "'\n";
            return 1;
        }
        
        evaluate(source->content(), vm);
        return 0;
    }
    
    printBanner();
    
    std::string line;
    std::string multilineBuffer;
    bool inMultiline = false;
    
    while (true) {
        // Print prompt
        if (inMultiline) {
            std::cout << "... ";
        } else {
            std::cout << "zepra> ";
        }
        std::cout.flush();
        
        if (!std::getline(std::cin, line)) {
            std::cout << "\n";
            break;
        }
        
        // Handle commands
        if (line == ".exit" || line == ".quit") {
            std::cout << "Goodbye!\n";
            break;
        }
        
        if (line == ".help") {
            printHelp();
            continue;
        }
        
        if (line == ".clear") {
            std::cout << "\033[2J\033[H";
            continue;
        }
        
        if (line.rfind(".tokens ", 0) == 0) {
            tokenize(line.substr(8));
            continue;
        }
        
        if (line.empty()) {
            continue;
        }
        
        // Handle multiline input (check for unclosed braces)
        int braceCount = 0;
        for (char c : line) {
            if (c == '{' || c == '(' || c == '[') braceCount++;
            if (c == '}' || c == ')' || c == ']') braceCount--;
        }
        
        if (inMultiline) {
            multilineBuffer += "\n" + line;
            for (char c : line) {
                if (c == '{' || c == '(' || c == '[') braceCount++;
                if (c == '}' || c == ')' || c == ']') braceCount--;
            }
            if (braceCount <= 0) {
                inMultiline = false;
                evaluate(multilineBuffer, vm);
                multilineBuffer.clear();
            }
        } else if (braceCount > 0) {
            inMultiline = true;
            multilineBuffer = line;
        } else {
            evaluate(line, vm);
        }
    }
    
    return 0;
}
