/* engine.js - عميل UCI بسيط يعمل مع Sara و Stockfish (أي محرك UCI) */
(function (root) {
  'use strict';

  function UciEngine(send) {
    this.send = send;            // function(line)
    this.options = {};           // الخيارات التي يعلنها المحرك
    this.name = '';
    this.waiters = [];
    this.cur = null;             // البحث الجاري
    this.queue = Promise.resolve();
  }

  UciEngine.prototype._wait = function (test, ms) {
    var self = this;
    return new Promise(function (resolve, reject) {
      var w = { test: test, resolve: null };
      var t = setTimeout(function () {
        var i = self.waiters.indexOf(w); if (i >= 0) self.waiters.splice(i, 1);
        reject(new Error('المحرك لم يرد في الوقت المناسب'));
      }, ms || 30000);
      w.resolve = function (line) { clearTimeout(t); resolve(line); };
      self.waiters.push(w);
    });
  };

  UciEngine.prototype.onLine = function (line) {
    line = String(line).trim();
    if (!line) return;
    for (var i = 0; i < this.waiters.length; i++) {
      var w = this.waiters[i];
      if (w.test(line)) { this.waiters.splice(i, 1); w.resolve(line); break; }
    }
    if (line.indexOf('id name ') === 0) { this.name = line.slice(8); return; }
    if (line.indexOf('option name ') === 0) {
      var m = /^option name (.+?) type /.exec(line);
      if (m) this.options[m[1]] = true;
      return;
    }
    if (!this.cur) return;
    if (line.indexOf('info ') === 0) { this._info(line); return; }
    if (line.indexOf('bestmove') === 0) {
      var c = this.cur; this.cur = null;
      if (c.timer) clearTimeout(c.timer);
      var bm = line.split(/\s+/)[1];
      if (bm === '0000' || bm === '(none)' || !bm) bm = null;
      c.resolve({ best: bm, score: c.score || c.fallback || null, depth: c.depth, pv: c.pv });
    }
  };

  UciEngine.prototype._info = function (line) {
    var c = this.cur;
    var mp = /\bmultipv (\d+)/.exec(line);
    if (mp && mp[1] !== '1') return;
    var sm = /\bscore (cp|mate) (-?\d+)(?: (lowerbound|upperbound))?/.exec(line);
    if (!sm) return;
    var d = /\bdepth (\d+)/.exec(line);
    var pv = / pv (.+)$/.exec(line);
    var sc = { type: sm[1], value: parseInt(sm[2], 10) };
    var pvl = pv ? pv[1].trim().split(/\s+/) : [];
    if (sm[3]) { if (!c.score) c.fallback = sc; return; }
    c.score = sc;
    c.depth = d ? parseInt(d[1], 10) : c.depth;
    if (pvl.length) c.pv = pvl;
  };

  // تهيئة المحرك. opts: {threads, hashMB, bookFile, book}
  UciEngine.prototype.init = function (opts) {
    var self = this;
    opts = opts || {};
    this.options = {};
    var p = this._wait(function (l) { return l === 'uciok'; }, 60000);
    this.send('uci');
    return p.then(function () {
      self.setOption(['Threads'], opts.threads || 1);
      self.setOption(['Hash'], opts.hashMB || 32);
      self.setOption(['Move Overhead', 'MoveOverhead'], 20);
      if (opts.bookFile) self.setOption(['BookFile'], opts.bookFile);
      return self.ready();
    });
  };

  UciEngine.prototype.setOption = function (names, value) {
    for (var i = 0; i < names.length; i++) {
      if (this.options[names[i]]) {
        this.send('setoption name ' + names[i] + ' value ' + value);
        return true;
      }
    }
    return false;
  };

  UciEngine.prototype.hasOption = function (name) { return !!this.options[name]; };

  UciEngine.prototype.ready = function () {
    var p = this._wait(function (l) { return l === 'readyok'; }, 60000);
    this.send('isready');
    return p;
  };

  // بحث واحد. o: {fen, movetime, depth, searchmoves}
  UciEngine.prototype.search = function (o) {
    var self = this;
    var p = this.queue.then(function () {
      return new Promise(function (resolve) {
        var ms = o.movetime || 1000;
        var cur = { resolve: resolve, score: null, fallback: null, depth: 0, pv: [], timer: null };
        cur.timer = setTimeout(function () { self.send('stop'); }, ms + 6000);
        self.cur = cur;
        self.send('position fen ' + o.fen);
        var cmd = 'go';
        if (o.depth) cmd += ' depth ' + o.depth; else cmd += ' movetime ' + ms;
        // searchmoves يجب أن تكون آخر شيء في السطر (شرط Stockfish)
        if (o.searchmoves && o.searchmoves.length) cmd += ' searchmoves ' + o.searchmoves.join(' ');
        self.send(cmd);
      });
    });
    this.queue = p.then(function () {}, function () {});
    return p;
  };

  UciEngine.prototype.stop = function () { if (this.cur) this.send('stop'); };

  UciEngine.prototype.newGame = function () { this.send('ucinewgame'); };

  // تحويل نتيجة المحرك إلى رقم (وجهة نظر من عليه الدور)
  function scoreToCp(sc) {
    if (!sc) return null;
    if (sc.type === 'cp') return sc.value;
    var n = Math.abs(sc.value);
    var v = 10000 - Math.min(n, 900);
    return sc.value > 0 ? v : -v;
  }

  var api = { UciEngine: UciEngine, scoreToCp: scoreToCp };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.EngineLib = api;
})(typeof window !== 'undefined' ? window : globalThis);
