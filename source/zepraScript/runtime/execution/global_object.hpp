#pragma once

/**
 * @file global_object.hpp
 * @brief Global object and built-in initialization
 */

#include "config.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"

namespace Zepra::Runtime {

// Forward declarations
class Context;
class VM;

/**
 * @brief The JavaScript global object
 * 
 * Contains all global properties, constructors, and built-in functions.
 */
class GlobalObject : public Object {
public:
    explicit GlobalObject(Context* context);
    
    /**
     * @brief Initialize all built-in objects and functions
     */
    void initialize();

    /**
     * @brief Copy every own global (built-ins included) into the VM's global
     *        table, which is what scripts resolve bare names against.
     * @param except Names the embedder defines itself (e.g. its own console)
     */
    void installInto(VM& vm, const std::vector<std::string>& except = {}) const;
    
    // Built-in constructors
    Function* objectConstructor() const { return objectConstructor_; }
    Function* arrayConstructor() const { return arrayConstructor_; }
    Function* stringConstructor() const { return stringConstructor_; }
    Function* numberConstructor() const { return numberConstructor_; }
    Function* booleanConstructor() const { return booleanConstructor_; }
    Function* functionConstructor() const { return functionConstructor_; }
    Function* errorConstructor() const { return errorConstructor_; }
    Function* typeErrorConstructor() const { return typeErrorConstructor_; }
    Function* syntaxErrorConstructor() const { return syntaxErrorConstructor_; }
    Function* referenceErrorConstructor() const { return referenceErrorConstructor_; }
    Function* rangeErrorConstructor() const { return rangeErrorConstructor_; }
    
    // Built-in prototypes
    Object* objectPrototype() const { return objectPrototype_; }
    Object* arrayPrototype() const { return arrayPrototype_; }
    Object* stringPrototype() const { return stringPrototype_; }
    Object* numberPrototype() const { return numberPrototype_; }
    Object* booleanPrototype() const { return booleanPrototype_; }
    Object* functionPrototype() const { return functionPrototype_; }
    Object* errorPrototype() const { return errorPrototype_; }

    // Built-in objects
    Object* mathObject() const { return mathObject_; }
    Object* jsonObject() const { return jsonObject_; }
    Object* consoleObject() const { return consoleObject_; }
    
    // Intrinsics
    Value undefined() const { return Value::undefined(); }
    Value null() const { return Value::null(); }
    Value NaN() const { return Value::number(std::nan("")); }
    Value infinity() const { return Value::number(std::numeric_limits<double>::infinity()); }
    
private:
    void initializeObjectConstructor();
    void initializeArrayConstructor();
    void initializeStringConstructor();
    void initializeNumberConstructor();
    void initializeBooleanConstructor();
    void initializeFunctionConstructor();
    void initializeErrorConstructors();
    void initializeMath();
    void initializeJSON();
    void initializeConsole();
    void initializeGlobalFunctions();
    
    Context* context_;
    
    // Constructors
    Function* objectConstructor_ = nullptr;
    Function* arrayConstructor_ = nullptr;
    Function* stringConstructor_ = nullptr;
    Function* numberConstructor_ = nullptr;
    Function* booleanConstructor_ = nullptr;
    Function* functionConstructor_ = nullptr;
    Function* errorConstructor_ = nullptr;
    Function* typeErrorConstructor_ = nullptr;
    Function* syntaxErrorConstructor_ = nullptr;
    Function* referenceErrorConstructor_ = nullptr;
    Function* rangeErrorConstructor_ = nullptr;
    
    // Prototypes
    Object* objectPrototype_ = nullptr;
    Object* arrayPrototype_ = nullptr;
    Object* stringPrototype_ = nullptr;
    Object* numberPrototype_ = nullptr;
    Object* booleanPrototype_ = nullptr;
    Object* functionPrototype_ = nullptr;
    Object* errorPrototype_ = nullptr;

    // Built-in objects
    Object* mathObject_ = nullptr;
    Object* jsonObject_ = nullptr;
    Object* consoleObject_ = nullptr;
};

} // namespace Zepra::Runtime
