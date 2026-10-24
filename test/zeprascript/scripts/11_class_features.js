// Class features: super, fields, accessors, computed and keyword names, class expressions.
// Runs without built-ins installed: no Object.*, Symbol or array methods.

var __failed = [];
function check(name, ok) { if (!ok) __failed.push(name); }

// super(...) with arguments, spread, and through an arrow
class Base {
    constructor(a, b) { this.sum = a + b; }
    describe() { return 'base'; }
    static kind() { return 'Base'; }
}
class Derived extends Base {
    constructor(...args) { super(...args); this.derived = true; }
    describe() { return super.describe() + '>derived'; }
    static kind() { return super.kind() + '>Derived'; }
}
class Leaf extends Derived {
    constructor() { var callSuper = () => super(1, 2); callSuper(); }
    describe() { var f = () => super.describe(); return f() + '>leaf'; }
}
var leaf = new Leaf();
check('super call through arrow', leaf.sum === 3 && leaf.derived === true);
check('super method chain', leaf.describe() === 'base>derived>leaf');
check('super static', Derived.kind() === 'Base>Derived');
check('spread super args', new Derived(4, 5).sum === 9);
check('instanceof chain', leaf instanceof Leaf && leaf instanceof Base);

// A derived class without a constructor passes its arguments on
class Implicit extends Base {}
check('implicit derived constructor', new Implicit(2, 3).sum === 5);

// The extends expression is evaluated once, when the class is defined
var heritageCalls = 0;
function mixin() { heritageCalls++; return Base; }
class Mixed extends mixin() {}
new Mixed(1, 1); new Mixed(1, 1);
check('extends evaluated once', heritageCalls === 1);

// super.x getters run with this as the receiver
class Named { get label() { return 'name:' + this.id; } }
class Labelled extends Named { get label() { return '[' + super.label + ']'; } }
var labelled = new Labelled(); labelled.id = 7;
check('super getter receiver', labelled.label === '[name:7]');

// Instance and static fields
var outer = 'outer';
class Fields {
    a = 1;
    b;
    c = this.a + 1;
    seen = outer;
    read = () => this.a;
    static count = 2;
    static twice = Fields.count * 2;
    static self = this;
    constructor(outer) { this.param = outer; }
}
var fields = new Fields('param');
check('instance fields', fields.a === 1 && fields.b === undefined && 'b' in fields && fields.c === 2);
check('field scope is the class, not the constructor', fields.seen === 'outer' && fields.param === 'param');
var read = fields.read;
check('arrow field keeps this', read() === 1);
check('static fields', Fields.count === 2 && Fields.twice === 4 && Fields.self === Fields);

// A derived class's fields are set when super() returns
class Ordered { constructor() { this.log = ['base']; } }
class OrderedChild extends Ordered {
    mark = this.log.push('field');
    constructor() { super(); this.log.push('body'); }
}
check('derived field order', new OrderedChild().log.join() === 'base,field,body');
class FieldsOnly extends Fields { extra = this.a + 10; }
check('fields with implicit constructor', new FieldsOnly().extra === 11);

// Accessors: instance and static, getter and setter sharing a name
class Temp {
    get value() { return this._v; }
    set value(v) { this._v = v * 2; }
    static get unit() { return 'C'; }
    static set unit(u) { Temp.lastUnit = u; }
}
var temp = new Temp(); temp.value = 5; Temp.unit = 'F';
check('getter/setter pair', temp.value === 10);
check('static accessors', Temp.unit === 'C' && Temp.lastUnit === 'F');

// Computed, keyword and generator method names
var dyn = 'dyn';
class Names {
    [dyn + 'Method']() { return 1; }
    static [dyn]() { return 2; }
    get() { return 3; }
    static() { return 4; }
    delete() { return 5; }
    *items() { yield 6; yield 7; }
}
var names = new Names();
check('computed method names', names.dynMethod() === 1 && Names.dyn() === 2);
check('keyword method names', names.get() + names.static() + names.delete() === 12);
var iterated = 0;
for (var v of names.items()) iterated += v;
check('generator method', iterated === 13);

// Class methods are not enumerable
var keys = [];
for (var k in new Base(1, 1)) keys.push(k);
check('methods not enumerable', keys.join() === 'sum');

// Class expressions
var Anonymous = class { f() { return 1; } };
var Extending = class Named2 extends Anonymous { g() { return this.f() + 1; } };
check('class expressions', new Extending().g() === 2);
check('class expression called inline', new (class { constructor() { this.k = 9; } })().k === 9);

__failedNames = __failed.join('; ');
__result = __failed.length === 0 ? 1 : 0;
