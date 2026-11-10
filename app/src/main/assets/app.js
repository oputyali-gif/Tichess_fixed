/* app.js - واجهة Tichess: اللعب ضد المحرك + تحليل المباريات */
(function () {
  'use strict';
  var Position = Chess.Position, UciEngine = EngineLib.UciEngine, A = Analysis;
  function $(id) { return document.getElementById(id); }
  function esc(s) { return String(s).replace(/[&<>]/g, function (c) { return { '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]; }); }

  function lsGet(k, d) { try { var v = localStorage.getItem('sara.' + k); return v === null ? d : v; } catch (e) { return d; } }
  function lsSet(k, v) { try { localStorage.setItem('sara.' + k, v); } catch (e) { } }

  // مجموعات القطع: لإضافة مجموعة جديدة ضع مجلداً فيه wK.svg ... bP.svg داخل assets/pieces ثم أضف سطراً هنا
  var PIECE_SETS = [
    { id: 'set1', name: 'كلاسيك (قطعك)', dir: 'pieces/set1' },
    { id: 'set2', name: 'ملكي (الأصلية)', dir: 'pieces/set2' }
  ];
  var pieceDir = PIECE_SETS[0].dir;
  var Bots = window.Bots;

  var AB = window.Android || null;
  function banner(msg) { var b = $('banner'); if (!msg) { b.hidden = true; return; } b.textContent = msg; b.hidden = false; }

  /* ================== الرقعة ================== */
  function BoardView(el, arrowsEl, onTap) {
    this.el = el; this.ar = arrowsEl; this.onTap = onTap; this.flip = false;
    var self = this;
    el.addEventListener('click', function (e) {
      var t = e.target;
      while (t && t !== el && !(t.dataset && t.dataset.i !== undefined)) t = t.parentNode;
      if (t && t !== el && self.onTap) self.onTap(parseInt(t.dataset.i, 10));
    });
  }
  BoardView.prototype.render = function (s) {
    var html = '', dr, dc, r, f, i, p, k;
    for (dr = 0; dr < 8; dr++) {
      for (dc = 0; dc < 8; dc++) {
        r = this.flip ? 7 - dr : dr; f = this.flip ? 7 - dc : dc; i = r * 8 + f;
        var cls = 'sq ' + (((r + f) & 1) ? 'dk' : 'lt');
        if (s.last && (s.last[0] === i || s.last[1] === i)) cls += ' last';
        if (s.sel === i) cls += ' sel';
        if (s.check === i) cls += ' chk';
        html += '<div class="' + cls + '" data-i="' + i + '">';
        if (dc === 0) html += '<span class="co r">' + (8 - r) + '</span>';
        if (dr === 7) html += '<span class="co f">' + Chess.FILES[f] + '</span>';
        p = s.board[i];
        if (p) html += '<img class="pc" draggable="false" src="' + pieceDir + '/' + (p === p.toUpperCase() ? 'w' : 'b') + p.toUpperCase() + '.svg">';
        if (s.targets) for (k = 0; k < s.targets.length; k++) if (s.targets[k].to === i) html += s.targets[k].cap ? '<i class="ring"></i>' : '<i class="dot"></i>';
        if (s.badge && s.badge.sq === i) {
          var c = A.CATS[s.badge.cat];
          html += '<div class="badge" style="background:' + c.color + '">' + c.emoji + '</div>';
        }
        html += '</div>';
      }
    }
    this.el.innerHTML = html;
    this.ar.innerHTML = s.arrow ? this.arrow(s.arrow.from, s.arrow.to) : '';
  };
  BoardView.prototype.arrow = function (from, to) {
    var fl = this.flip;
    function cx(i) { return (fl ? 7 - (i & 7) : (i & 7)) + 0.5; }
    function cy(i) { return (fl ? 7 - (i >> 3) : (i >> 3)) + 0.5; }
    var x1 = cx(from), y1 = cy(from), x2 = cx(to), y2 = cy(to);
    var dx = x2 - x1, dy = y2 - y1, len = Math.sqrt(dx * dx + dy * dy) || 1, ux = dx / len, uy = dy / len;
    var hl = 0.42, hw = 0.3;
    var bx = x2 - ux * hl, by = y2 - uy * hl;
    var col = 'rgba(255,170,0,0.85)';
    return '<line x1="' + x1 + '" y1="' + y1 + '" x2="' + bx + '" y2="' + by + '" stroke="' + col + '" stroke-width="0.17" stroke-linecap="round"/>' +
      '<polygon points="' + x2 + ',' + y2 + ' ' + (bx - uy * hw) + ',' + (by + ux * hw) + ' ' + (bx + uy * hw) + ',' + (by - ux * hw) + '" fill="' + col + '"/>';
  };

  /* ================== المحرك ================== */
  var engine = null, engineId = null, engineReady = false, engineList = [], modeApplied = '';
  var bookSet = null;

  window.onEngineLine = function (line) { if (engine) engine.onLine(line); };
  window.onEngineExit = function (msg) { engineReady = false; banner('توقف المحرك: ' + msg); setStatus('توقف المحرك'); };

  function engineDisplayName() {
    var o = $('engineSel').options[$('engineSel').selectedIndex];
    return o ? o.textContent : 'المحرك';
  }

  function startEngine(id) {
    engineReady = false; modeApplied = ''; banner('');
    setStatus('جاري تشغيل المحرك…');
    if (!AB) { banner('هذه الصفحة تعمل فقط داخل تطبيق الأندرويد (لا يوجد محرك).'); return Promise.reject(new Error('no bridge')); }
    var r = AB.startEngine(id);
    if (r !== 'ok') { banner('تعذّر تشغيل المحرك: ' + r); setStatus('تعذّر تشغيل المحرك'); return Promise.reject(new Error(r)); }
    engine = new UciEngine(function (l) { AB.engineSend(l); });
    var cores = 1; try { cores = AB.cores(); } catch (e) { }
    return engine.init({ threads: cores >= 4 ? 2 : 1, hashMB: 32, bookFile: AB.bookPath() }).then(function () {
      engineReady = true; engineId = id;
      renderBots();
      updatePlayStatus();
    }).catch(function (e) {
      banner('المحرك لم يبدأ: ' + e.message); setStatus('المحرك لا يستجيب');
      throw e;
    });
  }

  // تهيئة المحرك لوضع التحليل (أقصى قوة وبدون كتاب افتتاحيات)
  function applyAnalysisMode() {
    if (modeApplied === 'analysis') return engine.ready();
    engine.setOption(['Book'], 'false');
    if (engine.hasOption('UCI_LimitStrength')) {
      engine.setOption(['UCI_LimitStrength'], 'false');
      engine.setOption(['Skill Level'], 20);
    }
    modeApplied = 'analysis';
    return engine.ready();
  }

  /* ================== PGN ================== */
  function toPgn(startFen, ucis, result, white, black) {
    var d = new Date();
    var date = d.getFullYear() + '.' + ('0' + (d.getMonth() + 1)).slice(-2) + '.' + ('0' + d.getDate()).slice(-2);
    var h = '[Event "Tichess"]\n[Date "' + date + '"]\n[White "' + white + '"]\n[Black "' + black + '"]\n[Result "' + result + '"]\n';
    if (startFen !== Chess.START_FEN) h += '[SetUp "1"]\n[FEN "' + startFen + '"]\n';
    var p = new Position(startFen), out = [];
    for (var i = 0; i < ucis.length; i++) {
      var m = p.fromUci(ucis[i]); if (!m) break;
      if (p.turn === 'w') out.push(p.full + '.'); else if (i === 0) out.push(p.full + '...');
      out.push(p.san(m)); p.make(m);
    }
    return h + '\n' + out.join(' ') + ' ' + result + '\n';
  }

  /* ================== اللعب ================== */
  var pos = new Position(), hist = [], human = 'w', over = false, thinking = false, gameId = 0, resultText = '';
  var sel = -1, pendingPromo = null;
  var selBot = lsGet('bot', 'pawn'), gameBot = selBot;
  var pBoard = new BoardView($('pBoard'), $('pArrows'), onPlayTap);

  function setStatus(t) { $('pStatus').textContent = t; }

  function curEngineId() { return engineId || $('engineSel').value || 'sara'; }
  function botName(id) { if (id === 'max') return 'بلا حدود'; var b = Bots.botInfo(id); return b ? b.name : id; }
  function botLabel(id) {
    var lv = Bots.level(id, curEngineId());
    return botName(id) + (lv.rating ? ' (' + lv.rating + ')' : '');
  }

  function renderBots() {
    var el = $('pBots'), sx = el.scrollLeft, html = '';
    function card(id, av, name, rating) {
      return '<div class="bot' + (selBot === id ? ' on' : '') + '" data-bot="' + id + '"><div class="av">' + av +
        '</div><div class="bn">' + name + '</div><div class="br">' + rating + '</div></div>';
    }
    Bots.BOTS.forEach(function (b) {
      html += card(b.id, '<img src="' + pieceDir + '/w' + b.piece + '.svg">', b.name, Bots.level(b.id, curEngineId()).rating);
    });
    html += card('max', '🔥', 'بلا حدود', 'MAX');
    el.innerHTML = html; el.scrollLeft = sx;
    $('pTimeBox').hidden = selBot !== 'max';
  }
  $('pBots').addEventListener('click', function (e) {
    var t = e.target;
    while (t && t !== this && !t.getAttribute('data-bot')) t = t.parentNode;
    if (!t || t === this) return;
    selBot = t.getAttribute('data-bot'); lsSet('bot', selBot); renderBots();
    if (!hist.length || over) newGame();
    else setStatus('سيبدأ اللعب ضد ' + botLabel(selBot) + ' عند الضغط على «لعبة جديدة»');
  });

  function updatePlayStatus() {
    if (!engineReady) return;
    if (over) { setStatus(resultText); return; }
    if (thinking) { setStatus(botName(gameBot) + ' يفكر…'); return; }
    var t = 'دورك (' + (human === 'w' ? 'الأبيض' : 'الأسود') + ') · ضد ' + botLabel(gameBot);
    if (pos.inCheck()) t += ' — كش!';
    setStatus(t);
  }

  function renderPlay() {
    var last = hist.length ? [hist[hist.length - 1].from, hist[hist.length - 1].to] : null;
    var targets = [];
    if (sel >= 0 && !thinking && !over) targets = pos.movesFrom(sel).map(function (m) { return { to: m.to, cap: !!m.captured }; });
    pBoard.render({
      board: pos.board, last: last, sel: sel, targets: targets,
      check: pos.inCheck() ? pos.kingSq(pos.turn) : -1
    });
    // قائمة النقلات
    var s = '', p2 = new Position(Chess.START_FEN), i;
    for (i = 0; i < hist.length; i++) {
      if (p2.turn === 'w') s += p2.full + '. ';
      s += hist[i].san + ' '; p2.make(p2.fromUci(hist[i].uci));
    }
    var el = $('pMoves'); el.textContent = s || 'ستظهر النقلات هنا';
    el.scrollTop = el.scrollHeight;
    updatePlayStatus();
  }

  function newGame() {
    gameId++; if (engine && thinking) engine.stop();
    thinking = false; over = false; resultText = ''; sel = -1; hist = []; pos = new Position();
    var s = $('pSide').value; human = s === 'r' ? (Math.random() < 0.5 ? 'w' : 'b') : s;
    pBoard.flip = human === 'b';
    gameBot = selBot;
    lsSet('side', s); lsSet('time', $('pTime').value);
    if (engine && engineReady) engine.newGame();
    renderPlay();
    if (human === 'b') engineMove();
  }

  function finishIfOver() {
    var st = pos.status();
    if (!st.over) return false;
    over = true;
    var winner = st.result === '1-0' ? 'w' : st.result === '0-1' ? 'b' : null;
    if (st.reason === 'checkmate') resultText = winner === human ? 'كش مات — فزت! 🎉' : 'كش مات — خسرت';
    else if (st.reason === 'stalemate') resultText = 'تعادل (طريق مسدود)';
    else if (st.reason === 'insufficient') resultText = 'تعادل (مادة غير كافية)';
    else if (st.reason === 'fifty') resultText = 'تعادل (قاعدة الخمسين نقلة)';
    else resultText = 'تعادل (تكرار الوضعية)';
    hist.result = st.result;
    return true;
  }

  function doMove(m) {
    var san = pos.san(m);
    pos.make(m);
    hist.push({ uci: Chess.sqName(m.from) + Chess.sqName(m.to) + (m.promo || ''), san: san, from: m.from, to: m.to });
    sel = -1;
    var ended = finishIfOver();
    renderPlay();
    if (ended) Sound.play(pos.inCheck() ? 'mate' : 'end');
    else Sound.play(pos.inCheck() ? 'check' : (m.captured ? 'capture' : 'move'));
    if (ended) setTimeout(askReview, 700);
    else if (pos.turn !== human) engineMove();
  }

  // بعد انتهاء المباراة: هل تريد مراجعتها؟
  function askReview() {
    if (!over || hist.length < 2) return;
    $('reviewTxt').textContent = resultText;
    $('review').hidden = false;
  }
  $('reviewYes').onclick = function () { $('review').hidden = true; $('pAnalyze').onclick(); };
  $('reviewNo').onclick = function () { $('review').hidden = true; };

  function engineMove() {
    if (over || !engineReady) return;
    var gid = gameId; thinking = true; renderPlay();
    var T = parseInt($('pTime').value, 10) || 1000;
    var lv = Bots.level(gameBot, engineId, T);
    Bots.applyLevel(engine, lv).then(function () {
      modeApplied = 'play';
      return Bots.pickMove(engine, pos, lv);
    }).then(function (m) {
      if (gid !== gameId) return;
      thinking = false;
      if (!m) { renderPlay(); return; }
      doMove(m);
    }).catch(function (e) {
      if (gid !== gameId) return;
      thinking = false; banner('خطأ من المحرك: ' + e.message); renderPlay();
    });
  }

  function onPlayTap(i) {
    if (over || thinking || !engineReady || pos.turn !== human) return;
    if (sel >= 0) {
      var cands = pos.movesFrom(sel).filter(function (m) { return m.to === i; });
      if (cands.length === 1) { doMove(cands[0]); return; }
      if (cands.length > 1) { askPromotion(cands); return; }
    }
    var p = pos.board[i];
    if (p && Chess.colorOf(p) === human) sel = (sel === i) ? -1 : i; else sel = -1;
    renderPlay();
  }

  function askPromotion(cands) {
    pendingPromo = cands;
    var box = $('promoBtns'); box.innerHTML = '';
    ['q', 'r', 'b', 'n'].forEach(function (pc) {
      var m = cands.filter(function (x) { return x.promo === pc; })[0]; if (!m) return;
      var b = document.createElement('button');
      b.innerHTML = '<img src="' + pieceDir + '/' + human + pc.toUpperCase() + '.svg">';
      b.onclick = function () { $('promo').hidden = true; pendingPromo = null; doMove(m); };
      box.appendChild(b);
    });
    $('promo').hidden = false;
  }
  $('promo').addEventListener('click', function (e) { if (e.target === $('promo')) { $('promo').hidden = true; pendingPromo = null; } });

  $('pNew').onclick = newGame;
  $('pFlip').onclick = function () { pBoard.flip = !pBoard.flip; renderPlay(); };
  $('pUndo').onclick = function () {
    if (!hist.length) return;
    gameId++; if (engine && thinking) engine.stop();
    thinking = false; over = false; resultText = ''; sel = -1;
    do { pos.unmake(); hist.pop(); } while (hist.length && pos.turn !== human);
    renderPlay();
    if (pos.turn !== human) engineMove();
  };
  $('pResign').onclick = function () {
    if (over || !hist.length) return;
    gameId++; if (engine && thinking) engine.stop();
    thinking = false; over = true; resultText = 'استسلمت — فاز المحرك'; hist.result = human === 'w' ? '0-1' : '1-0';
    renderPlay();
    setTimeout(askReview, 400);
  };
  function currentPgn() {
    var res = hist.result || '*';
    var hn = 'لاعب', en = botLabel(gameBot) + ' - ' + engineDisplayName();
    return toPgn(Chess.START_FEN, hist.map(function (h) { return h.uci; }), res, human === 'w' ? hn : en, human === 'w' ? en : hn);
  }
  $('pCopy').onclick = function () {
    if (!hist.length) return;
    var t = currentPgn();
    if (AB && AB.copy) { AB.copy(t); setStatus('تم نسخ PGN'); setTimeout(updatePlayStatus, 1500); }
  };
  $('pAnalyze').onclick = function () {
    if (hist.length < 2) { setStatus('العب بضع نقلات أولاً'); setTimeout(updatePlayStatus, 1500); return; }
    $('aText').value = currentPgn();
    showTab('analyze');
    runAnalysis();
  };

  /* ================== التحليل ================== */
  var aRes = null, aIdx = 0, aRunning = false, aCancel = false;
  var aBoard = new BoardView($('aBoard'), $('aArrows'), null);
  var BAD = { dubious: 1, inaccuracy: 1, mistake: 1, blunder: 1, miss: 1 };

  function runAnalysis() {
    if (aRunning) return;
    if (!engineReady) { banner('المحرك غير جاهز بعد'); return; }
    if (thinking) { banner('انتظر حتى ينهي المحرك نقلته ثم أعد المحاولة'); return; }
    var txt = $('aText').value.trim();
    if (!txt) { banner('ألصق المباراة أولاً'); return; }
    var g;
    try { g = Chess.parsePgn(txt); } catch (e) { banner('تعذّر قراءة المباراة: ' + e.message); return; }
    if (!g.moves.length) { banner(g.error ? 'تعذّر فهم النقلة "' + g.error.token + '"' : 'لم أجد أي نقلة في النص'); return; }
    banner(g.error ? 'تنبيه: تعذّر فهم النقلة "' + g.error.token + '" (رقم ' + g.error.ply + ') — سأحلل ما قبلها فقط' : '');
    if (!bookSet) { try { bookSet = A.buildBook(AB && AB.getBook ? AB.getBook() : ''); } catch (e) { bookSet = {}; } }

    var T = parseInt($('aTime').value, 10) || 400;
    lsSet('atime', $('aTime').value);
    aRunning = true; aCancel = false;
    $('aInput').hidden = true; $('aResult').hidden = true; $('aProgress').hidden = false;
    $('aPbar').style.width = '0%'; $('aPtext').textContent = 'جاري التحضير…';
    if (AB && AB.keepAwake) AB.keepAwake(true);

    applyAnalysisMode().then(function () {
      engine.newGame();
      return A.analyzeGame({
        engine: engine, startFen: g.startFen, moves: g.moves, book: bookSet, movetime: T,
        isCancelled: function () { return aCancel; },
        onProgress: function (done, total) {
          $('aPbar').style.width = Math.round(100 * done / total) + '%';
          $('aPtext').textContent = 'جاري التحليل… ' + Math.round(100 * done / total) + '%';
        }
      });
    }).then(function (res) {
      aRes = res; aIdx = 0; showAnalysis();
    }).catch(function (e) {
      $('aInput').hidden = false;
      if (e && e.message !== 'cancelled') banner('فشل التحليل: ' + (e.message || e));
    }).then(function () {
      aRunning = false; $('aProgress').hidden = true;
      if (AB && AB.keepAwake) AB.keepAwake(false);
    });
  }

  function showAnalysis() {
    $('aInput').hidden = true; $('aResult').hidden = false;
    // ملخص
    var rows = '<tr><th></th><th>⚪ الأبيض</th><th>⚫ الأسود</th></tr>';
    var ac = function (v) { return v === null ? '-' : Math.round(v) + '%'; };
    rows += '<tr><td class="l"><b>الدقة</b></td><td><b>' + ac(aRes.accuracy.w) + '</b></td><td><b>' + ac(aRes.accuracy.b) + '</b></td></tr>';
    A.ORDER.forEach(function (c) {
      var info = A.CATS[c];
      rows += '<tr><td class="l">' + info.emoji + ' ' + info.ar + '</td><td>' + aRes.counts.w[c] + '</td><td>' + aRes.counts.b[c] + '</td></tr>';
    });
    $('aSummary').innerHTML = rows;
    // قائمة النقلات
    var html = '', i, fullStart = parseInt(aRes.startFen.split(' ')[5] || '1', 10) || 1;
    var offset = (aRes.plies.length && aRes.plies[0].color === 'b') ? 1 : 0;
    var total = aRes.plies.length + offset;
    for (var k = 0; k < total; k++) {
      if (k % 2 === 0) html += '<div class="n">' + (fullStart + Math.floor(k / 2)) + '</div>';
      var pi = k - offset;
      if (pi < 0) { html += '<div class="mv empty"></div>'; continue; }
      var p = aRes.plies[pi];
      html += '<div class="mv" data-ply="' + (pi + 1) + '">' + esc(p.san) + (p.cat ? '<span>' + A.CATS[p.cat].emoji + '</span>' : '') + '</div>';
    }
    $('aMoves').innerHTML = html;
    renderAnalysis();
  }

  function renderAnalysis() {
    var idx = aIdx, plies = aRes.plies;
    var ply = idx > 0 ? plies[idx - 1] : null;
    var p = new Position(ply ? ply.fenAfter : aRes.startFen);
    var badArrow = null;
    if (ply && BAD[ply.cat] && ply.bestUci && ply.bestUci !== ply.uci) {
      var bm = new Position(ply.fenBefore).fromUci(ply.bestUci);
      if (bm) badArrow = { from: bm.from, to: bm.to };
    }
    aBoard.render({
      board: p.board, last: ply ? [ply.from, ply.to] : null,
      check: p.inCheck() ? p.kingSq(p.turn) : -1,
      badge: ply && ply.cat ? { sq: ply.to, cat: ply.cat } : null, arrow: badArrow
    });
    // شريط التقييم
    var cp = ply ? ply.cpW : aRes.startCpW, mate = ply ? ply.mateW : null;
    $('aEvalW').style.height = A.wc(cp) + '%';
    var bar = $('aEvalBar'); bar.style.alignItems = aBoard.flip ? 'flex-start' : 'flex-end';
    var et = A.cpToText(cp, mate);
    $('aEvalTxt').textContent = et.replace('+', '');
    // بطاقة المعلومات
    var info = $('aInfo');
    if (!ply) {
      info.innerHTML = '<div class="big">🏁 بداية المباراة</div><div class="sub">استخدم الأسهم ◀ ▶ أو اضغط على أي نقلة. التقييم: <bdi dir="ltr">' + et + '</bdi></div>';
    } else {
      var c = ply.cat ? A.CATS[ply.cat] : null;
      var fullNo = parseInt(ply.fenBefore.split(' ')[5] || '1', 10) || 1;
      var num = fullNo + (ply.color === 'w' ? '.' : '…');
      var sub = 'التقييم: <bdi dir="ltr">' + et + '</bdi>';
      if (BAD[ply.cat] && ply.bestSan && ply.bestUci !== ply.uci) sub += ' · الأفضل كان: <b>' + esc(ply.bestSan) + '</b>';
      info.innerHTML = '<div class="big"><span class="em">' + (c ? c.emoji : '▫️') + '</span><span style="direction:ltr">' + num + ' ' + esc(ply.san) + '</span><span>' + (c ? c.ar : 'نقلة عادية') + '</span></div><div class="sub">' + sub + '</div>';
    }
    // تمييز النقلة الحالية
    var cells = $('aMoves').querySelectorAll('.mv'), cur = null;
    for (var i = 0; i < cells.length; i++) {
      var on = parseInt(cells[i].getAttribute('data-ply') || '-1', 10) === idx;
      cells[i].classList.toggle('cur', on); if (on) cur = cells[i];
    }
    if (cur && cur.scrollIntoView) { var box = $('aMoves'); box.scrollTop = cur.offsetTop - box.clientHeight / 2; }
  }

  function plySound(ply) {
    var k = ply.san.indexOf('#') >= 0 ? 'mate' : ply.san.indexOf('+') >= 0 ? 'check' : ply.san.indexOf('x') >= 0 ? 'capture' : 'move';
    Sound.play(k);
    if (ply.cat === 'brilliant') setTimeout(function () { Sound.play('brilliant'); }, 150);
  }
  function goto(i) {
    if (!aRes) return;
    aIdx = Math.max(0, Math.min(aRes.plies.length, i)); renderAnalysis();
    if (aIdx > 0) plySound(aRes.plies[aIdx - 1]);
  }
  $('aFirst').onclick = function () { goto(0); };
  $('aPrev').onclick = function () { goto(aIdx - 1); };
  $('aNext').onclick = function () { goto(aIdx + 1); };
  $('aLast').onclick = function () { goto(aRes ? aRes.plies.length : 0); };
  $('aFlip').onclick = function () { aBoard.flip = !aBoard.flip; renderAnalysis(); };
  $('aMoves').addEventListener('click', function (e) {
    var t = e.target; while (t && t !== this && !t.getAttribute('data-ply')) t = t.parentNode;
    if (t && t !== this) goto(parseInt(t.getAttribute('data-ply'), 10));
  });
  $('aGo').onclick = runAnalysis;
  $('aClear').onclick = function () { $('aText').value = ''; banner(''); };
  $('aCancel').onclick = function () { aCancel = true; if (engine) engine.stop(); };
  $('aNewAnalysis').onclick = function () { $('aResult').hidden = true; $('aInput').hidden = false; banner(''); };

  /* ================== تبويبات وبدء التشغيل ================== */
  function showTab(name) {
    var tabs = document.querySelectorAll('.tabs button');
    for (var i = 0; i < tabs.length; i++) tabs[i].classList.toggle('on', tabs[i].getAttribute('data-tab') === name);
    $('tab-play').hidden = name !== 'play';
    $('tab-analyze').hidden = name !== 'analyze';
  }
  (function () {
    var tabs = document.querySelectorAll('.tabs button');
    for (var i = 0; i < tabs.length; i++) tabs[i].onclick = function () { showTab(this.getAttribute('data-tab')); };
  })();

  function fillEngines() {
    var sel2 = $('engineSel'); sel2.innerHTML = '';
    try { engineList = JSON.parse(AB ? AB.engines() : '[]'); } catch (e) { engineList = []; }
    engineList.forEach(function (e) {
      var o = document.createElement('option'); o.value = e.id; o.textContent = e.name; sel2.appendChild(o);
    });
    var saved = lsGet('engine', 'stockfish');
    var ids = engineList.map(function (e) { return e.id; });
    sel2.value = ids.indexOf(saved) >= 0 ? saved : (ids[0] || '');
  }

  $('engineSel').onchange = function () {
    lsSet('engine', this.value);
    gameId++; thinking = false; aCancel = true;
    startEngine(this.value).then(function () { newGame(); }).catch(function () { });
  };

  function setPieces(id, quiet) {
    PIECE_SETS.forEach(function (x) { if (x.id === id) pieceDir = x.dir; });
    $('pieceSel').value = id; lsSet('pieces', id);
    $('brandImg').src = pieceDir + '/wK.svg';
    if (quiet) return;
    renderPlay(); renderBots(); if (aRes) renderAnalysis();
  }

  function setSound(on) {
    Sound.setEnabled(on); lsSet('sound', on ? '1' : '0');
    $('sndBtn').textContent = on ? '🔊' : '🔇';
  }
  $('sndBtn').onclick = function () { setSound(!Sound.isEnabled()); if (Sound.isEnabled()) Sound.play('move'); };

  function init() {
    setSound(lsGet('sound', '1') === '1');
    $('pSide').value = lsGet('side', 'w'); $('pTime').value = lsGet('time', '1000');
    $('aTime').value = lsGet('atime', '400');
    var ps = $('pieceSel'); ps.innerHTML = '';
    PIECE_SETS.forEach(function (x) { var o = document.createElement('option'); o.value = x.id; o.textContent = x.name; ps.appendChild(o); });
    var savedSet = lsGet('pieces', 'set1');
    setPieces(PIECE_SETS.some(function (x) { return x.id === savedSet; }) ? savedSet : 'set1', true);
    ps.onchange = function () { setPieces(this.value); };
    renderPlay(); renderBots();
    if (!AB) { banner('هذه الصفحة تعمل فقط داخل تطبيق الأندرويد (المحرك غير متوفر).'); return; }
    fillEngines();
    if (!engineList.length) {
      var dg = ''; try { dg = AB.diag(); } catch (e) { }
      banner('لم يتم العثور على أي محرك داخل التطبيق. ' + dg); setStatus('لا يوجد محرك'); return;
    }
    startEngine($('engineSel').value).then(function () { newGame(); }).catch(function () { });
  }
  window.SaraApp = { init: init, _state: function () { return { hist: hist, pos: pos, aRes: aRes }; } };
  init();
})();
