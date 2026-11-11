// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file web_animations.cpp
 * @brief The Web Animations API (Web Animations 1), written in script.
 *
 * Timing follows the spec's model: an Animation has a start time or a hold
 * time on the document timeline (performance.now()), its KeyframeEffect
 * turns the animation's current time into an iteration progress (delay,
 * iterations, direction, easing, fill), and the keyframes are interpolated
 * at that progress per property.
 *
 * The values an element's animations produce are handed to the engine as
 * CSS text with __zepraSetAnimatedStyle(element, text). The style resolver
 * applies them in the animations origin of the cascade (above the style
 * attribute, below !important), so getComputedStyle, layout and paint all
 * see them, and the page's own inline style is never touched. While any
 * animation runs, one requestAnimationFrame loop samples them all.
 *
 * Not done: composite modes other than replace (add/accumulate fall back to
 * replace), automatic removal of replaced animations, ScrollTimeline, and
 * interpolation of values whose shapes differ (they flip at half way, as
 * discrete values do).
 */

#include "scripting/web_animations.hpp"

namespace Zepra::WebCore {

const char* webAnimationsScript() {
    return R"JS(
(function (g) {
  var E = typeof Element !== 'undefined' ? Element.prototype : null;
  if (!E || typeof __zepraSetAnimatedStyle !== 'function') return;
  var setAnimated = __zepraSetAnimatedStyle;
  var now = function () { return (g.performance && performance.now) ? performance.now() : Date.now(); };
  var raf = g.requestAnimationFrame ? function (f) { return g.requestAnimationFrame(f); } : function (f) { return setTimeout(function () { f(now()); }, 16); };
  var microtask = g.queueMicrotask ? function (f) { g.queueMicrotask(f); } : function (f) { Promise.resolve().then(f); };
  var abortError = function (msg) {
    if (typeof DOMException === 'function') return new DOMException(msg, 'AbortError');
    var e = new Error(msg); e.name = 'AbortError'; return e;
  };

  // ---------------------------------------------------------------- easing
  // <easing-function> (CSS Easing 1): linear, the cubic-bezier keywords,
  // cubic-bezier(), steps(), step-start/step-end
  function cubicBezier(x1, y1, x2, y2) {
    function sample(a, b, t) { return ((1 - 3 * b + 3 * a) * t + (3 * b - 6 * a)) * t * t + 3 * a * t; }
    function slope(a, b, t) { return 3 * (1 - 3 * b + 3 * a) * t * t + 2 * (3 * b - 6 * a) * t + 3 * a; }
    return function (x) {
      if (x <= 0) return x1 > 0 ? (y1 / x1) * x : (x2 > 0 ? (y2 / x2) * x : 0);
      if (x >= 1) return x2 < 1 ? 1 + ((y2 - 1) / (x2 - 1)) * (x - 1) : 1;
      var t = x;
      for (var i = 0; i < 8; i++) {           // Newton
        var err = sample(x1, x2, t) - x, d = slope(x1, x2, t);
        if (Math.abs(err) < 1e-7) return sample(y1, y2, t);
        if (Math.abs(d) < 1e-7) break;
        t -= err / d;
      }
      var lo = 0, hi = 1; t = x;               // bisection fallback
      for (var j = 0; j < 40; j++) {
        var v = sample(x1, x2, t);
        if (Math.abs(v - x) < 1e-7) break;
        if (v < x) lo = t; else hi = t;
        t = (lo + hi) / 2;
      }
      return sample(y1, y2, t);
    };
  }
  function steps(n, pos) {
    var jumps = pos === 'jump-both' ? n + 1 : pos === 'jump-none' ? n - 1 : n;
    return function (x, before) {
      var step = Math.floor(x * n);
      if (pos === 'jump-start' || pos === 'start' || pos === 'jump-both') step++;
      if (before && Math.floor(x * n) === x * n) step--;   // the before flag (CSS Easing 1 §3.1)
      if (x >= 0 && step < 0) step = 0;
      if (x <= 1 && step > jumps) step = jumps;
      return step / jumps;
    };
  }
  var easingCache = {};
  function parseEasing(text) {
    var s = String(text === undefined ? 'linear' : text).trim().toLowerCase();
    if (easingCache[s]) return easingCache[s];
    var fn = null, m;
    if (s === 'linear') fn = function (x) { return x; };
    else if (s === 'ease') fn = cubicBezier(0.25, 0.1, 0.25, 1);
    else if (s === 'ease-in') fn = cubicBezier(0.42, 0, 1, 1);
    else if (s === 'ease-out') fn = cubicBezier(0, 0, 0.58, 1);
    else if (s === 'ease-in-out') fn = cubicBezier(0.42, 0, 0.58, 1);
    else if (s === 'step-start') fn = steps(1, 'jump-start');
    else if (s === 'step-end') fn = steps(1, 'jump-end');
    else if ((m = /^cubic-bezier\(\s*([^,]+),([^,]+),([^,]+),([^)]+)\)$/.exec(s))) {
      var a = [+m[1], +m[2], +m[3], +m[4]];
      if (a.every(isFinite) && a[0] >= 0 && a[0] <= 1 && a[2] >= 0 && a[2] <= 1) fn = cubicBezier(a[0], a[1], a[2], a[3]);
    } else if ((m = /^steps\(\s*(\d+)\s*(?:,\s*(jump-start|jump-end|jump-none|jump-both|start|end)\s*)?\)$/.exec(s))) {
      var n = parseInt(m[1], 10), p = m[2] || 'jump-end';
      if (n > 0 && !(p === 'jump-none' && n < 2)) fn = steps(n, p);
    }
    if (!fn) throw new TypeError("'" + text + "' is not a valid value for easing");
    easingCache[s] = fn;
    return fn;
  }

  // ---------------------------------------------------------------- values
  function cssName(prop) {
    if (prop === 'cssFloat') return 'float';
    if (prop.indexOf('--') === 0) return prop;
    var name = prop.replace(/[A-Z]/g, function (c) { return '-' + c.toLowerCase(); });
    if (/^(webkit|moz|ms)-/.test(name)) name = '-' + name;
    return name;
  }
  var NUM = /[-+]?(?:\d+\.?\d*|\.\d+)(?:e[-+]?\d+)?/gi;
  function hexToRgb(v) {
    return v.replace(/#([0-9a-f]{3,8})\b/gi, function (all, h) {
      if (h.length === 3 || h.length === 4) h = h.replace(/./g, function (c) { return c + c; });
      if (h.length !== 6 && h.length !== 8) return all;
      var r = parseInt(h.substr(0, 2), 16), gg = parseInt(h.substr(2, 2), 16), b = parseInt(h.substr(4, 2), 16);
      var a = h.length === 8 ? parseInt(h.substr(6, 2), 16) / 255 : 1;
      return 'rgba(' + r + ', ' + gg + ', ' + b + ', ' + a + ')';
    }).replace(/\brgb\(([^)]*)\)/gi, function (all, inner) {
      return inner.split(',').length === 3 ? 'rgba(' + inner + ', 1)' : all;
    }).replace(/\btransparent\b/gi, 'rgba(0, 0, 0, 0)');
  }
  function shape(v) { return { nums: (v.match(NUM) || []).map(Number), rest: v.split(NUM) }; }
  // Interpolates two values: numbers in the same places of the same text
  // (lengths, transforms with the same functions, rgba() colours), else
  // the second value from half way on (discrete animation)
  function interpolate(a, b, t) {
    if (t === 0) return a;
    if (t === 1) return b;
    var sa = String(a).trim(), sb = String(b).trim();
    if (/^-?0$/.test(sa) && /\d[a-z%]+$/i.test(sb)) sa = '0' + sb.replace(/^.*\d/, '');
    if (/^-?0$/.test(sb) && /\d[a-z%]+$/i.test(sa)) sb = '0' + sa.replace(/^.*\d/, '');
    var ca = hexToRgb(sa), cb = hexToRgb(sb);
    var A = shape(ca), B = shape(cb);
    var same = A.nums.length === B.nums.length && A.nums.length > 0 && A.rest.length === B.rest.length;
    for (var i = 0; same && i < A.rest.length; i++) if (A.rest[i].trim() !== B.rest[i].trim()) same = false;
    if (!same) return t < 0.5 ? a : b;
    var out = A.rest[0];
    for (var k = 0; k < A.nums.length; k++) {
      out += (Math.round((A.nums[k] + (B.nums[k] - A.nums[k]) * t) * 10000) / 10000) + A.rest[k + 1];
    }
    // Colour channels are whole numbers in [0, 255], alpha is in [0, 1]
    out = out.replace(/rgba\(([^)]*)\)/gi, function (all, inner) {
      var c = inner.split(',').map(function (x) { return +x; });
      if (c.length !== 4 || !c.every(isFinite)) return all;
      return 'rgba(' + [0, 1, 2].map(function (i) { return Math.max(0, Math.min(255, Math.round(c[i]))); }).join(', ') +
             ', ' + Math.max(0, Math.min(1, c[3])) + ')';
    });
    return out;
  }

  // ------------------------------------------------------------- keyframes
  // Keyframes in either form (Web Animations 1 §5.3.3): an array of
  // keyframe objects, or one object of property -> value(s)
  function normalizeKeyframes(input) {
    var frames = [];
    if (input === null || input === undefined) return frames;
    if (typeof input !== 'object') throw new TypeError("Failed to execute 'animate' on 'Element': The provided value is not of type '(sequence<object> or object)'.");
    var iter = typeof Symbol !== 'undefined' && Symbol.iterator && typeof input[Symbol.iterator] === 'function';
    if (Array.isArray(input) || iter) {
      var list = Array.isArray(input) ? input : Array.from(input);
      for (var i = 0; i < list.length; i++) {
        var src = list[i], f = { offset: null, easing: 'linear', composite: 'replace', props: {} };
        if (src === null || typeof src !== 'object') throw new TypeError('Keyframes must be objects');
        for (var k in src) {
          if (k === 'offset') f.offset = src[k] === null || src[k] === undefined ? null : +src[k];
          else if (k === 'easing') f.easing = String(src[k]);
          else if (k === 'composite') f.composite = String(src[k]);
          else if (src[k] !== undefined) f.props[k] = String(src[k]);
        }
        frames.push(f);
      }
    } else {
      // Property-indexed: each property's values are spaced evenly
      var props = Object.keys(input).filter(function (k) { return k !== 'offset' && k !== 'easing' && k !== 'composite'; });
      var count = 0;
      props.forEach(function (p) { var v = input[p]; count = Math.max(count, Array.isArray(v) ? v.length : 1); });
      var offsets = Array.isArray(input.offset) ? input.offset : input.offset !== undefined ? [input.offset] : [];
      var easings = Array.isArray(input.easing) ? input.easing : input.easing !== undefined ? [input.easing] : [];
      props.forEach(function (p) {
        var values = Array.isArray(input[p]) ? input[p] : [input[p]];
        values.forEach(function (v, idx) {
          var off = values.length === 1 ? 1 : idx / (values.length - 1);
          var f = null;
          for (var j = 0; j < frames.length; j++) if (frames[j]._computed === off) { f = frames[j]; break; }
          if (!f) { f = { offset: null, _computed: off, easing: 'linear', composite: 'replace', props: {} }; frames.push(f); }
          f.props[p] = String(v);
        });
      });
      frames.sort(function (a, b) { return a._computed - b._computed; });
      frames.forEach(function (f, idx) {
        if (offsets[idx] !== undefined && offsets[idx] !== null) f.offset = +offsets[idx];
        if (easings.length) f.easing = String(easings[idx % easings.length]);
        f.offset = f.offset === null ? f._computed : f.offset;
        delete f._computed;
      });
    }
    var last = -Infinity;
    frames.forEach(function (f) {
      if (f.offset !== null) {
        if (!(f.offset >= 0 && f.offset <= 1)) throw new TypeError('Offsets must be null or in the range [0,1].');
        if (f.offset < last) throw new TypeError('Offsets must be monotonically non-decreasing.');
        last = f.offset;
      }
      parseEasing(f.easing);
    });
    return frames;
  }
  // Missing offsets: first 0 (1 when alone), last 1, the rest evenly between
  function computedOffsets(frames) {
    var n = frames.length, out = frames.map(function (f) { return f.offset; });
    if (!n) return out;
    if (out[n - 1] === null) out[n - 1] = 1;
    if (n > 1 && out[0] === null) out[0] = 0;
    for (var i = 1; i < n; ) {
      if (out[i] !== null) { i++; continue; }
      var j = i; while (out[j] === null) j++;
      var a = out[i - 1], b = out[j];
      for (var k = i; k < j; k++) out[k] = a + (b - a) * (k - i + 1) / (j - i + 1);
      i = j;
    }
    return out;
  }

  // ---------------------------------------------------------------- timing
  function normalizeTiming(options, base) {
    var t = base ? Object.assign({}, base) : { delay: 0, endDelay: 0, fill: 'auto', iterationStart: 0, iterations: 1,
                                               duration: 'auto', direction: 'normal', easing: 'linear' };
    if (typeof options === 'number') options = { duration: options };
    if (options && typeof options === 'object') {
      ['delay', 'endDelay', 'iterationStart', 'iterations'].forEach(function (k) { if (options[k] !== undefined) t[k] = +options[k]; });
      if (options.duration !== undefined) t.duration = options.duration === 'auto' ? 'auto' : +options.duration;
      if (options.fill !== undefined) t.fill = String(options.fill);
      if (options.direction !== undefined) t.direction = String(options.direction);
      if (options.easing !== undefined) t.easing = String(options.easing);
    }
    if (t.duration !== 'auto' && !(t.duration >= 0)) throw new TypeError("Failed to execute 'animate' on 'Element': duration must be non-negative or auto.");
    if (!(t.iterations >= 0)) throw new TypeError("Failed to execute 'animate' on 'Element': iterations must be non-negative.");
    if (!(t.iterationStart >= 0) || !isFinite(t.iterationStart)) throw new TypeError('iterationStart must be a non-negative number.');
    if (!isFinite(t.delay) || !isFinite(t.endDelay)) throw new TypeError('delay and endDelay must be finite.');
    if (['none', 'forwards', 'backwards', 'both', 'auto'].indexOf(t.fill) < 0) throw new TypeError("'" + t.fill + "' is not a valid fill mode");
    if (['normal', 'reverse', 'alternate', 'alternate-reverse'].indexOf(t.direction) < 0) throw new TypeError("'" + t.direction + "' is not a valid direction");
    parseEasing(t.easing);
    return t;
  }

  // ---------------------------------------------------------- KeyframeEffect
  function AnimationEffect() { throw new TypeError('Illegal constructor'); }
  function KeyframeEffect(target, keyframes, options) {
    if (target !== null && target !== undefined && !(target instanceof Element)) throw new TypeError("Failed to construct 'KeyframeEffect': parameter 1 is not of type 'Element'.");
    if (keyframes instanceof KeyframeEffect) {      // copy constructor
      var src = keyframes;
      this._target = src._target; this._frames = src._frames.slice(); this._timing = Object.assign({}, src._timing);
    } else {
      this._target = target || null;
      this._frames = normalizeKeyframes(keyframes);
      this._timing = normalizeTiming(options);
    }
    this._animation = null;
    this.pseudoElement = null;
    this.composite = 'replace';
  }
  KeyframeEffect.prototype = Object.create(AnimationEffect.prototype);
  KeyframeEffect.prototype.constructor = KeyframeEffect;
  Object.defineProperty(KeyframeEffect.prototype, 'target', {
    get: function () { return this._target; },
    set: function (v) { var old = this._target; this._target = v || null; touch(old); touch(this._target); },
    configurable: true
  });
  KeyframeEffect.prototype.getKeyframes = function () {
    var offs = computedOffsets(this._frames);
    return this._frames.map(function (f, i) {
      var o = { offset: f.offset, computedOffset: offs[i], easing: f.easing, composite: 'auto' };
      for (var p in f.props) o[p] = f.props[p];
      return o;
    });
  };
  KeyframeEffect.prototype.setKeyframes = function (k) { this._frames = normalizeKeyframes(k); this._underlying = null; touch(this._target); };
  KeyframeEffect.prototype.getTiming = function () {
    var t = Object.assign({}, this._timing);
    return t;
  };
  KeyframeEffect.prototype.updateTiming = function (o) { this._timing = normalizeTiming(o || {}, this._timing); if (this._animation) this._animation._update(); };
  KeyframeEffect.prototype._duration = function () { return this._timing.duration === 'auto' ? 0 : this._timing.duration; };
  KeyframeEffect.prototype._activeDuration = function () {
    var d = this._duration(), n = this._timing.iterations;
    return d === 0 || n === 0 ? 0 : d * n;
  };
  KeyframeEffect.prototype._endTime = function () {
    return Math.max(this._timing.delay + this._activeDuration() + this._timing.endDelay, 0);
  };
  // Web Animations 1 §4.8: local time -> iteration progress (null: no effect)
  KeyframeEffect.prototype._computed = function (localTime, rate) {
    var t = this._timing, d = this._duration(), ad = this._activeDuration(), end = this._endTime();
    var fill = t.fill === 'auto' ? 'none' : t.fill;
    var r = { localTime: localTime, activeDuration: ad, endTime: end, progress: null, currentIteration: null, phase: 'idle' };
    if (localTime === null) return r;
    var beforeEnd = Math.max(Math.min(t.delay, end), 0), activeEnd = Math.max(Math.min(t.delay + ad, end), 0);
    var phase;
    if (localTime < beforeEnd || (rate < 0 && localTime === beforeEnd)) phase = 'before';
    else if (localTime > activeEnd || (rate >= 0 && localTime === activeEnd)) phase = 'after';
    else phase = 'active';
    r.phase = phase;
    var activeTime = null;
    if (phase === 'before') { if (fill === 'backwards' || fill === 'both') activeTime = Math.max(localTime - t.delay, 0); }
    else if (phase === 'active') activeTime = localTime - t.delay;
    else if (fill === 'forwards' || fill === 'both') activeTime = Math.max(Math.min(localTime - t.delay, ad), 0);
    if (activeTime === null) return r;
    var overall = d === 0 ? (phase === 'before' ? t.iterationStart : t.iterationStart + t.iterations) : activeTime / d + t.iterationStart;
    var simple = overall === Infinity ? t.iterationStart % 1 : overall % 1;
    if (simple === 0 && (phase === 'after' || (phase === 'active' && activeTime === ad && ad > 0)) && t.iterations !== 0 && overall !== 0) simple = 1;
    var iteration = phase === 'after' && t.iterations === Infinity ? Infinity : simple === 1 ? Math.floor(overall) - 1 : Math.floor(overall);
    // The iteration's direction (§4.9.1): an infinite one counts as forwards
    var forwards = t.direction === 'normal';
    if (t.direction === 'alternate' || t.direction === 'alternate-reverse') {
      var dIter = t.direction === 'alternate-reverse' ? iteration + 1 : iteration;
      forwards = dIter === Infinity || dIter % 2 === 0;
    }
    var directed = forwards ? simple : 1 - simple;
    var beforeFlag = (phase === 'before' && forwards) || (phase === 'after' && !forwards);
    r.progress = parseEasing(t.easing)(directed, beforeFlag);
    r.currentIteration = iteration;
    return r;
  };
  KeyframeEffect.prototype.getComputedTiming = function () {
    var a = this._animation;
    var c = this._computed(a ? a.currentTime : null, a ? a.playbackRate : 1);
    var t = this.getTiming();
    t.duration = this._duration();
    t.fill = t.fill === 'auto' ? 'none' : t.fill;
    t.activeDuration = c.activeDuration; t.endTime = c.endTime; t.localTime = c.localTime;
    t.progress = c.progress; t.currentIteration = c.currentIteration;
    return t;
  };
  // The effect's value of each property at `progress` (Web Animations 1 §5.3.4)
  KeyframeEffect.prototype._values = function (progress) {
    var out = {}, frames = this._frames, offs = computedOffsets(frames), self = this;
    var names = {};
    frames.forEach(function (f) { for (var p in f.props) names[p] = true; });
    Object.keys(names).forEach(function (p) {
      var list = [];
      frames.forEach(function (f, i) { if (p in f.props) list.push({ offset: offs[i], value: f.props[p], easing: f.easing }); });
      // An implicit keyframe at 0 or 1 holds the underlying value
      if (!list.length) return;
      if (list[0].offset !== 0) list.unshift({ offset: 0, value: self._underlyingValue(p), easing: 'linear' });
      if (list[list.length - 1].offset !== 1) list.push({ offset: 1, value: self._underlyingValue(p), easing: 'linear' });
      var a, b;
      if (progress < 0 && list.length > 1) { a = list[0]; b = list[1]; }
      else if (progress >= 1) {
        var lastAt1 = list.length - 1;
        if (progress === 1) { out[p] = list[lastAt1].value; return; }
        a = list[list.length - 2]; b = list[list.length - 1];
        if (!a) { out[p] = b.value; return; }
      } else {
        var i = 0;
        while (i < list.length - 1 && list[i + 1].offset <= progress) i++;
        a = list[i]; b = list[Math.min(i + 1, list.length - 1)];
        if (a === b) { out[p] = a.value; return; }
      }
      var span = b.offset - a.offset;
      var local = span === 0 ? 0 : (progress - a.offset) / span;
      out[p] = interpolate(a.value, b.value, parseEasing(a.easing)(local));
    });
    return out;
  };
  // The value of a property under this effect: the element's computed
  // value without its script animations, read once and kept
  KeyframeEffect.prototype._underlyingValue = function (p) {
    if (!this._underlying) this._underlying = {};
    if (!(p in this._underlying)) {
      var v = '';
      try {
        if (this._target) {
          var t = this._target, saved = t.__zepraAnimatedText || '';
          if (saved) setAnimated(t, '');
          v = getComputedStyle(t).getPropertyValue(cssName(p));
          if (saved) setAnimated(t, saved);
        }
      } catch (e) {}
      this._underlying[p] = v;
    }
    return this._underlying[p];
  };

  // --------------------------------------------------------------- timeline
  function AnimationTimeline() { throw new TypeError('Illegal constructor'); }
  function DocumentTimeline(options) {
    this._origin = options && options.originTime ? +options.originTime : 0;
  }
  DocumentTimeline.prototype = Object.create(AnimationTimeline.prototype);
  DocumentTimeline.prototype.constructor = DocumentTimeline;
  Object.defineProperty(DocumentTimeline.prototype, 'currentTime', {
    get: function () { return now() - this._origin; }, configurable: true
  });
  var docTimeline = new DocumentTimeline();

  // -------------------------------------------------------------- Animation
  var all = [];          // every animation not idle, in creation order (composite order)
  var touched = [];      // elements whose animated style must be recomposed
  var frameRequested = false;
  function touch(el) { if (el && touched.indexOf(el) < 0) touched.push(el); }

  function Animation(effect, timeline) {
    this._effect = effect || null;
    if (this._effect) this._effect._animation = this;
    this._timeline = timeline === undefined ? docTimeline : timeline;
    this._startTime = null;
    this._holdTime = null;
    this._rate = 1;
    this._state = 'idle';
    this._listeners = {};
    this._finishedSettled = false;
    this.id = '';
    this.onfinish = null;
    this.oncancel = null;
    this.onremove = null;
    this.replaceState = 'active';
    this.pending = false;
    this._newPromises();
    this._seq = ++Animation._count;
  }
  Animation._count = 0;
  Animation.prototype._newPromises = function () {
    var self = this;
    this._finished = new Promise(function (res, rej) { self._resolveFinished = res; self._rejectFinished = rej; });
    this._finished.catch(function () {});   // a cancel's rejection is not an unhandled one
    this._finishedSettled = false;
    this._ready = Promise.resolve(this);
  };
  Object.defineProperty(Animation.prototype, 'finished', { get: function () { return this._finished; }, configurable: true });
  Object.defineProperty(Animation.prototype, 'ready', { get: function () { return this._ready; }, configurable: true });
  Object.defineProperty(Animation.prototype, 'timeline', { get: function () { return this._timeline; }, configurable: true });
  Object.defineProperty(Animation.prototype, 'effect', {
    get: function () { return this._effect; },
    set: function (e) {
      if (this._effect) { this._effect._animation = null; touch(this._effect._target); }
      this._effect = e || null;
      if (e) e._animation = this;
      this._update();
    }, configurable: true
  });
  Object.defineProperty(Animation.prototype, 'playbackRate', {
    get: function () { return this._rate; },
    set: function (r) { var ct = this.currentTime; this._rate = +r; if (ct !== null) this.currentTime = ct; }, configurable: true
  });
  Animation.prototype._timelineTime = function () { return this._timeline ? this._timeline.currentTime : null; };
  Object.defineProperty(Animation.prototype, 'currentTime', {
    get: function () {
      if (this._holdTime !== null) return this._holdTime;
      var tt = this._timelineTime();
      if (tt === null || this._startTime === null) return null;
      return (tt - this._startTime) * this._rate;
    },
    set: function (v) {
      if (v === null) { if (this._holdTime !== null || this._startTime !== null) throw new TypeError('currentTime cannot be set to null'); return; }
      v = +v;
      var tt = this._timelineTime();
      if (this._holdTime !== null || this._startTime === null || tt === null || this._rate === 0) this._holdTime = v;
      else this._startTime = tt - v / this._rate;
      if (this._state === 'idle') this._state = 'paused';
      if (this._state === 'finished') this._state = 'running';
      this._finishedSettled = false;
      this._update();
    }, configurable: true
  });
  Object.defineProperty(Animation.prototype, 'startTime', {
    get: function () { return this._startTime; },
    set: function (v) {
      if (v === null) { var ct = this.currentTime; this._startTime = null; this._holdTime = ct; }
      else { this._startTime = +v; this._holdTime = null; if (this._state === 'idle' || this._state === 'paused') this._state = 'running'; }
      this._update();
    }, configurable: true
  });
  Object.defineProperty(Animation.prototype, 'playState', {
    get: function () {
      if (this._state === 'idle' || this.currentTime === null) return 'idle';
      if (this._state === 'paused') return 'paused';
      return this._isFinished() ? 'finished' : 'running';
    }, configurable: true
  });
  Animation.prototype._effectEnd = function () { return this._effect ? this._effect._endTime() : 0; };
  Animation.prototype._isFinished = function () {
    var ct = this.currentTime;
    if (ct === null) return false;
    return (this._rate > 0 && ct >= this._effectEnd()) || (this._rate < 0 && ct <= 0);
  };
  Animation.prototype._register = function () {
    if (all.indexOf(this) < 0) { all.push(this); all.sort(function (a, b) { return a._seq - b._seq; }); }
  };
  Animation.prototype.play = function () {
    var ct = this.currentTime, end = this._effectEnd();
    if (this._rate > 0 && (ct === null || ct < 0 || ct >= end)) this._holdTime = 0;
    else if (this._rate < 0 && (ct === null || ct <= 0 || ct > end)) {
      if (end === Infinity) throw abortError('Cannot play reversed Animation with infinite target effect end.');
      this._holdTime = end;
    } else if (this._rate === 0 && ct === null) this._holdTime = 0;
    else if (this._holdTime === null && ct !== null) this._holdTime = ct;
    // Starts at once (no pending play task): the hold time becomes a start time
    var tt = this._timelineTime();
    if (tt !== null && this._holdTime !== null && this._rate !== 0) {
      this._startTime = tt - this._holdTime / this._rate;
      this._holdTime = null;
    }
    this._newPromisesIfSettled();   // a settled finished promise is replaced
    this._state = 'running';
    this._register();
    this._update();
  };
  Animation.prototype._newPromisesIfSettled = function () { if (this._finishedSettled) this._newPromises(); };
  Animation.prototype.pause = function () {
    if (this._state === 'paused') return;
    var ct = this.currentTime;
    if (ct === null) ct = this._rate >= 0 ? 0 : this._effectEnd();
    this._holdTime = ct;
    this._startTime = null;
    this._newPromisesIfSettled();
    this._state = 'paused';
    this._register();
    this._update();
  };
  Animation.prototype.finish = function () {
    var end = this._effectEnd();
    if (this._rate === 0 || (this._rate > 0 && end === Infinity)) throw new (g.DOMException || Error)('Cannot finish Animation', 'InvalidStateError');
    var limit = this._rate > 0 ? end : 0;
    var tt = this._timelineTime();
    if (tt !== null) { this._startTime = tt - limit / this._rate; this._holdTime = null; }
    else this._holdTime = limit;
    if (this._state === 'idle' || this._state === 'paused') this._state = 'running';
    this._register();
    this._update(true);
  };
  Animation.prototype.cancel = function () {
    if (this._state !== 'idle') {
      if (!this._finishedSettled) { this._finishedSettled = true; this._rejectFinished(abortError('The user aborted a request.')); }
      this._newPromises();
      var self = this;
      microtask(function () { self._dispatch('cancel', { type: 'cancel', currentTime: null, timelineTime: self._timelineTime() }); });
    }
    this._state = 'idle';
    this._holdTime = null;
    this._startTime = null;
    var i = all.indexOf(this);
    if (i >= 0) all.splice(i, 1);
    if (this._effect) touch(this._effect._target);
    flush();
  };
  Animation.prototype.reverse = function () {
    this._rate = -this._rate;
    try { this.play(); } catch (e) { this._rate = -this._rate; throw e; }
  };
  Animation.prototype.updatePlaybackRate = function (r) { this.playbackRate = r; };
  Animation.prototype.persist = function () { this.replaceState = 'persisted'; };
  Animation.prototype.commitStyles = function () {
    var e = this._effect, el = e && e._target;
    if (!el || !el.style) return;
    var c = e._computed(this.currentTime, this._rate);
    if (c.progress === null) return;
    var vals = e._values(c.progress);
    for (var p in vals) el.style.setProperty(cssName(p), vals[p]);
  };
  Animation.prototype.addEventListener = function (type, fn) {
    if (typeof fn !== 'function' && !(fn && typeof fn.handleEvent === 'function')) return;
    (this._listeners[type] = this._listeners[type] || []).push(fn);
  };
  Animation.prototype.removeEventListener = function (type, fn) {
    var l = this._listeners[type]; if (!l) return;
    var i = l.indexOf(fn); if (i >= 0) l.splice(i, 1);
  };
  Animation.prototype.dispatchEvent = function (ev) { this._dispatch(ev.type, ev); return true; };
  Animation.prototype._dispatch = function (type, ev) {
    ev.target = ev.currentTarget = this;
    var h = this['on' + type];
    if (typeof h === 'function') { try { h.call(this, ev); } catch (e) { report(e); } }
    (this._listeners[type] || []).slice().forEach(function (fn) {
      try { typeof fn === 'function' ? fn.call(this, ev) : fn.handleEvent(ev); } catch (e) { report(e); }
    }, this);
  };
  function report(e) { if (typeof g.reportError === 'function') g.reportError(e); else setTimeout(function () { throw e; }); }
  // Updates the finished state (Web Animations 1 §4.4.15) and the styles
  Animation.prototype._update = function (synchronous) {
    this._checkFinished(synchronous);
    if (this._effect) touch(this._effect._target);
    flush();
  };
  Animation.prototype._checkFinished = function (synchronous) {
    if (this._state !== 'idle' && this._state !== 'paused' && this._isFinished()) {
      // A finished animation holds at its end
      var limit = this._rate > 0 ? this._effectEnd() : 0;
      var ct = this.currentTime;
      if ((this._rate > 0 && ct > limit) || (this._rate < 0 && ct < limit)) {
        this._holdTime = limit; this._startTime = null;
      }
      if (!this._finishedSettled) {
        this._finishedSettled = true;
        var self = this;
        // The finished promise resolves first; the finish event is a task
        // queued after it (§4.4.15 "finish notification steps")
        var fire = function () {
          var ev = { type: 'finish', currentTime: self.currentTime, timelineTime: self._timelineTime() };
          setTimeout(function () { self._dispatch('finish', ev); }, 0);
        };
        var notify = function () {
          if (self.playState !== 'finished') { self._finishedSettled = false; return; }
          self._resolveFinished(self);
          fire();
        };
        if (synchronous) { self._resolveFinished(self); fire(); }
        else microtask(notify);
      }
    }
  };

  // Composes each touched element's animated style from its animations
  function flush() {
    var targets = touched; touched = [];
    var running = false;
    var perTarget = new Map();
    for (var i = 0; i < all.length; i++) {
      var a = all[i], e = a._effect;
      if (a._state === 'running' && !a._isFinished()) running = true;
      if (!e || !e._target) continue;
      if (!perTarget.has(e._target)) perTarget.set(e._target, []);
      perTarget.get(e._target).push(a);
    }
    perTarget.forEach(function (list, el) { if (targets.indexOf(el) < 0) targets.push(el); });
    targets.forEach(function (el) {
      var vals = {};
      (perTarget.get(el) || []).forEach(function (a) {
        var c = a._effect._computed(a.currentTime, a._rate);
        if (c.progress === null) return;
        var v = a._effect._values(c.progress);
        for (var p in v) vals[cssName(p)] = v[p];   // later animations replace earlier ones
      });
      var text = Object.keys(vals).map(function (p) { return p + ': ' + vals[p]; }).join('; ');
      if (text) text += ';';
      if ((el.__zepraAnimatedText || '') !== text) {
        try { Object.defineProperty(el, '__zepraAnimatedText', { value: text, writable: true, configurable: true, enumerable: false }); }
        catch (x) { el.__zepraAnimatedText = text; }
        setAnimated(el, text);
      }
    });
    if (running && !frameRequested) {
      frameRequested = true;
      raf(function () {
        frameRequested = false;
        var list = all.slice();
        for (var i = 0; i < list.length; i++) {
          var a = list[i];
          a._checkFinished(false);
          if (a._effect) touch(a._effect._target);
          // A finished animation that does not fill has no more effect
          if (a.playState === 'finished' && (!a._effect || a._effect._computed(a.currentTime, a._rate).progress === null)) {
            var at = all.indexOf(a);
            if (at >= 0) all.splice(at, 1);
          }
        }
        flush();
      });
    }
  }

  // ---------------------------------------------------------------- install
  E.animate = function animate(keyframes, options) {
    var effect = new KeyframeEffect(this, keyframes, options);
    var a = new Animation(effect, docTimeline);
    if (options && typeof options === 'object' && options.id !== undefined) a.id = String(options.id);
    a.play();
    return a;
  };
  function relevant(a) {
    if (a._state === 'idle' || !a._effect) return false;
    if (a.playState !== 'finished') return true;
    var c = a._effect._computed(a.currentTime, a._rate);
    return c.progress !== null;   // still filling
  }
  E.getAnimations = function getAnimations(options) {
    var el = this, sub = options && options.subtree;
    return all.filter(function (a) {
      var t = a._effect && a._effect._target;
      return relevant(a) && t && (t === el || (sub && el.contains && el.contains(t)));
    });
  };
  // On Document.prototype: the page's document object can be made after this
  // script runs (a navigation keeps the realm)
  var D = typeof Document !== 'undefined' && Document.prototype ? Document.prototype
        : (typeof document !== 'undefined' ? document : null);
  if (D) {
    D.getAnimations = function getAnimations() { return all.filter(relevant); };
    try { Object.defineProperty(D, 'timeline', { get: function () { return docTimeline; }, configurable: true }); }
    catch (x) { D.timeline = docTimeline; }
  }
  var def = typeof __zepraDefineGlobal === 'function' ? __zepraDefineGlobal : function (n, v) { g[n] = v; };
  def('Animation', Animation);
  def('AnimationEffect', AnimationEffect);
  def('KeyframeEffect', KeyframeEffect);
  def('AnimationTimeline', AnimationTimeline);
  def('DocumentTimeline', DocumentTimeline);
})(typeof globalThis !== 'undefined' ? globalThis : this);
)JS";
}

} // namespace Zepra::WebCore
