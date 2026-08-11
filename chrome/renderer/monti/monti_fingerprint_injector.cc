// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/renderer/monti/monti_fingerprint_injector.h"

#include "base/strings/strcat.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_script_source.h"
#include "url/gurl.h"

namespace monti {

namespace {

// The spoofing script body. `FP` (the fingerprint object) and the masking
// helpers are already defined when this runs. Every surface is wrapped in its
// own try/catch so a single failure cannot break the others or surface a JS
// error (an error would itself be a detection tell). Each per-surface mode is
// honored: "real" leaves the surface untouched; "noise" perturbs determinist;
// "off"/"manual" use the provided values.
//
// Determinism: canvas/audio noise is seeded by FP.seed via a position-stable
// hash, so a profile's hashes are identical across reloads and restarts but
// differ from other profiles. toString() of every override is masked to look
// native to defeat the obvious `fn.toString().includes('native code')` check.
constexpr char kSpoofBody[] = R"JS(
  'use strict';

  // ---- native-looking toString() masking ----------------------------------
  const _nativeToString = Function.prototype.toString;
  const _masks = new WeakMap();
  const _patchedToString = function toString() {
    if (_masks.has(this)) { return _masks.get(this); }
    return _nativeToString.call(this);
  };
  _masks.set(_patchedToString, 'function toString() { [native code] }');
  try { Function.prototype.toString = _patchedToString; } catch (e) {}
  // Mark `fn` so fn.toString() reports a native body for `display`.
  const mask = (fn, display) => { try { _masks.set(fn, display); } catch (e) {} return fn; };

  // Replace a method on `proto` with `impl`, masking its toString().
  const patchMethod = (proto, name, impl) => {
    if (!proto || typeof proto[name] !== 'function') { return; }
    const orig = proto[name];
    const wrapped = function(...args) { return impl.call(this, orig, args); };
    mask(wrapped, 'function ' + name + '() { [native code] }');
    try {
      Object.defineProperty(proto, name, {
        value: wrapped, writable: true, enumerable: false, configurable: true,
      });
    } catch (e) {}
  };

  // Define a spoofed accessor whose getter toString() also looks native.
  const defineGetter = (obj, name, getter) => {
    mask(getter, 'function get ' + name + '() { [native code] }');
    try {
      Object.defineProperty(obj, name, {
        get: getter, enumerable: true, configurable: true,
      });
    } catch (e) {}
  };

  // ---- navigator ----------------------------------------------------------
  try {
    const N = window.Navigator && Navigator.prototype;
    const platformMap = { windows: 'Win32', macos: 'MacIntel', linux: 'Linux x86_64' };
    if (N) {
      const plat = platformMap[FP.preset] || FP.platform;
      if (plat) { defineGetter(N, 'platform', () => plat); }
      if (FP.cpu_cores > 0) { defineGetter(N, 'hardwareConcurrency', () => FP.cpu_cores); }
      if (FP.memory_gb > 0) {
        // deviceMemory is clamped by spec to {0.25,0.5,1,2,4,8}; round down.
        const buckets = [8, 4, 2, 1, 0.5, 0.25];
        const dm = buckets.find(b => FP.memory_gb >= b) || 0.25;
        defineGetter(N, 'deviceMemory', () => dm);
      }
      if (Array.isArray(FP.languages) && FP.languages.length) {
        const langs = Object.freeze(FP.languages.slice());
        defineGetter(N, 'languages', () => langs);
        defineGetter(N, 'language', () => langs[0]);
      }
      if (FP.do_not_track === true) {
        defineGetter(N, 'doNotTrack', () => '1');
        defineGetter(N, 'msDoNotTrack', () => '1');
        try { Object.defineProperty(window, 'doNotTrack', {
          value: '1', enumerable: true, configurable: true,
        }); } catch (e) {}
      }
    }
  } catch (e) {}

  // ---- WebGPU -------------------------------------------------------------
  try {
    if (FP.webgpu_mode === 'off') {
      const N = window.Navigator && Navigator.prototype;
      if (N) { defineGetter(N, 'gpu', () => undefined); }
    }
  } catch (e) {}

  // ---- local port protection ---------------------------------------------
  try {
    const protectedPorts = new Set(String(FP.ports_to_protect || '')
      .split(',')
      .map(p => parseInt(p.trim(), 10))
      .filter(p => Number.isInteger(p) && p > 0 && p < 65536));
    const localHosts = new Set([
      '127.0.0.1', 'localhost', '0.0.0.0', '[::1]', '::1',
    ]);
    const isProtectedLocalUrl = (value) => {
      if (!protectedPorts.size) { return false; }
      try {
        const raw = typeof value === 'string' ? value :
          value && typeof value.url === 'string' ? value.url : String(value || '');
        const url = new URL(raw, location.href);
        if (!['http:', 'https:', 'ws:', 'wss:'].includes(url.protocol)) { return false; }
        const host = url.hostname.toLowerCase();
        const port = url.port ? parseInt(url.port, 10) :
          url.protocol === 'https:' || url.protocol === 'wss:' ? 443 : 80;
        return localHosts.has(host) && protectedPorts.has(port);
      } catch (e) {
        return false;
      }
    };
    const blockedFetch = () => Promise.reject(new TypeError('Failed to fetch'));
    if (typeof window.fetch === 'function') {
      const origFetch = window.fetch;
      const wrappedFetch = function(input, init) {
        if (isProtectedLocalUrl(input)) { return blockedFetch(); }
        return origFetch.apply(this, arguments);
      };
      mask(wrappedFetch, 'function fetch() { [native code] }');
      try { window.fetch = wrappedFetch; } catch (e) {}
    }
    if (window.XMLHttpRequest) {
      const originalOpen = XMLHttpRequest.prototype.open;
      const originalSend = XMLHttpRequest.prototype.send;
      patchMethod(XMLHttpRequest.prototype, 'open', function(orig, args) {
        this.__monti_blocked_port__ = isProtectedLocalUrl(args[1]);
        return originalOpen.apply(this, args);
      });
      patchMethod(XMLHttpRequest.prototype, 'send', function(orig, args) {
        if (this.__monti_blocked_port__) {
          try { this.abort(); } catch (e) {}
          throw new DOMException('NetworkError', 'NetworkError');
        }
        return originalSend.apply(this, args);
      });
    }
    if (window.WebSocket) {
      const NativeWebSocket = window.WebSocket;
      const WrappedWebSocket = function(url, protocols) {
        if (isProtectedLocalUrl(url)) {
          throw new DOMException('Failed to construct WebSocket', 'SecurityError');
        }
        return arguments.length > 1 ? new NativeWebSocket(url, protocols) : new NativeWebSocket(url);
      };
      WrappedWebSocket.prototype = NativeWebSocket.prototype;
      try {
        Object.setPrototypeOf(WrappedWebSocket, NativeWebSocket);
        Object.defineProperty(window, 'WebSocket', {
          value: mask(WrappedWebSocket, 'function WebSocket() { [native code] }'),
          writable: true, enumerable: false, configurable: true,
        });
      } catch (e) {}
    }
    if (window.EventSource) {
      const NativeEventSource = window.EventSource;
      const WrappedEventSource = function(url, config) {
        if (isProtectedLocalUrl(url)) {
          throw new DOMException('Failed to construct EventSource', 'SecurityError');
        }
        return arguments.length > 1 ? new NativeEventSource(url, config) : new NativeEventSource(url);
      };
      WrappedEventSource.prototype = NativeEventSource.prototype;
      try {
        Object.setPrototypeOf(WrappedEventSource, NativeEventSource);
        Object.defineProperty(window, 'EventSource', {
          value: mask(WrappedEventSource, 'function EventSource() { [native code] }'),
          writable: true, enumerable: false, configurable: true,
        });
      } catch (e) {}
    }
    const blankBlockedUrl = 'about:blank';
    patchMethod(Element.prototype, 'setAttribute', function(orig, args) {
      const name = String(args[0] || '').toLowerCase();
      if ((name === 'src' || name === 'href') && isProtectedLocalUrl(args[1])) {
        args[1] = blankBlockedUrl;
      }
      return orig.apply(this, args);
    });
    const patchUrlProperty = (proto, name) => {
      if (!proto) { return; }
      const desc = Object.getOwnPropertyDescriptor(proto, name);
      if (!desc || typeof desc.set !== 'function' || typeof desc.get !== 'function') { return; }
      const setter = function(value) {
        return desc.set.call(this, isProtectedLocalUrl(value) ? blankBlockedUrl : value);
      };
      const getter = function() { return desc.get.call(this); };
      mask(setter, 'function set ' + name + '() { [native code] }');
      mask(getter, 'function get ' + name + '() { [native code] }');
      try {
        Object.defineProperty(proto, name, {
          get: getter, set: setter, enumerable: desc.enumerable, configurable: true,
        });
      } catch (e) {}
    };
    patchUrlProperty(window.HTMLImageElement && HTMLImageElement.prototype, 'src');
    patchUrlProperty(window.HTMLScriptElement && HTMLScriptElement.prototype, 'src');
    patchUrlProperty(window.HTMLIFrameElement && HTMLIFrameElement.prototype, 'src');
    patchUrlProperty(window.HTMLLinkElement && HTMLLinkElement.prototype, 'href');
  } catch (e) {}

  // ---- screen -------------------------------------------------------------
  try {
    // FP.screen is "W × H · depth-bit", e.g. "1512 × 982 · 30-bit".
    const m = /^\s*(\d+)\s*[×x]\s*(\d+)\s*(?:[·.|-]+\s*(\d+))?/.exec(FP.screen || '');
    if (m) {
      const w = parseInt(m[1], 10), h = parseInt(m[2], 10);
      const depth = m[3] ? parseInt(m[3], 10) : 24;
      const S = window.Screen && Screen.prototype;
      if (S && w && h) {
        defineGetter(S, 'width', () => w);
        defineGetter(S, 'height', () => h);
        defineGetter(S, 'availWidth', () => w);
        // Leave a little vertical room for an OS bar, consistent across reads.
        defineGetter(S, 'availHeight', () => h);
        defineGetter(S, 'colorDepth', () => depth);
        defineGetter(S, 'pixelDepth', () => depth);
      }
    }
  } catch (e) {}

  // ---- timezone -----------------------------------------------------------
  try {
    if (FP.timezone) {
      const tz = FP.timezone;
      const ODTF = Intl.DateTimeFormat;
      const origGetTZO = Date.prototype.getTimezoneOffset;  // for safe fallback
      // Compute the real offset (minutes, JS sign) for `tz` at `date` using the
      // host ICU tz database, so getTimezoneOffset stays coherent with the name.
      const offsetFor = (date) => {
        try {
          const dtf = new ODTF('en-US', {
            timeZone: tz, hour12: false, year: 'numeric', month: '2-digit',
            day: '2-digit', hour: '2-digit', minute: '2-digit', second: '2-digit',
          });
          const p = {};
          for (const part of dtf.formatToParts(date)) { p[part.type] = part.value; }
          const asUTC = Date.UTC(+p.year, +p.month - 1, +p.day,
                                 +p.hour % 24, +p.minute, +p.second);
          return Math.round((date.getTime() - asUTC) / 60000);
        } catch (e) { return origGetTZO.call(date); }
      };
      patchMethod(Date.prototype, 'getTimezoneOffset', function(orig) {
        return offsetFor(this);
      });
      patchMethod(Intl.DateTimeFormat.prototype, 'resolvedOptions', function(orig, args) {
        const opts = orig.apply(this, args);
        opts.timeZone = tz;
        return opts;
      });
    }
  } catch (e) {}

  // ---- WebGL vendor / renderer -------------------------------------------
  try {
    if (FP.webgl_mode !== 'real' && (FP.webgl_vendor || FP.webgl_renderer)) {
      // Only spoof the UNMASKED_* params (read via WEBGL_debug_renderer_info).
      // The masked VENDOR/RENDERER (0x1F00/0x1F01) intentionally stay native
      // ("WebKit" / "WebKit WebGL") to match real browser behavior.
      const UNMASKED_VENDOR = 0x9245;    // 37445
      const UNMASKED_RENDERER = 0x9246;  // 37446
      const patchGetParam = (proto) => {
        patchMethod(proto, 'getParameter', function(orig, args) {
          const p = args[0];
          if (p === UNMASKED_VENDOR && FP.webgl_vendor) { return FP.webgl_vendor; }
          if (p === UNMASKED_RENDERER && FP.webgl_renderer) { return FP.webgl_renderer; }
          return orig.apply(this, args);
        });
      };
      if (window.WebGLRenderingContext) { patchGetParam(WebGLRenderingContext.prototype); }
      if (window.WebGL2RenderingContext) { patchGetParam(WebGL2RenderingContext.prototype); }
    }
  } catch (e) {}

  // ---- seeded PRNG for canvas / audio noise -------------------------------
  // Position-stable: depends only on (seed, index), never on call order.
  const seed = (FP.seed >>> 0) || 0x9e3779b9;
  const noiseAt = (i) => {
    let h = (seed ^ (i * 2654435761)) >>> 0;
    h ^= h >>> 15; h = Math.imul(h, 0x85ebca6b); h ^= h >>> 13;
    return (h & 0xff) / 255;  // [0,1)
  };

  // ---- client rects noise ------------------------------------------------
  try {
    if (FP.client_rects_mode !== 'real' && FP.client_rects_mode !== 'off') {
      const adjust = (rect, salt) => {
        const delta = (noiseAt(salt) - 0.5) * 0.02;
        const init = {
          x: rect.x + delta,
          y: rect.y + delta,
          width: Math.max(0, rect.width + delta),
          height: Math.max(0, rect.height + delta),
        };
        init.left = init.x;
        init.top = init.y;
        init.right = init.x + init.width;
        init.bottom = init.y + init.height;
        try { return DOMRectReadOnly.fromRect(init); } catch (e) {}
        try { return new DOMRect(init.x, init.y, init.width, init.height); } catch (e) {}
        return rect;
      };
      patchMethod(Element.prototype, 'getBoundingClientRect', function(orig) {
        const rect = orig.apply(this);
        return adjust(rect, ((this.tagName || '').length + (this.id || '').length) * 97);
      });
    }
  } catch (e) {}

  // ---- media devices -----------------------------------------------------
  try {
    const MD = navigator.mediaDevices && Object.getPrototypeOf(navigator.mediaDevices);
    if (MD && FP.media_devices) {
      const text = String(FP.media_devices).toLowerCase();
      const countFor = (pattern) => {
        const m = new RegExp('(\\d+)\\s*' + pattern).exec(text);
        return m ? Math.max(0, Math.min(4, parseInt(m[1], 10))) : 0;
      };
      const cameras = countFor('(?:camera|cam|video)');
      const microphones = countFor('(?:microphone|mic|audioinput)');
      const speakers = countFor('(?:speaker|audiooutput)');
      const makeDevice = (kind, index, label) => Object.freeze({
        deviceId: 'monti-' + kind + '-' + index + '-' + seed.toString(16),
        groupId: 'monti-group-' + ((seed + index) >>> 0).toString(16),
        kind,
        label,
        toJSON() { return {
          deviceId: this.deviceId,
          groupId: this.groupId,
          kind: this.kind,
          label: this.label,
        }; },
      });
      const devices = [];
      for (let i = 0; i < cameras; i++) {
        devices.push(makeDevice('videoinput', i, cameras > 1 ? 'Integrated Camera ' + (i + 1) : 'Integrated Camera'));
      }
      for (let i = 0; i < microphones; i++) {
        devices.push(makeDevice('audioinput', i, microphones > 1 ? 'Microphone ' + (i + 1) : 'Microphone'));
      }
      for (let i = 0; i < speakers; i++) {
        devices.push(makeDevice('audiooutput', i, speakers > 1 ? 'Speakers ' + (i + 1) : 'Speakers'));
      }
      if (devices.length) {
        patchMethod(MD, 'enumerateDevices', function() {
          return Promise.resolve(devices.slice());
        });
      }
    }
  } catch (e) {}

  // ---- canvas noise -------------------------------------------------------
  try {
    if (FP.canvas_mode !== 'real' && FP.canvas_mode !== 'off') {
      // Capture pristine originals so the clone path never re-enters our own
      // patched methods (which would recurse / double-perturb).
      const origGetImageData = CanvasRenderingContext2D.prototype.getImageData;
      const perturb = (data) => {
        // Nudge a deterministic subset of channels by ±1 (imperceptible, but it
        // shifts the canvas hash to a stable per-profile value).
        for (let i = 0; i < data.length; i += 4) {
          const n = noiseAt(i);
          if (n < 0.20) {
            const d = n < 0.10 ? -1 : 1;
            data[i]   = Math.max(0, Math.min(255, data[i]   + d));
            data[i+1] = Math.max(0, Math.min(255, data[i+1] + d));
            data[i+2] = Math.max(0, Math.min(255, data[i+2] + d));
          }
        }
      };
      patchMethod(CanvasRenderingContext2D.prototype, 'getImageData', function(orig, args) {
        const img = orig.apply(this, args);
        try { perturb(img.data); } catch (e) {}
        return img;
      });
      // Returns a fresh canvas with `canvas`'s noised pixels baked in, using
      // only pristine originals.
      const noisyClone = (canvas) => {
        const w = canvas.width, h = canvas.height;
        const tmp = document.createElement('canvas');
        tmp.width = w; tmp.height = h;
        const ctx = tmp.getContext('2d');
        ctx.drawImage(canvas, 0, 0);
        const img = origGetImageData.call(ctx, 0, 0, w, h);
        perturb(img.data);
        ctx.putImageData(img, 0, 0);
        return tmp;
      };
      patchMethod(HTMLCanvasElement.prototype, 'toDataURL', function(orig, args) {
        try { return orig.apply(noisyClone(this), args); }
        catch (e) { return orig.apply(this, args); }
      });
      patchMethod(HTMLCanvasElement.prototype, 'toBlob', function(orig, args) {
        try { return orig.apply(noisyClone(this), args); }
        catch (e) { return orig.apply(this, args); }
      });
    }
  } catch (e) {}

  // ---- audio noise --------------------------------------------------------
  try {
    if (FP.audio_mode !== 'real' && FP.audio_mode !== 'off') {
      const noised = new WeakSet();
      patchMethod(AudioBuffer.prototype, 'getChannelData', function(orig, args) {
        const data = orig.apply(this, args);
        if (!noised.has(data)) {
          noised.add(data);
          try {
            for (let i = 0; i < data.length; i += 100) {
              data[i] = data[i] + (noiseAt(i) - 0.5) * 1e-7;
            }
          } catch (e) {}
        }
        return data;
      });
      patchMethod(AnalyserNode.prototype, 'getFloatFrequencyData', function(orig, args) {
        orig.apply(this, args);
        try {
          const arr = args[0];
          for (let i = 0; i < arr.length; i += 50) {
            arr[i] = arr[i] + (noiseAt(i) - 0.5) * 1e-5;
          }
        } catch (e) {}
      });
    }
  } catch (e) {}
)JS";

// Returns true for documents that should receive the spoof: web content only,
// never privileged/WebUI surfaces (chrome://, devtools://, extensions, the
// chrome://monti manager UI itself).
bool ShouldInject(blink::WebLocalFrame* frame) {
  GURL url = frame->GetDocument().Url();
  if (url.is_empty() || url.IsAboutBlank() || url.IsAboutSrcdoc()) {
    return true;  // Inherits its parent's context; leak tests use blank frames.
  }
  return url.SchemeIsHTTPOrHTTPS() || url.SchemeIs("file") ||
         url.SchemeIs("data") || url.SchemeIs("ftp");
}

}  // namespace

void InjectFingerprintSpoof(blink::WebLocalFrame* frame,
                            const std::string& fingerprint_json) {
  if (!frame || fingerprint_json.empty() || !ShouldInject(frame)) {
    return;
  }
  // Embed the fingerprint as a JS object literal (it is valid JSON, hence a
  // valid JS expression) and run the body once per window object.
  std::string script = base::StrCat({
      "(function(){try{",
      "if(window.__monti_fp__)return;",
      "window.__monti_fp__=true;",
      "var FP=", fingerprint_json, ";",
      kSpoofBody,
      "}catch(e){}})();",
  });
  frame->ExecuteScript(
      blink::WebScriptSource(blink::WebString::FromUtf8(script)));
}

}  // namespace monti
