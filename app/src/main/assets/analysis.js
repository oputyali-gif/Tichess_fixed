/* analysis.js - تحليل المباراة وتصنيف النقلات
 * التصنيفات: brilliant ‼️ | great ❗ | good 👍 | book 📒 | inaccuracy ⁉️ |
 *            mistake ❓ | blunder ❓❓ | miss ❌ | forced ➡️
 */
(function (root) {
  'use strict';

  var Chess = (typeof require !== 'undefined' && typeof module !== 'undefined') ? require('./rules.js') : root.Chess;
  var EngineLib = (typeof require !== 'undefined' && typeof module !== 'undefined') ? require('./engine.js') : root.EngineLib;
  var Position = Chess.Position;

  var CATS = {
    brilliant:  { emoji: '‼️', ar: 'نقلة مدهشة', color: '#1baca6' },
    great:      { emoji: '❗', ar: 'نقلة رائعة', color: '#5c8bb0' },
    excellent:  { emoji: '⭐', ar: 'نقلة ممتازة', color: '#d9a21b' },
    good:       { emoji: '🔥', ar: 'نقلة جيدة', color: '#e8742a' },
    correct:    { emoji: '✅', ar: 'نقلة صحيحة', color: '#6fae5a' },
    book:       { emoji: '📒', ar: 'افتتاح', color: '#a88865' },
    forced:     { emoji: '➡️', ar: 'نقلة إجبارية', color: '#8a8a8a' },
    dubious:    { emoji: '❔', ar: 'قد تكون غير دقيقة', color: '#b5a14a' },
    inaccuracy: { emoji: '⁉️', ar: 'غير دقيقة', color: '#f7c631' },
    mistake:    { emoji: '❓', ar: 'خطأ', color: '#e58f2a' },
    miss:       { emoji: '❌', ar: 'إضاعة فرصة', color: '#dc5a8a' },
    blunder:    { emoji: '✖️', ar: 'خطأ فادح', color: '#ca3431' },
    mate:       { emoji: '#️⃣', ar: 'كش مات', color: '#3a3a3a' }
  };
  var ORDER = ['brilliant', 'great', 'excellent', 'good', 'correct', 'book', 'forced', 'dubious', 'inaccuracy', 'mistake', 'miss', 'blunder', 'mate'];

  // نسبة الفوز (0..100) من التقييم بالسنتيبون
  function wc(cp) {
    cp = Math.max(-2000, Math.min(2000, cp));
    return 50 + 50 * (2 / (1 + Math.exp(-0.00368208 * cp)) - 1);
  }

  function moveAccuracy(delta) {
    var a = 103.1668 * Math.exp(-0.04354 * delta) - 3.1669;
    return Math.max(0, Math.min(100, a));
  }

  // مجموعة مفاتيح الوضعيات الموجودة في كتاب الافتتاحيات (سطر = نقلات UCI)
  function buildBook(text) {
    var set = {};
    String(text || '').split(/\r?\n/).forEach(function (line) {
      line = line.trim();
      if (!line || line[0] === '#') return;
      var p = new Position();
      var toks = line.split(/\s+/);
      for (var i = 0; i < toks.length; i++) {
        var m = p.fromUci(toks[i]);
        if (!m) break;
        p.make(m);
        set[p.key()] = true;
      }
    });
    return set;
  }

  function cpToText(cp, mate) {
    if (mate === 0 || (mate === null || mate === undefined) && Math.abs(cp) >= 9000) return cp > 0 ? '+#' : '-#';
    if (mate !== null && mate !== undefined) return (mate > 0 ? '+#' : '-#') + Math.abs(mate);
    var v = cp / 100;
    return (v > 0 ? '+' : '') + v.toFixed(2);
  }

  // opts: {engine, startFen, moves[], book (set), movetime, onProgress(done,total), isCancelled()}
  function analyzeGame(opts) {
    var engine = opts.engine, T = opts.movetime || 400, book = opts.book || {};
    var cancelled = opts.isCancelled || function () { return false; };
    var progress = opts.onProgress || function () {};

    // 1) إعادة تشغيل المباراة
    var pos = new Position(opts.startFen || Chess.START_FEN);
    var n = opts.moves.length;
    var fens = [pos.fen()], keysAfter = [], sans = [], legalCnt = [], legalUci = [], moveObjs = [], sac = [], bestSans = [];
    var i;
    for (i = 0; i < n; i++) {
      var legal = pos.moves();
      var m = pos.fromUci(opts.moves[i], legal);
      if (!m) throw new Error('نقلة غير قانونية: ' + opts.moves[i]);
      legalCnt.push(legal.length);
      legalUci.push(legal.map(function (x) { return pos.uci(x); }));
      sans.push(pos.san(m, legal));
      sac.push(pos.sacNet(m));
      moveObjs.push(m);
      pos.make(m);
      fens.push(pos.fen());
      keysAfter.push(pos.key());
    }
    var finalStatus = pos.status();

    // 2) تقييم كل وضعية
    var E = new Array(n + 1);
    var total = n + 1;
    var work = 0;

    function evalPos(idx) {
      var p = new Position(fens[idx]);
      var legal = p.moves();
      if (legal.length === 0) {
        E[idx] = p.inCheck() ? { cp: -10000, mate: 0, best: null } : { cp: 0, mate: null, best: null };
        return Promise.resolve();
      }
      if (idx === n && finalStatus.over) { E[idx] = { cp: 0, mate: null, best: null }; return Promise.resolve(); }
      return engine.search({ fen: fens[idx], movetime: T }).then(function (r) {
        if (!r.score) { E[idx] = { cp: null, mate: null, best: r.best }; return; }
        E[idx] = {
          cp: EngineLib.scoreToCp(r.score),
          mate: r.score.type === 'mate' ? r.score.value : null,
          best: r.best, pv: r.pv, depth: r.depth
        };
      });
    }

    function loopEval(idx) {
      if (idx > n) return Promise.resolve();
      if (cancelled()) return Promise.reject(new Error('cancelled'));
      return evalPos(idx).then(function () { work++; progress(work, total + n); return loopEval(idx + 1); });
    }

    var plies = [];

    function classify() {
      // وضعيات بلا تقييم (نقلة وحيدة): نشتقها من الوضعية التالية
      for (var k = n; k >= 0; k--) {
        if (E[k].cp === null) E[k] = { cp: k < n ? -E[k + 1].cp : 0, mate: null, best: E[k].best, derived: true };
      }
      var prevCat = null;

      function step(idx) {
        if (idx >= n) return Promise.resolve();
        if (cancelled()) return Promise.reject(new Error('cancelled'));
        var color = new Position(fens[idx]).turn;
        var played = opts.moves[idx];
        var best = E[idx].cp;
        var bestUci = E[idx].best || played;
        var after = (played === bestUci) ? best : -E[idx + 1].cp;
        if (after > best) after = best;           // ضجيج البحث
        var delta = Math.max(0, wc(best) - wc(after));
        var wBest = wc(best), wAfter = wc(after);
        var cat = null;
        var isBestMate = best >= 9000, afterMate = after >= 9000;
        var prevErr = prevCat === 'mistake' || prevCat === 'blunder';
        var needGreat = false;

        var givesMate = sans[idx].charAt(sans[idx].length - 1) === '#';

        if (givesMate) cat = 'mate';
        else if (legalCnt[idx] === 1) cat = 'forced';
        else if (idx < 40 && book[keysAfter[idx]]) cat = 'book';
        else if (delta >= 15) cat = 'blunder';
        else if (delta >= 5) {
          if ((prevErr && wBest >= 65) || (isBestMate && !afterMate)) cat = 'miss';
          else cat = delta >= 10 ? 'mistake' : 'inaccuracy';
        } else if (isBestMate && !afterMate && delta >= 3) cat = 'miss';
        else if (delta <= 2) {
          var topMove = played === bestUci;
          if (sac[idx] <= -200 && wAfter >= 45 && (wBest <= 92 || sac[idx] <= -450)) cat = 'brilliant';
          else if (delta <= 1 && wAfter >= 12 && wAfter <= 93) needGreat = true;
          else cat = topMove ? 'excellent' : 'good';
        } else if (delta <= 3.5) cat = 'correct';
        else cat = 'dubious';

        function finish() {
          var bestSan = null;
          if (E[idx].best) {
            var bp = new Position(fens[idx]);
            var bm = bp.fromUci(E[idx].best);
            if (bm) bestSan = bp.san(bm);
          }
          var nextTurn = new Position(fens[idx + 1]).turn;
          var cpW = nextTurn === 'w' ? E[idx + 1].cp : -E[idx + 1].cp;
          var mateW = null;
          if (E[idx + 1].mate !== null && E[idx + 1].mate !== undefined) {
            mateW = nextTurn === 'w' ? E[idx + 1].mate : -E[idx + 1].mate;
            if (E[idx + 1].mate === 0) mateW = 0;
          }
          plies.push({
            ply: idx + 1, color: color, uci: played, san: sans[idx],
            fenBefore: fens[idx], fenAfter: fens[idx + 1],
            from: moveObjs[idx].from, to: moveObjs[idx].to,
            cat: cat, bestUci: E[idx].best, bestSan: bestSan,
            cpW: cpW, mateW: mateW, loss: delta, acc: moveAccuracy(delta)
          });
          prevCat = cat;
          work++; progress(work, total + n);
        }

        if (!needGreat) { finish(); return step(idx + 1); }
        var others = legalUci[idx].filter(function (u) { return u !== played; });
        var isTop = played === bestUci;
        if (!others.length) { cat = isTop ? 'excellent' : 'good'; finish(); return step(idx + 1); }
        return engine.search({ fen: fens[idx], movetime: Math.max(100, Math.round(T * 0.7)), searchmoves: others }).then(function (r) {
          var s2 = r.score ? EngineLib.scoreToCp(r.score) : null;
          if (s2 !== null && wAfter - wc(s2) >= 12) cat = 'great'; else cat = isTop ? 'excellent' : 'good';
          finish();
          return step(idx + 1);
        });
      }
      return step(0);
    }

    return loopEval(0).then(classify).then(function () {
      var counts = { w: {}, b: {} }, accSum = { w: 0, b: 0 }, accN = { w: 0, b: 0 };
      ORDER.forEach(function (c) { counts.w[c] = 0; counts.b[c] = 0; });
      plies.forEach(function (p) {
        if (p.cat) counts[p.color][p.cat]++;
        accSum[p.color] += p.acc; accN[p.color]++;
      });
      var startTurn = new Position(fens[0]).turn;
      return {
        startFen: fens[0], startCpW: startTurn === 'w' ? E[0].cp : -E[0].cp, plies: plies, counts: counts,
        accuracy: { w: accN.w ? accSum.w / accN.w : null, b: accN.b ? accSum.b / accN.b : null },
        result: finalStatus
      };
    });
  }

  var api = { analyzeGame: analyzeGame, buildBook: buildBook, CATS: CATS, ORDER: ORDER, wc: wc, cpToText: cpToText };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.Analysis = api;
})(typeof window !== 'undefined' ? window : globalThis);
