// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

/**
 * @file language_gaps_tests.cpp
 * @brief test262-style checks for language/runtime features real bundles use:
 *        class private names and static blocks, numeric separators, ASI
 *        restricted productions, UTF-16 string semantics, RegExp accessors
 *        and matching, Date parsing, Intl, new Function / eval, and
 *        closures over later let/const bindings
 */

#include <gtest/gtest.h>

#include "runtime/execution/context.hpp"
#include "runtime/execution/global_object.hpp"
#include "runtime/execution/vm.hpp"

#include <string>

using namespace Zepra::Runtime;

namespace {

class LanguageGaps : public ::testing::Test {
protected:
    LanguageGaps() : context_(&vm_) {
        global_ = new GlobalObject(&context_);
        global_->initialize();
        global_->installInto(vm_);
    }

    // The value of `r` after running `source`, as a string
    std::string run(const std::string& source) {
        void* chunk = vm_.compile("var r;\n" + source, "language_gaps_test");
        EXPECT_NE(chunk, nullptr) << source;
        if (!chunk) return "<syntax error>";
        ExecutionResult result = vm_.execute(static_cast<const Zepra::Bytecode::BytecodeChunk*>(chunk));
        EXPECT_EQ(result.status, ExecutionResult::Status::Success) << result.error << "\n" << source;
        return vm_.getGlobal("r").toString();
    }

    bool parses(const std::string& source) { return vm_.compile(source, "language_gaps_parse") != nullptr; }

    VM vm_{nullptr};
    Context context_;
    GlobalObject* global_ = nullptr;
};

} // namespace

TEST_F(LanguageGaps, PrivateFieldsMethodsAccessors) {
    EXPECT_EQ(run("class A{#x=1; static #s=5; #m(){return this.#x+1} get #g(){return 10}"
                  " run(){return [this.#x,this.#m(),this.#g,A.#s].join()}} r=new A().run();"), "1,2,10,5");
}

TEST_F(LanguageGaps, PrivateNamesAreHidden) {
    EXPECT_EQ(run("class A{#x=1; y=2} var a=new A; r=JSON.stringify(a)+Object.keys(a).length;"), "{\"y\":2}1");
}

TEST_F(LanguageGaps, PrivateIn) {
    EXPECT_EQ(run("class A{#x; static has(o){return #x in o}} r=A.has(new A)+','+A.has({});"), "true,false");
}

TEST_F(LanguageGaps, PrivateBrandCheckThrows) {
    EXPECT_EQ(run("class A{#x=1; static of(o){return o.#x}} try{A.of({})}catch(e){r=e instanceof TypeError}"), "true");
}

TEST_F(LanguageGaps, PrivateNamesFromNestedClass) {
    EXPECT_EQ(run("class O{#a=1; m(){class I{#b=2; f(o){return o.#a+this.#b}} return new I().f(this)}} r=new O().m();"), "3");
}

TEST_F(LanguageGaps, UndeclaredPrivateNameIsSyntaxError) {
    EXPECT_FALSE(parses("class A{ m(){ return this.#nope } }"));
    EXPECT_FALSE(parses("this.#x"));
}

TEST_F(LanguageGaps, StaticBlocks) {
    EXPECT_EQ(run("class A{static q; static{A.q=9; var z=1; this.r=z+1}} r=A.q+','+A.r;"), "9,2");
}

TEST_F(LanguageGaps, NumericSeparators) {
    EXPECT_EQ(run("r=1_000_000+0x1_0+0b1_1+0o1_7;"), "1000034");
    EXPECT_EQ(run("r=1_0.5_0e1_0;"), "105000000000");
    EXPECT_FALSE(parses("1__0"));
    EXPECT_FALSE(parses("1_"));
    EXPECT_FALSE(parses("10n"));  // BigInt is not supported: a SyntaxError, not 10
}

TEST_F(LanguageGaps, LegacyOctalAndHex) {
    EXPECT_EQ(run("r=[010, 08, 0xFFFFFFFFFFFF, .5, 5.].join();"), "8,8,281474976710655,0.5,5");
}

TEST_F(LanguageGaps, AsiRestrictedProductions) {
    EXPECT_EQ(run("r=(function(){ return\n1 })();"), "undefined");
    EXPECT_EQ(run("var a=1,b=2; a\n++b; r=[a,b].join();"), "1,3");
    EXPECT_EQ(run("x: for(var i=0;i<3;i++){ for(;;){ continue x } } r=i;"), "3");
    EXPECT_FALSE(parses("throw\nnew Error()"));
}

TEST_F(LanguageGaps, StringsAreUtf16) {
    EXPECT_EQ(run("r=['é'.length, '\\u{1F600}'.length, '😀'.charCodeAt(0), '😀'.charCodeAt(1),"
                  " 'a😀b'.slice(1,3)==='😀', 'a😀b'.indexOf('b'), 'héllo'.substring(1,3), 'héllo'.charAt(1),"
                  " '😀'.codePointAt(0), [...'a😀'].length, 'é😀'.split('').length].join();"),
              "1,2,55357,56832,true,3,él,é,128512,2,3");
}

TEST_F(LanguageGaps, LoneSurrogatesRoundTrip) {
    EXPECT_EQ(run("var s=String.fromCharCode(0xD83D)+String.fromCharCode(0xDE00); r=(s==='😀')+','+s.length+','+"
                  "'\\uD83D'.length+','+'😀'[0].charCodeAt(0);"),
              "true,2,1,55357");
}

TEST_F(LanguageGaps, RegExpAccessors) {
    EXPECT_EQ(run("var x=/a/gi; r=[x.source,x.flags,x.global,x.ignoreCase,x.sticky,/./su.flags,/a/d.hasIndices,"
                  "new RegExp('a/b').source,String(/x/gi),x.lastIndex].join();"),
              "a,gi,true,true,false,su,true,a\\/b,/x/gi,0");
}

TEST_F(LanguageGaps, RegExpMatching) {
    EXPECT_EQ(run("r=JSON.stringify([/a|b/.exec('b')[0], /\\d{4}/.exec('x 2024')[0], /(?<y>\\d+)-/.exec('12-').groups.y,"
                  " /(?<=a)b/.test('ab'), /(?<!a)b/.test('ab'), /x(?!y)/.exec('xyxz').index, /[a-z]/i.test('A'),"
                  " /[\\u0000-\\u001f]/.test('\\x01'), /é./.exec('aébc')[0], /\\p{L}/u.test('é')]);"),
              "[\"b\",\"2024\",\"12\",true,false,2,true,true,\"éb\",true]");
}

TEST_F(LanguageGaps, RegExpIndicesAreUtf16) {
    EXPECT_EQ(run("var re=/a/g, s='ñaña', out=[]; var m; while((m=re.exec(s))) out.push(m.index+':'+re.lastIndex); r=out.join();"),
              "1:2,3:4");
}

TEST_F(LanguageGaps, DateParsing) {
    EXPECT_EQ(run("r=[new Date('2020-01-01T00:00:00Z').getTime(), Date.parse('2020-01-01'),"
                  " Date.parse('Tue, 01 Jan 2020 10:00:00 GMT'), Date.parse('2020-01-01T00:00:00.5+01:00'),"
                  " new Date(Date.UTC(2020,0,31)).toISOString(), isNaN(Date.parse('nope'))].join();"),
              "1577836800000,1577836800000,1577872800000,1577833200500,2020-01-31T00:00:00.000Z,true");
}

TEST_F(LanguageGaps, DateArithmeticBefore1970) {
    EXPECT_EQ(run("r=new Date(-1).toISOString()+','+new Date(Date.UTC(1900,1,28)).getUTCDay();"),
              "1969-12-31T23:59:59.999Z,3");
}

TEST_F(LanguageGaps, IntlIsInstalled) {
    EXPECT_EQ(run("r=[typeof Intl, new Intl.NumberFormat('en-US').format(1234567.891),"
                  " new Intl.NumberFormat('en-US',{style:'currency',currency:'USD'}).format(-5),"
                  " new Intl.NumberFormat('en',{notation:'compact'}).format(1234),"
                  " new Intl.DateTimeFormat('en-US',{month:'short',day:'numeric',year:'numeric',timeZone:'UTC'}).format(new Date(Date.UTC(2020,0,5))),"
                  " typeof Intl.DateTimeFormat().resolvedOptions().timeZone].join('|');"),
              "object|1,234,567.891|-$5.00|1.2K|Jan 5, 2020|string");
}

TEST_F(LanguageGaps, FunctionConstructorAndEval) {
    EXPECT_EQ(run("r=[new Function('a','b','return a+b')(2,3), Function('return 7')(), eval('1+2'),"
                  " (function(){ try { new Function('return )') } catch(e) { return e.name } })()].join();"),
              "5,7,3,SyntaxError");
}

TEST_F(LanguageGaps, ClosureOverLaterLexicalBinding) {
    EXPECT_EQ(run("r=(()=>{ let f=()=>S.size; let S=new Map([[1,2]]); return f(); })();"), "1");
    EXPECT_EQ(run("r=(()=>{ function f(){ return S.size } let R=1, S=new Map; return f(); })();"), "0");
    EXPECT_EQ(run("r=(()=>{ let g; { function f(){ return S } g=f; } const S=4; return g(); })();"), "4");
}

TEST_F(LanguageGaps, UnicodeIdentifiers) {
    EXPECT_EQ(run("var d={id:{\\u0275prov:5}}; var ɵx=1; r=d.id.ɵprov+d.id.\\u0275prov+ɵx;"), "11");
}

TEST_F(LanguageGaps, HtmlCommentsAndHashbang) {
    EXPECT_EQ(run("var x=1 <!-- comment\n--> also a comment\nr=x;"), "1");
}

// ---- jsparse2: jump/constant limits, finally, holes, sloppy this, Proxy, ... ----

TEST_F(LanguageGaps, LargeFunctionsJumpAndConstants) {
    // More than 64 KB of bytecode inside one branch and more than 65536 constants
    std::string body = "var a=[]; if (a.length === 0) {";
    for (int i = 0; i < 70000; ++i) body += "a.push('s" + std::to_string(i) + "');";
    body += "} r = a.length + ':' + a[69999];";
    EXPECT_EQ(run(body), "70000:s69999");
}

TEST_F(LanguageGaps, FinallyRunsOnReturnBreakContinue) {
    EXPECT_EQ(run("var log=[]; function f(){ try { return 1 } finally { log.push('f') } }"
                  " function g(){ for(var i=0;i<3;i++){ try { if(i==1) continue; if(i==2) break } finally { log.push('g'+i) } } }"
                  " function k(){ try { try { throw 1 } catch(e) { throw 2 } finally { log.push('k') } } catch(e) { return e } }"
                  " function m(){ try { return 1 } finally { return 5 } }"
                  " r=[f(),g(),k(),m()].join()+'|'+log.join();"),
              "1,,2,5|f,g0,g1,g2,k");
}

TEST_F(LanguageGaps, ArrayHoles) {
    EXPECT_EQ(run("var n=0; var a=Array(3).concat('x').map(function(v){ n++; return v+'!' }); r=n+':'+a.length+':'+a[3]+':'+[undefined].map(function(){return 1})[0];"),
              "1:4:x!:1");
}

TEST_F(LanguageGaps, SloppyThisIsGlobal) {
    EXPECT_EQ(run("r=[typeof (function(){return this})(), typeof (function(){'use strict'; return this})(),"
                  " typeof Function('return this')(), typeof (class { static m(){ return (function(){ return this })() } }).m()].join();"),
              "object,undefined,object,undefined");
}

TEST_F(LanguageGaps, ProxyTraps) {
    EXPECT_EQ(run("var t={}; var p=new Proxy(t,{get:function(o,k){return 'got '+String(k)}, set:function(o,k,v){o[k]=v*2; return true}, has:function(){return true}});"
                  " p.x=2; var f=new Proxy(function(a){return a},{apply:function(t,th,args){return args[0]*10}});"
                  " r=[p.foo, 'zz' in p, t.x, f(2), new Proxy({a:1},{}).a].join();"),
              "got foo,true,4,20,1");
}

TEST_F(LanguageGaps, ProtoAndDescriptorsAndNewTarget) {
    EXPECT_EQ(run("var p={a:1}; var o={__proto__:p}; var q={}; q.__proto__=p;"
                  " var d=Object.getOwnPropertyDescriptors({x:1});"
                  " function F(){ return new.target === F }"
                  " r=[o.a, Object.getPrototypeOf(q)===p, d.x.value, F(), Object.keys(o).length].join();"),
              "1,true,1,false,0");
    EXPECT_EQ(run("function G(){ this.t = new.target === G } r = new G().t;"), "true");
}

TEST_F(LanguageGaps, RegexLiteralBraces) {
    EXPECT_EQ(run("r=JSON.stringify([/u{[\\dA-F]+}/.exec('xu{1F}')[0], /a{2}/.test('aa'), /x{,2}/.test('x{,2}')]);"),
              "[\"u{1F}\",true,true]");
}

TEST_F(LanguageGaps, ReflectConstructNewTarget) {
    EXPECT_EQ(run("function A(){ this.a=1 } function B(){} B.prototype.hi=function(){return 2};"
                  " var o=Reflect.construct(A,[],B); r=[o.a, o.hi(), o instanceof B].join();"), "1,2,true");
}

TEST_F(LanguageGaps, DataView) {
    EXPECT_EQ(run("var b=new ArrayBuffer(8); var d=new DataView(b); d.setUint16(0,0x1234); d.setFloat32(4,1.5,true);"
                  " var u=new Uint8Array(b); r=[u[0],u[1],d.getUint16(0,true),d.getFloat32(4,true),d.byteLength].join();"),
              "18,52,13330,1.5,8");
}

// Bundler interop: Turbopack copies a module's exports onto its default
// function with Object.assign, through the exports' getters
TEST_F(LanguageGaps, ObjectAssignRunsSourceGetters) {
    EXPECT_EQ(run("var n={}; Object.defineProperty(n,'default',{enumerable:true,get:function(){return R}});"
                  " function R(){} Object.assign(R,n); var t={}; var log=[];"
                  " Object.defineProperty(t,'s',{set:function(v){log.push(v)}}); Object.assign(t,{s:7});"
                  " r=[R.default===R, log.join(), typeof Object.getOwnPropertyDescriptor(t,'s').set].join();"), "true,7,function");
}

TEST_F(LanguageGaps, JsonAndFromEntriesObjectsAreOrdinary) {
    EXPECT_EQ(run("var a=JSON.parse('{\"x\":{\"y\":1}}'), b=Object.fromEntries([['k',1]]);"
                  " r=[Object.getPrototypeOf(a)===Object.prototype, a.x.hasOwnProperty('y'),"
                  "    Object.getPrototypeOf(b)===Object.prototype, typeof b.toString].join();"),
              "true,true,true,function");
}

TEST_F(LanguageGaps, OptionalChainShortCircuitsWholeChain) {
    EXPECT_EQ(run("var a=null, b={c:{d:[function(){return 7}]}}; var calls=0; function f(){calls++; return 1}"
                  " r=[a?.b.c.d, b?.c.d[0](), a?.b(), a?.[f()], b.c?.x?.y, calls].join('|');"),
              "|7||||0");
}

TEST_F(LanguageGaps, BitwiseOperatorsWrapModulo2To32) {
    EXPECT_EQ(run("r=[0xffffffff & 1, 4294967295|0, 4294967296|0, -1>>>0, 1<<31, 2147483648>>0].join();"),
              "1,-1,0,4294967295,-2147483648,-2147483648");
}

TEST_F(LanguageGaps, ContextualKeywordsNameFunctionExpressions) {
    EXPECT_EQ(run("var e={to:function to(x){return x}, from:function from(x){return x+1}, of:function of(){return 3}};"
                  " r=[e.from(1), e.of(), e.from.name].join();"), "2,3,from");
}
