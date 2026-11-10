/* sound.js - أصوات مولّدة بالكود (WebAudio) بدون ملفات صوت */
(function (root) {
  'use strict';
  var ctx = null, enabled = true, noiseBuf = null;

  function ensure() {
    try {
      if (!ctx) {
        var C = root.AudioContext || root.webkitAudioContext;
        if (!C) return null;
        ctx = new C();
      }
      if (ctx.state === 'suspended') ctx.resume();
    } catch (e) { return null; }
    return ctx;
  }

  function tone(freq, t0, dur, type, gain, slideTo) {
    var o = ctx.createOscillator(), g = ctx.createGain();
    o.type = type || 'sine';
    o.frequency.setValueAtTime(freq, t0);
    if (slideTo) o.frequency.exponentialRampToValueAtTime(slideTo, t0 + dur);
    g.gain.setValueAtTime(0.0001, t0);
    g.gain.exponentialRampToValueAtTime(gain, t0 + 0.006);
    g.gain.exponentialRampToValueAtTime(0.0001, t0 + dur);
    o.connect(g); g.connect(ctx.destination);
    o.start(t0); o.stop(t0 + dur + 0.03);
  }

  function noise(t0, dur, gain, cutoff) {
    if (!noiseBuf) {
      noiseBuf = ctx.createBuffer(1, Math.floor(ctx.sampleRate * 0.3), ctx.sampleRate);
      var d = noiseBuf.getChannelData(0);
      for (var i = 0; i < d.length; i++) d[i] = Math.random() * 2 - 1;
    }
    var s = ctx.createBufferSource(), f = ctx.createBiquadFilter(), g = ctx.createGain();
    s.buffer = noiseBuf; f.type = 'lowpass'; f.frequency.value = cutoff;
    g.gain.setValueAtTime(gain, t0);
    g.gain.exponentialRampToValueAtTime(0.0001, t0 + dur);
    s.connect(f); f.connect(g); g.connect(ctx.destination);
    s.start(t0); s.stop(t0 + dur + 0.02);
  }

  var SOUNDS = {
    move: function (t) { noise(t, 0.05, 0.5, 2200); tone(230, t, 0.07, 'triangle', 0.3, 110); },
    capture: function (t) { noise(t, 0.1, 0.8, 1400); tone(150, t, 0.13, 'triangle', 0.5, 70); tone(95, t + 0.01, 0.12, 'sine', 0.35); },
    check: function (t) { SOUNDS.move(t); tone(660, t + 0.07, 0.12, 'square', 0.12); tone(880, t + 0.15, 0.14, 'square', 0.1); },
    mate: function (t) { SOUNDS.capture(t); tone(392, t + 0.12, 0.28, 'triangle', 0.3); tone(311, t + 0.3, 0.28, 'triangle', 0.3); tone(233, t + 0.5, 0.5, 'triangle', 0.32); },
    end: function (t) { tone(330, t, 0.25, 'triangle', 0.28); tone(262, t + 0.22, 0.4, 'triangle', 0.28); },
    brilliant: function (t) {
      var n = [523.25, 659.25, 783.99, 1046.5, 1318.5];
      for (var i = 0; i < n.length; i++) { tone(n[i], t + i * 0.075, 0.28, 'sine', 0.22); tone(n[i] * 2, t + i * 0.075, 0.18, 'sine', 0.05); }
      tone(1568, t + 0.4, 0.5, 'sine', 0.12);
    }
  };

  function play(kind) {
    if (!enabled) return;
    var c = ensure(); if (!c) return;
    try { (SOUNDS[kind] || SOUNDS.move)(c.currentTime + 0.01); } catch (e) { }
  }

  // أول لمسة من المستخدم تفعّل الصوت (سياسة المتصفح)
  if (root.document) {
    root.document.addEventListener('pointerdown', function () { ensure(); }, true);
  }

  var api = { play: play, setEnabled: function (v) { enabled = !!v; }, isEnabled: function () { return enabled; } };
  root.Sound = api;
})(typeof window !== 'undefined' ? window : globalThis);
