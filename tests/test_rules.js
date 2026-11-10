const { Position, parsePgn } = require('../app/src/main/assets/rules.js');
const cases = [
  ['rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1', [20, 400, 8902, 197281]],
  ['r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1', [48, 2039, 97862]],
  ['8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1', [14, 191, 2812, 43238]],
  ['r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1', [6, 264, 9467]],
  ['rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8', [44, 1486, 62379]],
];
let ok = true;
for (const [fen, exp] of cases) {
  const p = new Position(fen);
  exp.forEach((e, i) => { const n = p.perft(i + 1); if (n !== e) { ok = false; console.log('FAIL', fen, 'depth', i + 1, n, 'expected', e); } });
  if (p.fen().split(' ').slice(0, 4).join(' ') !== fen.split(' ').slice(0, 4).join(' ')) { ok = false; console.log('FEN roundtrip changed', p.fen()); }
}
console.log(ok ? 'PERFT OK' : 'PERFT FAILED');
// SAN / PGN
const g = parsePgn('[Event "x"]\n1. e4 e5 2. Nf3 {comment} Nc6 3. Bb5 a6 (3... Nf6) 4. Ba4 Nf6 5. O-O Be7 6. Re1 b5 7. Bb3 d6 8. c3 O-O 1/2-1/2');
console.log(g.moves.join(' '), g.error);
const p = new Position();
const sans = []; g.moves.forEach(u => { const m = p.fromUci(u); sans.push(p.san(m)); p.make(m); });
console.log(sans.join(' '));
// fool's mate and stalemate
const f = parsePgn('1. f3 e5 2. g4 Qh4#'); const q = new Position(); f.moves.forEach(u => q.make(q.fromUci(u)));
console.log(JSON.stringify(q.status()));
console.log(JSON.stringify(new Position('7k/5Q2/6K1/8/8/8/8/8 b - - 0 1').status()));
// promotion SAN + ambiguity + ep
const pp = new Position('r3k2r/1P4P1/8/3pP3/8/8/8/R3K2R w KQkq d6 0 1');
console.log(pp.moves().map(m => pp.san(m)).sort().join(' '));
// sacrifice check: Qxd5 type? queen to attacked square
const sac = new Position('4k3/8/8/3p4/8/8/8/3QK3 w - - 0 1'); const qm = sac.fromUci('d1d5'); console.log('sacNet Qxd5 (pawn protected? no):', sac.sacNet(qm));
const sac2 = new Position('4k3/8/2p5/3p4/8/8/8/3QK3 w - - 0 1'); console.log('sacNet Qxd5 protected by c6:', sac2.sacNet(sac2.fromUci('d1d5')));
