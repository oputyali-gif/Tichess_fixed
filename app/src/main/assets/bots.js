/* bots.js - بوتات بمستويات مختلفة (البيدق أضعفها ... الملك أقواها)
 * التقييم المعروض تقريبي (اسمي) وليس مقاساً رسمياً. عدّل الأرقام هنا بحرية.
 *
 * لكل محرك جدول مستويات خاص به:
 *   rating : التقييم المعروض على البطاقة
 *   rnd    : احتمال أن يلعب نقلة عشوائية بدل نقلة المحرك (يضعف المستوى)
 *   depth  : أقصى عمق بحث (للمستويات الضعيفة)
 *   ms     : وقت البحث بالملي ثانية (للمستويات القوية)
 *   elo    : تحديد قوة Stockfish (UCI_Elo، الحد الأدنى 1320)
 *   book   : استعمال كتاب الافتتاحيات (Sara)
 */
(function (root) {
  'use strict';

  var BOTS = [
    { id: 'pawn',   piece: 'P', name: 'البيدق' },
    { id: 'knight', piece: 'N', name: 'الحصان' },
    { id: 'bishop', piece: 'B', name: 'الفيل' },
    { id: 'rook',   piece: 'R', name: 'الطابية' },
    { id: 'queen',  piece: 'Q', name: 'الوزيرة' },
    { id: 'king',   piece: 'K', name: 'الملك' }
  ];

  var LEVELS = {
    sara: {
      pawn:   { rating: 250,  depth: 1, rnd: 0.80 },
      knight: { rating: 450,  depth: 1, rnd: 0.55 },
      bishop: { rating: 500,  depth: 1, rnd: 0.45 },
      rook:   { rating: 1000, depth: 2, rnd: 0.20 },
      queen:  { rating: 1500, depth: 3, rnd: 0.06 },
      king:   { rating: 2100, ms: 60,   rnd: 0,    book: true }
    },
    stockfish: {
      pawn:   { rating: 300,  depth: 1, rnd: 0.78, skill: 0 },
      knight: { rating: 500,  depth: 1, rnd: 0.53, skill: 0 },
      bishop: { rating: 550,  depth: 1, rnd: 0.43, skill: 0 },
      rook:   { rating: 1050, depth: 2, rnd: 0.19, skill: 3 },
      queen:  { rating: 1550, ms: 150, elo: 1550, rnd: 0 },
      king:   { rating: 2150, ms: 300, elo: 2150, rnd: 0 }
    }
  };

  function kindOf(engineId) { return engineId === 'stockfish' ? 'stockfish' : 'sara'; }

  // مستوى بوت لمحرك معيّن. 'max' = بلا حدود (الوقت من الواجهة)
  function level(botId, engineId, maxMs) {
    if (botId === 'max') return { rating: null, ms: maxMs || 1000, rnd: 0, book: true, max: true };
    var t = LEVELS[kindOf(engineId)];
    return t[botId] || t.king;
  }

  function botInfo(botId) {
    for (var i = 0; i < BOTS.length; i++) if (BOTS[i].id === botId) return BOTS[i];
    return null;
  }

  // تهيئة خيارات المحرك للمستوى (تُرجع Promise)
  function applyLevel(engine, lv) {
    if (engine.hasOption('UCI_LimitStrength')) {
      if (lv.elo) {
        engine.setOption(['UCI_LimitStrength'], 'true');
        engine.setOption(['UCI_Elo'], lv.elo);
      } else {
        engine.setOption(['UCI_LimitStrength'], 'false');
      }
      engine.setOption(['Skill Level'], lv.skill === undefined ? 20 : lv.skill);
    }
    engine.setOption(['Book'], lv.book ? 'true' : 'false');
    return engine.ready();
  }

  function delay(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

  function randomMove(legal) { return legal[Math.floor(Math.random() * legal.length)]; }

  // اختيار نقلة البوت. pos: Position. fast=true يلغي التأخير الشكلي (للاختبارات)
  function pickMove(engine, pos, lv, fast) {
    var legal = pos.moves();
    if (!legal.length) return Promise.resolve(null);
    if (legal.length === 1) return Promise.resolve(legal[0]);
    if (lv.rnd && Math.random() < lv.rnd) {
      return (fast ? Promise.resolve() : delay(350 + Math.random() * 500)).then(function () { return randomMove(legal); });
    }
    var o = { fen: pos.fen() };
    if (lv.depth) { o.depth = lv.depth; o.movetime = 1000; } else o.movetime = lv.ms || 300;
    var t0 = Date.now();
    return engine.search(o).then(function (r) {
      var m = r.best ? pos.fromUci(r.best, legal) : null;
      var wait = (!fast && lv.depth) ? Math.max(0, 450 - (Date.now() - t0)) : 0;   // لا نجعل البوت الضعيف يرد فوراً
      return delay(wait).then(function () { return m || randomMove(legal); });
    });
  }

  var api = { BOTS: BOTS, LEVELS: LEVELS, level: level, botInfo: botInfo, applyLevel: applyLevel, pickMove: pickMove, kindOf: kindOf };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.Bots = api;
})(typeof window !== 'undefined' ? window : globalThis);
