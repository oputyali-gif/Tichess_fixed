// ============================================================================
//  Engine99 - UCI chess engine (single file, C++17)
//
//  Board   : bitboards, magic bitboards, make/unmake, Zobrist hashing
//  Search  : iterative deepening up to depth 99, aspiration windows, PVS,
//            transposition table (lock-less), null move, reverse futility,
//            razoring, futility, late move pruning, SEE pruning, LMR,
//            singular extensions, IIR, check extensions, quiescence,
//            killers + counter moves + history + continuation history,
//            Lazy-SMP multi-threading
//  Eval    : NNUE-style net (768 -> H x2 -> 1, incremental) loaded from nn.bin,
//            falls back to PeSTO tapered eval when no net is present
//  Extras  : "perft N", "bench", "datagen" (self-play data for train_nn.py)
//
//  Build (Linux / Termux):
//      clang++ -O3 -std=c++17 -pthread -o engine99 engine99.cpp
//      (add -march=native on x86 PCs for more speed)
// ============================================================================
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
using namespace std;

typedef uint64_t U64;
typedef uint16_t Move;
typedef int16_t i16;
typedef int32_t i32;

enum { WHITE, BLACK };
enum { PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING };
enum { QUIET = 0, DOUBLE = 1, KCASTLE = 2, QCASTLE = 3, CAPTURE = 4, EP = 5, PROMO = 8 };
enum { BOUND_LOWER = 1, BOUND_UPPER = 2, BOUND_EXACT = 3 };

const int NO_PC = 12;
const int MAXPLY = 256;
const int MAXSTACK = 2048;
const int MATE = 32000, INF = 32700, MATE_BOUND = MATE - MAXPLY, NO_EVAL = 32701;
const char* START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

const U64 FILE_A = 0x0101010101010101ULL, FILE_H = FILE_A << 7;
const U64 RANK_1 = 0xFFULL, RANK_2 = RANK_1 << 8, RANK_7 = RANK_1 << 48, RANK_8 = RANK_1 << 56;

inline U64 bit(int s) { return 1ULL << s; }
inline int popcnt(U64 b) { return __builtin_popcountll(b); }
inline int lsb(U64 b) { return __builtin_ctzll(b); }
inline int pop_lsb(U64& b) { int s = __builtin_ctzll(b); b &= b - 1; return s; }

// ----------------------------------------------------------------------------
//  Attack tables
// ----------------------------------------------------------------------------
U64 KnightAtt[64], KingAtt[64], PawnAtt[2][64];
struct Magic { U64 mask, magic; U64* att; int shift; };
Magic RM[64], BM[64];
U64 rookTab[102400], bishopTab[5248];

U64 slideAtt(int sq, U64 occ, bool rook) {
    static const int rd[4] = {1, -1, 0, 0}, rf[4] = {0, 0, 1, -1};
    static const int bd[4] = {1, 1, -1, -1}, bf[4] = {1, -1, 1, -1};
    U64 a = 0;
    for (int d = 0; d < 4; d++) {
        int dr = rook ? rd[d] : bd[d], df = rook ? rf[d] : bf[d];
        int r = sq / 8 + dr, f = sq % 8 + df;
        while (r >= 0 && r < 8 && f >= 0 && f < 8) {
            int s = r * 8 + f;
            a |= bit(s);
            if (occ & bit(s)) break;
            r += dr; f += df;
        }
    }
    return a;
}

void initMagics() {
    mt19937_64 rng(0x1234567ULL);
    for (int pass = 0; pass < 2; pass++) {
        bool rook = pass == 0;
        Magic* M = rook ? RM : BM;
        U64* ptr = rook ? rookTab : bishopTab;
        for (int sq = 0; sq < 64; sq++) {
            int r = sq / 8, f = sq % 8;
            U64 edges = ((RANK_1 | RANK_8) & ~(RANK_1 << (8 * r))) | ((FILE_A | FILE_H) & ~(FILE_A << f));
            U64 mask = slideAtt(sq, 0, rook) & ~edges;
            int bits = popcnt(mask), size = 1 << bits;
            vector<U64> occv(size), ref(size);
            vector<int> epoch(size, 0);
            U64 b = 0; int n = 0;
            do { occv[n] = b; ref[n] = slideAtt(sq, b, rook); n++; b = (b - mask) & mask; } while (b);
            M[sq].mask = mask; M[sq].shift = 64 - bits; M[sq].att = ptr;
            int cnt = 0;
            for (;;) {
                U64 magic = rng() & rng() & rng();
                if (popcnt((mask * magic) >> 56) < 6) continue;
                cnt++;
                bool ok = true;
                for (int i = 0; i < size; i++) {
                    int idx = (int)((occv[i] * magic) >> M[sq].shift);
                    if (epoch[idx] != cnt) { epoch[idx] = cnt; M[sq].att[idx] = ref[i]; }
                    else if (M[sq].att[idx] != ref[i]) { ok = false; break; }
                }
                if (ok) { M[sq].magic = magic; break; }
            }
            ptr += size;
        }
    }
}

inline U64 rookAtt(int sq, U64 occ) { const Magic& m = RM[sq]; return m.att[((occ & m.mask) * m.magic) >> m.shift]; }
inline U64 bishopAtt(int sq, U64 occ) { const Magic& m = BM[sq]; return m.att[((occ & m.mask) * m.magic) >> m.shift]; }

void initLeapers() {
    static const int kn[8][2] = {{1,2},{2,1},{-1,2},{-2,1},{1,-2},{2,-1},{-1,-2},{-2,-1}};
    static const int kg[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
    for (int sq = 0; sq < 64; sq++) {
        int r = sq / 8, f = sq % 8;
        KnightAtt[sq] = KingAtt[sq] = PawnAtt[0][sq] = PawnAtt[1][sq] = 0;
        for (auto& d : kn) { int rr = r + d[0], ff = f + d[1]; if (rr >= 0 && rr < 8 && ff >= 0 && ff < 8) KnightAtt[sq] |= bit(rr * 8 + ff); }
        for (auto& d : kg) { int rr = r + d[0], ff = f + d[1]; if (rr >= 0 && rr < 8 && ff >= 0 && ff < 8) KingAtt[sq] |= bit(rr * 8 + ff); }
        for (int df = -1; df <= 1; df += 2) {
            if (r < 7 && f + df >= 0 && f + df < 8) PawnAtt[WHITE][sq] |= bit((r + 1) * 8 + f + df);
            if (r > 0 && f + df >= 0 && f + df < 8) PawnAtt[BLACK][sq] |= bit((r - 1) * 8 + f + df);
        }
    }
}

// ----------------------------------------------------------------------------
//  Zobrist
// ----------------------------------------------------------------------------
U64 ZPiece[12][64], ZCastle[16], ZEp[8], ZSide;
void initZobrist() {
    mt19937_64 rng(0xC0FFEE123ULL);
    for (auto& a : ZPiece) for (auto& k : a) k = rng();
    for (auto& k : ZCastle) k = rng();
    for (auto& k : ZEp) k = rng();
    ZSide = rng();
}

const int castleMask[64] = {
    13,15,15,15,12,15,15,14, 15,15,15,15,15,15,15,15, 15,15,15,15,15,15,15,15, 15,15,15,15,15,15,15,15,
    15,15,15,15,15,15,15,15, 15,15,15,15,15,15,15,15, 15,15,15,15,15,15,15,15,  7,15,15,15, 3,15,15,11};

// ----------------------------------------------------------------------------
//  Neural net (loaded from file)
// ----------------------------------------------------------------------------
// Settings used when loading a RAW external net (bullet "simple" format): set with setoption NetQA/NetQB/NetScale/NetSCReLU/NetResidual
int optQA = 255, optQB = 64, optScale = 400; bool optSCReLU = false, optResidual = false;

struct Net {
    int H = 0, QA = 255, QB = 64, SCALE = 400; bool screlu = false;
    vector<i16> W1, b1, W2;   // W1: [768][H] feature-major
    i32 b2 = 0;
    bool loaded = false, residual = false;   // residual: eval = classical eval + net output
} net;

inline int feat(int p, int pc, int sq) {
    int c = pc / 6, t = pc % 6;
    return (c == p ? 0 : 384) + t * 64 + (p == WHITE ? sq : (sq ^ 56));
}

// External net in the common "bullet simple" layout, no header:
//   i16 featureWeights[768][H], i16 featureBias[H], i16 outputWeights[2][H] (side-to-move first), i16 outputBias
// (file may be padded to a multiple of 64 bytes). H is deduced from the file size.
bool loadRawNet(const string& path) {
    ifstream f(path, ios::binary | ios::ate);
    if (!f) return false;
    size_t size = (size_t)f.tellg(); f.seekg(0);
    int H = 0;
    for (int h = 1; h <= 2048 && !H; h++) {
        size_t n = 2 * ((size_t)771 * h + 1), padded = (n + 63) / 64 * 64;
        if (size == n || size == padded) H = h;
    }
    if (!H) return false;
    Net n; n.H = H; n.QA = optQA; n.QB = optQB; n.SCALE = optScale; n.screlu = optSCReLU;
    n.W1.resize((size_t)768 * H); n.b1.resize(H); n.W2.resize(2 * H);
    i16 ob = 0;
    f.read((char*)n.W1.data(), n.W1.size() * 2); f.read((char*)n.b1.data(), n.b1.size() * 2);
    f.read((char*)n.W2.data(), n.W2.size() * 2); f.read((char*)&ob, 2);
    if (!f) return false;
    n.b2 = ob; n.loaded = true; n.residual = optResidual;
    net = n;
    return true;
}

bool loadNet(const string& path) {
    ifstream f(path, ios::binary);
    if (!f) return false;
    char magic[4];
    f.read(magic, 4);
    if (!f || (memcmp(magic, "NN99", 4) != 0 && memcmp(magic, "NN9R", 4) != 0)) { f.close(); return loadRawNet(path); }
    i32 H, QA, QB, SC;
    f.read((char*)&H, 4); f.read((char*)&QA, 4); f.read((char*)&QB, 4); f.read((char*)&SC, 4);
    if (!f || H <= 0 || H > 2048 || QA <= 0 || QB <= 0 || SC <= 0) return false;
    Net n;
    n.H = H; n.QA = QA; n.QB = QB; n.SCALE = SC;
    n.W1.resize((size_t)768 * H); n.b1.resize(H); n.W2.resize(2 * H);
    f.read((char*)n.W1.data(), n.W1.size() * 2);
    f.read((char*)n.b1.data(), n.b1.size() * 2);
    f.read((char*)n.W2.data(), n.W2.size() * 2);
    f.read((char*)&n.b2, 4);
    if (!f) return false;
    n.loaded = true;
    n.residual = memcmp(magic, "NN9R", 4) == 0;
    net = n;
    return true;
}

// ----------------------------------------------------------------------------
//  Position
// ----------------------------------------------------------------------------
struct Hist { U64 key; int castling, ep, halfmove, captured; };

struct Position {
    U64 bb[12], occ[2], all, key;
    int board[64];
    int side, castling, ep, halfmove, fullmove, sp;
    Hist hist[MAXSTACK];
    vector<i16> accv;

    Position() { clear(); }

    i16* accPtr(int s, int p) const { return const_cast<i16*>(accv.data()) + ((size_t)s * 2 + p) * net.H; }

    void clear() {
        memset(bb, 0, sizeof bb);
        occ[0] = occ[1] = all = 0;
        for (int i = 0; i < 64; i++) board[i] = NO_PC;
        side = WHITE; castling = 0; ep = -1; halfmove = 0; fullmove = 1; key = 0; sp = 0;
    }
    inline void put(int pc, int sq) { bb[pc] |= bit(sq); occ[pc / 6] |= bit(sq); all |= bit(sq); board[sq] = pc; }
    inline void del(int pc, int sq) { bb[pc] &= ~bit(sq); occ[pc / 6] &= ~bit(sq); all &= ~bit(sq); board[sq] = NO_PC; }

    void accUpdate(int pc, int sq, bool add) {
        int H = net.H;
        for (int p = 0; p < 2; p++) {
            i16* a = accPtr(sp, p);
            const i16* w = &net.W1[(size_t)feat(p, pc, sq) * H];
            if (add) for (int i = 0; i < H; i++) a[i] += w[i];
            else     for (int i = 0; i < H; i++) a[i] -= w[i];
        }
    }
    inline void addP(int pc, int sq) { put(pc, sq); key ^= ZPiece[pc][sq]; if (net.loaded) accUpdate(pc, sq, true); }
    inline void delP(int pc, int sq) { del(pc, sq); key ^= ZPiece[pc][sq]; if (net.loaded) accUpdate(pc, sq, false); }

    void refreshAcc() {
        if (!net.loaded) return;
        accv.assign((size_t)MAXSTACK * 2 * net.H, 0);
        for (int p = 0; p < 2; p++) {
            i16* a = accPtr(sp, p);
            for (int i = 0; i < net.H; i++) a[i] = net.b1[i];
        }
        for (int pc = 0; pc < 12; pc++) { U64 b = bb[pc]; while (b) accUpdate(pc, pop_lsb(b), true); }
    }

    bool setFen(const string& fen) {
        clear();
        istringstream is(fen);
        string placement, stm, cast, eps;
        is >> placement >> stm >> cast >> eps;
        int hm = 0, fm = 1; is >> hm >> fm;
        int r = 7, f = 0;
        for (char c : placement) {
            if (c == '/') { r--; f = 0; }
            else if (isdigit((unsigned char)c)) f += c - '0';
            else {
                size_t pc = string("PNBRQKpnbrqk").find(c);
                if (pc == string::npos || r < 0 || f > 7) return false;
                put((int)pc, r * 8 + f); f++;
            }
        }
        side = (stm == "b") ? BLACK : WHITE;
        for (char c : cast) { if (c == 'K') castling |= 1; if (c == 'Q') castling |= 2; if (c == 'k') castling |= 4; if (c == 'q') castling |= 8; }
        ep = -1;
        if (eps.size() == 2 && eps != "-") {
            int e = (eps[1] - '1') * 8 + (eps[0] - 'a');
            if (e >= 0 && e < 64 && (PawnAtt[side ^ 1][e] & bb[side * 6 + PAWN])) ep = e;
        }
        halfmove = hm; fullmove = fm;
        key = 0;
        for (int pc = 0; pc < 12; pc++) { U64 b = bb[pc]; while (b) key ^= ZPiece[pc][pop_lsb(b)]; }
        key ^= ZCastle[castling];
        if (ep >= 0) key ^= ZEp[ep & 7];
        if (side == BLACK) key ^= ZSide;
        if (!bb[KING] || !bb[6 + KING]) return false;
        refreshAcc();
        return true;
    }

    string fen() const {
        ostringstream o;
        for (int r = 7; r >= 0; r--) {
            int e = 0;
            for (int f = 0; f < 8; f++) {
                int pc = board[r * 8 + f];
                if (pc == NO_PC) e++;
                else { if (e) { o << e; e = 0; } o << "PNBRQKpnbrqk"[pc]; }
            }
            if (e) o << e;
            if (r) o << '/';
        }
        o << (side == WHITE ? " w " : " b ");
        if (!castling) o << '-';
        else { if (castling & 1) o << 'K'; if (castling & 2) o << 'Q'; if (castling & 4) o << 'k'; if (castling & 8) o << 'q'; }
        o << ' ';
        if (ep < 0) o << '-'; else o << char('a' + ep % 8) << char('1' + ep / 8);
        o << ' ' << halfmove << ' ' << fullmove;
        return o.str();
    }

    inline U64 attackersTo(int sq, U64 o) const {
        return (PawnAtt[BLACK][sq] & bb[PAWN]) | (PawnAtt[WHITE][sq] & bb[6 + PAWN]) |
               (KnightAtt[sq] & (bb[KNIGHT] | bb[6 + KNIGHT])) | (KingAtt[sq] & (bb[KING] | bb[6 + KING])) |
               (bishopAtt(sq, o) & (bb[BISHOP] | bb[6 + BISHOP] | bb[QUEEN] | bb[6 + QUEEN])) |
               (rookAtt(sq, o) & (bb[ROOK] | bb[6 + ROOK] | bb[QUEEN] | bb[6 + QUEEN]));
    }
    inline bool isAttacked(int sq, int by) const {
        return (PawnAtt[by ^ 1][sq] & bb[by * 6 + PAWN]) || (KnightAtt[sq] & bb[by * 6 + KNIGHT]) ||
               (KingAtt[sq] & bb[by * 6 + KING]) ||
               (bishopAtt(sq, all) & (bb[by * 6 + BISHOP] | bb[by * 6 + QUEEN])) ||
               (rookAtt(sq, all) & (bb[by * 6 + ROOK] | bb[by * 6 + QUEEN]));
    }
    inline bool inCheck() const { return isAttacked(lsb(bb[side * 6 + KING]), side ^ 1); }
    inline bool hasNonPawn(int c) const { return occ[c] & ~(bb[c * 6 + PAWN] | bb[c * 6 + KING]); }

    bool isDraw() const {
        if (halfmove >= 100) return true;
        if (!(bb[PAWN] | bb[6 + PAWN] | bb[ROOK] | bb[6 + ROOK] | bb[QUEEN] | bb[6 + QUEEN]) && popcnt(all) <= 3) return true;
        int lim = min(halfmove, sp);
        for (int i = 4; i <= lim; i += 2) if (hist[sp - i].key == key) return true;
        return false;
    }

    static inline void addMove(Move* ml, int& n, int from, int to, int fl) { ml[n++] = (Move)(from | (to << 6) | (fl << 12)); }

    // all=false -> captures and (queen) promotions only
    int gen(Move* ml, bool allMoves) const {
        int n = 0, us = side, them = us ^ 1;
        U64 ours = occ[us], theirs = occ[them], empty = ~all;
        int up = us == WHITE ? 8 : -8;
        U64 promoRank = us == WHITE ? RANK_7 : RANK_2, startRank = us == WHITE ? RANK_2 : RANK_7;
        U64 pawns = bb[us * 6 + PAWN];
        while (pawns) {
            int from = pop_lsb(pawns), to = from + up;
            bool promo = bit(from) & promoRank;
            if (!(all & bit(to))) {
                if (promo) {
                    addMove(ml, n, from, to, PROMO + 3);
                    if (allMoves) { addMove(ml, n, from, to, PROMO + 2); addMove(ml, n, from, to, PROMO + 1); addMove(ml, n, from, to, PROMO); }
                } else if (allMoves) {
                    addMove(ml, n, from, to, QUIET);
                    if ((bit(from) & startRank) && !(all & bit(to + up))) addMove(ml, n, from, to + up, DOUBLE);
                }
            }
            U64 caps = PawnAtt[us][from] & theirs;
            while (caps) {
                int t = pop_lsb(caps);
                if (promo) {
                    addMove(ml, n, from, t, PROMO + 4 + 3);
                    if (allMoves) { addMove(ml, n, from, t, PROMO + 4 + 2); addMove(ml, n, from, t, PROMO + 4 + 1); addMove(ml, n, from, t, PROMO + 4); }
                } else addMove(ml, n, from, t, CAPTURE);
            }
            if (ep >= 0 && (PawnAtt[us][from] & bit(ep))) addMove(ml, n, from, ep, EP);
        }
        for (int pt = KNIGHT; pt <= KING; pt++) {
            U64 b = bb[us * 6 + pt];
            while (b) {
                int from = pop_lsb(b);
                U64 att;
                switch (pt) {
                    case KNIGHT: att = KnightAtt[from]; break;
                    case BISHOP: att = bishopAtt(from, all); break;
                    case ROOK:   att = rookAtt(from, all); break;
                    case QUEEN:  att = bishopAtt(from, all) | rookAtt(from, all); break;
                    default:     att = KingAtt[from]; break;
                }
                att &= ~ours;
                U64 caps = att & theirs;
                while (caps) addMove(ml, n, from, pop_lsb(caps), CAPTURE);
                if (allMoves) { U64 q = att & empty; while (q) addMove(ml, n, from, pop_lsb(q), QUIET); }
            }
        }
        if (allMoves) {
            if (us == WHITE) {
                if ((castling & 1) && !(all & (bit(5) | bit(6))) && !isAttacked(4, BLACK) && !isAttacked(5, BLACK)) addMove(ml, n, 4, 6, KCASTLE);
                if ((castling & 2) && !(all & (bit(1) | bit(2) | bit(3))) && !isAttacked(4, BLACK) && !isAttacked(3, BLACK)) addMove(ml, n, 4, 2, QCASTLE);
            } else {
                if ((castling & 4) && !(all & (bit(61) | bit(62))) && !isAttacked(60, WHITE) && !isAttacked(61, WHITE)) addMove(ml, n, 60, 62, KCASTLE);
                if ((castling & 8) && !(all & (bit(57) | bit(58) | bit(59))) && !isAttacked(60, WHITE) && !isAttacked(59, WHITE)) addMove(ml, n, 60, 58, QCASTLE);
            }
        }
        return n;
    }

    bool makeMove(Move m) {
        int from = m & 63, to = (m >> 6) & 63, fl = m >> 12;
        int pc = board[from], us = side, them = us ^ 1;
        Hist& h = hist[sp];
        h.key = key; h.castling = castling; h.ep = ep; h.halfmove = halfmove; h.captured = NO_PC;
        if (net.loaded) memcpy(accPtr(sp + 1, 0), accPtr(sp, 0), 2 * (size_t)net.H * sizeof(i16));
        sp++;
        if (ep >= 0) { key ^= ZEp[ep & 7]; ep = -1; }
        halfmove++;
        if (pc % 6 == PAWN) halfmove = 0;
        if (fl == EP) {
            int cs = to + (us == WHITE ? -8 : 8);
            h.captured = board[cs]; delP(h.captured, cs); halfmove = 0;
        } else if (fl & 4) {
            h.captured = board[to]; delP(h.captured, to); halfmove = 0;
        }
        delP(pc, from);
        if (fl & 8) addP(us * 6 + (fl & 3) + 1, to); else addP(pc, to);
        if (fl == KCASTLE) {
            int rf = us == WHITE ? 7 : 63, rt = us == WHITE ? 5 : 61, rk = us * 6 + ROOK;
            delP(rk, rf); addP(rk, rt);
        } else if (fl == QCASTLE) {
            int rf = us == WHITE ? 0 : 56, rt = us == WHITE ? 3 : 59, rk = us * 6 + ROOK;
            delP(rk, rf); addP(rk, rt);
        } else if (fl == DOUBLE) {
            int e = from + (us == WHITE ? 8 : -8);
            if (PawnAtt[us][e] & bb[them * 6 + PAWN]) { ep = e; key ^= ZEp[e & 7]; }
        }
        int nc = castling & castleMask[from] & castleMask[to];
        key ^= ZCastle[castling] ^ ZCastle[nc];
        castling = nc;
        side = them; key ^= ZSide;
        if (isAttacked(lsb(bb[us * 6 + KING]), them)) { unmakeMove(m); return false; }
        return true;
    }

    void unmakeMove(Move m) {
        int from = m & 63, to = (m >> 6) & 63, fl = m >> 12;
        sp--;
        const Hist& h = hist[sp];
        side ^= 1;
        int us = side;
        if (fl & 8) { del(board[to], to); put(us * 6 + PAWN, from); }
        else { int pc = board[to]; del(pc, to); put(pc, from); }
        if (fl == EP) put(h.captured, to + (us == WHITE ? -8 : 8));
        else if (fl & 4) put(h.captured, to);
        if (fl == KCASTLE) {
            int rf = us == WHITE ? 7 : 63, rt = us == WHITE ? 5 : 61, rk = us * 6 + ROOK;
            del(rk, rt); put(rk, rf);
        } else if (fl == QCASTLE) {
            int rf = us == WHITE ? 0 : 56, rt = us == WHITE ? 3 : 59, rk = us * 6 + ROOK;
            del(rk, rt); put(rk, rf);
        }
        key = h.key; castling = h.castling; ep = h.ep; halfmove = h.halfmove;
    }

    void makeNull() {
        Hist& h = hist[sp];
        h.key = key; h.castling = castling; h.ep = ep; h.halfmove = halfmove; h.captured = NO_PC;
        if (net.loaded) memcpy(accPtr(sp + 1, 0), accPtr(sp, 0), 2 * (size_t)net.H * sizeof(i16));
        sp++;
        if (ep >= 0) { key ^= ZEp[ep & 7]; ep = -1; }
        halfmove++;
        side ^= 1; key ^= ZSide;
    }
    void unmakeNull() {
        sp--;
        const Hist& h = hist[sp];
        side ^= 1; key = h.key; ep = h.ep; halfmove = h.halfmove; castling = h.castling;
    }

    // Static exchange evaluation: does move m gain at least `threshold`?
    bool see(Move m, int threshold) const {
        static const int V[6] = {100, 320, 330, 500, 900, 20000};
        int from = m & 63, to = (m >> 6) & 63, fl = m >> 12;
        if (fl & 8) return true;
        if (fl == KCASTLE || fl == QCASTLE) return threshold <= 0;
        int victim = fl == EP ? PAWN : (board[to] == NO_PC ? -1 : board[to] % 6);
        int swap = (victim < 0 ? 0 : V[victim]) - threshold;
        if (swap < 0) return false;
        swap = V[board[from] % 6] - swap;
        if (swap <= 0) return true;
        U64 o = all ^ bit(from) ^ bit(to);
        if (fl == EP) o ^= bit(to + (side == WHITE ? -8 : 8));
        int stm = side;
        U64 attackers = attackersTo(to, o);
        U64 bishops = bb[BISHOP] | bb[6 + BISHOP] | bb[QUEEN] | bb[6 + QUEEN];
        U64 rooks = bb[ROOK] | bb[6 + ROOK] | bb[QUEEN] | bb[6 + QUEEN];
        int res = 1;
        for (;;) {
            stm ^= 1;
            attackers &= o;
            U64 sa = attackers & occ[stm];
            if (!sa) break;
            res ^= 1;
            U64 b; int pt;
            if ((b = sa & bb[stm * 6 + PAWN])) pt = PAWN;
            else if ((b = sa & bb[stm * 6 + KNIGHT])) pt = KNIGHT;
            else if ((b = sa & bb[stm * 6 + BISHOP])) pt = BISHOP;
            else if ((b = sa & bb[stm * 6 + ROOK])) pt = ROOK;
            else if ((b = sa & bb[stm * 6 + QUEEN])) pt = QUEEN;
            else return (attackers & ~occ[stm]) ? (res ^ 1) : res;
            swap = V[pt] - swap;
            if (swap < res) break;
            o ^= b & (~b + 1);
            if (pt == PAWN || pt == BISHOP || pt == QUEEN) attackers |= bishopAtt(to, o) & bishops;
            if (pt == ROOK || pt == QUEEN) attackers |= rookAtt(to, o) & rooks;
        }
        return res;
    }
};

string moveStr(Move m) {
    if (!m) return "0000";
    int f = m & 63, t = (m >> 6) & 63, fl = m >> 12;
    string s;
    s += char('a' + f % 8); s += char('1' + f / 8); s += char('a' + t % 8); s += char('1' + t / 8);
    if (fl & 8) s += "nbrq"[fl & 3];
    return s;
}

Move parseMove(Position& p, const string& s) {
    Move l[256];
    int n = p.gen(l, true);
    for (int i = 0; i < n; i++) if (moveStr(l[i]) == s) return l[i];
    return 0;
}

U64 perft(Position& p, int depth) {
    if (depth == 0) return 1;
    Move l[256];
    int n = p.gen(l, true);
    U64 c = 0;
    for (int i = 0; i < n; i++)
        if (p.makeMove(l[i])) { c += depth == 1 ? 1 : perft(p, depth - 1); p.unmakeMove(l[i]); }
    return c;
}

// ----------------------------------------------------------------------------
//  Evaluation
// ----------------------------------------------------------------------------
static const int mg_pawn[64] = {0,0,0,0,0,0,0,0, 98,134,61,95,68,126,34,-11, -6,7,26,31,65,56,25,-20, -14,13,6,21,23,12,17,-23, -27,-2,-5,12,17,6,10,-25, -26,-4,-4,-10,3,3,33,-12, -35,-1,-20,-23,-15,24,38,-22, 0,0,0,0,0,0,0,0};
static const int eg_pawn[64] = {0,0,0,0,0,0,0,0, 178,173,158,134,147,132,165,187, 94,100,85,67,56,53,82,84, 32,24,13,5,-2,4,17,17, 13,9,-3,-7,-7,-8,3,-1, 4,7,-6,1,0,-5,-1,-8, 13,8,8,10,13,0,2,-7, 0,0,0,0,0,0,0,0};
static const int mg_knight[64] = {-167,-89,-34,-49,61,-97,-15,-107, -73,-41,72,36,23,62,7,-17, -47,60,37,65,84,129,73,44, -9,17,19,53,37,69,18,22, -13,4,16,13,28,19,21,-8, -23,-9,12,10,19,17,25,-16, -29,-53,-12,-3,-1,18,-14,-19, -105,-21,-58,-33,-17,-28,-19,-23};
static const int eg_knight[64] = {-58,-38,-13,-28,-31,-27,-63,-99, -25,-8,-25,-2,-9,-25,-24,-52, -24,-20,10,9,-1,-9,-19,-41, -17,3,22,22,22,11,8,-18, -18,-6,16,25,16,17,4,-18, -23,-3,-1,15,10,-3,-20,-22, -42,-20,-10,-5,-2,-20,-23,-44, -29,-51,-23,-15,-22,-18,-50,-64};
static const int mg_bishop[64] = {-29,4,-82,-37,-25,-42,7,-8, -26,16,-18,-13,30,59,18,-47, -16,37,43,40,35,50,37,-2, -4,5,19,50,37,37,7,-2, -6,13,13,26,34,12,10,4, 0,15,15,15,14,27,18,10, 4,15,16,0,7,21,33,1, -33,-3,-14,-21,-13,-12,-39,-21};
static const int eg_bishop[64] = {-14,-21,-11,-8,-7,-9,-17,-24, -8,-4,7,-12,-3,-13,-4,-14, 2,-8,0,-1,-2,6,0,4, -3,9,12,9,14,10,3,2, -6,3,13,19,7,10,-3,-9, -12,-3,8,10,13,3,-7,-15, -14,-18,-7,-1,4,-9,-15,-27, -23,-9,-23,-5,-9,-16,-5,-17};
static const int mg_rook[64] = {32,42,32,51,63,9,31,43, 27,32,58,62,80,67,26,44, -5,19,26,36,17,45,61,16, -24,-11,7,26,24,35,-8,-20, -36,-26,-12,-1,9,-7,6,-23, -45,-25,-16,-17,3,0,-5,-33, -44,-16,-20,-9,-1,11,-6,-71, -19,-13,1,17,16,7,-37,-26};
static const int eg_rook[64] = {13,10,18,15,12,12,8,5, 11,13,13,11,-3,3,8,3, 7,7,7,5,4,-3,-5,-3, 4,3,13,1,2,1,-1,2, 3,5,8,4,-5,-6,-8,-11, -4,0,-5,-1,-7,-12,-8,-16, -6,-6,0,2,-9,-9,-11,-3, -9,2,3,-1,-5,-13,4,-20};
static const int mg_queen[64] = {-28,0,29,12,59,44,43,45, -24,-39,-5,1,-16,57,28,54, -13,-17,7,8,29,56,47,57, -27,-27,-16,-16,-1,17,-2,1, -9,-26,-9,-10,-2,-4,3,-3, -14,2,-11,-2,-5,2,14,5, -35,-8,11,2,8,15,-3,1, -1,-18,-9,10,-15,-25,-31,-50};
static const int eg_queen[64] = {-9,22,22,27,27,19,10,20, -17,20,32,41,58,25,30,0, -20,6,9,49,47,35,19,9, 3,22,24,45,57,40,57,36, -18,28,19,47,31,34,39,23, -16,-27,15,6,9,17,10,5, -22,-23,-30,-16,-16,-23,-36,-32, -33,-28,-22,-43,-5,-32,-20,-41};
static const int mg_king[64] = {-65,23,16,-15,-56,-34,2,13, 29,-1,-20,-7,-8,-4,-38,-29, -9,24,2,-16,-20,6,22,-22, -17,-20,-12,-27,-30,-25,-14,-36, -49,-1,-27,-39,-46,-44,-33,-51, -14,-14,-22,-46,-44,-30,-15,-27, 1,7,-8,-64,-43,-16,9,8, -15,36,12,-54,8,-28,24,14};
static const int eg_king[64] = {-74,-35,-18,-18,-11,15,4,-17, -12,17,14,17,17,38,23,11, 10,17,23,15,20,45,44,13, -8,22,24,27,26,33,26,3, -18,-4,21,24,27,23,9,-11, -19,-3,11,21,23,16,7,-9, -27,-11,4,13,14,4,-5,-17, -53,-34,-21,-11,-28,-14,-24,-43};

int MGT[6][64], EGT[6][64];
U64 FileMask[8], AdjFiles[8], PassedMask[2][64], ShieldMask[2][64];
const int phaseInc[6] = {0, 1, 1, 2, 4, 0};

void initEval() {
    static const int mgVal[6] = {82, 337, 365, 477, 1025, 0}, egVal[6] = {94, 281, 297, 512, 936, 0};
    const int* mgt[6] = {mg_pawn, mg_knight, mg_bishop, mg_rook, mg_queen, mg_king};
    const int* egt[6] = {eg_pawn, eg_knight, eg_bishop, eg_rook, eg_queen, eg_king};
    for (int t = 0; t < 6; t++)
        for (int s = 0; s < 64; s++) { MGT[t][s] = mgVal[t] + mgt[t][s ^ 56]; EGT[t][s] = egVal[t] + egt[t][s ^ 56]; }
    for (int f = 0; f < 8; f++) {
        FileMask[f] = FILE_A << f;
        AdjFiles[f] = (f > 0 ? FILE_A << (f - 1) : 0) | (f < 7 ? FILE_A << (f + 1) : 0);
    }
    for (int sq = 0; sq < 64; sq++) {
        int r = sq / 8, f = sq % 8;
        PassedMask[0][sq] = PassedMask[1][sq] = ShieldMask[0][sq] = ShieldMask[1][sq] = 0;
        for (int ff = max(0, f - 1); ff <= min(7, f + 1); ff++) {
            for (int rr = r + 1; rr < 8; rr++) PassedMask[WHITE][sq] |= bit(rr * 8 + ff);
            for (int rr = r - 1; rr >= 0; rr--) PassedMask[BLACK][sq] |= bit(rr * 8 + ff);
            for (int rr = r + 1; rr <= min(7, r + 2); rr++) ShieldMask[WHITE][sq] |= bit(rr * 8 + ff);
            for (int rr = r - 1; rr >= max(0, r - 2); rr--) ShieldMask[BLACK][sq] |= bit(rr * 8 + ff);
        }
    }
}

int evalHCE(const Position& p) {
    static const int passMg[8] = {0, 2, 5, 12, 30, 60, 100, 0}, passEg[8] = {0, 8, 16, 35, 65, 110, 170, 0};
    int mg = 0, eg = 0, ph = 0;
    for (int pc = 0; pc < 12; pc++) {
        U64 b = p.bb[pc];
        int t = pc % 6, c = pc / 6;
        while (b) {
            int s = pop_lsb(b);
            if (c == 0) { mg += MGT[t][s]; eg += EGT[t][s]; }
            else { mg -= MGT[t][s ^ 56]; eg -= EGT[t][s ^ 56]; }
            ph += phaseInc[t];
        }
    }
    if (popcnt(p.bb[BISHOP]) >= 2) { mg += 30; eg += 45; }
    if (popcnt(p.bb[6 + BISHOP]) >= 2) { mg -= 30; eg -= 45; }

    U64 occ = p.all, wp = p.bb[PAWN], bp = p.bb[6 + PAWN];
    U64 pawnAtt[2];
    pawnAtt[0] = ((wp & ~FILE_A) << 7) | ((wp & ~FILE_H) << 9);
    pawnAtt[1] = ((bp & ~FILE_A) >> 9) | ((bp & ~FILE_H) >> 7);
    for (int c = 0; c < 2; c++) {
        int them = c ^ 1, sign = c == 0 ? 1 : -1;
        int cmg = 0, ceg = 0, danger = 0, attackers = 0;
        int kThem = lsb(p.bb[them * 6 + KING]);
        U64 zone = KingAtt[kThem] | bit(kThem);
        U64 safe = ~p.occ[c] & ~pawnAtt[them];
        U64 b = p.bb[c * 6 + KNIGHT];
        while (b) {
            int s = pop_lsb(b); U64 a = KnightAtt[s];
            int mob = popcnt(a & safe); cmg += (mob - 4) * 4; ceg += (mob - 4) * 4;
            if (U64 z = a & zone) { attackers++; danger += 2 * popcnt(z); }
        }
        b = p.bb[c * 6 + BISHOP];
        while (b) {
            int s = pop_lsb(b); U64 a = bishopAtt(s, occ);
            int mob = popcnt(a & safe); cmg += (mob - 6) * 4; ceg += (mob - 6) * 5;
            if (U64 z = a & zone) { attackers++; danger += 2 * popcnt(z); }
        }
        b = p.bb[c * 6 + ROOK];
        while (b) {
            int s = pop_lsb(b); U64 a = rookAtt(s, occ); int f = s & 7;
            int mob = popcnt(a & safe); cmg += (mob - 6) * 2; ceg += (mob - 6) * 4;
            if (!(p.bb[c * 6 + PAWN] & FileMask[f])) {
                if (!(p.bb[them * 6 + PAWN] & FileMask[f])) { cmg += 22; ceg += 10; }
                else { cmg += 10; ceg += 8; }
            }
            if (U64 z = a & zone) { attackers++; danger += 3 * popcnt(z); }
        }
        b = p.bb[c * 6 + QUEEN];
        while (b) {
            int s = pop_lsb(b); U64 a = bishopAtt(s, occ) | rookAtt(s, occ);
            int mob = popcnt(a & safe); cmg += (mob - 13); ceg += (mob - 13) * 2;
            if (U64 z = a & zone) { attackers++; danger += 5 * popcnt(z); }
        }
        // pawns
        U64 pawns = p.bb[c * 6 + PAWN], ep = p.bb[them * 6 + PAWN];
        b = pawns;
        while (b) {
            int s = pop_lsb(b), f = s & 7, rr = c == 0 ? s / 8 : 7 - s / 8;
            if (!(pawns & AdjFiles[f])) { cmg -= 10; ceg -= 15; }
            if (!(ep & PassedMask[c][s]) && !(pawns & FileMask[f] & PassedMask[c][s])) {
                int bm = passMg[rr], be = passEg[rr];
                if (p.all & bit(s + (c == 0 ? 8 : -8))) { bm /= 2; be /= 2; }
                cmg += bm; ceg += be;
            }
            if (pawnAtt[c] & bit(s)) { cmg += 5; ceg += 4; }
        }
        for (int f = 0; f < 8; f++) {
            int n = popcnt(pawns & FileMask[f]);
            if (n > 1) { cmg -= 8 * (n - 1); ceg -= 16 * (n - 1); }
        }
        // attack on the enemy king (middlegame only)
        if (attackers >= 2) cmg += min(danger * danger / 10, 220);
        // own king pawn shield
        int ks = lsb(p.bb[c * 6 + KING]);
        int shield = popcnt(pawns & ShieldMask[c][ks]);
        cmg += shield * 9 - 15;
        mg += sign * cmg; eg += sign * ceg;
    }
    if (ph > 24) ph = 24;
    int e = (mg * ph + eg * (24 - ph)) / 24;
    return (p.side == WHITE ? e : -e) + 10;
}

int evalNN(const Position& p) {
    int H = net.H, QA = net.QA;
    const i16* us = p.accPtr(p.sp, p.side);
    const i16* them = p.accPtr(p.sp, p.side ^ 1);
    const i16* w = net.W2.data();
    if (net.screlu) {                                   // squared clipped ReLU nets
        int64_t s2 = 0;
        for (int i = 0; i < H; i++) {
            int x = us[i]; x = x < 0 ? 0 : (x > QA ? QA : x);
            int y = them[i]; y = y < 0 ? 0 : (y > QA ? QA : y);
            s2 += (int64_t)x * x * w[i] + (int64_t)y * y * w[H + i];
        }
        s2 /= QA;
        return (int)(((s2 + net.b2) * net.SCALE) / ((int64_t)net.QA * net.QB));
    }
    i32 sum = 0;
    for (int i = 0; i < H; i++) {
        int x = us[i]; x = x < 0 ? 0 : (x > QA ? QA : x);
        int y = them[i]; y = y < 0 ? 0 : (y > QA ? QA : y);
        sum += x * w[i] + y * w[H + i];
    }
    return (int)(((int64_t)(sum + net.b2) * net.SCALE) / (net.QA * net.QB));
}

int evaluate(const Position& p) {
    int e = net.loaded ? (net.residual ? evalHCE(p) + evalNN(p) : evalNN(p)) : evalHCE(p);
    e = e * (200 - p.halfmove) / 200;
    return max(-MATE_BOUND + 1, min(MATE_BOUND - 1, e));
}

// ----------------------------------------------------------------------------
//  Transposition table (lock-less: key is stored xor data)
// ----------------------------------------------------------------------------
struct TTEntry { atomic<U64> k, d; };
struct TransTable {
    TTEntry* table = nullptr;
    size_t mask = 0;
    unsigned age = 0;
    void resize(size_t mb) {
        if (table) free(table);
        size_t n = 1;
        while (n * 2 * sizeof(TTEntry) <= mb * 1024 * 1024) n *= 2;
        table = (TTEntry*)calloc(n, sizeof(TTEntry));
        mask = n - 1;
    }
    void clear() { memset((void*)table, 0, (mask + 1) * sizeof(TTEntry)); age = 0; }
    void newSearch() { age = (age + 1) & 63; }
    bool probe(U64 key, Move& mv, int& score, int& eval, int& depth, int& flag) const {
        TTEntry& e = table[key & mask];
        U64 d = e.d.load(memory_order_relaxed), k = e.k.load(memory_order_relaxed);
        if ((k ^ d) != key) return false;
        mv = (Move)(d & 0xFFFF);
        score = (i16)((d >> 16) & 0xFFFF);
        eval = (i16)((d >> 32) & 0xFFFF);
        depth = (int)((d >> 48) & 255);
        flag = (int)((d >> 56) & 3);
        return true;
    }
    void store(U64 key, Move mv, int score, int eval, int depth, int flag) {
        TTEntry& e = table[key & mask];
        U64 od = e.d.load(memory_order_relaxed), ok = e.k.load(memory_order_relaxed);
        if ((ok ^ od) == key) {
            if (!mv) mv = (Move)(od & 0xFFFF);
            int odepth = (int)((od >> 48) & 255);
            unsigned oage = (unsigned)((od >> 58) & 63);
            if (flag != BOUND_EXACT && depth < odepth - 3 && oage == age) return;
        }
        U64 d = (U64)mv | ((U64)(uint16_t)(i16)score << 16) | ((U64)(uint16_t)(i16)eval << 32) |
                ((U64)(depth & 255) << 48) | ((U64)flag << 56) | ((U64)age << 58);
        e.d.store(d, memory_order_relaxed);
        e.k.store(key ^ d, memory_order_relaxed);
    }
} tt;

inline int toTT(int s, int ply) { return s >= MATE_BOUND ? s + ply : (s <= -MATE_BOUND ? s - ply : s); }
inline int fromTT(int s, int ply) { return s >= MATE_BOUND ? s - ply : (s <= -MATE_BOUND ? s + ply : s); }

// ----------------------------------------------------------------------------
//  Search
// ----------------------------------------------------------------------------
atomic<bool> gStop(false);
chrono::steady_clock::time_point gStart;
inline double elapsedMs() { return chrono::duration<double, milli>(chrono::steady_clock::now() - gStart).count(); }

// ---- search options (UCI): Book, BookFile, Verify, CorrHist, ProbCut ----
// ProbCut and CorrHist are OFF by default: in my 100 ms self-play tests (30 games each) neither showed a gain. Test them yourself with sara_match.
bool optBook = true, optVerify = true, optCorr = false, optProbCut = false;
string bookFile = "../book/openings.txt";
atomic<int> gVerChecks(0), gVerFlag(0), gVerSwitch(0), gBookHits(0);

struct Limits { int depth = 99; U64 nodes = 0; double soft = 0, hard = 0; bool infinite = false; vector<Move> searchMoves; double hardMax = 0, extend = 1.0; };

int lmrTab[64][64];
void initLMR() {
    for (int d = 1; d < 64; d++) for (int m = 1; m < 64; m++) lmrTab[d][m] = (int)(0.75 + log((double)d) * log((double)m) / 2.25);
}

vector<class Searcher*> gSearchers;

class Searcher {
public:
    int id = 0;
    bool isMain = false, verbose = true;
    Position pos;
    U64 nodes = 0;
    int seldepth = 0, rootDepth = 0;
    Limits lim;
    i16 hist[2][64][64];
    i16 cont[12][64][12][64];
    Move counter[12][64];
    Move killers[MAXPLY + 4][2];
    int sEval[MAXPLY + 4], sPiece[MAXPLY + 4], sTo[MAXPLY + 4];
    Move pvT[MAXPLY + 4][MAXPLY + 4];
    int pvLen[MAXPLY + 4];
    bool safe = false;                 // "safe" mode: plain alpha-beta with no pruning/reductions/TT cutoffs (used to verify a move)
    int corr[2][16384];                // correction history (by pawn structure)
    Move bestMove = 0;
    vector<Move> bestPv;
    int rootScore = 0, completedDepth = 0;
    bool rootChanged = false, rootFailLow = false;   // difficulty signals used to extend the time budget

    Searcher() { clearHistory(); }
    void clearHistory() {
        memset(hist, 0, sizeof hist); memset(cont, 0, sizeof cont);
        memset(counter, 0, sizeof counter); memset(killers, 0, sizeof killers); memset(corr, 0, sizeof corr);
    }
    void checkTime() {
        double h = lim.hard;
        if (lim.hardMax > h && (rootChanged || rootFailLow)) h = lim.hardMax;   // unstable root: allow extra time
        if (h > 0 && elapsedMs() >= h) gStop = true;
        if (lim.nodes && nodes >= lim.nodes) gStop = true;
    }
    inline int corrIdx() const {
        U64 h = (pos.bb[0] * 0x9E3779B97F4A7C15ULL) ^ (pos.bb[6] * 0xC2B2AE3D27D4EB4FULL);
        h ^= h >> 29; return (int)(h & 16383);
    }
    static inline void updHist(i16& h, int bonus) {
        int v = h; v += bonus - v * abs(bonus) / 16384; h = (i16)v;
    }

    int qsearch(int alpha, int beta, int ply) {
        nodes++;
        if (isMain && (nodes & 1023) == 0) checkTime();
        if (gStop.load(memory_order_relaxed)) return 0;
        if (ply > seldepth) seldepth = ply;
        if (pos.isDraw()) return 0;
        if (ply >= MAXPLY - 2) return evaluate(pos);
        bool pv = beta - alpha > 1, inCheck = pos.inCheck();
        Move ttMove = 0; int ttScore = 0, ttEval = NO_EVAL, ttDepth = 0, ttFlag = 0;
        bool hit = tt.probe(pos.key, ttMove, ttScore, ttEval, ttDepth, ttFlag);
        if (hit) {
            ttScore = fromTT(ttScore, ply);
            if (!pv && !safe && (((ttFlag & 1) && ttScore >= beta) || ((ttFlag & 2) && ttScore <= alpha))) return ttScore;
        }
        int best, stand = NO_EVAL;
        if (inCheck) best = -MATE + ply;
        else {
            stand = (hit && ttEval != NO_EVAL) ? ttEval : evaluate(pos);
            best = stand;
            if (best >= beta) return best;
            if (best > alpha) alpha = best;
        }
        Move list[256]; int sc[256];
        int n = pos.gen(list, inCheck);
        static const int V[6] = {100, 320, 330, 500, 900, 20000};
        for (int i = 0; i < n; i++) {
            Move m = list[i]; int fl = m >> 12;
            int victim = fl == EP ? PAWN : (pos.board[(m >> 6) & 63] == NO_PC ? -1 : pos.board[(m >> 6) & 63] % 6);
            sc[i] = (victim < 0 ? 0 : V[victim] * 16) - pos.board[m & 63] % 6 + ((fl & 8) ? 8000 : 0);
            if (m == ttMove) sc[i] += 1000000;
        }
        int origAlpha = alpha, moveCount = 0; Move bestM = 0;
        for (int i = 0; i < n; i++) {
            int bi = i;
            for (int j = i + 1; j < n; j++) if (sc[j] > sc[bi]) bi = j;
            swap(list[i], list[bi]); swap(sc[i], sc[bi]);
            Move m = list[i]; int fl = m >> 12;
            if (!inCheck && best > -MATE_BOUND) {
                if (!(fl & 8)) {
                    int victim = fl == EP ? PAWN : pos.board[(m >> 6) & 63] % 6;
                    if (stand + V[victim] + 150 < alpha) continue;
                }
                if (!pos.see(m, 0)) continue;
            }
            if (!pos.makeMove(m)) continue;
            moveCount++;
            int v = -qsearch(-beta, -alpha, ply + 1);
            pos.unmakeMove(m);
            if (gStop.load(memory_order_relaxed)) return 0;
            if (v > best) { best = v; bestM = m; if (v > alpha) { alpha = v; if (v >= beta) break; } }
        }
        if (inCheck && moveCount == 0) return -MATE + ply;
        int flag = best >= beta ? BOUND_LOWER : (best > origAlpha ? BOUND_EXACT : BOUND_UPPER);
        if (!safe) tt.store(pos.key, bestM, toTT(best, ply), stand, 0, flag);
        return best;
    }

    int search(int alpha, int beta, int depth, int ply, bool cutNode, Move excluded = 0) {
        pvLen[ply] = ply;
        bool pv = beta - alpha > 1, root = ply == 0;
        bool inCheck = pos.inCheck();
        if (inCheck && ply < 2 * rootDepth + 4 && ply < MAXPLY - 8) depth++;
        if (depth <= 0) return qsearch(alpha, beta, ply);
        nodes++;
        if (isMain && (nodes & 1023) == 0) checkTime();
        if (gStop.load(memory_order_relaxed)) return 0;
        if (ply > seldepth) seldepth = ply;
        int us = pos.side;
        if (!root) {
            if (pos.isDraw()) return 0;
            if (ply >= MAXPLY - 2) return inCheck ? 0 : evaluate(pos);
            alpha = max(alpha, -MATE + ply); beta = min(beta, MATE - ply - 1);
            if (alpha >= beta) return alpha;
        }
        Move ttMove = 0; int ttScore = 0, ttEval = NO_EVAL, ttDepth = 0, ttFlag = 0;
        bool ttHit = tt.probe(pos.key, ttMove, ttScore, ttEval, ttDepth, ttFlag);
        if (ttHit) ttScore = fromTT(ttScore, ply);
        if (!pv && ttHit && !excluded && !safe && ttDepth >= depth &&
            (((ttFlag & 1) && ttScore >= beta) || ((ttFlag & 2) && ttScore <= alpha)))
            return ttScore;

        int staticE, eval, rawE = NO_EVAL;
        if (inCheck) { staticE = eval = NO_EVAL; }
        else {
            rawE = (ttHit && ttEval != NO_EVAL) ? ttEval : evaluate(pos);
            staticE = rawE;
            if (optCorr && !safe) staticE += corr[us][corrIdx()] / 256;     // learned correction of the evaluation
            eval = staticE;
            if (ttHit && abs(ttScore) < MATE_BOUND &&
                (ttFlag == BOUND_EXACT || ((ttFlag & 1) && ttScore > eval) || ((ttFlag & 2) && ttScore < eval)))
                eval = ttScore;
        }
        sEval[ply] = staticE;
        bool improving = !inCheck && ply >= 2 && sEval[ply - 2] != NO_EVAL && staticE > sEval[ply - 2];

        if (!pv && !inCheck && !excluded && !safe) {
            // razoring
            if (depth <= 3 && eval + 250 * depth <= alpha && abs(alpha) < MATE_BOUND) {
                int v = qsearch(alpha, beta, ply);
                if (v <= alpha) return v;
            }
            // reverse futility
            if (depth <= 8 && eval - 75 * depth + (improving ? 0 : 40) >= beta && abs(eval) < MATE_BOUND && abs(beta) < MATE_BOUND)
                return eval;
            // null move
            if (depth >= 3 && eval >= beta && staticE >= beta - 20 * depth + 150 && ply >= 1 &&
                sPiece[ply - 1] != NO_PC && pos.hasNonPawn(us) && abs(beta) < MATE_BOUND) {
                int R = 3 + depth / 3 + min((eval - beta) / 200, 3);
                sPiece[ply] = NO_PC; sTo[ply] = 0;
                pos.makeNull();
                int v = -search(-beta, -beta + 1, depth - R, ply + 1, !cutNode);
                pos.unmakeNull();
                if (gStop.load(memory_order_relaxed)) return 0;
                if (v >= beta) return v >= MATE_BOUND ? beta : v;
            }
            // ProbCut: a good capture that still beats beta by a margin at reduced depth almost surely refutes this node
            if (optProbCut && depth >= 5 && abs(beta) < MATE_BOUND) {
                int pcBeta = beta + 190 - (improving ? 40 : 0);
                if (!(ttHit && ttDepth >= depth - 3 && (ttFlag & 2) && ttScore < pcBeta)) {
                    Move pl[256]; int pn = pos.gen(pl, true);
                    for (int i = 0; i < pn; i++) {
                        Move pm = pl[i];
                        if (!((pm >> 12) & 4)) continue;
                        if (!pos.see(pm, pcBeta - staticE)) continue;
                        int movedPc2 = pos.board[pm & 63];
                        if (!pos.makeMove(pm)) continue;
                        sPiece[ply] = movedPc2; sTo[ply] = (pm >> 6) & 63;
                        int pv2 = -qsearch(-pcBeta, -pcBeta + 1, ply + 1);
                        if (pv2 >= pcBeta) pv2 = -search(-pcBeta, -pcBeta + 1, depth - 4, ply + 1, !cutNode);
                        pos.unmakeMove(pm);
                        if (gStop.load(memory_order_relaxed)) return 0;
                        if (pv2 >= pcBeta) { tt.store(pos.key, pm, toTT(pv2, ply), rawE, depth - 3, BOUND_LOWER); return pv2; }
                    }
                }
            }
        }
        if (depth >= 4 && !ttMove && (pv || cutNode) && !safe) depth--;   // internal iterative reduction

        // ---- move generation and scoring
        Move list[256]; int sc[256];
        int n = pos.gen(list, true);
        i16 (*cb)[64] = (ply >= 1 && sPiece[ply - 1] != NO_PC) ? cont[sPiece[ply - 1]][sTo[ply - 1]] : nullptr;
        static const int V[6] = {100, 320, 330, 500, 900, 20000};
        for (int i = 0; i < n; i++) {
            Move m = list[i]; int fl = m >> 12, from = m & 63, to = (m >> 6) & 63;
            if (m == ttMove) { sc[i] = 1000000000; continue; }
            if (fl & 8) { sc[i] = ((fl & 3) == 3 ? 7000000 : -5000000) + (fl & 4 ? 1000 : 0); continue; }
            if (fl & 4) {
                int victim = fl == EP ? PAWN : pos.board[to] % 6;
                int s = V[victim] * 10 - pos.board[from] % 6;
                sc[i] = pos.see(m, 0) ? 8000000 + s : -4000000 + s;
                continue;
            }
            if (m == killers[ply][0]) sc[i] = 6000000;
            else if (m == killers[ply][1]) sc[i] = 5900000;
            else if (ply >= 1 && sPiece[ply - 1] != NO_PC && m == counter[sPiece[ply - 1]][sTo[ply - 1]]) sc[i] = 5800000;
            else {
                int pc = pos.board[from];
                sc[i] = hist[us][from][to] + (cb ? cb[pc][to] : 0);
            }
        }

        int bestScore = -INF, moveCount = 0, quietCount = 0, origAlpha = alpha;
        Move bestM = 0;
        Move quietsTried[64]; int quietPc[64]; int nq = 0;
        bool skipQuiets = false;

        for (int i = 0; i < n; i++) {
            int bi = i;
            for (int j = i + 1; j < n; j++) if (sc[j] > sc[bi]) bi = j;
            swap(list[i], list[bi]); swap(sc[i], sc[bi]);
            Move m = list[i];
            if (m == excluded) continue;
            if (root && !lim.searchMoves.empty() && find(lim.searchMoves.begin(), lim.searchMoves.end(), m) == lim.searchMoves.end()) continue;
            int fl = m >> 12, from = m & 63, to = (m >> 6) & 63;
            bool capture = fl & 4, promo = fl & 8, quiet = !capture && !promo;
            int movedPc = pos.board[from];

            if (!safe && !root && bestScore > -MATE_BOUND && pos.hasNonPawn(us)) {
                if (quiet) {
                    if (skipQuiets) continue;
                    if (depth <= 8 && quietCount >= (3 + depth * depth) / (improving ? 1 : 2)) { skipQuiets = true; continue; }
                    if (!inCheck && depth <= 6 && eval + 90 + 100 * depth <= alpha) { skipQuiets = true; continue; }
                }
                if (moveCount > 0 && depth <= 8 && !pos.see(m, quiet ? -60 * depth : -100 * depth)) continue;
            }

            int ext = 0;
            if (!safe && !root && depth >= 8 && m == ttMove && !excluded && ttHit && abs(ttScore) < MATE_BOUND &&
                (ttFlag & 1) && ttDepth >= depth - 3) {
                int sBeta = ttScore - 2 * depth;
                int v = search(sBeta - 1, sBeta, (depth - 1) / 2, ply, cutNode, m);
                if (gStop.load(memory_order_relaxed)) return 0;
                if (v < sBeta) ext = 1;
                else if (sBeta >= beta) return sBeta;
                else if (ttScore >= beta) ext = -1;
            }

            if (!pos.makeMove(m)) continue;
            moveCount++;
            if (quiet) { quietCount++; if (nq < 64) { quietsTried[nq] = m; quietPc[nq] = movedPc; nq++; } }
            sPiece[ply] = movedPc; sTo[ply] = to;
            bool givesCheck = pos.inCheck();
            int newDepth = depth - 1 + ext;
            int v;
            if (moveCount == 1) {
                v = -search(-beta, -alpha, newDepth, ply + 1, pv ? false : !cutNode);
            } else {
                int r = 0;
                if (depth >= 3 && !safe) {
                    r = lmrTab[min(depth, 63)][min(moveCount, 63)];
                    if (!pv) r++;
                    if (cutNode) r++;
                    if (improving) r--;
                    if (inCheck || givesCheck) r--;
                    if (quiet) {
                        int hs = hist[us][from][to] + (cb ? cb[movedPc][to] : 0);
                        r -= hs / 8192;
                    } else r--;
                    r = max(0, min(r, newDepth - 1));
                }
                v = -search(-alpha - 1, -alpha, newDepth - r, ply + 1, true);
                if (v > alpha && r > 0) v = -search(-alpha - 1, -alpha, newDepth, ply + 1, !cutNode);
                if (pv && v > alpha && v < beta) v = -search(-beta, -alpha, newDepth, ply + 1, false);
            }
            pos.unmakeMove(m);
            if (gStop.load(memory_order_relaxed)) return 0;

            if (v > bestScore) {
                bestScore = v; bestM = m;
                if (v > alpha) {
                    alpha = v;
                    if (root && moveCount > 1) rootChanged = true;
                    pvT[ply][ply] = m;
                    for (int k = ply + 1; k < pvLen[ply + 1]; k++) pvT[ply][k] = pvT[ply + 1][k];
                    pvLen[ply] = max(pvLen[ply + 1], ply + 1);
                    if (v >= beta) break;
                }
            }
        }

        if (moveCount == 0) return excluded ? alpha : (inCheck ? -MATE + ply : 0);

        if (bestScore >= beta && !(bestM >> 12 & 12)) {
            int bonus = min(300 * depth - 250, 2000);
            int bf = bestM & 63, bt = (bestM >> 6) & 63, bpc = pos.board[bf];
            if (killers[ply][0] != bestM) { killers[ply][1] = killers[ply][0]; killers[ply][0] = bestM; }
            if (ply >= 1 && sPiece[ply - 1] != NO_PC) counter[sPiece[ply - 1]][sTo[ply - 1]] = bestM;
            updHist(hist[us][bf][bt], bonus);
            if (cb) updHist(cb[bpc][bt], bonus);
            for (int k = 0; k < nq; k++) {
                if (quietsTried[k] == bestM) continue;
                int f2 = quietsTried[k] & 63, t2 = (quietsTried[k] >> 6) & 63;
                updHist(hist[us][f2][t2], -bonus);
                if (cb) updHist(cb[quietPc[k]][t2], -bonus);
            }
        }
        if (!excluded && !safe) {
            int flag = bestScore >= beta ? BOUND_LOWER : (bestScore > origAlpha ? BOUND_EXACT : BOUND_UPPER);
            tt.store(pos.key, bestM, toTT(bestScore, ply), rawE, depth, flag);
            if (optCorr && !inCheck && rawE != NO_EVAL && abs(bestScore) < MATE_BOUND && (!bestM || !((bestM >> 12) & 12)) &&
                !((flag == BOUND_LOWER && bestScore <= staticE) || (flag == BOUND_UPPER && bestScore >= staticE))) {
                int diff = max(-200, min(200, bestScore - rawE)) * 256;
                int& c = corr[us][corrIdx()];
                c += (diff - c) * min(depth + 1, 16) / 256;
                c = max(-100 * 256, min(100 * 256, c));
            }
        }
        return bestScore;
    }

    string scoreStr(int s) {
        if (abs(s) >= MATE_BOUND) {
            int m = s > 0 ? (MATE - s + 1) / 2 : -(MATE + s) / 2;
            return "mate " + to_string(m);
        }
        return "cp " + to_string(s);
    }

    void think() {
        nodes = 0; seldepth = 0; completedDepth = 0; bestMove = 0; bestPv.clear(); rootScore = 0;
        Move l[256];
        int n = pos.gen(l, true);
        for (int i = 0; i < n && !bestMove; i++) {
            if (!lim.searchMoves.empty() && find(lim.searchMoves.begin(), lim.searchMoves.end(), l[i]) == lim.searchMoves.end()) continue;
            if (pos.makeMove(l[i])) { pos.unmakeMove(l[i]); bestMove = l[i]; }
        }
        if (!bestMove) { lim.searchMoves.clear(); for (int i = 0; i < n && !bestMove; i++) if (pos.makeMove(l[i])) { pos.unmakeMove(l[i]); bestMove = l[i]; } }
        if (!bestMove) return;
        if (isMain && lim.soft > 0 && !lim.infinite && lim.depth >= 99 && lim.searchMoves.empty()) {     // forced move: no need to think
            int legalCnt = 0;
            for (int i = 0; i < n && legalCnt < 2; i++) if (pos.makeMove(l[i])) { pos.unmakeMove(l[i]); legalCnt++; }
            if (legalCnt == 1) return;
        }
        for (int i = 0; i < MAXPLY + 4; i++) sEval[i] = NO_EVAL, sPiece[i] = NO_PC, sTo[i] = 0;
        int score = 0, prevScore = 0; Move prevBest = 0; int stable = 0;
        for (int depth = 1; depth <= lim.depth && depth < MAXPLY - 10; depth++) {
            rootDepth = depth;
            int sd = depth;
            if (id > 0 && (depth + id) % 3 == 0) sd++;
            int delta = 14, alpha = -INF, beta = INF; bool failLow = false;
            if (depth >= 5) { delta = 14 + abs(score) / 64; alpha = max(score - delta, -INF); beta = min(score + delta, INF); }
            int v;
            rootChanged = rootFailLow = false;
            for (;;) {
                v = search(alpha, beta, sd, 0, false);
                if (gStop.load()) break;
                if (v <= alpha) { failLow = true; rootFailLow = true; beta = (alpha + beta) / 2; alpha = max(v - delta, -INF); }
                else if (v >= beta) beta = min(v + delta, INF);
                else break;
                delta += delta / 2 + 5;
                if (delta > 800) { alpha = -INF; beta = INF; }
            }
            if (gStop.load() && depth > 1) break;
            score = v; rootScore = v; completedDepth = depth;
            if (pvLen[0] > 0 && pvT[0][0]) {
                bestMove = pvT[0][0];
                bestPv.assign(pvT[0], pvT[0] + pvLen[0]);
            }
            if (isMain && verbose) {
                U64 total = 0;
                for (auto* s : gSearchers) total += s->nodes;
                double el = max(1.0, elapsedMs());
                ostringstream o;
                o << "info depth " << depth << " seldepth " << seldepth << " multipv 1 score " << scoreStr(score)
                  << " nodes " << total << " nps " << (U64)(total * 1000.0 / el) << " hashfull " << 0
                  << " time " << (long long)el << " pv";
                for (Move m : bestPv) o << ' ' << moveStr(m);
                cout << o.str() << endl;
            }
            if (gStop.load()) break;
            if (isMain && lim.soft > 0) {
                bool changed = bestMove != prevBest;
                if (changed) stable = 0; else stable++;
                // difficult position: the best move changed, the score fell, or the window failed low
                bool hardPos = depth >= 6 && (changed || failLow || score < prevScore - 25);
                double scale = stable >= 7 ? 0.6 : (stable >= 4 ? 0.75 : (stable >= 2 ? 0.9 : 1.15));
                if (hardPos && lim.extend > 1.0) { scale *= lim.extend; if (lim.hardMax > lim.hard) lim.hard = lim.hardMax; }
                double thr = min(lim.soft * 0.62 * scale, lim.hard * 0.8);
                if (elapsedMs() > thr) break;
            }
            prevScore = score; prevBest = bestMove;
            if (isMain && !lim.infinite && abs(score) >= MATE_BOUND && depth >= (score > 0 ? MATE - score : MATE + score) + 2) break;
            if (isMain && lim.nodes && nodes >= lim.nodes) break;
        }
    }
};

// ----------------------------------------------------------------------------
//  Data generation (self-play) for train_nn.py
// ----------------------------------------------------------------------------
void datagen(int games, int depth, const string& file, int threads) {
    atomic<int> next(0), done(0);
    mutex mu;
    ofstream out(file, ios::app);
    long long total = 0;
    auto worker = [&](int tid) {
        mt19937 rng((unsigned)chrono::steady_clock::now().time_since_epoch().count() + tid * 7919);
        Searcher* s = new Searcher();
        s->isMain = false; s->verbose = false; s->id = 0;
        while (next.fetch_add(1) < games) {
            s->clearHistory();
            Position& p = s->pos;
            p.setFen(START_FEN);
            bool ok = true;
            for (int i = 0; i < 8 + (int)(rng() % 2); i++) {
                Move l[256]; int n = p.gen(l, true);
                vector<Move> legal;
                for (int k = 0; k < n; k++) if (p.makeMove(l[k])) { p.unmakeMove(l[k]); legal.push_back(l[k]); }
                if (legal.empty()) { ok = false; break; }
                p.makeMove(legal[rng() % legal.size()]);
            }
            if (!ok) continue;
            vector<pair<string, int>> rec;
            double result = 0.5;
            int win = 0, drawc = 0;
            for (int ply = 0; ply < 400; ply++) {
                if (p.isDraw()) { result = 0.5; break; }
                Move l[256]; int n = p.gen(l, true); int legalN = 0;
                for (int k = 0; k < n; k++) if (p.makeMove(l[k])) { p.unmakeMove(l[k]); legalN++; }
                if (!legalN) { result = p.inCheck() ? (p.side == WHITE ? 0.0 : 1.0) : 0.5; break; }
                s->lim = Limits(); s->lim.depth = depth;
                s->think();
                Move m = s->bestMove; int sc = s->rootScore;
                int cpWhite = p.side == WHITE ? sc : -sc;
                if (!p.inCheck() && !(m >> 12 & 12) && abs(sc) < MATE_BOUND && ply >= 4) rec.push_back({p.fen(), cpWhite});
                if (abs(sc) >= 1800) win++; else win = 0;
                if (win >= 4) { result = cpWhite > 0 ? 1.0 : 0.0; break; }
                if (abs(sc) <= 8 && ply >= 80) drawc++; else drawc = 0;
                if (drawc >= 12) { result = 0.5; break; }
                p.makeMove(m);
            }
            lock_guard<mutex> g(mu);
            for (auto& r : rec) out << r.first << ',' << r.second << ',' << result << '\n';
            total += (long long)rec.size();
            out.flush();
            int d = ++done;
            if (d % 10 == 0) fprintf(stderr, "games %d/%d, positions %lld\n", d, games, total);
        }
        delete s;
    };
    vector<thread> th;
    for (int i = 0; i < threads; i++) th.emplace_back(worker, i);
    for (auto& t : th) t.join();
    fprintf(stderr, "done: %d games, %lld positions -> %s\n", games, total, file.c_str());
}

// ----------------------------------------------------------------------------
//  UCI
// ----------------------------------------------------------------------------
Position rootPos;
string lastPosition = "startpos";
thread searchThread;
int numThreads = 1, moveOverhead = 50, hashMB = 64, extendPct = 100, softPct = 130;   // softPct: with "go movetime", the soft limit = this % of the movetime (lower = stops earlier in easy positions)
string evalFile = "<none>";

void stopSearch() {
    gStop = true;
    if (searchThread.joinable()) searchThread.join();
}

void doPosition(const string& args) {
    istringstream is(args);
    string tok, fen;
    is >> tok;
    if (tok == "startpos") { fen = START_FEN; is >> tok; }
    else if (tok == "fen") { while (is >> tok && tok != "moves") fen += tok + " "; }
    else return;
    if (!rootPos.setFen(fen)) rootPos.setFen(START_FEN);
    while (is >> tok) {
        if (tok == "moves") continue;
        Move m = parseMove(rootPos, tok);
        if (!m || !rootPos.makeMove(m)) break;
    }
    lastPosition = args;
}

// ----------------------------------------------------------------------------
//  Opening book (text lines of UCI moves, e.g. "e2e4 e7e5 g1f3 ...")
// ----------------------------------------------------------------------------
struct BookMove { Move m; int w; };
unordered_map<U64, vector<BookMove>> gBook;

static void addBookFile(const string& path, int& lines, int& bad) {
    ifstream in(path); if (!in) return;
    string line;
    while (getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        istringstream is(line); string tok; Position p; p.setFen(START_FEN);
        int ply = 0; bool any = false;
        while (is >> tok && ply < 24) {
            Move mv = parseMove(p, tok);
            if (!mv) { bad++; break; }
            bool found = false;
            for (auto& bm : gBook[p.key]) if (bm.m == mv) { bm.w++; found = true; break; }
            if (!found) gBook[p.key].push_back({mv, 1});
            if (!p.makeMove(mv)) { bad++; break; }
            ply++; any = true;
        }
        if (any) lines++;
    }
}
void loadBook() {
    gBook.clear(); int lines = 0, bad = 0;
    if (!optBook || bookFile.empty()) return;
    addBookFile(bookFile, lines, bad);
    size_t k = bookFile.find_last_of('/');
    addBookFile((k == string::npos ? string("") : bookFile.substr(0, k + 1)) + "extra.txt", lines, bad);   // optional: learned from your games
    cout << "info string book: " << lines << " lines, " << gBook.size() << " positions" << (bad ? ", " + to_string(bad) + " bad moves ignored" : string("")) << endl;
}
Move bookMove(Position& p, const vector<Move>& allowed) {
    if (!optBook) return 0;
    auto it = gBook.find(p.key);
    if (it == gBook.end() || it->second.empty()) return 0;
    static mt19937 rng((unsigned)chrono::steady_clock::now().time_since_epoch().count());
    int total = 0; for (auto& bm : it->second) total += bm.w;
    int r = (int)(rng() % total);
    for (auto& bm : it->second) { r -= bm.w; if (r < 0) { Position q = p; if (q.makeMove(bm.m)) return bm.m; } }
    return 0;
}

// ----------------------------------------------------------------------------
//  Move verification: before a move is played, re-check it with an independent,
//  completely unpruned search. If it looks refuted, look for a better move.
// ----------------------------------------------------------------------------
static int verifiedValue(Position& root, Move mv, double untilMs, int& depthDone) {
    Searcher* v = new Searcher(); v->safe = true; v->isMain = true; v->verbose = false;
    v->pos = root;
    depthDone = 0;
    if (!v->pos.makeMove(mv)) { delete v; return -INF; }
    Limits vl; vl.depth = 14; vl.hard = untilMs; vl.soft = 0;
    gStop = false; v->lim = vl; v->think();
    depthDone = v->completedDepth;
    int val = -v->rootScore;
    delete v;
    return val;
}

Move verifyMove(Searcher* m, const Limits& lim, Move best, bool& keepPv) {
    int s = m->rootScore;
    if (!best || abs(s) >= 1500 || m->completedDepth < 6) return best;      // mate / decided / too shallow: nothing to check
    double used = elapsedMs(), budget = min(lim.hard * 0.22, lim.hard * 0.9 - used);
    if (budget < 12) return best;
    gVerChecks++;
    int d1 = 0;
    int ver = verifiedValue(rootPos, best, used + budget * 0.4, d1);
    if (d1 < 4 || ver == -INF || ver >= s - 90) return best;               // fine (or inconclusive)
    gVerFlag++;
    // suspicious: best alternative among the other legal moves
    Limits nl; nl.depth = 99; nl.soft = 0; nl.hard = used + budget * 0.4 + budget * 0.25;
    { Move l[256]; int n = rootPos.gen(l, true); Position q = rootPos;
      for (int i = 0; i < n; i++) if (l[i] != best && q.makeMove(l[i])) { q.unmakeMove(l[i]); nl.searchMoves.push_back(l[i]); } }
    if (nl.searchMoves.empty()) return best;
    Move keep = m->bestMove; vector<Move> keepPvV = m->bestPv;
    gStop = false; m->lim = nl; m->pos = rootPos; m->isMain = true; m->think();
    Move alt = m->bestMove; int s2 = m->rootScore, d2 = m->completedDepth;
    m->bestMove = keep; m->bestPv = keepPvV;
    if (!alt || d2 < 4 || s2 <= ver + 25) return best;
    int d3 = 0;
    double t = elapsedMs();
    int ver2 = verifiedValue(rootPos, alt, min(lim.hard * 0.95, t + budget * 0.35), d3);
    if (d3 >= 4 && ver2 > ver + 15) { gVerSwitch++; keepPv = false; return alt; }
    return best;
}

void startSearch(Limits lim) {
    stopSearch();
    gStop = false; gStart = chrono::steady_clock::now();
    tt.newSearch();
    while ((int)gSearchers.size() < numThreads) { auto* s = new Searcher(); s->id = (int)gSearchers.size(); gSearchers.push_back(s); }
    while ((int)gSearchers.size() > numThreads) { delete gSearchers.back(); gSearchers.pop_back(); }
    for (int i = 0; i < numThreads; i++) {
        Searcher* s = gSearchers[i];
        s->id = i; s->isMain = i == 0; s->verbose = true; s->lim = lim; s->pos = rootPos;
    }
    searchThread = thread([lim]() {
        vector<thread> helpers;
        for (int i = 1; i < (int)gSearchers.size(); i++) helpers.emplace_back([i]() { gSearchers[i]->think(); });
        Searcher* m = gSearchers[0];
        m->think();
        if (lim.infinite) while (!gStop.load()) this_thread::sleep_for(chrono::milliseconds(2));
        gStop = true;
        for (auto& h : helpers) h.join();
        Move finalMove = m->bestMove; bool keepPv = true;
        if (optVerify && !lim.infinite && lim.hard > 0 && lim.depth >= 99) finalMove = verifyMove(m, lim, finalMove, keepPv);
        string o = "bestmove " + moveStr(finalMove);
        if (keepPv && m->bestPv.size() > 1 && m->bestPv[0] == finalMove) o += " ponder " + moveStr(m->bestPv[1]);
        cout << o << endl;
    });
}

void doGo(istringstream& is) {
    Limits lim;
    string tok;
    int wtime = -1, btime = -1, winc = 0, binc = 0, mtg = 0, movetime = -1;
    bool anyLimit = false;
    bool inSM = false;
    while (is >> tok) {
        if (inSM) {
            if (tok.size() >= 4 && tok[0] >= 'a' && tok[0] <= 'h' && isdigit((unsigned char)tok[1])) {
                Move mv = parseMove(rootPos, tok);
                if (mv) lim.searchMoves.push_back(mv);
                continue;
            }
            inSM = false;
        }
        if (tok == "searchmoves") inSM = true;
        else if (tok == "wtime") is >> wtime;
        else if (tok == "btime") is >> btime;
        else if (tok == "winc") is >> winc;
        else if (tok == "binc") is >> binc;
        else if (tok == "movestogo") is >> mtg;
        else if (tok == "movetime") is >> movetime;
        else if (tok == "depth") { is >> lim.depth; anyLimit = true; }
        else if (tok == "nodes") { is >> lim.nodes; anyLimit = true; }
        else if (tok == "infinite") lim.infinite = true;
    }
    if (lim.depth > 99) lim.depth = 99;
    int me = rootPos.side;
    int rem = me == WHITE ? wtime : btime, inc = me == WHITE ? winc : binc;
    if (movetime > 0) {
        lim.hard = max(1, movetime - moveOverhead); lim.soft = lim.hard * softPct / 100.0; anyLimit = true;
        lim.hardMax = lim.hard * extendPct / 100.0; lim.extend = extendPct / 100.0;
    } else if (rem >= 0) {
        int m = mtg > 0 ? min(mtg, 40) : 28;
        double base = rem / (double)m + inc * 0.75;
        base = min(base, rem * 0.5);
        lim.soft = max(5.0, base);
        lim.hard = max(5.0, min(base * 3.0, rem * 0.5) - moveOverhead);
        if (lim.hard < lim.soft) lim.soft = lim.hard;
        lim.hardMax = lim.hard; lim.extend = min(extendPct / 100.0, 1.5);
        anyLimit = true;
    }
    if (!anyLimit) lim.infinite = true;
    if (optBook && !lim.infinite && lim.depth >= 99 && !lim.nodes && lim.searchMoves.empty()) {
        stopSearch();
        Move bm = bookMove(rootPos, lim.searchMoves);
        if (bm) { gBookHits++; cout << "info string book move" << endl << "bestmove " << moveStr(bm) << endl; return; }
    }
    startSearch(lim);
}

void loadEval() {
    if (evalFile.empty() || evalFile == "<none>") { net = Net(); }
    else if (loadNet(evalFile)) cout << "info string NNUE loaded: " << evalFile << " (H=" << net.H << ")" << endl;
    else { net = Net(); cout << "info string no net (" << evalFile << "), using classical eval" << endl; }
    doPosition(lastPosition);
    for (auto* s : gSearchers) s->pos = rootPos;
}

void bench() {
    static const char* fens[] = {
        START_FEN,
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
        "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
        "2rq1rk1/pp3ppp/2n1pn2/3p4/3P4/2NBPN2/PP3PPP/2RQ1RK1 w - - 0 12"};
    Limits lim; lim.depth = 11;
    U64 total = 0;
    auto t0 = chrono::steady_clock::now();
    Searcher* s = new Searcher();
    s->isMain = false; s->verbose = false;
    for (auto f : fens) {
        s->pos.setFen(f); s->clearHistory(); tt.clear(); gStop = false;
        s->lim = lim; s->think(); total += s->nodes;
    }
    double ms = chrono::duration<double, milli>(chrono::steady_clock::now() - t0).count();
    delete s;
    cout << "bench: " << total << " nodes " << (U64)(total * 1000.0 / max(1.0, ms)) << " nps" << endl;
}

bool handle(const string& line) {
    istringstream is(line);
    string cmd;
    is >> cmd;
    if (cmd == "uci") {
        cout << "id name Engine99\nid author you\n"
             << "option name Hash type spin default 64 min 1 max 65536\n"
             << "option name Threads type spin default 1 min 1 max 128\n"
             << "option name MoveOverhead type spin default 50 min 0 max 5000\n"
             << "option name DifficultExtend type spin default 100 min 100 max 400\n"
             << "option name SoftPct type spin default 130 min 20 max 130\n"
             << "option name Book type check default true\n"
             << "option name BookFile type string default ../book/openings.txt\n"
             << "option name Verify type check default true\n"
             << "option name CorrHist type check default false\n"
             << "option name ProbCut type check default false\n"
             << "option name EvalFile type string default <none>\n"
             << "option name NetQA type spin default 255 min 1 max 32767\n"
             << "option name NetQB type spin default 64 min 1 max 32767\n"
             << "option name NetScale type spin default 400 min 1 max 4000\n"
             << "option name NetSCReLU type check default false\n"
             << "option name NetResidual type check default false\n"
             << "uciok" << endl;
    } else if (cmd == "isready") cout << "readyok" << endl;
    else if (cmd == "ucinewgame") { stopSearch(); tt.clear(); for (auto* s : gSearchers) s->clearHistory(); }
    else if (cmd == "setoption") {
        string tok, name, value; is >> tok;
        while (is >> tok && tok != "value") name += (name.empty() ? "" : " ") + tok;
        getline(is, value);
        size_t a = value.find_first_not_of(' ');
        value = a == string::npos ? "" : value.substr(a);
        stopSearch();
        if (name == "Hash") { hashMB = max(1, atoi(value.c_str())); tt.resize(hashMB); }
        else if (name == "Threads") numThreads = max(1, min(128, atoi(value.c_str())));
        else if (name == "MoveOverhead") moveOverhead = max(0, atoi(value.c_str()));
        else if (name == "SoftPct") softPct = max(20, min(130, atoi(value.c_str())));
        else if (name == "DifficultExtend") extendPct = max(100, min(400, atoi(value.c_str())));
        else if (name == "EvalFile") { evalFile = value; loadEval(); }
        else if (name == "Book") { optBook = (value == "true"); loadBook(); }
        else if (name == "BookFile") { bookFile = value; loadBook(); }
        else if (name == "Verify") optVerify = (value == "true");
        else if (name == "CorrHist") { optCorr = (value == "true"); for (auto* s2 : gSearchers) memset(s2->corr, 0, sizeof s2->corr); }
        else if (name == "ProbCut") optProbCut = (value == "true");
        else if (name == "NetQA") { optQA = max(1, atoi(value.c_str())); loadEval(); }
        else if (name == "NetQB") { optQB = max(1, atoi(value.c_str())); loadEval(); }
        else if (name == "NetScale") { optScale = max(1, atoi(value.c_str())); loadEval(); }
        else if (name == "NetSCReLU") { optSCReLU = (value == "true"); loadEval(); }
        else if (name == "NetResidual") { optResidual = (value == "true"); loadEval(); }
    } else if (cmd == "position") {
        stopSearch();
        string rest; getline(is, rest);
        doPosition(rest);
    } else if (cmd == "go") doGo(is);
    else if (cmd == "stop") stopSearch();
    else if (cmd == "stats") { stopSearch(); cout << "info string verify: checked " << gVerChecks << ", flagged " << gVerFlag << ", switched " << gVerSwitch << ", book moves " << gBookHits << endl; }
    else if (cmd == "quit") { stopSearch(); return false; }
    else if (cmd == "perft") {
        int d = 5; is >> d;
        auto t0 = chrono::steady_clock::now();
        U64 n = perft(rootPos, d);
        double ms = chrono::duration<double, milli>(chrono::steady_clock::now() - t0).count();
        cout << "perft " << d << " = " << n << "  (" << (U64)(n * 1000.0 / max(1.0, ms)) << " nps)" << endl;
    } else if (cmd == "addhce") {   // addhce in.txt out.txt : append the classical eval (white view) as a 4th column
        string in, out; is >> in >> out;
        ifstream fi(in); ofstream fo(out);
        string ln; Position* tmp = new Position(); long long cnt = 0;
        while (getline(fi, ln)) {
            size_t c = ln.find(',');
            if (c == string::npos || !tmp->setFen(ln.substr(0, c))) continue;
            int h = evalHCE(*tmp);
            fo << ln << ',' << (tmp->side == WHITE ? h : -h) << '\n'; cnt++;
        }
        delete tmp;
        cout << "addhce: " << cnt << " lines -> " << out << endl;
    } else if (cmd == "bench") bench();
    else if (cmd == "d") cout << rootPos.fen() << endl;
    else if (cmd == "eval") cout << "eval (side to move): " << evaluate(rootPos) << endl;
    else if (cmd == "datagen") {
        int games = 100, depth = 8, th = 1; string file = "data.txt";
        is >> games >> depth >> file >> th;
        datagen(games, depth, file, max(1, th));
    }
    return true;
}

int main(int argc, char** argv) {
    initLeapers(); initMagics(); initZobrist(); initEval(); initLMR();
    tt.resize(hashMB);
    rootPos.setFen(START_FEN);
    loadBook();
    if (evalFile != "<none>" && loadNet(evalFile)) { cout << "info string NNUE loaded: " << evalFile << " (H=" << net.H << ")" << endl; rootPos.setFen(START_FEN); }
    if (argc > 1) {
        string line;
        for (int i = 1; i < argc; i++) line += string(i > 1 ? " " : "") + argv[i];
        handle(line);
        if (searchThread.joinable()) searchThread.join();
        return 0;
    }
    string line;
    while (getline(cin, line)) if (!handle(line)) break;
    stopSearch();
    return 0;
}
