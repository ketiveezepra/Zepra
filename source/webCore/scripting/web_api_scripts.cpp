// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file web_api_scripts.cpp
 * @brief URL and URLSearchParams (WHATWG URL) and a
 *        minimal ReadableStream (Streams Standard: default controller and
 *        default reader), written in script. React's Flight client, which
 *        Next.js hydrates from, reads the page's inline RSC payload through
 *        exactly these: a ReadableStream fed with TextEncoder output and
 *        decoded with TextDecoder({stream:true}).
 */

#include "scripting/web_api_scripts.hpp"

namespace Zepra::WebCore {

const char* webApiScripts() {
    return R"JS(
(function (g) {
  function toBytes(input) {
    if (input === undefined || input === null) return new Uint8Array(0);
    if (input instanceof Uint8Array) return input;
    if (input instanceof ArrayBuffer) return new Uint8Array(input);
    if (input && input.buffer instanceof ArrayBuffer)
      return new Uint8Array(input.buffer, input.byteOffset || 0, input.byteLength);
    throw new TypeError('The provided value is not of type (ArrayBuffer or ArrayBufferView)');
  }

  // TextEncoder/TextDecoder: script_context.cpp's web shims

  if (typeof g.ReadableStream === 'undefined') {
    var resolved = function (v) { return Promise.resolve(v); };

    // The stream's state lives on the stream; the controller and reader are views of it
    var ReadableStreamDefaultController = function ReadableStreamDefaultController() {
      throw new TypeError('Illegal constructor');
    };
    var makeController = function (stream) {
      var c = Object.create(ReadableStreamDefaultController.prototype);
      c.__stream = stream;
      return c;
    };
    Object.defineProperty(ReadableStreamDefaultController.prototype, 'desiredSize', {
      get: function () {
        var s = this.__stream;
        if (s.__state === 'errored') return null;
        if (s.__state === 'closed') return 0;
        return s.__highWaterMark - s.__queue.length;
      }
    });
    ReadableStreamDefaultController.prototype.enqueue = function (chunk) {
      var s = this.__stream;
      if (s.__closeRequested || s.__state !== 'readable') throw new TypeError('Cannot enqueue a chunk into a closed stream');
      if (s.__waiting.length) s.__waiting.shift().resolve({ value: chunk, done: false });
      else s.__queue.push(chunk);
      pullIfNeeded(s);
    };
    ReadableStreamDefaultController.prototype.close = function () {
      var s = this.__stream;
      if (s.__closeRequested || s.__state !== 'readable') throw new TypeError('Cannot close a closed stream');
      s.__closeRequested = true;
      if (!s.__queue.length) finishClose(s);
    };
    ReadableStreamDefaultController.prototype.error = function (e) {
      var s = this.__stream;
      if (s.__state !== 'readable') return;
      s.__state = 'errored';
      s.__storedError = e;
      s.__queue = [];
      var w = s.__waiting; s.__waiting = [];
      w.forEach(function (p) { p.reject(e); });
      if (s.__reader) s.__reader.__closedReject(e);
    };

    function finishClose(s) {
      s.__state = 'closed';
      var w = s.__waiting; s.__waiting = [];
      w.forEach(function (p) { p.resolve({ value: undefined, done: true }); });
      if (s.__reader) s.__reader.__closedResolve();
    }

    function pullIfNeeded(s) {
      if (!s.__source || typeof s.__source.pull !== 'function' || !s.__started) return;
      if (s.__state !== 'readable' || s.__closeRequested) return;
      if (!(s.__waiting.length > 0 || s.__queue.length < s.__highWaterMark)) return;
      if (s.__pulling) { s.__pullAgain = true; return; }
      s.__pulling = true;
      resolved().then(function () { return s.__source.pull(s.__controller); }).then(function () {
        s.__pulling = false;
        if (s.__pullAgain) { s.__pullAgain = false; pullIfNeeded(s); }
      }, function (e) { s.__controller.error(e); });
    }

    var ReadableStream = function ReadableStream(source, strategy) {
      this.__source = source || {};
      this.__state = 'readable';
      this.__queue = [];
      this.__waiting = [];
      this.__reader = null;
      this.__closeRequested = false;
      this.__started = false;
      this.__highWaterMark = strategy && typeof strategy.highWaterMark === 'number' ? strategy.highWaterMark : 1;
      this.__controller = makeController(this);
      var s = this;
      var started = typeof this.__source.start === 'function' ? this.__source.start(this.__controller) : undefined;
      resolved(started).then(function () { s.__started = true; pullIfNeeded(s); },
                             function (e) { s.__controller.error(e); });
    };
    Object.defineProperty(ReadableStream.prototype, 'locked', { get: function () { return !!this.__reader; } });
    ReadableStream.prototype.getReader = function (options) {
      if (options && options.mode === 'byob') throw new TypeError('BYOB readers are not supported');
      return new ReadableStreamDefaultReader(this);
    };
    ReadableStream.prototype.cancel = function (reason) {
      if (this.__reader) return Promise.reject(new TypeError('Cannot cancel a locked stream'));
      return cancelStream(this, reason);
    };
    function cancelStream(s, reason) {
      if (s.__state === 'closed') return resolved();
      if (s.__state === 'errored') return Promise.reject(s.__storedError);
      s.__queue = [];
      finishClose(s);
      var r = s.__source && typeof s.__source.cancel === 'function' ? s.__source.cancel(reason) : undefined;
      return resolved(r).then(function () {});
    }
    ReadableStream.prototype.tee = function () {
      var reader = this.getReader(), a, b, ca, cb;
      a = new ReadableStream({ start: function (c) { ca = c; } });
      b = new ReadableStream({ start: function (c) { cb = c; } });
      (function pump() {
        reader.read().then(function (r) {
          if (r.done) { try { ca.close(); } catch (e) {} try { cb.close(); } catch (e) {} return; }
          try { ca.enqueue(r.value); } catch (e) {}
          try { cb.enqueue(r.value); } catch (e) {}
          pump();
        }, function (e) { ca.error(e); cb.error(e); });
      })();
      return [a, b];
    };
    ReadableStream.prototype.pipeThrough = function (transform) {
      this.pipeTo(transform.writable);
      return transform.readable;
    };
    ReadableStream.prototype.pipeTo = function (dest) {
      var reader = this.getReader(), writer = dest.getWriter();
      return (function pump() {
        return reader.read().then(function (r) {
          if (r.done) return writer.close();
          return resolved(writer.write(r.value)).then(pump);
        });
      })();
    };
    if (typeof Symbol !== 'undefined' && Symbol.asyncIterator) {
      ReadableStream.prototype[Symbol.asyncIterator] = function () {
        var reader = this.getReader();
        return {
          next: function () { return reader.read(); },
          'return': function () { reader.releaseLock(); return resolved({ value: undefined, done: true }); }
        };
      };
      ReadableStream.prototype.values = ReadableStream.prototype[Symbol.asyncIterator];
    }

    var ReadableStreamDefaultReader = function ReadableStreamDefaultReader(stream) {
      if (stream.__reader) throw new TypeError('ReadableStream is already locked to a reader');
      this.__stream = stream;
      stream.__reader = this;
      var r = this;
      this.__closed = new Promise(function (res, rej) { r.__closedResolve = res; r.__closedReject = rej; });
      this.__closed.catch(function () {});
      if (stream.__state === 'closed') this.__closedResolve();
      else if (stream.__state === 'errored') this.__closedReject(stream.__storedError);
    };
    Object.defineProperty(ReadableStreamDefaultReader.prototype, 'closed', { get: function () { return this.__closed; } });
    ReadableStreamDefaultReader.prototype.read = function () {
      var s = this.__stream;
      if (!s) return Promise.reject(new TypeError('This reader has been released'));
      if (s.__queue.length) {
        var v = s.__queue.shift();
        if (s.__closeRequested && !s.__queue.length) finishClose(s);
        else pullIfNeeded(s);
        return resolved({ value: v, done: false });
      }
      if (s.__state === 'closed') return resolved({ value: undefined, done: true });
      if (s.__state === 'errored') return Promise.reject(s.__storedError);
      var p = {};
      var promise = new Promise(function (res, rej) { p.resolve = res; p.reject = rej; });
      s.__waiting.push(p);
      pullIfNeeded(s);
      return promise;
    };
    ReadableStreamDefaultReader.prototype.releaseLock = function () {
      if (this.__stream) { this.__stream.__reader = null; this.__stream = null; }
    };
    ReadableStreamDefaultReader.prototype.cancel = function (reason) {
      return this.__stream ? cancelStream(this.__stream, reason) : Promise.reject(new TypeError('released'));
    };

    g.ReadableStream = ReadableStream;
    g.ReadableStreamDefaultReader = ReadableStreamDefaultReader;
    g.ReadableStreamDefaultController = ReadableStreamDefaultController;
  }

  // URLSearchParams and URL (WHATWG URL, the parts pages use): the native
  // stubs answered nothing (no relative URLs, search, searchParams, origin)
  (function () {
    function encodeForm(str) {
      return encodeURIComponent(str).replace(/%20/g, '+').replace(/[!'()~]/g, function (c) {
        return '%' + c.charCodeAt(0).toString(16).toUpperCase();
      });
    }
    function decodeForm(str) {
      try { return decodeURIComponent(str.replace(/\+/g, ' ')); } catch (e) { return str.replace(/\+/g, ' '); }
    }
    function USP(init) {
      this.__list = [];
      this.__url = null;
      if (init === undefined || init === null) return;
      if (typeof init === 'object' || typeof init === 'function') {
        if (init instanceof USP) { this.__list = init.__list.map(function (p) { return [p[0], p[1]]; }); return; }
        if (typeof Symbol !== 'undefined' && typeof init[Symbol.iterator] === 'function') {
          for (var it = init[Symbol.iterator](), r = it.next(); !r.done; r = it.next()) {
            var pair = Array.prototype.slice.call(r.value);
            if (pair.length !== 2) throw new TypeError('Each query pair must be an iterable [name, value] tuple');
            this.__list.push([String(pair[0]), String(pair[1])]);
          }
          return;
        }
        for (var k in init) if (Object.prototype.hasOwnProperty.call(init, k)) this.__list.push([k, String(init[k])]);
        return;
      }
      this.__parse(String(init));
    }
    USP.prototype.__parse = function (str) {
      this.__list = [];
      if (str.charAt(0) === '?') str = str.slice(1);
      if (!str) return;
      var parts = str.split('&');
      for (var i = 0; i < parts.length; i++) {
        if (!parts[i]) continue;
        var eq = parts[i].indexOf('=');
        var name = eq < 0 ? parts[i] : parts[i].slice(0, eq);
        var value = eq < 0 ? '' : parts[i].slice(eq + 1);
        this.__list.push([decodeForm(name), decodeForm(value)]);
      }
    };
    USP.prototype.__update = function () {
      if (this.__url) { var q = this.toString(); this.__url.__search = q ? '?' + q : ''; }
    };
    USP.prototype.append = function (n, v) { this.__list.push([String(n), String(v)]); this.__update(); };
    USP.prototype['delete'] = function (n, v) {
      n = String(n);
      this.__list = this.__list.filter(function (p) { return !(p[0] === n && (v === undefined || p[1] === String(v))); });
      this.__update();
    };
    USP.prototype.get = function (n) {
      n = String(n);
      for (var i = 0; i < this.__list.length; i++) if (this.__list[i][0] === n) return this.__list[i][1];
      return null;
    };
    USP.prototype.getAll = function (n) {
      n = String(n);
      return this.__list.filter(function (p) { return p[0] === n; }).map(function (p) { return p[1]; });
    };
    USP.prototype.has = function (n, v) {
      n = String(n);
      return this.__list.some(function (p) { return p[0] === n && (v === undefined || p[1] === String(v)); });
    };
    USP.prototype.set = function (n, v) {
      n = String(n); v = String(v);
      var found = false, out = [];
      for (var i = 0; i < this.__list.length; i++) {
        if (this.__list[i][0] !== n) out.push(this.__list[i]);
        else if (!found) { out.push([n, v]); found = true; }
      }
      if (!found) out.push([n, v]);
      this.__list = out;
      this.__update();
    };
    USP.prototype.sort = function () {
      var indexed = this.__list.map(function (p, i) { return [p, i]; });
      indexed.sort(function (a, b) { return a[0][0] < b[0][0] ? -1 : a[0][0] > b[0][0] ? 1 : a[1] - b[1]; });
      this.__list = indexed.map(function (x) { return x[0]; });
      this.__update();
    };
    USP.prototype.toString = function () {
      return this.__list.map(function (p) { return encodeForm(p[0]) + '=' + encodeForm(p[1]); }).join('&');
    };
    USP.prototype.forEach = function (cb, thisArg) {
      var list = this.__list.slice();
      for (var i = 0; i < list.length; i++) cb.call(thisArg, list[i][1], list[i][0], this);
    };
    function iter(list, pick) {
      var i = 0;
      var it = { next: function () { return i < list.length ? { value: pick(list[i++]), done: false } : { value: undefined, done: true }; } };
      if (typeof Symbol !== 'undefined') it[Symbol.iterator] = function () { return it; };
      return it;
    }
    USP.prototype.entries = function () { return iter(this.__list.slice(), function (p) { return [p[0], p[1]]; }); };
    USP.prototype.keys = function () { return iter(this.__list.slice(), function (p) { return p[0]; }); };
    USP.prototype.values = function () { return iter(this.__list.slice(), function (p) { return p[1]; }); };
    if (typeof Symbol !== 'undefined') {
      USP.prototype[Symbol.iterator] = USP.prototype.entries;
      if (Symbol.toStringTag) USP.prototype[Symbol.toStringTag] = 'URLSearchParams';
    }
    Object.defineProperty(USP.prototype, 'size', { get: function () { return this.__list.length; } });
    Object.defineProperty(USP, 'name', { value: 'URLSearchParams' });

    var special = { 'http:': 80, 'https:': 443, 'ws:': 80, 'wss:': 443, 'ftp:': 21, 'file:': null };
    var schemeRe = /^([a-zA-Z][a-zA-Z0-9+.\-]*):/;

    function encodePart(str, extra) {
      // Percent-encode what the URL parser would: controls, space, non-ASCII, and `extra`
      var out = '';
      for (var i = 0; i < str.length; i++) {
        var c = str.charAt(i), code = str.charCodeAt(i);
        if (code <= 0x20 || code >= 0x7F || extra.indexOf(c) >= 0) {
          if (code >= 0xD800 && code <= 0xDBFF && i + 1 < str.length) { c += str.charAt(++i); }
          try { out += encodeURIComponent(c); } catch (e) { out += c; }
        } else {
          out += c;
        }
      }
      return out;
    }
    function removeDots(path) {
      var input = path.split('/'), out = [];
      for (var i = 0; i < input.length; i++) {
        var seg = input[i], low = seg.toLowerCase();
        if (seg === '..' || low === '%2e%2e' || low === '.%2e' || low === '%2e.') {
          if (out.length > 1) out.pop();
          if (i === input.length - 1) out.push('');
        } else if (seg === '.' || low === '%2e') {
          if (i === input.length - 1) out.push('');
        } else {
          out.push(seg);
        }
      }
      var r = out.join('/');
      return r.charAt(0) === '/' ? r : '/' + r;
    }

    // Parses `input` against the parsed `base` (or null); returns the parts or null
    function parse(input, base) {
      input = String(input).replace(/^[\u0000- ]+|[\u0000- ]+$/g, '').replace(/[\t\n\r]/g, '');
      var u = { protocol: '', username: '', password: '', hostname: '', port: '', pathname: '', search: '', hash: '', opaque: false };
      var m = schemeRe.exec(input), rest;
      var hashAt = function (str) {
        var h = str.indexOf('#');
        if (h >= 0) { u.hash = '#' + encodePart(str.slice(h + 1), '"<>`'); str = str.slice(0, h); }
        var q = str.indexOf('?');
        if (q >= 0) { u.search = '?' + encodePart(str.slice(q + 1), '"#<>\''); str = str.slice(0, q); }
        return str;
      };
      if (m) {
        u.protocol = m[1].toLowerCase() + ':';
        rest = input.slice(m[0].length);
        var isSpecial = special.hasOwnProperty(u.protocol);
        if (isSpecial) rest = rest.replace(/\\/g, '/');
        if (rest.slice(0, 2) === '//' || (isSpecial && u.protocol !== 'file:')) {
          rest = rest.replace(/^\/*/, '');
          var end = rest.search(/[\/?#]/);
          var authority = end < 0 ? rest : rest.slice(0, end);
          rest = end < 0 ? '' : rest.slice(end);
          var at = authority.lastIndexOf('@');
          if (at >= 0) {
            var cred = authority.slice(0, at), colon = cred.indexOf(':');
            u.username = encodePart(colon < 0 ? cred : cred.slice(0, colon), '"#<>?`{}/:;=@[\\]^|');
            u.password = colon < 0 ? '' : encodePart(cred.slice(colon + 1), '"#<>?`{}/:;=@[\\]^|');
            authority = authority.slice(at + 1);
          }
          var portAt = authority.match(/:(\d*)$/);
          if (portAt && (authority.charAt(0) !== '[' || authority.indexOf(']') < authority.lastIndexOf(':'))) {
            u.port = portAt[1];
            authority = authority.slice(0, authority.length - portAt[0].length);
          }
          u.hostname = isSpecial ? authority.toLowerCase() : authority;
          if (isSpecial && !u.hostname && u.protocol !== 'file:') return null;
          if (u.port !== '' && special[u.protocol] === Number(u.port)) u.port = '';
          if (u.port !== '') u.port = String(Number(u.port));
          rest = hashAt(rest);
          u.pathname = isSpecial ? removeDots(encodePart(rest || '/', '"<>`{}#?')) : encodePart(rest, '"<>`{}#?');
        } else if (rest.charAt(0) === '/') {
          rest = hashAt(rest);
          u.pathname = removeDots(encodePart(rest, '"<>`{}#?'));
        } else {
          // Opaque path (mailto:, data:, about:, blob:, javascript:)
          u.opaque = true;
          rest = hashAt(rest);
          u.pathname = rest;
        }
        return u;
      }
      if (!base) return null;
      if (base.opaque) {
        if (input.charAt(0) !== '#') return null;
        u = Object.assign({}, base);
        u.hash = '#' + encodePart(input.slice(1), '"<>`');
        return u;
      }
      // Relative reference
      u.protocol = base.protocol; u.username = base.username; u.password = base.password;
      u.hostname = base.hostname; u.port = base.port;
      var isSp = special.hasOwnProperty(u.protocol);
      if (isSp) input = input.replace(/\\/g, '/');
      if (input.slice(0, 2) === '//') return parse(u.protocol + input, null);
      var str = hashAt(input);
      if (str === '') {
        u.pathname = base.pathname;
        if (input.indexOf('?') < 0) u.search = base.search;
      } else if (str.charAt(0) === '/') {
        u.pathname = removeDots(encodePart(str, '"<>`{}#?'));
      } else {
        var dir = base.pathname.slice(0, base.pathname.lastIndexOf('/') + 1);
        u.pathname = removeDots(encodePart(dir + str, '"<>`{}#?'));
      }
      return u;
    }

    function URLImpl(url, base) {
      if (!(this instanceof URLImpl)) throw new TypeError("Failed to construct 'URL': Please use the 'new' operator");
      var b = null;
      if (base !== undefined) {
        b = parse(base instanceof URLImpl ? base.href : String(base), null);
        if (!b) throw new TypeError("Failed to construct 'URL': Invalid base URL");
      }
      var u = parse(url instanceof URLImpl ? url.href : String(url), b);
      if (!u) throw new TypeError("Failed to construct 'URL': Invalid URL");
      this.__set(u);
    }
    URLImpl.prototype.__set = function (u) {
      this.__protocol = u.protocol; this.__username = u.username; this.__password = u.password;
      this.__hostname = u.hostname; this.__port = u.port; this.__pathname = u.pathname;
      this.__search = u.search === '?' ? '' : u.search; this.__hash = u.hash === '#' ? '' : u.hash;
      this.__opaque = u.opaque;
      if (!this.__sp) { this.__sp = new USP(); this.__sp.__url = this; }
      this.__sp.__parse(this.__search);
    };
    function host(t) { return t.__hostname + (t.__port ? ':' + t.__port : ''); }
    function hasAuthority(t) { return !t.__opaque && (special.hasOwnProperty(t.__protocol) || t.__hostname !== ''); }
    var props = {
      href: {
        get: function () {
          var auth = '';
          if (hasAuthority(this) || this.__protocol === 'file:') {
            auth = '//' + (this.__username || this.__password ? this.__username + (this.__password ? ':' + this.__password : '') + '@' : '') + host(this);
          }
          return this.__protocol + auth + this.__pathname + this.__search + this.__hash;
        },
        set: function (v) {
          var u = parse(String(v), null);
          if (!u) throw new TypeError("Failed to set the 'href' property on 'URL': Invalid URL");
          this.__set(u);
        }
      },
      origin: {
        get: function () {
          var p = this.__protocol;
          if (p === 'blob:') { try { return new URLImpl(this.__pathname).origin; } catch (e) { return 'null'; } }
          return special.hasOwnProperty(p) && p !== 'file:' ? p + '//' + host(this) : 'null';
        }
      },
      protocol: { get: function () { return this.__protocol; },
                  set: function (v) { var m = schemeRe.exec(String(v) + ':'); if (m) this.__protocol = m[1].toLowerCase() + ':'; } },
      username: { get: function () { return this.__username; }, set: function (v) { this.__username = encodePart(String(v), '"#<>?`{}/:;=@[\\]^|'); } },
      password: { get: function () { return this.__password; }, set: function (v) { this.__password = encodePart(String(v), '"#<>?`{}/:;=@[\\]^|'); } },
      host: { get: function () { return host(this); },
              set: function (v) { var parts = String(v).split(':'); this.__hostname = parts[0].toLowerCase(); if (parts.length > 1) this.__port = parts[1]; } },
      hostname: { get: function () { return this.__hostname; }, set: function (v) { this.__hostname = String(v).toLowerCase(); } },
      port: { get: function () { return this.__port; },
              set: function (v) { v = String(v); this.__port = v === '' || special[this.__protocol] === Number(v) ? '' : String(parseInt(v, 10)); } },
      pathname: { get: function () { return this.__pathname; },
                  set: function (v) { if (this.__opaque) return; v = String(v); if (v.charAt(0) !== '/') v = '/' + v; this.__pathname = removeDots(encodePart(v, '"<>`{}#?')); } },
      search: { get: function () { return this.__search; },
                set: function (v) { v = String(v); if (v.charAt(0) === '?') v = v.slice(1); this.__search = v ? '?' + encodePart(v, '"#<>\'') : ''; this.__sp.__parse(this.__search); } },
      hash: { get: function () { return this.__hash; },
              set: function (v) { v = String(v); if (v.charAt(0) === '#') v = v.slice(1); this.__hash = v ? '#' + encodePart(v, '"<>`') : ''; } },
      searchParams: { get: function () { return this.__sp; } }
    };
    for (var k in props) { props[k].configurable = true; props[k].enumerable = true; Object.defineProperty(URLImpl.prototype, k, props[k]); }
    URLImpl.prototype.toString = function () { return this.href; };
    URLImpl.prototype.toJSON = function () { return this.href; };
    URLImpl.canParse = function (url, base) { try { new URLImpl(url, base); return true; } catch (e) { return false; } };
    URLImpl.parse = function (url, base) { try { return new URLImpl(url, base); } catch (e) { return null; } };
    if (typeof Symbol !== 'undefined' && Symbol.toStringTag) URLImpl.prototype[Symbol.toStringTag] = 'URL';
    Object.defineProperty(URLImpl, 'name', { value: 'URL' });
    // Blob URLs stay with the native object, if it has them
    var nativeURL = g.URL;
    if (nativeURL) {
      if (typeof nativeURL.createObjectURL === 'function') URLImpl.createObjectURL = nativeURL.createObjectURL;
      if (typeof nativeURL.revokeObjectURL === 'function') URLImpl.revokeObjectURL = nativeURL.revokeObjectURL;
    }
    g.URL = URLImpl;
    g.URLSearchParams = USP;
    g.__zepraScriptURL = URLImpl;
    g.__zepraScriptUSP = USP;
  })();
})(typeof globalThis !== 'undefined' ? globalThis : this);
// The engine's own globals are separate from window's properties
__zepraDefineGlobal('URL', globalThis.__zepraScriptURL);
__zepraDefineGlobal('URLSearchParams', globalThis.__zepraScriptUSP);
if (typeof ReadableStream === 'undefined' && globalThis.ReadableStream) {
  __zepraDefineGlobal('ReadableStream', globalThis.ReadableStream);
  __zepraDefineGlobal('ReadableStreamDefaultReader', globalThis.ReadableStreamDefaultReader);
  __zepraDefineGlobal('ReadableStreamDefaultController', globalThis.ReadableStreamDefaultController);
}
)JS";
}

} // namespace Zepra::WebCore
