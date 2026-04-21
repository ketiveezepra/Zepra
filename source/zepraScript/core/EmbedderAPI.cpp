// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "core/EmbedderAPI.h"
#include "runtime/execution/vm.hpp"
#include "heap/gc_heap.hpp"
#include "config.hpp"

#include <thread>

namespace Zepra {

// =============================================================================
// ZebraIsolate::Impl
// =============================================================================

struct ZebraIsolate::Impl {
    IsolateConfig config;
    std::unique_ptr<Runtime::VM> vm;
    bool entered = false;
    bool terminated = false;
    int64_t externalMemory = 0;

    explicit Impl(const IsolateConfig& cfg) : config(cfg) {
        vm = std::make_unique<Runtime::VM>();
    }
};

static thread_local ZebraIsolate* tl_currentIsolate = nullptr;

std::unique_ptr<ZebraIsolate> ZebraIsolate::Create(const IsolateConfig& config) {
    return std::unique_ptr<ZebraIsolate>(new ZebraIsolate(config));
}

ZebraIsolate::ZebraIsolate(const IsolateConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

ZebraIsolate::~ZebraIsolate() {
    if (impl_->entered) Exit();
}

void ZebraIsolate::Enter() {
    impl_->entered = true;
    tl_currentIsolate = this;
}

void ZebraIsolate::Exit() {
    impl_->entered = false;
    if (tl_currentIsolate == this) tl_currentIsolate = nullptr;
}

void ZebraIsolate::RequestGC() {
    if (impl_->vm) impl_->vm->requestGC();
}

void ZebraIsolate::ForceGC() {
    if (impl_->vm) impl_->vm->forceGC();
}

ZebraIsolate::HeapStats ZebraIsolate::GetHeapStats() const {
    HeapStats stats{};
    if (impl_->vm) {
        auto gcStats = impl_->vm->gcStats();
        stats.totalHeapSize = gcStats.heapSize;
        stats.usedHeapSize = gcStats.usedSize;
        stats.heapLimit = impl_->config.maxHeapSize;
        stats.externalMemory = static_cast<size_t>(impl_->externalMemory);
        stats.gcCount = gcStats.collectionCount;
    }
    return stats;
}

void ZebraIsolate::AdjustExternalMemory(int64_t delta) {
    impl_->externalMemory += delta;
    if (impl_->externalMemory < 0) impl_->externalMemory = 0;
}

void ZebraIsolate::TerminateExecution() {
    impl_->terminated = true;
    if (impl_->vm) impl_->vm->terminate();
}

bool ZebraIsolate::IsExecutionTerminated() const {
    return impl_->terminated;
}

void ZebraIsolate::CancelTerminateExecution() {
    impl_->terminated = false;
}

ZebraIsolate* ZebraIsolate::Current() {
    return tl_currentIsolate;
}

// =============================================================================
// ZebraContext::Impl
// =============================================================================

struct ZebraContext::Impl {
    ZebraIsolate* isolate;
    void* dataSlots[ZebraContext::kDataSlots] = {};
    std::string securityToken;

    explicit Impl(ZebraIsolate* iso) : isolate(iso) {}
};

static thread_local ZebraContext* tl_currentContext = nullptr;

std::unique_ptr<ZebraContext> ZebraContext::Create(ZebraIsolate* isolate) {
    return std::unique_ptr<ZebraContext>(new ZebraContext(isolate));
}

ZebraContext::ZebraContext(ZebraIsolate* isolate)
    : impl_(std::make_unique<Impl>(isolate)) {}

ZebraContext::~ZebraContext() = default;

void ZebraContext::Enter() { tl_currentContext = this; }
void ZebraContext::Exit() { if (tl_currentContext == this) tl_currentContext = nullptr; }

ZebraObject* ZebraContext::Global() { return nullptr; }

void ZebraContext::SetSecurityToken(const std::string& token) {
    impl_->securityToken = token;
}

std::string ZebraContext::GetSecurityToken() const {
    return impl_->securityToken;
}

void ZebraContext::SetData(uint32_t index, void* data) {
    if (index < kDataSlots) impl_->dataSlots[index] = data;
}

void* ZebraContext::GetData(uint32_t index) const {
    if (index < kDataSlots) return impl_->dataSlots[index];
    return nullptr;
}

ZebraContext* ZebraContext::Current() { return tl_currentContext; }

// =============================================================================
// ZebraValue::Impl
// =============================================================================

struct ZebraValue::Impl {
    enum class Kind : uint8_t {
        Undefined, Null, Boolean, Number, String, Object
    };
    Kind kind = Kind::Undefined;
    double number = 0;
    bool boolean = false;
    std::string string;
};

ZebraValue::ZebraValue() : impl_(std::make_shared<Impl>()) {}
ZebraValue::~ZebraValue() = default;
ZebraValue::ZebraValue(const ZebraValue& other) = default;
ZebraValue::ZebraValue(ZebraValue&& other) noexcept = default;
ZebraValue& ZebraValue::operator=(const ZebraValue& other) = default;
ZebraValue& ZebraValue::operator=(ZebraValue&& other) noexcept = default;

bool ZebraValue::IsUndefined() const { return impl_->kind == Impl::Kind::Undefined; }
bool ZebraValue::IsNull() const { return impl_->kind == Impl::Kind::Null; }
bool ZebraValue::IsBoolean() const { return impl_->kind == Impl::Kind::Boolean; }
bool ZebraValue::IsNumber() const { return impl_->kind == Impl::Kind::Number; }
bool ZebraValue::IsString() const { return impl_->kind == Impl::Kind::String; }
bool ZebraValue::IsObject() const { return impl_->kind == Impl::Kind::Object; }
bool ZebraValue::IsArray() const { return false; }
bool ZebraValue::IsFunction() const { return false; }
bool ZebraValue::IsSymbol() const { return false; }
bool ZebraValue::IsBigInt() const { return false; }

bool ZebraValue::ToBoolean() const {
    switch (impl_->kind) {
        case Impl::Kind::Undefined: case Impl::Kind::Null: return false;
        case Impl::Kind::Boolean: return impl_->boolean;
        case Impl::Kind::Number: return impl_->number != 0;
        case Impl::Kind::String: return !impl_->string.empty();
        case Impl::Kind::Object: return true;
    }
    return false;
}

double ZebraValue::ToNumber() const { return impl_->number; }
int32_t ZebraValue::ToInt32() const { return static_cast<int32_t>(impl_->number); }
uint32_t ZebraValue::ToUint32() const { return static_cast<uint32_t>(impl_->number); }
std::string ZebraValue::ToString() const { return impl_->string; }
ZebraObject* ZebraValue::ToObject() const { return nullptr; }

ZebraValue ZebraValue::Undefined() { return ZebraValue(); }
ZebraValue ZebraValue::Null() {
    ZebraValue v; v.impl_->kind = Impl::Kind::Null; return v;
}
ZebraValue ZebraValue::Boolean(bool value) {
    ZebraValue v; v.impl_->kind = Impl::Kind::Boolean; v.impl_->boolean = value; return v;
}
ZebraValue ZebraValue::Number(double value) {
    ZebraValue v; v.impl_->kind = Impl::Kind::Number; v.impl_->number = value; return v;
}
ZebraValue ZebraValue::String(ZebraContext*, std::string_view str) {
    ZebraValue v; v.impl_->kind = Impl::Kind::String; v.impl_->string = str; return v;
}
ZebraValue ZebraValue::Integer(int32_t value) {
    return Number(static_cast<double>(value));
}

bool ZebraValue::StrictEquals(const ZebraValue& other) const {
    if (impl_->kind != other.impl_->kind) return false;
    switch (impl_->kind) {
        case Impl::Kind::Undefined: case Impl::Kind::Null: return true;
        case Impl::Kind::Boolean: return impl_->boolean == other.impl_->boolean;
        case Impl::Kind::Number: return impl_->number == other.impl_->number;
        case Impl::Kind::String: return impl_->string == other.impl_->string;
        case Impl::Kind::Object: return impl_.get() == other.impl_.get();
    }
    return false;
}

bool ZebraValue::Equals(const ZebraValue& other) const {
    return StrictEquals(other);
}

// =============================================================================
// ZebraException
// =============================================================================

struct ZebraException::Impl {
    std::string message;
    std::string stackTrace;
    std::string sourceFile;
    int line = 0;
    int column = 0;
};

std::string ZebraException::Message() const { return impl_->message; }
std::string ZebraException::StackTrace() const { return impl_->stackTrace; }
std::string ZebraException::SourceFile() const { return impl_->sourceFile; }
int ZebraException::LineNumber() const { return impl_->line; }
int ZebraException::ColumnNumber() const { return impl_->column; }

ZebraValue ZebraException::GetValue() const { return ZebraValue(); }

ZebraException ZebraException::Error(std::string_view msg) {
    ZebraException ex;
    ex.impl_ = std::make_shared<Impl>();
    ex.impl_->message = msg;
    return ex;
}
ZebraException ZebraException::TypeError(std::string_view msg) { return Error(msg); }
ZebraException ZebraException::RangeError(std::string_view msg) { return Error(msg); }
ZebraException ZebraException::ReferenceError(std::string_view msg) { return Error(msg); }
ZebraException ZebraException::SyntaxError(std::string_view msg) { return Error(msg); }

// =============================================================================
// TryCatch
// =============================================================================

struct TryCatch::Impl {
    ZebraIsolate* isolate;
    bool hasCaught = false;
    ZebraException exception;
};

TryCatch::TryCatch(ZebraIsolate* isolate)
    : impl_(std::make_unique<Impl>()) {
    impl_->isolate = isolate;
}
TryCatch::~TryCatch() = default;

bool TryCatch::HasCaught() const { return impl_->hasCaught; }
ZebraException TryCatch::Exception() const { return impl_->exception; }
void TryCatch::Reset() { impl_->hasCaught = false; }
void TryCatch::ReThrow() {}

// =============================================================================
// Global Functions
// =============================================================================

bool ZebraInitialize() { return true; }
void ZebraShutdown() {}
const char* ZebraVersion() { return ZEPRA_VERSION; }

} // namespace Zepra
