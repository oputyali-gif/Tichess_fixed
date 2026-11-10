// معايرة سريعة: node tests/bots_match.js <engine-exe> <sara|stockfish> <games-per-pair>
const { spawn } = require('child_process'); const readline = require('readline'); const path = require('path');
const { UciEngine } = require('../app/src/main/assets/engine.js');
const { Position, VAL } = require('../app/src/main/assets/rules.js');
const Bots = require('../app/src/main/assets/bots.js');
const exe = process.argv[2], kind = process.argv[3], N = parseInt(process.argv[4] || '8', 10);
const child = spawn(exe, [], { cwd: path.dirname(exe) });
const rl = readline.createInterface({ input: child.stdout });
const eng = new UciEngine(l => child.stdin.write(l + '\n')); rl.on('line', l => eng.onLine(l)); child.stderr.on('data', () => {});

function material(p) { let s = 0; for (const c of p.board) { if (!c || c.toLowerCase() === 'k') continue; const v = VAL[c.toLowerCase()]; s += c === c.toUpperCase() ? v : -v; } return s; }

async function game(white, black) {
  const p = new Position(); const lv = { w: Bots.level(white, kind), b: Bots.level(black, kind) };
  let ply = 0;
  while (ply < 160) {
    const st = p.status(); if (st.over) return st.result === '1-0' ? 1 : st.result === '0-1' ? 0 : 0.5;
    const l = lv[p.turn]; await Bots.applyLevel(eng, l);
    const m = await Bots.pickMove(eng, p, l, true); if (!m) break; p.make(m); ply++;
  }
  const d = material(p); return d >= 300 ? 1 : d <= -300 ? 0 : 0.5;   // حسم بالمادة عند الوصول للحد
}

(async () => {
  await eng.init({ threads: 1, hashMB: 16 }); console.log(eng.name, 'kind', kind);
  const order = ['pawn', 'knight', 'bishop', 'rook', 'queen', 'king'];
  for (let i = 0; i < order.length - 1; i++) {
    const a = order[i], b = order[i + 1]; let sb = 0; const t0 = Date.now();
    for (let g = 0; g < N; g++) { if (g % 2 === 0) sb += 1 - await game(a, b); else sb += await game(b, a); }
    const r = Bots.LEVELS[kind]; console.log(`${a}(${r[a].rating}) vs ${b}(${r[b].rating}): stronger scored ${sb}/${N}  (${((Date.now() - t0) / 1000).toFixed(0)}s)`);
  }
  child.stdin.write('quit\n'); setTimeout(() => process.exit(0), 200);
})();
