// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file json.cpp
 * @brief JSON builtin implementation (JSON.parse, JSON.stringify)
 */

#include "builtins/json.hpp"
#include <cmath>
#include "builtins/boolean.hpp"
#include "builtins/number.hpp"
#include "runtime/objects/number_conversion.hpp"
#include "runtime/execution/js_error.hpp"
#include "runtime/execution/vm.hpp"
#include <algorithm>
#include "runtime/objects/object.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/value.hpp"
#include <sstream>
#include <iomanip>
#include <cctype>
#include <stack>
#include <stdexcept>

namespace Zepra::Builtins {

// ============================================================================
// JSON Parser (Tokenizer + Recursive Descent)
// ============================================================================

enum class JsonTokenType {
    LeftBrace,    // {
    RightBrace,   // }
    LeftBracket,  // [
    RightBracket, // ]
    Colon,        // :
    Comma,        // ,
    String,
    Number,
    True,
    False,
    Null,
    EndOfInput,
    Error
};

struct JsonToken {
    JsonTokenType type;
    std::string value;
    double numValue = 0.0;
};

class JsonLexer {
public:
    explicit JsonLexer(const std::string& input) : input_(input), pos_(0) {}
    
    JsonToken nextToken() {
        skipWhitespace();
        
        if (pos_ >= input_.size()) {
            return {JsonTokenType::EndOfInput, ""};
        }
        
        char c = input_[pos_];
        
        switch (c) {
            case '{': pos_++; return {JsonTokenType::LeftBrace, "{"};
            case '}': pos_++; return {JsonTokenType::RightBrace, "}"};
            case '[': pos_++; return {JsonTokenType::LeftBracket, "["};
            case ']': pos_++; return {JsonTokenType::RightBracket, "]"};
            case ':': pos_++; return {JsonTokenType::Colon, ":"};
            case ',': pos_++; return {JsonTokenType::Comma, ","};
            case '"': return scanString();
            case 't': return scanKeyword("true", JsonTokenType::True);
            case 'f': return scanKeyword("false", JsonTokenType::False);
            case 'n': return scanKeyword("null", JsonTokenType::Null);
            default:
                if (c == '-' || std::isdigit(c)) {
                    return scanNumber();
                }
                return {JsonTokenType::Error, std::string("Unexpected character: ") + c};
        }
    }
    
private:
    void skipWhitespace() {
        while (pos_ < input_.size() && std::isspace(input_[pos_])) {
            pos_++;
        }
    }
    
    JsonToken scanString() {
        pos_++; // Skip opening quote
        std::string result;
        
        while (pos_ < input_.size() && input_[pos_] != '"') {
            if (input_[pos_] == '\\') {
                pos_++;
                if (pos_ >= input_.size()) break;
                
                switch (input_[pos_]) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case '/': result += '/'; break;
                    case 'b': result += '\b'; break;
                    case 'f': result += '\f'; break;
                    case 'n': result += '\n'; break;
                    case 'r': result += '\r'; break;
                    case 't': result += '\t'; break;
                    case 'u': {
                        // Unicode escape
                        if (pos_ + 4 >= input_.size()) {
                            return {JsonTokenType::Error, "Invalid unicode escape"};
                        }
                        std::string hex = input_.substr(pos_ + 1, 4);
                        int codePoint = std::stoi(hex, nullptr, 16);
                        if (codePoint < 128) {
                            result += static_cast<char>(codePoint);
                        } else {
                            // Basic UTF-8 encoding for common characters
                            result += static_cast<char>(codePoint);
                        }
                        pos_ += 4;
                        break;
                    }
                    default:
                        result += input_[pos_];
                }
            } else {
                result += input_[pos_];
            }
            pos_++;
        }
        
        if (pos_ >= input_.size()) {
            return {JsonTokenType::Error, "Unterminated string"};
        }
        
        pos_++; // Skip closing quote
        return {JsonTokenType::String, result};
    }
    
    JsonToken scanNumber() {
        size_t start = pos_;
        
        if (input_[pos_] == '-') pos_++;
        
        while (pos_ < input_.size() && std::isdigit(input_[pos_])) {
            pos_++;
        }
        
        if (pos_ < input_.size() && input_[pos_] == '.') {
            pos_++;
            while (pos_ < input_.size() && std::isdigit(input_[pos_])) {
                pos_++;
            }
        }
        
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            pos_++;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) {
                pos_++;
            }
            while (pos_ < input_.size() && std::isdigit(input_[pos_])) {
                pos_++;
            }
        }
        
        std::string numStr = input_.substr(start, pos_ - start);
        JsonToken token{JsonTokenType::Number, numStr};
        token.numValue = std::stod(numStr);
        return token;
    }
    
    JsonToken scanKeyword(const std::string& keyword, JsonTokenType type) {
        if (input_.substr(pos_, keyword.size()) == keyword) {
            pos_ += keyword.size();
            return {type, keyword};
        }
        return {JsonTokenType::Error, "Invalid keyword"};
    }
    
    std::string input_;
    size_t pos_;
};

class JsonParser {
public:
    explicit JsonParser(const std::string& input) : lexer_(input) {
        advance();
    }
    
    Runtime::Value parse() {
        Runtime::Value result = parseValue();
        if (current_.type != JsonTokenType::EndOfInput) {
            throw std::runtime_error("Unexpected token after JSON value");
        }
        return result;
    }
    
private:
    void advance() {
        current_ = lexer_.nextToken();
        if (current_.type == JsonTokenType::Error) {
            throw std::runtime_error(current_.value);
        }
    }
    
    Runtime::Value parseValue() {
        switch (current_.type) {
            case JsonTokenType::LeftBrace:
                return parseObject();
            case JsonTokenType::LeftBracket:
                return parseArray();
            case JsonTokenType::String:
                return parseString();
            case JsonTokenType::Number:
                return parseNumber();
            case JsonTokenType::True:
                advance();
                return Runtime::Value::boolean(true);
            case JsonTokenType::False:
                advance();
                return Runtime::Value::boolean(false);
            case JsonTokenType::Null:
                advance();
                return Runtime::Value::null();
            default:
                throw std::runtime_error("Unexpected token in JSON");
        }
    }
    
    Runtime::Value parseObject() {
        advance(); // Skip {
        
        // An ordinary object (ES2024 §25.5.1: OrdinaryObjectCreate(%Object.prototype%))
        Runtime::Object* obj = new Runtime::Object();
        if (Runtime::VM* vm = Runtime::VM::current()) obj->setPrototype(vm->intrinsic(Runtime::VM::Intrinsic::Object));
        
        if (current_.type == JsonTokenType::RightBrace) {
            advance();
            return Runtime::Value::object(obj);
        }
        
        while (true) {
            if (current_.type != JsonTokenType::String) {
                throw std::runtime_error("Expected string key in object");
            }
            std::string key = current_.value;
            advance();
            
            if (current_.type != JsonTokenType::Colon) {
                throw std::runtime_error("Expected ':' after key");
            }
            advance();
            
            Runtime::Value value = parseValue();
            obj->set(key, value);
            
            if (current_.type == JsonTokenType::RightBrace) {
                advance();
                break;
            }
            
            if (current_.type != JsonTokenType::Comma) {
                throw std::runtime_error("Expected ',' or '}' in object");
            }
            advance();
        }
        
        return Runtime::Value::object(obj);
    }
    
    Runtime::Value parseArray() {
        advance(); // Skip [
        
        std::vector<Runtime::Value> elements;
        
        if (current_.type == JsonTokenType::RightBracket) {
            advance();
            return Runtime::Value::object(new Runtime::Array(std::move(elements)));
        }
        
        while (true) {
            elements.push_back(parseValue());
            
            if (current_.type == JsonTokenType::RightBracket) {
                advance();
                break;
            }
            
            if (current_.type != JsonTokenType::Comma) {
                throw std::runtime_error("Expected ',' or ']' in array");
            }
            advance();
        }
        
        return Runtime::Value::object(new Runtime::Array(std::move(elements)));
    }
    
    Runtime::Value parseString() {
        std::string value = current_.value;
        advance();
        return Runtime::Value::string(new Runtime::String(value));
    }
    
    Runtime::Value parseNumber() {
        double value = current_.numValue;
        advance();
        return Runtime::Value::number(value);
    }
    
    JsonLexer lexer_;
    JsonToken current_;
};

// ============================================================================
// JSON.stringify implementation
// ============================================================================

// JSON.stringify per ECMA-262 SerializeJSONProperty: toJSON, the replacer
// (function or property list), the gap, getters (read through the VM),
// boxed primitives, shortest round-trip numbers, cycle detection, and
// lone surrogates written as \uXXXX escapes.
class JsonStringifier {
public:
    JsonStringifier(std::string gap, Runtime::Value replacerFn, std::vector<std::string> propertyList, bool hasList)
        : gap_(std::move(gap)), replacer_(replacerFn), propertyList_(std::move(propertyList)), hasList_(hasList) {}

    // false: the result is undefined
    bool stringify(Runtime::Value value, std::string& out) {
        auto* wrapper = new Runtime::Object();
        wrapper->set("", value);
        return serializeProperty(Runtime::Value::object(wrapper), "", value, out);
    }

private:
    Runtime::Value get(const Runtime::Value& holder, const std::string& key) {
        if (Runtime::VM* vm = Runtime::VM::current()) return vm->getMember(holder, key);
        return holder.asObject()->get(key);
    }

    Runtime::Value call(const Runtime::Value& fn, const Runtime::Value& self, std::vector<Runtime::Value> args) {
        Runtime::VM* vm = Runtime::VM::current();
        if (!vm) return Runtime::Value::undefined();
        return vm->executeCallback(static_cast<Runtime::Function*>(fn.asObject()), self, args);
    }

    static bool isCallable(const Runtime::Value& v) { return v.isObject() && v.asObject()->isFunction(); }

    bool serializeProperty(const Runtime::Value& holder, const std::string& key, Runtime::Value value, std::string& out) {
        if (value.isObject()) {
            Runtime::Value toJSON = get(value, "toJSON");
            if (isCallable(toJSON)) value = call(toJSON, value, {Runtime::Value::string(new Runtime::String(key))});
        }
        if (isCallable(replacer_)) {
            value = call(replacer_, holder, {Runtime::Value::string(new Runtime::String(key)), value});
        }
        if (value.isObject()) {
            Runtime::Object* obj = value.asObject();
            if (auto* n = dynamic_cast<NumberObject*>(obj)) value = Runtime::Value::number(n->valueOf());
            else if (auto* b = dynamic_cast<BooleanObject*>(obj)) value = Runtime::Value::boolean(b->valueOf());
            else if (auto* str = dynamic_cast<Runtime::String*>(obj)) value = Runtime::Value::string(new Runtime::String(str->value()));
        }
        if (value.isNull()) { out += "null"; return true; }
        if (value.isBoolean()) { out += value.asBoolean() ? "true" : "false"; return true; }
        if (value.isString()) { quote(value.asString()->value(), out); return true; }
        if (value.isNumber()) {
            const double d = value.asNumber();
            out += std::isfinite(d) ? Runtime::NumberConversion::toString(d) : "null";
            return true;
        }
        if (value.isObject() && !value.asObject()->isFunction()) {
            Runtime::Object* obj = value.asObject();
            if (std::find(stack_.begin(), stack_.end(), obj) != stack_.end()) {
                throw Runtime::JSError("TypeError", "Converting circular structure to JSON");
            }
            if (stack_.size() > 5000) throw Runtime::JSError("RangeError", "Maximum call stack size exceeded");
            stack_.push_back(obj);
            if (obj->isArray()) serializeArray(value, out);
            else serializeObject(value, out);
            stack_.pop_back();
            return true;
        }
        return false;  // undefined, functions, symbols
    }

    void newline(std::string& out) {
        if (gap_.empty()) return;
        out += '\n';
        for (size_t i = 0; i < stack_.size(); ++i) out += gap_;
    }

    void serializeObject(const Runtime::Value& value, std::string& out) {
        std::vector<std::string> keys = hasList_ ? propertyList_ : value.asObject()->keys();
        out += '{';
        bool any = false;
        for (const auto& key : keys) {
            std::string member;
            if (!serializeProperty(value, key, get(value, key), member)) continue;
            if (any) out += ',';
            newline(out);
            quote(key, out);
            out += gap_.empty() ? ":" : ": ";
            out += member;
            any = true;
        }
        if (any) {
            stack_.pop_back();
            newline(out);
            stack_.push_back(value.asObject());
        }
        out += '}';
    }

    void serializeArray(const Runtime::Value& value, std::string& out) {
        const size_t length = static_cast<size_t>(get(value, "length").toNumber());
        out += '[';
        for (size_t i = 0; i < length; ++i) {
            if (i) out += ',';
            newline(out);
            const std::string key = std::to_string(i);
            if (!serializeProperty(value, key, value.asObject()->get(static_cast<uint32_t>(i)), out)) out += "null";
        }
        if (length) {
            stack_.pop_back();
            newline(out);
            stack_.push_back(value.asObject());
        }
        out += ']';
    }

    static void quote(const std::string& text, std::string& out) {
        static const char* hex = "0123456789abcdef";
        out += '"';
        for (size_t i = 0; i < text.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        out += "\\u00";
                        out += hex[c >> 4];
                        out += hex[c & 15];
                    } else if (c == 0xED && i + 2 < text.size() &&
                               (static_cast<unsigned char>(text[i + 1]) & 0xE0) == 0xA0) {
                        // A lone surrogate (WTF-8): well-formed JSON.stringify escapes it
                        const unsigned unit = 0xD000 | ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6) |
                                              (static_cast<unsigned char>(text[i + 2]) & 0x3F);
                        out += "\\u";
                        out += hex[(unit >> 12) & 15];
                        out += hex[(unit >> 8) & 15];
                        out += hex[(unit >> 4) & 15];
                        out += hex[unit & 15];
                        i += 2;
                    } else {
                        out += static_cast<char>(c);
                    }
            }
        }
        out += '"';
    }

    std::string gap_;
    Runtime::Value replacer_;
    std::vector<std::string> propertyList_;
    bool hasList_;
    std::vector<Runtime::Object*> stack_;
};

// ============================================================================
// JSON builtin methods
// ============================================================================

namespace {
// JSON.parse's reviver walk (InternalizeJSONProperty), children first
Runtime::Value internalize(Runtime::VM* vm, Runtime::Function* reviver, const Runtime::Value& holder,
                           const std::string& key, int depth) {
    Runtime::Value value = vm->getMember(holder, key);
    if (value.isObject() && depth < 5000) {
        Runtime::Object* obj = value.asObject();
        std::vector<std::string> keys;
        if (obj->isArray()) {
            for (size_t i = 0; i < obj->length(); ++i) keys.push_back(std::to_string(i));
        } else {
            keys = obj->keys();
        }
        for (const auto& k : keys) {
            Runtime::Value element = internalize(vm, reviver, value, k, depth + 1);
            if (element.isUndefined()) {
                obj->deleteProperty(k);
            } else if (obj->isArray()) {
                obj->set(static_cast<uint32_t>(std::stoul(k)), element);
            } else {
                obj->set(k, element);
            }
        }
    }
    return vm->executeCallback(reviver, holder, {Runtime::Value::string(new Runtime::String(key)), value});
}
} // namespace

Runtime::Value JSONBuiltin::parse(const Runtime::FunctionCallInfo& info) {
    std::string jsonStr = info.argument(0).toString();
    Runtime::Value result;
    try {
        JsonParser parser(jsonStr);
        result = parser.parse();
    } catch (const Runtime::JSError&) {
        throw;
    } catch (const std::exception& e) {
        throw Runtime::JSError("SyntaxError", std::string(e.what()));
    }
    const Runtime::Value reviver = info.argument(1);
    Runtime::VM* vm = Runtime::VM::current();
    if (vm && reviver.isObject() && reviver.asObject()->isFunction()) {
        auto* root = new Runtime::Object();
        root->set("", result);
        return internalize(vm, static_cast<Runtime::Function*>(reviver.asObject()), Runtime::Value::object(root), "", 0);
    }
    return result;
}

Runtime::Value JSONBuiltin::stringify(const Runtime::FunctionCallInfo& info) {
    const Runtime::Value replacer = info.argument(1);
    Runtime::Value replacerFn = Runtime::Value::undefined();
    std::vector<std::string> list;
    bool hasList = false;
    if (replacer.isObject()) {
        if (replacer.asObject()->isFunction()) {
            replacerFn = replacer;
        } else if (auto* array = dynamic_cast<Runtime::Array*>(replacer.asObject())) {
            hasList = true;
            for (size_t i = 0; i < array->length(); ++i) {
                Runtime::Value item = array->at(i);
                if (!(item.isString() || item.isNumber() || item.isObject())) continue;
                std::string name = item.toString();
                if (std::find(list.begin(), list.end(), name) == list.end()) list.push_back(name);
            }
        }
    }
    Runtime::Value space = info.argument(2);
    if (space.isObject()) {
        if (auto* n = dynamic_cast<NumberObject*>(space.asObject())) space = Runtime::Value::number(n->valueOf());
        else if (auto* str = dynamic_cast<Runtime::String*>(space.asObject())) space = Runtime::Value::string(new Runtime::String(str->value()));
    }
    std::string gap;
    if (space.isNumber()) {
        const double n = std::min(10.0, std::floor(space.asNumber()));
        if (n >= 1) gap.assign(static_cast<size_t>(n), ' ');
    } else if (space.isString()) {
        gap = space.asString()->value().substr(0, 10);
    }

    JsonStringifier stringifier(gap, replacerFn, list, hasList);
    std::string result;
    if (!stringifier.stringify(info.argument(0), result)) return Runtime::Value::undefined();
    return Runtime::Value::string(new Runtime::String(result));
}

Runtime::Object* JSONBuiltin::createJSONObject(Runtime::Context*) {
    Runtime::Object* json = new Runtime::Object();
    
    json->set("parse", Runtime::Value::object(
        new Runtime::Function("parse", parse, 1)));
    json->set("stringify", Runtime::Value::object(
        new Runtime::Function("stringify", stringify, 3)));
    
    return json;
}

} // namespace Zepra::Builtins
