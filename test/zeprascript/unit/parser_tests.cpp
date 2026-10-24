// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

/**
 * @file parser_tests.cpp
 * @brief Comprehensive parser tests for ES6 features
 */
#include <gtest/gtest.h>
#include "frontend/parser.hpp"
#include "frontend/ast.hpp"

using namespace Zepra::Frontend;

// =============================================================================
// Empty Program Tests
// =============================================================================

TEST(ParserTests, EmptyProgram) {
    auto program = parse("", "test.js");
    ASSERT_NE(program, nullptr);
    EXPECT_EQ(program->body().size(), 0);
}

// =============================================================================
// ES6 Class Declaration Tests
// =============================================================================

TEST(ParserTests, BasicClassDeclaration) {
    auto program = parse(R"(
        class Animal {
            constructor(name) {
                this.name = name;
            }
        }
    )", "test.js");
    
    ASSERT_NE(program, nullptr);
    ASSERT_EQ(program->body().size(), 1);
    
    auto* classDecl = dynamic_cast<ClassDecl*>(program->body()[0].get());
    ASSERT_NE(classDecl, nullptr);
    EXPECT_EQ(classDecl->name(), "Animal");
    EXPECT_EQ(classDecl->superClass(), nullptr);
    ASSERT_NE(classDecl->constructor(), nullptr);
}

TEST(ParserTests, ClassWithExtends) {
    auto program = parse(R"(
        class Dog extends Animal {
            constructor(name) {
                this.name = name;
            }
            bark() {
                return "woof";
            }
        }
    )", "test.js");
    
    ASSERT_NE(program, nullptr);
    ASSERT_EQ(program->body().size(), 1);
    
    auto* classDecl = dynamic_cast<ClassDecl*>(program->body()[0].get());
    ASSERT_NE(classDecl, nullptr);
    EXPECT_EQ(classDecl->name(), "Dog");
    ASSERT_NE(classDecl->superClass(), nullptr);
    EXPECT_EQ(classDecl->methods().size(), 1);
}

TEST(ParserTests, ClassWithStaticMethod) {
    auto program = parse(R"(
        class Factory {
            static create(type) {
                return new Factory();
            }
        }
    )", "test.js");
    
    ASSERT_NE(program, nullptr);
    auto* classDecl = dynamic_cast<ClassDecl*>(program->body()[0].get());
    ASSERT_NE(classDecl, nullptr);
    ASSERT_EQ(classDecl->methods().size(), 1);
    EXPECT_TRUE(classDecl->methods()[0].isStatic);
}

TEST(ParserTests, ClassWithGetterSetter) {
    auto program = parse(R"(
        class Person {
            get name() {
                return this._name;
            }
            set name(value) {
                this._name = value;
            }
        }
    )", "test.js");
    
    ASSERT_NE(program, nullptr);
    auto* classDecl = dynamic_cast<ClassDecl*>(program->body()[0].get());
    ASSERT_NE(classDecl, nullptr);
    ASSERT_EQ(classDecl->methods().size(), 2);
    EXPECT_TRUE(classDecl->methods()[0].isGetter);
    EXPECT_TRUE(classDecl->methods()[1].isSetter);
}

// =============================================================================
// for...of Loop Tests
// =============================================================================

TEST(ParserTests, ForOfWithLet) {
    auto program = parse(R"(
        for (let x of arr) {
            console.log(x);
        }
    )", "test.js");
    
    ASSERT_NE(program, nullptr);
    ASSERT_EQ(program->body().size(), 1);
    
    auto* forOf = dynamic_cast<ForOfStmt*>(program->body()[0].get());
    ASSERT_NE(forOf, nullptr);
    ASSERT_NE(forOf->left(), nullptr);
    ASSERT_NE(forOf->right(), nullptr);
    ASSERT_NE(forOf->body(), nullptr);
}

TEST(ParserTests, ForOfWithConst) {
    auto program = parse(R"(
        for (const item of items) {
            process(item);
        }
    )", "test.js");
    
    ASSERT_NE(program, nullptr);
    auto* forOf = dynamic_cast<ForOfStmt*>(program->body()[0].get());
    ASSERT_NE(forOf, nullptr);
}

TEST(ParserTests, ForOfWithExistingVariable) {
    auto program = parse(R"(
        let x;
        for (x of arr) {
            console.log(x);
        }
    )", "test.js");
    
    ASSERT_NE(program, nullptr);
    ASSERT_EQ(program->body().size(), 2);
    
    auto* forOf = dynamic_cast<ForOfStmt*>(program->body()[1].get());
    ASSERT_NE(forOf, nullptr);
}

// =============================================================================
// Regular For Loop Tests (ensure not broken)
// =============================================================================

TEST(ParserTests, RegularForLoop) {
    auto program = parse(R"(
        for (let i = 0; i < 10; i++) {
            console.log(i);
        }
    )", "test.js");
    
    ASSERT_NE(program, nullptr);
    auto* forStmt = dynamic_cast<ForStmt*>(program->body()[0].get());
    ASSERT_NE(forStmt, nullptr);
    ASSERT_NE(forStmt->init(), nullptr);
    ASSERT_NE(forStmt->test(), nullptr);
    ASSERT_NE(forStmt->update(), nullptr);
}

TEST(ParserTests, ForLoopNoInit) {
    auto program = parse(R"(
        let i = 0;
        for (; i < 10; i++) {
            console.log(i);
        }
    )", "test.js");
    
    ASSERT_NE(program, nullptr);
    auto* forStmt = dynamic_cast<ForStmt*>(program->body()[1].get());
    ASSERT_NE(forStmt, nullptr);
    EXPECT_EQ(forStmt->init(), nullptr);
}

// =============================================================================
// Arrow Function Tests
// =============================================================================

TEST(ParserTests, ArrowFunctionSingleParam) {
    auto program = parse("const fn = x => x * 2;", "test.js");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTests, ArrowFunctionMultipleParams) {
    auto program = parse("const add = (a, b) => a + b;", "test.js");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTests, ArrowFunctionWithBlock) {
    auto program = parse(R"(
        const fn = (x) => {
            console.log(x);
            return x * 2;
        };
    )", "test.js");
    ASSERT_NE(program, nullptr);
}

// =============================================================================
// Template Literal Tests
// =============================================================================

TEST(ParserTests, VariableDeclarations) {
    auto program = parse(R"(
        var a = 1;
        let b = 2;
        const c = 3;
    )", "test.js");
    
    ASSERT_NE(program, nullptr);
    EXPECT_EQ(program->body().size(), 3);
}

// =============================================================================
// Destructuring Tests
// =============================================================================

TEST(ParserTests, FunctionDefaultParams) {
    auto program = parse(R"(
        function greet(name = "World") {
            return "Hello, " + name;
        }
    )", "test.js");
    
    ASSERT_NE(program, nullptr);
    auto* funcDecl = dynamic_cast<FunctionDecl*>(program->body()[0].get());
    ASSERT_NE(funcDecl, nullptr);
    ASSERT_EQ(funcDecl->params().size(), 1);
}

TEST(ParserTests, RestParameter) {
    auto program = parse(R"(
        function sum(...numbers) {
            return numbers.reduce((a, b) => a + b, 0);
        }
    )", "test.js");
    
    ASSERT_NE(program, nullptr);
    auto* funcDecl = dynamic_cast<FunctionDecl*>(program->body()[0].get());
    ASSERT_NE(funcDecl, nullptr);
    ASSERT_EQ(funcDecl->params().size(), 1);
    EXPECT_TRUE(funcDecl->params()[0].rest);
}

// =============================================================================
// Async/Await Tests
// =============================================================================

TEST(ParserTests, AwaitExpression) {
    auto program = parse(R"(
        async function fetchData() {
            const result = await fetch("/api");
            return result;
        }
    )", "test.js");
    
    ASSERT_NE(program, nullptr);
}

// =============================================================================
// Module Import/Export Tests
// =============================================================================

TEST(ParserTests, ImportDeclaration) {
    auto program = parse(R"(
        import { foo, bar } from './module.js';
    )", "test.js");
    
    ASSERT_NE(program, nullptr);
}

TEST(ParserTests, ExportDeclaration) {
    auto program = parse(R"(
        export function greet() {}
    )", "test.js");
    
    ASSERT_NE(program, nullptr);
}

// =============================================================================
// Classes, object literals and arrows (super, fields, accessors, async)
// =============================================================================

namespace {
// Parses source that must be free of errors
std::unique_ptr<Program> parseClean(const std::string& code) {
    auto source = SourceCode::fromString(code, "test.js");
    Parser parser(source.get());
    auto program = parser.parseProgram();
    EXPECT_FALSE(parser.hasErrors()) << (parser.hasErrors() ? parser.errors()[0] : "");
    return program;
}

bool hasParseErrors(const std::string& code) {
    auto source = SourceCode::fromString(code, "test.js");
    Parser parser(source.get());
    parser.parseProgram();
    return parser.hasErrors();
}
} // namespace

TEST(ParserTests, ClassFieldsAndSuper) {
    auto program = parseClean(R"(
        class B extends A {
            x = 1;
            static y = 2;
            z;
            constructor() { super(); }
            m() { return super.m(); }
            plain() { return 1; }
        }
    )");
    ASSERT_EQ(program->body().size(), 1);
    auto* decl = dynamic_cast<ClassDecl*>(program->body()[0].get());
    ASSERT_NE(decl, nullptr);
    ASSERT_EQ(decl->fields().size(), 3);
    EXPECT_FALSE(decl->fields()[0].isStatic);
    EXPECT_TRUE(decl->fields()[1].isStatic);
    EXPECT_EQ(decl->fields()[2].value, nullptr);
    EXPECT_FALSE(decl->hasImplicitConstructor());
    EXPECT_TRUE(decl->constructor()->usesSuper());
    ASSERT_EQ(decl->methods().size(), 2);
    EXPECT_TRUE(decl->methods()[0].function->usesSuper());
    EXPECT_FALSE(decl->methods()[1].function->usesSuper());
}

TEST(ParserTests, ClassMemberModifiersAndNames) {
    auto program = parseClean(R"(
        class C {
            static get v() { return 1; }
            get() { return 2; }
            static() { return 3; }
            async load() {}
            *gen() {}
            [key]() {}
            'quoted'() {}
        }
    )");
    auto* decl = dynamic_cast<ClassDecl*>(program->body()[0].get());
    ASSERT_NE(decl, nullptr);
    ASSERT_EQ(decl->methods().size(), 7);
    EXPECT_TRUE(decl->methods()[0].isStatic && decl->methods()[0].isGetter);
    EXPECT_EQ(decl->methods()[1].name, "get");
    EXPECT_FALSE(decl->methods()[1].isGetter);
    EXPECT_EQ(decl->methods()[2].name, "static");
    EXPECT_FALSE(decl->methods()[2].isStatic);
    EXPECT_TRUE(decl->methods()[3].function->isAsync());
    EXPECT_TRUE(decl->methods()[4].function->isGenerator());
    EXPECT_TRUE(decl->methods()[5].computed);
    EXPECT_EQ(decl->methods()[6].name, "quoted");
    EXPECT_TRUE(decl->hasImplicitConstructor());
}

TEST(ParserTests, ClassExpression) {
    auto program = parseClean("var A = class Named extends B { f() {} };");
    auto* decl = dynamic_cast<VariableDecl*>(program->body()[0].get());
    ASSERT_NE(decl, nullptr);
    auto* expr = decl->declarators()[0].init.get();
    ASSERT_EQ(expr->type(), NodeType::ClassExpression);
    auto* cls = static_cast<ClassExpr*>(expr)->decl();
    EXPECT_EQ(cls->name(), "Named");
    EXPECT_NE(cls->superClass(), nullptr);
}

TEST(ParserTests, ObjectLiteralAccessorsAndSpread) {
    auto program = parseClean("var o = { get a() { return 1; }, set a(v) {}, get: 1, ...rest, get };");
    auto* decl = dynamic_cast<VariableDecl*>(program->body()[0].get());
    ASSERT_NE(decl, nullptr);
    auto* obj = static_cast<ObjectExpr*>(decl->declarators()[0].init.get());
    ASSERT_EQ(obj->properties().size(), 5);
    EXPECT_EQ(obj->properties()[0].kind, ObjectProperty::Kind::Get);
    EXPECT_EQ(obj->properties()[1].kind, ObjectProperty::Kind::Set);
    EXPECT_EQ(obj->properties()[2].kind, ObjectProperty::Kind::Init);
    EXPECT_TRUE(obj->properties()[3].spread);
    EXPECT_TRUE(obj->properties()[4].shorthand);
}

TEST(ParserTests, AsyncAndDestructuringArrows) {
    auto program = parseClean(R"(
        var a = async x => x;
        var b = async (p, q = 1) => p + q;
        var c = ({ k }, [m], ...rest) => k;
        var d = (1, 2);
        var e = async(3);
    )");
    ASSERT_EQ(program->body().size(), 5);
    auto initOf = [&](size_t i) {
        return static_cast<VariableDecl*>(program->body()[i].get())->declarators()[0].init.get();
    };
    ASSERT_EQ(initOf(0)->type(), NodeType::ArrowFunction);
    EXPECT_TRUE(static_cast<ArrowFunctionExpr*>(initOf(0))->isAsync());
    ASSERT_EQ(initOf(1)->type(), NodeType::ArrowFunction);
    EXPECT_TRUE(static_cast<ArrowFunctionExpr*>(initOf(1))->isAsync());
    EXPECT_NE(static_cast<ArrowFunctionExpr*>(initOf(1))->params()[1].defaultValue, nullptr);
    ASSERT_EQ(initOf(2)->type(), NodeType::ArrowFunction);
    EXPECT_EQ(static_cast<ArrowFunctionExpr*>(initOf(2))->params().size(), 3);
    EXPECT_EQ(initOf(3)->type(), NodeType::BinaryExpression);  // Comma operator
    EXPECT_EQ(initOf(4)->type(), NodeType::CallExpression);    // async used as a name
}

TEST(ParserTests, AsyncArrowAsStatement) {
    // `async` at the start of a statement must not be dropped
    auto program = parseClean("async () => {};");
    ASSERT_EQ(program->body().size(), 1);
    auto* stmt = dynamic_cast<ExprStmt*>(program->body()[0].get());
    ASSERT_NE(stmt, nullptr);
    ASSERT_EQ(stmt->expression()->type(), NodeType::ArrowFunction);
    EXPECT_TRUE(static_cast<ArrowFunctionExpr*>(stmt->expression())->isAsync());
}

TEST(ParserTests, SuperOutsideMethodsIsAnError) {
    EXPECT_TRUE(hasParseErrors("function f() { return super.x; }"));
    EXPECT_TRUE(hasParseErrors("super();"));
    EXPECT_TRUE(hasParseErrors("class A { m() { function inner() { super.m(); } } }"));
    EXPECT_FALSE(hasParseErrors("class A { m() { var f = () => super.m(); } }"));
    EXPECT_FALSE(hasParseErrors("var o = { m() { return super.m(); } };"));
}

TEST(ParserTests, NestedParenthesesParseInLinearTime) {
    // Each '(' is first tried as an arrow parameter list; without
    // remembering failed attempts this nesting takes 2^40 steps
    std::string code = "var v = ";
    for (int i = 0; i < 40; i++) code += "(a = ";
    code += "1";
    for (int i = 0; i < 40; i++) code += ")";
    code += ";";
    auto program = parseClean(code);
    ASSERT_EQ(program->body().size(), 1);
}
