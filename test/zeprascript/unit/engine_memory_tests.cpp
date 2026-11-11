// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

/**
 * @file engine_memory_tests.cpp
 * @brief Engine fixes found on YouTube's memory growth: Object.create
 *        property descriptors (the attestation VM), string ropes, freed
 *        eval chunks, and ToUint32 / element conversions in Math and
 *        typed arrays
 */

#include <gtest/gtest.h>

#include "runtime/execution/context.hpp"
#include "runtime/execution/global_object.hpp"
#include "runtime/execution/vm.hpp"

#include <string>

using namespace Zepra::Runtime;

namespace {

class EngineMemory : public ::testing::Test {
protected:
    EngineMemory() : context_(&vm_) {
        global_ = new GlobalObject(&context_);
        global_->initialize();
        global_->installInto(vm_);
    }

    // The value of `r` after running `source`, as a string
    std::string run(const std::string& source) {
        void* chunk = vm_.compile("var r;\n" + source, "engine_memory_test");
        EXPECT_NE(chunk, nullptr) << source;
        if (!chunk) return "<syntax error>";
        ExecutionResult result = vm_.execute(static_cast<const Zepra::Bytecode::BytecodeChunk*>(chunk));
        EXPECT_EQ(result.status, ExecutionResult::Status::Success) << result.error << "\n" << source;
        return vm_.getGlobal("r").toString();
    }

    VM vm_{nullptr};
    Context context_;
    GlobalObject* global_ = nullptr;
};

} // namespace

// Math.imul / Math.clz32 take ToUint32 operands (wrap modulo 2^32; the
// bitwise operators themselves are fixed on scratch/reveal)
TEST_F(EngineMemory, MathIntegerBuiltinsUseToUint32) {
    EXPECT_EQ(run("r=[Math.imul(0xffffffff,5), Math.imul(2**32+3,2**32+4), Math.imul(3e9,3e9), Math.clz32(2**32), Math.clz32(-1),"
                  " Math.clz32(0.5), Math.clz32(NaN), Math.round(-0.5), Math.round(2.5), Math.round(-2.5), Math.abs('-3')].join();"),
              "-5,12,-494665728,32,0,32,32,0,3,-2,3");
}

TEST_F(EngineMemory, TypedArrayElementConversion) {
    EXPECT_EQ(run("var i8=new Int8Array(4); i8[0]=200; i8[1]=-129; i8[2]=1e10; i8[3]=NaN;"
                  " var u8=new Uint8Array(3); u8[0]=-1; u8[1]=256.7; u8[2]=-Infinity;"
                  " var c=new Uint8ClampedArray(4); c[0]=300; c[1]=-5; c[2]=2.5; c[3]=3.5;"
                  " var i32=new Int32Array(2); i32[0]=2**31; i32[1]=-(2**32)-1; var u32=new Uint32Array(1); u32[0]=-1;"
                  " var u16=new Uint16Array(1); u16[0]=65537;"
                  " r=[...i8, ...u8, ...c, ...i32, ...u32, ...u16].join();"),
              "-56,127,0,0,255,0,0,255,0,2,4,-2147483648,-1,4294967295,1");
}

// Object.create(proto, properties) defines the properties (the YouTube
// attestation VM builds its registers this way; without it every read of a
// register threw and was retried ~30,000 times)
TEST_F(EngineMemory, ObjectCreateWithPropertyDescriptors) {
    EXPECT_EQ(run("var P=Object.create(Object.prototype,{prototype:{get:function(){return this.concat()}}});"
                  " var Q=Object.create(P,{value:{value:{}}}); var v=Object.create(Q); v.concat=function(){return 'cc'};"
                  " var d=Object.getOwnPropertyDescriptor(Q,'value');"
                  " r=[typeof Q.value, v.prototype, Object.keys(Q).length, d.writable, d.enumerable, d.configurable,"
                  " Object.create(null,{a:{value:1,enumerable:true}}).a].join();"),
              "object,cc,0,false,false,false,1");
    EXPECT_EQ(run("try{Object.create({},null)}catch(e){r=e instanceof TypeError}"), "true");
    EXPECT_EQ(run("r=Object.getPrototypeOf(Object.create(Array.prototype,undefined))===Array.prototype;"), "true");
}

// `s += piece` builds ropes: long, deep, prepending, non-ASCII and
// surrogate-splitting concatenations all read back as the flat text
TEST_F(EngineMemory, StringConcatenationRopes) {
    EXPECT_EQ(run("var s=''; for (var i=0;i<200000;i++) s+='abcdefghij';"
                  " r=[s.length, s.charCodeAt(1999999), s.slice(5,12), s.indexOf('jab'), (s+'x').length].join('|');"),
              "2000000|106|fghijab|9|2000001");
    EXPECT_EQ(run("var u=''; for (var i=0;i<3000;i++) u+='\\u00e9'+i; var p=''; for (var i=0;i<3000;i++) p=i+p;"
                  " r=[u.length, u.charAt(1), u.slice(0,3)==='\\u00e90\\u00e9', p.length, p.slice(0,6)].join('|');"),
              "13890|0|true|10890|299929");
    EXPECT_EQ(run("var big='x'.repeat(300); var j=(big+'\\uD83D')+('\\uDE00'+big);"
                  " var k='q'.repeat(300)+5+'z'.repeat(300); var o={}; o[k]=1;"
                  " r=[j.length, j.codePointAt(300), k.length, k[300], o['q'.repeat(300)+'5'+'z'.repeat(300)],"
                  " k==='q'.repeat(300)+'5'+'z'.repeat(300), JSON.stringify(big+big).length].join('|');"),
              "602|128512|601|5|1|true|602");
}

// The top-level chunk of each eval / new Function is freed after it runs;
// the functions, classes, generators and closures it made keep working
TEST_F(EngineMemory, EvalChunksFreedAfterRunning) {
    EXPECT_EQ(run("var fs=[]; for (var i=0;i<300;i++){ fs.push(new Function('a','return a*'+i));"
                  " eval('var g'+i+'=function(){ return '+i+' }; class C'+i+'{ m(){ return \"c'+i+'\" } }');"
                  " fs.push(eval('(function*(){ yield '+i+'; })')); }"
                  " try { eval('throw new Error(\"x\")') } catch (e) { var msg=e.message }"
                  " var mk=eval('(function(){ var n=0; return function(){ return ++n } })')(); mk(); mk();"
                  " r=[fs[0](2), fs[2](3), fs[598](2), g7(), new C299().m(), fs[3]().next().value, msg, mk()].join();"),
              "0,3,598,7,c299,1,x,3");
}
