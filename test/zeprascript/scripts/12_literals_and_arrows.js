// Object literals (accessors, keyword keys, spread) and arrow functions.
// Runs without built-ins installed: no Object.*, Symbol or array methods.

var __failed = [];
function check(name, ok) { if (!ok) __failed.push(name); }

// Keyword and contextual-keyword property names
var keywords = { get: 1, set: 2, default: 3, new: 4, class: 5, async: 6, static: 7, of: 8 };
check('keyword keys', keywords.get + keywords.set + keywords.default + keywords.new +
                      keywords.class + keywords.async + keywords.static + keywords.of === 36);
check('division after keyword name', keywords.get / 1 === 1);

// Contextual keywords as ordinary names
var get = 10, set = 20;
var asyncCalls = 0;
function async(x) { asyncCalls++; return x * 2; }
var shorthand = { get, set };
check('contextual keywords as names', shorthand.get === 10 && shorthand.set === 20 && async(4) === 8);

// Getters, setters and methods named get/set
var thermo = {
    _c: 0,
    get celsius() { return this._c; },
    set celsius(v) { this._c = v; },
    get() { return 'method'; },
};
thermo.celsius = 25;
check('literal getter and setter', thermo.celsius === 25);
check('method named get', thermo.get() === 'method');
var prop = 'computed';
var withComputedGetter = { get [prop]() { return 42; } };
check('computed getter', withComputedGetter.computed === 42);
check('generator method', ({ *gen() { yield 1; } }).gen().next().value === 1);
var descriptor = { get: function () { return 8; }, set: undefined, configurable: true };
check('get and set as data keys', descriptor.get() === 8 && 'set' in descriptor);

// Spread
var spread = { a: 1, ...{ a: 2, b: 3 }, c: 4 };
check('spread order', spread.a === 2 && spread.b === 3 && spread.c === 4);
var spreadKeys = [];
for (var key in { ...null, ...undefined, x: 1 }) spreadKeys.push(key);
check('spread of null and undefined', spreadKeys.join() === 'x');
var fromArray = { ...[5, 6] };
check('spread of an array', fromArray[0] === 5 && fromArray[1] === 6);
var restA, rest;
({ a: restA, ...rest } = { a: 1, b: 2, c: 3 });
check('rest in assignment pattern', restA === 1 && rest.b === 2 && rest.c === 3 && !('a' in rest));
var { default: d, new: n } = { default: 1, new: 2 };
check('keyword keys in patterns', d === 1 && n === 2);

// Arrows: defaults, destructuring, rest, nesting
check('default params', ((a, b = 10) => a + b)(1) === 11);
check('object pattern param', (({ a, b }) => a + b)({ a: 2, b: 3 }) === 5);
check('array pattern param', (([x, , z]) => x + z)([1, 2, 3]) === 4);
check('rest param', ((...r) => r.length)(1, 2, 3) === 3);
check('default referring to earlier param', ((a, f = () => a * 2) => f())(4) === 8);

// Parenthesised expressions are still expressions
var p = 2, q = 3, assigned;
var commaSteps = 0;
check('comma in parens', (commaSteps++, q) === 3 && commaSteps === 1);
check('division after parens', (p) * q === 6 && (p + q) / 5 === 1);
(assigned = 5);
check('assignment in parens', assigned === 5);

// async arrows and async function expressions
var asyncArrows = [async x => x, async (a, b) => a + b, async () => 1, async function () { return 1; }];
var allFunctions = true;
for (var i = 0; i < asyncArrows.length; i++) allFunctions = allFunctions && typeof asyncArrows[i] === 'function';
check('async arrows', allFunctions);
// A statement starting with `async` that is a call keeps its callee
var callsBefore = asyncCalls;
async(1);
check('async call statement', asyncCalls === callsBefore + 1);

// function* expressions
check('generator expression', (function* () { yield 4; })().next().value === 4);

__failedNames = __failed.join('; ');
__result = __failed.length === 0 ? 1 : 0;
