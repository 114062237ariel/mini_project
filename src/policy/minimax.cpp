#include <utility>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cstdint>

#include "state.hpp"
#include "minimax.hpp"

/* ============================================================
 *  Globals: time control, transposition table, killers
 *  (all static -> single translation unit, no header changes)
 * ============================================================ */

/* --- Time control ---------------------------------------------------
 * Project spec: 10s per move. Keep a safety margin because the TA
 * machine may be slower than yours. Tune SEARCH_LIMIT_MS if you ever
 * see a timeout loss.
 * If your SearchContext already carries a deadline, prefer that.
 * ------------------------------------------------------------------ */
static std::chrono::steady_clock::time_point g_search_start;
static double SEARCH_LIMIT_MS = 9000.0;

static inline bool time_up(){
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - g_search_start).count() > SEARCH_LIMIT_MS;
}

/* --- Piece values for move ordering (index = piece type 1..6) ------ */
static const int ORD_VAL[7] = {0, 20, 60, 70, 80, 200, 1000};

/* --- Killer moves --------------------------------------------------- */
static const int  MAX_PLY      = 64;
static const Move INVALID_MOVE = Move{Point(-1,-1), Point(-1,-1)};
static Move g_killers[2][MAX_PLY];

static inline void clear_killers(){
    for(int i=0;i<2;i++)
        for(int j=0;j<MAX_PLY;j++)
            g_killers[i][j] = INVALID_MOVE;
}
static inline void store_killer(int ply, const Move& m){
    if(ply < 0 || ply >= MAX_PLY) return;
    if(g_killers[0][ply] == m) return;
    g_killers[1][ply] = g_killers[0][ply];
    g_killers[0][ply] = m;
}

/* --- Transposition table (fixed size, depth-preferred replacement) -- */
enum : uint8_t { TT_NONE=0, TT_EXACT=1, TT_LOWER=2, TT_UPPER=3 };
struct TTEntry {
    uint64_t key   = 0;
    int      score = 0;
    int16_t  depth = -1;
    uint8_t  flag  = TT_NONE;
    Move     best  = INVALID_MOVE;
};
static const size_t TT_BITS = 20;                 /* 1<<20 entries ~ tens of MB */
static const size_t TT_SIZE = (size_t)1 << TT_BITS;
static const size_t TT_MASK = TT_SIZE - 1;
static std::vector<TTEntry> g_tt(TT_SIZE);

static inline TTEntry* tt_probe(uint64_t key){
    TTEntry& e = g_tt[key & TT_MASK];
    return (e.flag != TT_NONE && e.key == key) ? &e : nullptr;
}
static inline void tt_store(uint64_t key,int depth,int score,uint8_t flag,const Move& best){
    TTEntry& e = g_tt[key & TT_MASK];
    if(e.flag == TT_NONE || e.key != key || depth >= e.depth){
        e.key=key; e.depth=(int16_t)depth; e.score=score; e.flag=flag; e.best=best;
    }
}

/* ============================================================
 *  Move ordering: TT move -> MVV-LVA captures -> killers -> rest
 * ============================================================ */
static inline bool is_capture(State* s, const Move& m){
    return s->board.board[1 - s->player][m.second.first][m.second.second] != 0;
}

static inline int move_score(State* s, const Move& m, const Move& ttMove, int ply){
    if(m == ttMove) return 1 << 28;                       /* hash move first */
    int me  = s->player, opp = 1 - s->player;
    int victim = s->board.board[opp][m.second.first][m.second.second];
    if(victim){                                           /* MVV-LVA */
        int att = s->board.board[me][m.first.first][m.first.second];
        return (1 << 24) + ORD_VAL[victim] * 32 - ORD_VAL[att];
    }
    if(ply >= 0 && ply < MAX_PLY){                        /* killer quiets */
        if(m == g_killers[0][ply]) return (1 << 20) + 1;
        if(m == g_killers[1][ply]) return (1 << 20);
    }
    return 0;
}

static void order_moves(State* s, std::vector<Move>& moves, const Move& ttMove, int ply){
    /* precompute scores so we don't recompute inside the comparator */
    std::vector<std::pair<int,Move>> scored;
    scored.reserve(moves.size());
    for(auto& m : moves) scored.push_back({move_score(s, m, ttMove, ply), m});
    std::stable_sort(scored.begin(), scored.end(),
        [](const std::pair<int,Move>& a, const std::pair<int,Move>& b){
            return a.first > b.first;
        });
    for(size_t i=0;i<moves.size();++i) moves[i] = scored[i].second;
}

/* ============================================================
 *  Quiescence search (captures only) — removes horizon effect
 * ============================================================ */
static int qsearch(State* state, GameHistory& history, int ply,
                   SearchContext& ctx, const MMParams& p, int alpha, int beta){
    ctx.nodes++;
    if(ply > ctx.seldepth) ctx.seldepth = ply;
    if((ctx.nodes & 2047) == 0 && time_up()) ctx.stop = true;
    if(ctx.stop) return 0;

    if(state->legal_actions.empty() && state->game_state == UNKNOWN)
        state->get_legal_actions();

    if(state->game_state == WIN)  return P_MAX - ply;
    if(state->game_state == DRAW) return 0;

    /* stand-pat: assume we can at least keep the static eval */
    int stand = state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
    if(stand >= beta) return beta;
    if(stand > alpha) alpha = stand;
    if(ply >= MAX_PLY - 1) return alpha;

    /* collect captures only, then order by MVV-LVA */
    std::vector<Move> caps;
    for(auto& m : state->legal_actions)
        if(is_capture(state, m)) caps.push_back(m);
    order_moves(state, caps, INVALID_MOVE, ply);

    for(auto& m : caps){
        State* next = state->next_state(m);
        bool same   = next->same_player_as_parent();
        int  raw    = qsearch(next, history, ply + 1, ctx, p, -beta, -alpha);
        int  score  = same ? raw : -raw;
        delete next;
        if(ctx.stop) return 0;
        if(score >= beta) return beta;
        if(score > alpha) alpha = score;
    }
    return alpha;
}

/* ============================================================
 *  MiniMax::eval_ctx — PVS + alpha-beta + TT + ordering
 * ============================================================ */
int MiniMax::eval_ctx(
    State *state,
    int depth,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const MMParams& p,
    int alpha,
    int beta
){
    ctx.nodes++;
    if(ply > ctx.seldepth) ctx.seldepth = ply;
    if((ctx.nodes & 2047) == 0 && time_up()) ctx.stop = true;
    if(ctx.stop) return 0;

    /* === Lazy move generation (sets game_state) === */
    if(state->legal_actions.empty() && state->game_state == UNKNOWN)
        state->get_legal_actions();

    /* === Terminal / leaf checks === */
    if(state->game_state == WIN)  return P_MAX - ply;     /* prefer faster wins */
    if(state->game_state == DRAW) return 0;

    int rep_score;
    if(state->check_repetition(history, rep_score)) return rep_score;

    if(depth <= 0)
        return qsearch(state, history, ply, ctx, p, alpha, beta);

    /* === Transposition table probe === */
    uint64_t key       = state->hash();
    int      alpha_orig = alpha;
    Move     ttMove     = INVALID_MOVE;
    if(TTEntry* tte = tt_probe(key)){
        ttMove = tte->best;
        if(tte->depth >= depth){
            if(tte->flag == TT_EXACT) return tte->score;
            else if(tte->flag == TT_LOWER){ if(tte->score > alpha) alpha = tte->score; }
            else if(tte->flag == TT_UPPER){ if(tte->score < beta)  beta  = tte->score; }
            if(alpha >= beta) return tte->score;
        }
    }

    history.push(key);
    order_moves(state, state->legal_actions, ttMove, ply);

    int  best_score = M_MAX;
    Move best_move  = state->legal_actions.empty()
                        ? INVALID_MOVE : state->legal_actions[0];
    bool first = true;

    for(auto& action : state->legal_actions){
        State *next = state->next_state(action);
        bool same   = next->same_player_as_parent();
        int  raw, score;

        if(first){
            /* full-window search on the principal variation */
            raw   = eval_ctx(next, depth-1, history, ply+1, ctx, p, -beta, -alpha);
            score = same ? raw : -raw;
        }else{
            /* PVS: null-window probe, re-search only if it might improve */
            raw   = eval_ctx(next, depth-1, history, ply+1, ctx, p, -alpha-1, -alpha);
            score = same ? raw : -raw;
            if(score > alpha && score < beta){
                raw   = eval_ctx(next, depth-1, history, ply+1, ctx, p, -beta, -alpha);
                score = same ? raw : -raw;
            }
        }
        delete next;

        if(ctx.stop){ history.pop(key); return 0; }

        if(score > best_score){ best_score = score; best_move = action; }
        if(score > alpha) alpha = score;
        if(alpha >= beta){                               /* beta cutoff */
            if(!is_capture(state, action)) store_killer(ply, action);
            break;
        }
        first = false;
    }

    history.pop(key);

    uint8_t flag = (best_score <= alpha_orig) ? TT_UPPER
                 : (best_score >= beta)       ? TT_LOWER : TT_EXACT;
    tt_store(key, depth, best_score, flag, best_move);

    return best_score;
}

/* ============================================================
 *  MiniMax::search — iterative deepening root with time control
 * ============================================================ */
SearchResult MiniMax::search(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    ctx.reset();
    MMParams p = MMParams::from_map(ctx.params);
    g_search_start = std::chrono::steady_clock::now();
    clear_killers();

    SearchResult result;
    result.depth = 0;

    if(!state->legal_actions.size()) state->get_legal_actions();
    if(state->legal_actions.empty()){
        result.best_move = INVALID_MOVE;
        result.score     = 0;
        result.nodes     = ctx.nodes;
        result.pv        = {result.best_move};
        return result;
    }

    /* always hold a valid fallback move */
    result.best_move = state->legal_actions[0];
    result.score     = 0;

    /* Iterative deepening, governed by the clock.
     * NOTE: the incoming `depth` is treated as an upper cap only; we stop
     * on time. If your runner needs an exact depth, change the loop bound
     * to `d <= depth`. */
    int max_d = (depth > 0 && depth < MAX_PLY) ? depth : (MAX_PLY - 1);

    for(int d = 1; d <= max_d; ++d){
        int  alpha = M_MAX, beta = P_MAX;
        Move iter_best  = result.best_move;
        int  iter_score = M_MAX;
        bool completed  = true, first = true;
        int  idx = 0;
        int  total = (int)state->legal_actions.size();

        /* search previous iteration's best move first */
        order_moves(state, state->legal_actions, result.best_move, 0);

        for(auto& action : state->legal_actions){
            State *next = state->next_state(action);
            bool same   = next->same_player_as_parent();
            int  raw, score;

            if(first){
                raw   = eval_ctx(next, d-1, history, 1, ctx, p, -beta, -alpha);
                score = same ? raw : -raw;
            }else{
                raw   = eval_ctx(next, d-1, history, 1, ctx, p, -alpha-1, -alpha);
                score = same ? raw : -raw;
                if(score > alpha && score < beta){
                    raw   = eval_ctx(next, d-1, history, 1, ctx, p, -beta, -alpha);
                    score = same ? raw : -raw;
                }
            }
            delete next;

            if(ctx.stop){ completed = false; break; }

            if(score > iter_score){
                iter_score = score;
                iter_best  = action;
                if(p.report_partial && ctx.on_root_update)
                    ctx.on_root_update({iter_best, iter_score, d, idx + 1, total});
            }
            if(score > alpha) alpha = score;
            first = false;
            idx++;
        }

        if(!completed) break;          /* discard incomplete depth, keep last good */

        result.best_move = iter_best;
        result.score     = iter_score;
        result.depth     = d;

        if(time_up()) break;
        /* forced mate found (for or against us) -> no need to go deeper */
        if(iter_score >= P_MAX - MAX_PLY || iter_score <= M_MAX + MAX_PLY) break;
    }

    result.nodes    = ctx.nodes;
    result.seldepth = ctx.seldepth;
    result.pv       = {result.best_move};
    return result;
}

/* ============================================================
 *  MiniMax — default_params / param_defs
 * ============================================================ */
ParamMap MiniMax::default_params(){
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
    };
}

std::vector<ParamDef> MiniMax::param_defs(){
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
    };
}