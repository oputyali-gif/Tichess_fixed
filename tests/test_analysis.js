// usage: node tests/test_analysis.js <engine-exe> [movetime]
const { spawn } = require('child_process');
const readline = require('readline');
const fs = require('fs');
const path = require('path');
const { UciEngine } = require('../app/src/main/assets/engine.js');
const A = require('../app/src/main/assets/analysis.js');
const { parsePgn } = require('../app/src/main/assets/rules.js');

const exe = process.argv[2];
const T = parseInt(process.argv[3] || '300', 10);
const pgn = process.argv[4] || '1. e4 e5 2. Nf3 d6 3. d4 Bg4 4. dxe5 Bxf3 5. Qxf3 dxe5 6. Bc4 Nf6 7. Qb3 Qe7 8. Nc3 c6 9. Bg5 b5 10. Nxb5 cxb5 11. Bxb5+ Nbd7 12. O-O-O Rd8 13. Rxd7 Rxd7 14. Rd1 Qe6 15. Bxd7+ Nxd7 16. Qb8+ Nxb8 17. Rd8#';

const child = spawn(exe, [], { cwd: path.dirname(exe) });
const rl = readline.createInterface({ input: child.stdout });
const eng = new UciEngine(l => child.stdin.write(l + '\n'));
rl.on('line', l => eng.onLine(l));
child.stderr.on('data', () => {});

(async () => {
  await eng.init({ threads: 1, hashMB: 32 });
  console.log('engine:', eng.name);
  eng.setOption(['Book'], 'false');
  await eng.ready();
  const game = parsePgn(pgn);
  if (game.error) { console.log('PGN error', game.error); process.exit(1); }
  const book = A.buildBook(fs.readFileSync(path.join(__dirname, '../app/src/main/assets/book/openings.txt'), 'utf8'));
  console.log('book positions:', Object.keys(book).length);
  const t0 = Date.now();
  const res = await A.analyzeGame({ engine: eng, startFen: game.startFen, moves: game.moves, book, movetime: T,
    onProgress: () => {} });
  console.log('time', ((Date.now() - t0) / 1000).toFixed(1) + 's');
  res.plies.forEach(p => {
    const c = p.cat ? A.CATS[p.cat].emoji + ' ' + p.cat : '-';
    console.log(String(p.ply).padStart(2), p.color, p.san.padEnd(7), c.padEnd(16), 'loss', p.loss.toFixed(1).padStart(5), 'eval', A.cpToText(p.cpW, p.mateW), p.bestSan && p.bestUci !== p.uci ? ('best ' + p.bestSan) : '');
  });
  console.log('accuracy', res.accuracy.w.toFixed(0), res.accuracy.b.toFixed(0));
  console.log(JSON.stringify(res.counts));
  child.stdin.write('quit\n'); setTimeout(() => process.exit(0), 200);
})().catch(e => { console.error('ERR', e); process.exit(1); });
