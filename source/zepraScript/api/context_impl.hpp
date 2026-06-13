#pragma once
/**
 * @file context.cpp
 * @brief Concrete Context implementation — wires API to VM pipeline
 *
 * ContextImpl owns a VM and provides evaluate() which runs
 * the full parse → syntax-check → compile → execute pipeline.
 */

#include "zepra_api.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"
#include "runtime/execution/vm.hpp"
#include "runtime/execution/Sandbox.h"
#include "heap/gc_heap.hpp"
#include "frontend/source_code.hpp"
#include "frontend/parser.hpp"
#include "frontend/syntax_checker.hpp"
#include "bytecode/bytecode_generator.hpp"

// JS-DOM bridge (minimal implementation for beta)
#include "webCore/dom/dom_node.hpp"
#include "webCore/scripting/js_dom_bridge.hpp"

#include <limits>
#include <memory>

namespace Zepra {

// Forward — defined in isolate.cpp
class IsolateImpl;

class ContextImpl : public Context {
public:
    explicit ContextImpl(IsolateImpl* isolate)
        : isolate_(isolate)
        , globalObject_(new Runtime::Object(Runtime::ObjectType::Global))
        , gcHeap_(std::make_unique<Runtime::GCHeap>())
        , sandbox_(Runtime::SandboxConfig::browser())
        , resourceMonitor_(std::make_unique<Runtime::ResourceMonitor>(sandbox_.limits))
        , vm_(std::make_unique<Runtime::VM>(nullptr))
        , domDocument_(std::make_unique<Zepra::WebCore::DOMDocument>())
        , jsDomBridge_(std::make_unique<Zepra::Bridge::JSDOMBridge>())
    {
        vm_->setGCHeap(gcHeap_.get());
        vm_->setSandbox(resourceMonitor_.get());
        initializeGlobals();
        initializeDOMBridge();
    }

    ~ContextImpl() override {
        delete globalObject_;
    }

    Result<Runtime::Value> evaluate(std::string_view source,
                                    std::string_view filename) override {
        try {
            // Parse
            std::string srcStr(source);
            std::string fnStr(filename);
            auto sourceCode = Frontend::SourceCode::fromString(srcStr, fnStr);

            Frontend::Parser parser(sourceCode.get());
            auto ast = parser.parseProgram();
            if (parser.hasErrors()) {
                return Result<Runtime::Value>(std::string("SyntaxError: ") + parser.errors().front());
            }

            // Syntax check
            Frontend::SyntaxChecker checker;
            checker.check(ast.get());
            if (checker.hasErrors()) {
                return Result<Runtime::Value>(std::string("SyntaxError: ") + checker.errors().front());
            }

            // Compile to bytecode
            Bytecode::BytecodeGenerator generator;
            auto chunk = generator.compile(ast.get());

            // Execute via VM
            auto result = vm_->execute(chunk.get());
            if (result.status == Runtime::ExecutionResult::Status::Success) {
                return Result<Runtime::Value>(result.value);
            } else {
                return Result<Runtime::Value>(result.error);
            }
        } catch (const std::exception& e) {
            return Result<Runtime::Value>(std::string(e.what()));
        }
    }

    Runtime::Object* globalObject() override {
        return globalObject_;
    }

    Isolate* isolate() override;

    Runtime::VM* vm() { return vm_.get(); }

private:
    void initializeGlobals() {
        vm_->setGlobal("undefined", Runtime::Value::undefined());
        vm_->setGlobal("NaN", Runtime::Value::number(
            std::numeric_limits<double>::quiet_NaN()));
        vm_->setGlobal("Infinity", Runtime::Value::number(
            std::numeric_limits<double>::infinity()));
        vm_->setGlobal("null", Runtime::Value::null());
    }

    /**
     * @brief Wire the minimal JS-DOM bridge so that window/document exist
     *        before any user script runs. This is the key fix for SPA rendering.
     */
    void initializeDOMBridge() {
        // Create a fresh DOM document for this context
        domDocument_->appendChild(domDocument_->createElement("html"));

        // Initialize the bridge
        jsDomBridge_->initialize(vm_.get(), domDocument_.get());

        // Expose window + document + console to the JS global scope
        jsDomBridge_->exposeWindowObject();

        // Store the bridge so it lives as long as the Context
    }

    IsolateImpl* isolate_;
    Runtime::Object* globalObject_;
    std::unique_ptr<Runtime::GCHeap> gcHeap_;
    Runtime::SandboxConfig sandbox_;
    std::unique_ptr<Runtime::ResourceMonitor> resourceMonitor_;
    std::unique_ptr<Runtime::VM> vm_;

    // DOM bridge members (added for beta SPA support)
    std::unique_ptr<Zepra::WebCore::DOMDocument> domDocument_;
    std::unique_ptr<Zepra::Bridge::JSDOMBridge> jsDomBridge_;
};

} // namespace Zepra
