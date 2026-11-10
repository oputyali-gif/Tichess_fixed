/* rules.js - قواعد الشطرنج الكاملة (بدون أي مكتبة خارجية)
 * اللوحة: الفهرس 0 = a8 ... 63 = h1
 * القطع: حروف كبيرة للأبيض (PNBRQK) وصغيرة للأسود (pnbrqk)
 */
(function (root) {
  'use strict';

  var FILES = 'abcdefgh';
  var START_FEN = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1';
  var VAL = { p: 100, n: 320, b: 330, r: 500, q: 900, k: 20000 };
  var N_D = [[-2, -1], [-2, 1], [-1, -2], [-1, 2], [1, -2], [1, 2], [2, -1], [2, 1]];
  var K_D = [[-1, -1], [-1, 0], [-1, 1], [0, -1], [0, 1], [1, -1], [1, 0], [1, 1]];
  var B_D = [[-1, -1], [-1, 1], [1, -1], [1, 1]];
  var R_D = [[-1, 0], [1, 0], [0, -1], [0, 1]];

  function sqName(i) { return FILES[i & 7] + (8 - (i >> 3)); }
  function sqIdx(s) { return (8 - parseInt(s[1], 10)) * 8 + FILES.indexOf(s[0]); }
  function isW(p) { return p >= 'A' && p <= 'Z'; }
  function colorOf(p) { return isW(p) ? 'w' : 'b'; }

  function Position(fen) {
    this.board = new Array(64).fill('');
    this.turn = 'w';
    this.castle = { K: false, Q: false, k: false, q: false };
    this.ep = -1;
    this.half = 0;
    this.full = 1;
    this.stack = [];
    this.keys = [];
    this.load(fen || START_FEN);
  }

  Position.prototype.load = function (fen) {
    var parts = String(fen).trim().split(/\s+/);
    if (parts.length < 2) throw new Error('FEN غير صالح');
    var rows = parts[0].split('/');
    if (rows.length !== 8) throw new Error('FEN غير صالح');
    this.board = new Array(64).fill('');
    for (var r = 0; r < 8; r++) {
      var f = 0;
      for (var k = 0; k < rows[r].length; k++) {
        var ch = rows[r][k];
        if (ch >= '1' && ch <= '8') f += parseInt(ch, 10);
        else { this.board[r * 8 + f] = ch; f++; }
      }
      if (f !== 8) throw new Error('FEN غير صالح');
    }
    this.turn = parts[1] === 'b' ? 'b' : 'w';
    var c = parts[2] || '-';
    this.castle = { K: c.indexOf('K') >= 0, Q: c.indexOf('Q') >= 0, k: c.indexOf('k') >= 0, q: c.indexOf('q') >= 0 };
    this.ep = (parts[3] && parts[3] !== '-') ? sqIdx(parts[3]) : -1;
    this.half = parseInt(parts[4] || '0', 10) || 0;
    this.full = parseInt(parts[5] || '1', 10) || 1;
    this.stack = [];
    this.keys = [this.key()];
  };

  Position.prototype.clone = function () { return new Position(this.fen()); };

  Position.prototype.fen = function () {
    var rows = [];
    for (var r = 0; r < 8; r++) {
      var s = '', e = 0;
      for (var f = 0; f < 8; f++) {
        var p = this.board[r * 8 + f];
        if (!p) e++; else { if (e) { s += e; e = 0; } s += p; }
      }
      if (e) s += e;
      rows.push(s);
    }
    var c = (this.castle.K ? 'K' : '') + (this.castle.Q ? 'Q' : '') + (this.castle.k ? 'k' : '') + (this.castle.q ? 'q' : '');
    return rows.join('/') + ' ' + this.turn + ' ' + (c || '-') + ' ' + (this.ep >= 0 ? sqName(this.ep) : '-') + ' ' + this.half + ' ' + this.full;
  };

  // مفتاح الوضعية (بدون عدادات). نذكر خانة الأخذ بالمرور فقط إذا كان ممكناً فعلاً.
  Position.prototype.key = function () {
    var f = this.fen().split(' ');
    var ep = '-';
    if (this.ep >= 0) {
      var me = this.turn;
      var pawn = me === 'w' ? 'P' : 'p';
      var row = this.ep >> 3, col = this.ep & 7;
      var pr = me === 'w' ? row + 1 : row - 1;
      var can = false;
      if (pr >= 0 && pr < 8) {
        if (col > 0 && this.board[pr * 8 + col - 1] === pawn) can = true;
        if (col < 7 && this.board[pr * 8 + col + 1] === pawn) can = true;
      }
      if (can) ep = f[3];
    }
    return f[0] + ' ' + f[1] + ' ' + f[2] + ' ' + ep;
  };

  Position.prototype.kingSq = function (color) {
    var k = color === 'w' ? 'K' : 'k';
    for (var i = 0; i < 64; i++) if (this.board[i] === k) return i;
    return -1;
  };

  // هل الخانة sq مهاجَمة من قِبل قطع اللون by؟
  Position.prototype.attacked = function (sq, by) {
    var b = this.board, r = sq >> 3, f = sq & 7, i, d, rr, ff, p;
    var white = by === 'w';
    // بيادق
    var pr = white ? r + 1 : r - 1;
    if (pr >= 0 && pr < 8) {
      var pp = white ? 'P' : 'p';
      if (f > 0 && b[pr * 8 + f - 1] === pp) return true;
      if (f < 7 && b[pr * 8 + f + 1] === pp) return true;
    }
    var N = white ? 'N' : 'n', K = white ? 'K' : 'k';
    for (i = 0; i < 8; i++) {
      rr = r + N_D[i][0]; ff = f + N_D[i][1];
      if (rr >= 0 && rr < 8 && ff >= 0 && ff < 8 && b[rr * 8 + ff] === N) return true;
      rr = r + K_D[i][0]; ff = f + K_D[i][1];
      if (rr >= 0 && rr < 8 && ff >= 0 && ff < 8 && b[rr * 8 + ff] === K) return true;
    }
    var B = white ? 'B' : 'b', R = white ? 'R' : 'r', Q = white ? 'Q' : 'q';
    for (d = 0; d < 4; d++) {
      rr = r + B_D[d][0]; ff = f + B_D[d][1];
      while (rr >= 0 && rr < 8 && ff >= 0 && ff < 8) {
        p = b[rr * 8 + ff];
        if (p) { if (p === B || p === Q) return true; break; }
        rr += B_D[d][0]; ff += B_D[d][1];
      }
      rr = r + R_D[d][0]; ff = f + R_D[d][1];
      while (rr >= 0 && rr < 8 && ff >= 0 && ff < 8) {
        p = b[rr * 8 + ff];
        if (p) { if (p === R || p === Q) return true; break; }
        rr += R_D[d][0]; ff += R_D[d][1];
      }
    }
    return false;
  };

  Position.prototype.inCheck = function (color) {
    color = color || this.turn;
    var k = this.kingSq(color);
    return k >= 0 && this.attacked(k, color === 'w' ? 'b' : 'w');
  };

  function mk(from, to, piece, captured, extra) {
    var m = { from: from, to: to, piece: piece, captured: captured || '', promo: '', ep: false, castle: '', dbl: false };
    if (extra) for (var k in extra) m[k] = extra[k];
    return m;
  }

  Position.prototype.pseudo = function () {
    var b = this.board, me = this.turn, out = [], white = me === 'w';
    var opp = white ? 'b' : 'w';
    for (var s = 0; s < 64; s++) {
      var p = b[s];
      if (!p || colorOf(p) !== me) continue;
      var t = p.toLowerCase(), r = s >> 3, f = s & 7, rr, ff, to, q, i, d;
      if (t === 'p') {
        var dir = white ? -1 : 1, startRow = white ? 6 : 1, lastRow = white ? 1 : 6;
        rr = r + dir;
        if (rr >= 0 && rr < 8) {
          to = rr * 8 + f;
          if (!b[to]) {
            if (r === lastRow) {
              ['q', 'r', 'b', 'n'].forEach(function (pr) { out.push(mk(s, to, p, '', { promo: pr })); });
            } else {
              out.push(mk(s, to, p, ''));
              if (r === startRow && !b[(r + 2 * dir) * 8 + f]) out.push(mk(s, (r + 2 * dir) * 8 + f, p, '', { dbl: true }));
            }
          }
          for (var df = -1; df <= 1; df += 2) {
            ff = f + df;
            if (ff < 0 || ff > 7) continue;
            to = rr * 8 + ff;
            q = b[to];
            if (q && colorOf(q) === opp) {
              if (r === lastRow) {
                (function (to2, q2) {
                  ['q', 'r', 'b', 'n'].forEach(function (pr) { out.push(mk(s, to2, p, q2, { promo: pr })); });
                })(to, q);
              } else out.push(mk(s, to, p, q));
            } else if (!q && to === this.ep) {
              out.push(mk(s, to, p, white ? 'p' : 'P', { ep: true }));
            }
          }
        }
      } else if (t === 'n' || t === 'k') {
        var D = t === 'n' ? N_D : K_D;
        for (i = 0; i < 8; i++) {
          rr = r + D[i][0]; ff = f + D[i][1];
          if (rr < 0 || rr > 7 || ff < 0 || ff > 7) continue;
          to = rr * 8 + ff; q = b[to];
          if (!q || colorOf(q) === opp) out.push(mk(s, to, p, q));
        }
        if (t === 'k') this.genCastle(out, s, p);
      } else {
        var dirs = t === 'b' ? B_D : t === 'r' ? R_D : B_D.concat(R_D);
        for (d = 0; d < dirs.length; d++) {
          rr = r + dirs[d][0]; ff = f + dirs[d][1];
          while (rr >= 0 && rr < 8 && ff >= 0 && ff < 8) {
            to = rr * 8 + ff; q = b[to];
            if (!q) out.push(mk(s, to, p, ''));
            else { if (colorOf(q) === opp) out.push(mk(s, to, p, q)); break; }
            rr += dirs[d][0]; ff += dirs[d][1];
          }
        }
      }
    }
    return out;
  };

  Position.prototype.genCastle = function (out, s, p) {
    var b = this.board, white = this.turn === 'w', opp = white ? 'b' : 'w';
    var home = white ? 60 : 4;
    if (s !== home) return;
    if (this.attacked(home, opp)) return;
    var K = white ? this.castle.K : this.castle.k, Q = white ? this.castle.Q : this.castle.q;
    var rook = white ? 'R' : 'r';
    if (K && b[home + 3] === rook && !b[home + 1] && !b[home + 2] &&
        !this.attacked(home + 1, opp) && !this.attacked(home + 2, opp)) {
      out.push(mk(home, home + 2, p, '', { castle: 'K' }));
    }
    if (Q && b[home - 4] === rook && !b[home - 1] && !b[home - 2] && !b[home - 3] &&
        !this.attacked(home - 1, opp) && !this.attacked(home - 2, opp)) {
      out.push(mk(home, home - 2, p, '', { castle: 'Q' }));
    }
  };

  Position.prototype.make = function (m) {
    var b = this.board, white = this.turn === 'w';
    var rec = { m: m, castle: { K: this.castle.K, Q: this.castle.Q, k: this.castle.k, q: this.castle.q }, ep: this.ep, half: this.half, full: this.full };
    this.stack.push(rec);
    b[m.from] = '';
    var placed = m.piece;
    if (m.promo) placed = white ? m.promo.toUpperCase() : m.promo;
    b[m.to] = placed;
    if (m.ep) b[white ? m.to + 8 : m.to - 8] = '';
    if (m.castle === 'K') { b[m.to + 1] = ''; b[m.to - 1] = white ? 'R' : 'r'; }
    if (m.castle === 'Q') { b[m.to - 2] = ''; b[m.to + 1] = white ? 'R' : 'r'; }
    // حقوق التبييت
    var c = this.castle;
    if (m.piece === 'K') { c.K = false; c.Q = false; }
    if (m.piece === 'k') { c.k = false; c.q = false; }
    if (m.from === 63 || m.to === 63) c.K = false;
    if (m.from === 56 || m.to === 56) c.Q = false;
    if (m.from === 7 || m.to === 7) c.k = false;
    if (m.from === 0 || m.to === 0) c.q = false;
    this.ep = m.dbl ? (m.from + m.to) >> 1 : -1;
    if (m.piece.toLowerCase() === 'p' || m.captured) this.half = 0; else this.half++;
    if (!white) this.full++;
    this.turn = white ? 'b' : 'w';
    this.keys.push(this.key());
  };

  Position.prototype.unmake = function () {
    var rec = this.stack.pop();
    if (!rec) return null;
    var m = rec.m, b = this.board;
    this.turn = this.turn === 'w' ? 'b' : 'w';
    var white = this.turn === 'w';
    b[m.from] = m.piece;
    if (m.ep) { b[m.to] = ''; b[white ? m.to + 8 : m.to - 8] = white ? 'p' : 'P'; }
    else b[m.to] = m.captured || '';
    if (m.castle === 'K') { b[m.to + 1] = white ? 'R' : 'r'; b[m.to - 1] = ''; }
    if (m.castle === 'Q') { b[m.to - 2] = white ? 'R' : 'r'; b[m.to + 1] = ''; }
    this.castle = rec.castle; this.ep = rec.ep; this.half = rec.half; this.full = rec.full;
    this.keys.pop();
    return m;
  };

  Position.prototype.moves = function () {
    var ps = this.pseudo(), out = [], me = this.turn, opp = me === 'w' ? 'b' : 'w';
    for (var i = 0; i < ps.length; i++) {
      this.make(ps[i]);
      var k = this.kingSq(me);
      var bad = k >= 0 && this.attacked(k, opp);
      this.unmake();
      if (!bad) out.push(ps[i]);
    }
    return out;
  };

  Position.prototype.movesFrom = function (sq) {
    return this.moves().filter(function (m) { return m.from === sq; });
  };

  Position.prototype.uci = function (m) { return sqName(m.from) + sqName(m.to) + (m.promo || ''); };

  Position.prototype.fromUci = function (u, legal) {
    legal = legal || this.moves();
    u = String(u).trim();
    for (var i = 0; i < legal.length; i++) if (this.uci(legal[i]) === u) return legal[i];
    return null;
  };

  Position.prototype.san = function (m, legal) {
    legal = legal || this.moves();
    var s;
    if (m.castle) s = m.castle === 'K' ? 'O-O' : 'O-O-O';
    else {
      var P = m.piece.toUpperCase();
      var cap = m.captured ? 'x' : '';
      if (P === 'P') {
        s = (m.captured ? FILES[m.from & 7] + 'x' : '') + sqName(m.to);
        if (m.promo) s += '=' + m.promo.toUpperCase();
      } else {
        var sameFile = false, sameRank = false, amb = false;
        for (var i = 0; i < legal.length; i++) {
          var o = legal[i];
          if (o.from === m.from || o.to !== m.to || o.piece !== m.piece) continue;
          amb = true;
          if ((o.from & 7) === (m.from & 7)) sameFile = true;
          if ((o.from >> 3) === (m.from >> 3)) sameRank = true;
        }
        var dis = '';
        if (amb) {
          if (!sameFile) dis = FILES[m.from & 7];
          else if (!sameRank) dis = String(8 - (m.from >> 3));
          else dis = sqName(m.from);
        }
        s = P + dis + cap + sqName(m.to);
      }
    }
    this.make(m);
    if (this.inCheck()) s += this.moves().length === 0 ? '#' : '+';
    this.unmake();
    return s;
  };

  // تحويل نص SAN إلى نقلة قانونية (يتسامح مع الرموز !? والصفر في التبييت)
  Position.prototype.fromSan = function (str, legal) {
    legal = legal || this.moves();
    var s = String(str).trim().replace(/[!?]+$/g, '').replace(/[+#]+$/g, '').replace(/0/g, 'O').replace(/\u2013|\u2014/g, '-');
    if (/^O-O-O$/i.test(s) || /^O-O$/i.test(s)) {
      var want = /^O-O-O$/i.test(s) ? 'Q' : 'K';
      for (var i = 0; i < legal.length; i++) if (legal[i].castle === want) return legal[i];
      return null;
    }
    var mt = /^([KQRBN])?([a-h])?([1-8])?x?([a-h][1-8])(?:=?([QRBNqrbn]))?$/.exec(s);
    if (!mt) return null;
    var piece = mt[1] || 'P', ff = mt[2], rk = mt[3], to = sqIdx(mt[4]), pr = mt[5] ? mt[5].toLowerCase() : '';
    var c = legal.filter(function (m) {
      if (m.to !== to) return false;
      if (m.piece.toUpperCase() !== piece) return false;
      if (ff && FILES[m.from & 7] !== ff) return false;
      if (rk && String(8 - (m.from >> 3)) !== rk) return false;
      if ((m.promo || '') !== pr) return false;
      return true;
    });
    if (c.length === 1) return c[0];
    if (c.length > 1 && piece === 'P' && !ff) return null;
    if (c.length > 1) {
      // غموض: نقبل فقط إذا كان النص كاملاً (مع الخانة الأصلية)
      return null;
    }
    // بيدق يترقى بدون ذكر القطعة => الوزير
    if (piece === 'P' && !pr) {
      var c2 = legal.filter(function (m) {
        return m.to === to && m.piece.toUpperCase() === 'P' && m.promo === 'q' && (!ff || FILES[m.from & 7] === ff);
      });
      if (c2.length === 1) return c2[0];
    }
    return null;
  };

  Position.prototype.insufficient = function () {
    var minors = [], i, p;
    for (i = 0; i < 64; i++) {
      p = this.board[i];
      if (!p) continue;
      var t = p.toLowerCase();
      if (t === 'k') continue;
      if (t === 'p' || t === 'r' || t === 'q') return false;
      minors.push({ t: t, sq: i, c: colorOf(p) });
    }
    if (minors.length <= 1) return true;
    if (minors.every(function (x) { return x.t === 'b'; })) {
      var col = (minors[0].sq >> 3) + (minors[0].sq & 7) & 1;
      return minors.every(function (x) { return (((x.sq >> 3) + (x.sq & 7)) & 1) === col; });
    }
    return false;
  };

  Position.prototype.repetitions = function () {
    var k = this.keys[this.keys.length - 1], n = 0;
    for (var i = 0; i < this.keys.length; i++) if (this.keys[i] === k) n++;
    return n;
  };

  // حالة المباراة
  Position.prototype.status = function (legal) {
    legal = legal || this.moves();
    if (legal.length === 0) {
      if (this.inCheck()) return { over: true, result: this.turn === 'w' ? '0-1' : '1-0', reason: 'checkmate' };
      return { over: true, result: '1/2-1/2', reason: 'stalemate' };
    }
    if (this.insufficient()) return { over: true, result: '1/2-1/2', reason: 'insufficient' };
    if (this.half >= 100) return { over: true, result: '1/2-1/2', reason: 'fifty' };
    if (this.repetitions() >= 3) return { over: true, result: '1/2-1/2', reason: 'repetition' };
    return { over: false, result: '*', reason: '' };
  };

  // ---------- تبادل ثابت (SEE) على خانة ----------
  function attackersOf(b, sq, by) {
    var res = [], r = sq >> 3, f = sq & 7, i, d, rr, ff, p;
    var white = by === 'w';
    var pr = white ? r + 1 : r - 1;
    if (pr >= 0 && pr < 8) {
      var pp = white ? 'P' : 'p';
      if (f > 0 && b[pr * 8 + f - 1] === pp) res.push(pr * 8 + f - 1);
      if (f < 7 && b[pr * 8 + f + 1] === pp) res.push(pr * 8 + f + 1);
    }
    var N = white ? 'N' : 'n', K = white ? 'K' : 'k';
    for (i = 0; i < 8; i++) {
      rr = r + N_D[i][0]; ff = f + N_D[i][1];
      if (rr >= 0 && rr < 8 && ff >= 0 && ff < 8 && b[rr * 8 + ff] === N) res.push(rr * 8 + ff);
      rr = r + K_D[i][0]; ff = f + K_D[i][1];
      if (rr >= 0 && rr < 8 && ff >= 0 && ff < 8 && b[rr * 8 + ff] === K) res.push(rr * 8 + ff);
    }
    var B = white ? 'B' : 'b', R = white ? 'R' : 'r', Q = white ? 'Q' : 'q';
    for (d = 0; d < 4; d++) {
      rr = r + B_D[d][0]; ff = f + B_D[d][1];
      while (rr >= 0 && rr < 8 && ff >= 0 && ff < 8) {
        p = b[rr * 8 + ff];
        if (p) { if (p === B || p === Q) res.push(rr * 8 + ff); break; }
        rr += B_D[d][0]; ff += B_D[d][1];
      }
      rr = r + R_D[d][0]; ff = f + R_D[d][1];
      while (rr >= 0 && rr < 8 && ff >= 0 && ff < 8) {
        p = b[rr * 8 + ff];
        if (p) { if (p === R || p === Q) res.push(rr * 8 + ff); break; }
        rr += R_D[d][0]; ff += R_D[d][1];
      }
    }
    return res;
  }

  // أكبر مكسب مادي يحققه اللون side إذا بدأ بالأخذ على الخانة sq
  // (القطعة الموجودة على الخانة قيمتها targetValue)
  function exchangeGain(board, sq, side, targetValue) {
    var b = board.slice(), gains = [], victim = targetValue, s = side, guard = 0;
    while (guard++ < 32) {
      var list = attackersOf(b, sq, s);
      if (!list.length) break;
      var best = -1, bv = 1e9;
      for (var i = 0; i < list.length; i++) {
        var v = VAL[b[list[i]].toLowerCase()];
        if (v < bv) { bv = v; best = list[i]; }
      }
      var isKing = b[best].toLowerCase() === 'k';
      var other = s === 'w' ? 'b' : 'w';
      if (isKing && attackersOf(b, sq, other).length) break; // الملك لا يأخذ على خانة محمية
      gains.push(victim);
      victim = bv;
      b[sq] = b[best]; b[best] = '';
      s = other;
    }
    var g = 0;
    for (var j = gains.length - 1; j >= 0; j--) g = Math.max(0, gains[j] - g);
    return g;
  }

  // صافي المادة للاعب بعد نقلته m (موجب = ربح، سالب = تضحية) - يُستدعى قبل تنفيذ m
  Position.prototype.sacNet = function (m) {
    var capVal = m.captured ? VAL[m.captured.toLowerCase()] : 0;
    var white = this.turn === 'w';
    this.make(m);
    var placed = this.board[m.to];
    var oppGain = exchangeGain(this.board, m.to, white ? 'b' : 'w', VAL[placed.toLowerCase()]);
    this.unmake();
    return capVal - oppGain;
  };

  Position.prototype.perft = function (d) {
    if (d === 0) return 1;
    var ms = this.moves(), n = 0;
    if (d === 1) return ms.length;
    for (var i = 0; i < ms.length; i++) { this.make(ms[i]); n += this.perft(d - 1); this.unmake(); }
    return n;
  };

  // ---------- قراءة PGN ----------
  function parsePgn(text) {
    var fen = null;
    var hm = /\[FEN\s+"([^"]+)"\]/i.exec(text);
    if (hm) fen = hm[1];
    var headers = {};
    text.replace(/\[(\w+)\s+"([^"]*)"\]/g, function (_, k, v) { headers[k] = v; return ''; });
    var body = text.replace(/\[[^\]]*\]/g, ' ');
    body = body.replace(/\{[^}]*\}/g, ' ').replace(/;[^\n]*/g, ' ');
    var prev;
    do { prev = body; body = body.replace(/\([^()]*\)/g, ' '); } while (body !== prev);
    body = body.replace(/\$\d+/g, ' ').replace(/\d+\s*\.(\.\.)?/g, ' ');
    var toks = body.split(/\s+/).filter(function (t) { return t && !/^(1-0|0-1|1\/2-1\/2|\*)$/.test(t); });
    var pos = new Position(fen || START_FEN);
    var startFen = pos.fen();
    var moves = [], err = null;
    for (var i = 0; i < toks.length; i++) {
      var legal = pos.moves();
      var m = pos.fromSan(toks[i], legal) || pos.fromUci(toks[i], legal);
      if (!m) { err = { token: toks[i], ply: moves.length + 1 }; break; }
      moves.push(pos.uci(m));
      pos.make(m);
    }
    return { startFen: startFen, moves: moves, headers: headers, error: err };
  }

  var api = {
    Position: Position, START_FEN: START_FEN, VAL: VAL, FILES: FILES,
    sqName: sqName, sqIdx: sqIdx, colorOf: colorOf, parsePgn: parsePgn
  };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.Chess = api;
})(typeof window !== 'undefined' ? window : globalThis);
