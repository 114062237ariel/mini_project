#include <utility>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>

#include "state.hpp"
#include "minimax.hpp"

/* ============================================================
 *  Globals: time control, transposition table, killers, history
 * ============================================================ */

/* --- Time control: per-move hard limit is 2000ms; overrun = instant loss.
 *     Keep margin for unwinding + encoding + stdout flush -> 1800ms.
 *     If your ubgi.c passes the movetime, drive this from it instead. */
static std::chrono::steady_clock::time_point g_search_start;
static double SEARCH_LIMIT_MS = 1800.0;

static inline bool time_up(){
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - g_search_start).count() > SEARCH_LIMIT_MS;
}

static const int ORD_VAL[7] = {0, 20, 60, 70, 80, 200, 1000};

/* --- Killer + history tables --------------------------------------- */
static const int  MAX_PLY      = 64;
static const Move INVALID_MOVE = Move{Point(-1,-1), Point(-1,-1)};
static Move g_killers[2][MAX_PLY];
static int  g_history[2][32][32];
static const int KILLER_BASE = 1 << 23;
static const int HISTORY_CAP = 1 << 22;

static inline void clear_search_tables(){
    for(int i=0;i<2;i++) for(int j=0;j<MAX_PLY;j++) g_killers[i][j]=INVALID_MOVE;
    std::memset(g_history, 0, sizeof(g_history));
}
static inline void store_killer(int ply, const Move& m){
    if(ply<0||ply>=MAX_PLY) return;
    if(g_killers[0][ply]==m) return;
    g_killers[1][ply]=g_killers[0][ply];
    g_killers[0][ply]=m;
}

/* --- Transposition table ------------------------------------------- */
enum : uint8_t { TT_NONE=0, TT_EXACT=1, TT_LOWER=2, TT_UPPER=3 };
struct TTEntry { uint64_t key=0; int score=0; int16_t depth=-1; uint8_t flag=TT_NONE; Move best=INVALID_MOVE; };
static const size_t TT_SIZE = (size_t)1 << 20;
static const size_t TT_MASK = TT_SIZE - 1;
static std::vector<TTEntry> g_tt(TT_SIZE);

static inline TTEntry* tt_probe(uint64_t key){
    TTEntry& e = g_tt[key & TT_MASK];
    return (e.flag!=TT_NONE && e.key==key) ? &e : nullptr;
}
static inline void tt_store(uint64_t key,int depth,int score,uint8_t flag,const Move& best){
    TTEntry& e = g_tt[key & TT_MASK];
    if(e.flag==TT_NONE || e.key!=key || depth>=e.depth){
        e.key=key; e.depth=(int16_t)depth; e.score=score; e.flag=flag; e.best=best;
    }
}

/* ============================================================
 *  Move ordering: TT move -> MVV-LVA -> killers -> history
 * ============================================================ */
static inline bool is_capture(State* s, const Move& m){
    return s->board.board[1 - s->player][m.second.first][m.second.second] != 0;
}
static inline int from_sq(const Move& m){ return m.first.first  * BOARD_W + m.first.second;  }
static inline int to_sq  (const Move& m){ return m.second.first * BOARD_W + m.second.second; }

static inline int move_score(State* s, const Move& m, const Move& ttMove, int ply){
    if(m == ttMove) return 1 << 28;
    int me = s->player, opp = 1 - s->player;
    int victim = s->board.board[opp][m.second.first][m.second.second];
    if(victim){
        int att = s->board.board[me][m.first.first][m.first.second];
        return (1 << 24) + ORD_VAL[victim]*32 - ORD_VAL[att];
    }
    if(ply>=0 && ply<MAX_PLY){
        if(m == g_killers[0][ply]) return KILLER_BASE + 1;
        if(m == g_killers[1][ply]) return KILLER_BASE;
    }
    int h = g_history[me][from_sq(m)][to_sq(m)];
    return h > HISTORY_CAP ? HISTORY_CAP : h;
}

static void order_moves(State* s, std::vector<Move>& moves, const Move& ttMove, int ply){
    std::vector<std::pair<int,Move>> scored;
    scored.reserve(moves.size());
    for(auto& m : moves) scored.push_back({move_score(s,m,ttMove,ply), m});
    std::stable_sort(scored.begin(), scored.end(),
        [](const std::pair<int,Move>& a, const std::pair<int,Move>& b){ return a.first > b.first; });
    for(size_t i=0;i<moves.size();++i) moves[i] = scored[i].second;
}

/* ============================================================
 *  Quiescence search
 * ============================================================ */
static int qsearch(State* state, GameHistory& history, int ply,
                   SearchContext& ctx, const MMParams& p, int alpha, int beta){
    ctx.nodes++;
    if(ply > ctx.seldepth) ctx.seldepth = ply;
    if((ctx.nodes & 1023)==0 && time_up()) ctx.stop = true;
    if(ctx.stop) return 0;

    if(state->legal_actions.empty() && state->game_state==UNKNOWN)
        state->get_legal_actions();
    if(state->game_state==WIN)  return P_MAX - ply;
    if(state->game_state==DRAW) return 0;

    int stand = state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
    if(stand >= beta) return beta;
    if(stand > alpha) alpha = stand;
    if(ply >= MAX_PLY-1) return alpha;

    std::vector<Move> caps;
    for(auto& m : state->legal_actions) if(is_capture(state,m)) caps.push_back(m);
    order_moves(state, caps, INVALID_MOVE, ply);

    for(auto& m : caps){
        State* next = state->next_state(m);
        bool same = next->same_player_as_parent();
        int raw = qsearch(next, history, ply+1, ctx, p, -beta, -alpha);
        int score = same? raw : -raw;
        delete next;
        if(ctx.stop) return 0;
        if(score >= beta) return beta;
        if(score > alpha) alpha = score;
    }
    return alpha;
}

/* ============================================================
 *  eval_ctx — PVS + LMR + alpha-beta + TT
 * ============================================================ */
int MiniMax::eval_ctx(State *state, int depth, GameHistory& history, int ply,
                      SearchContext& ctx, const MMParams& p, int alpha, int beta){
    ctx.nodes++;
    if(ply > ctx.seldepth) ctx.seldepth = ply;
    if((ctx.nodes & 1023)==0 && time_up()) ctx.stop = true;
    if(ctx.stop) return 0;

    if(state->legal_actions.empty() && state->game_state==UNKNOWN)
        state->get_legal_actions();
    if(state->game_state==WIN)  return P_MAX - ply;
    if(state->game_state==DRAW) return 0;

    int rep_score;
    if(state->check_repetition(history, rep_score)) return rep_score;

    if(depth <= 0)
        return qsearch(state, history, ply, ctx, p, alpha, beta);

    uint64_t key = state->hash();
    int alpha_orig = alpha;
    Move ttMove = INVALID_MOVE;
    if(TTEntry* tte = tt_probe(key)){
        ttMove = tte->best;
        if(tte->depth >= depth){
            if(tte->flag==TT_EXACT) return tte->score;
            else if(tte->flag==TT_LOWER){ if(tte->score>alpha) alpha=tte->score; }
            else if(tte->flag==TT_UPPER){ if(tte->score<beta)  beta =tte->score; }
            if(alpha>=beta) return tte->score;
        }
    }

    history.push(key);
    order_moves(state, state->legal_actions, ttMove, ply);

    int best_score = M_MAX;
    Move best_move = state->legal_actions.empty()? INVALID_MOVE : state->legal_actions[0];
    bool first = true;
    int move_count = 0;

    for(auto& action : state->legal_actions){
        State *next = state->next_state(action);
        bool same  = next->same_player_as_parent();
        bool quiet = !is_capture(state, action);
        int raw, score;

        if(first){
            raw = eval_ctx(next, depth-1, history, ply+1, ctx, p, -beta, -alpha);
            score = same? raw : -raw;
        }else{
            int R = 0;
            if(depth>=3 && quiet && move_count>=3) R = 1 + (move_count>=6?1:0);
            int dr = depth-1-R; if(dr<0) dr=0;

            raw = eval_ctx(next, dr, history, ply+1, ctx, p, -alpha-1, -alpha);
            score = same? raw : -raw;
            if(R>0 && score>alpha){
                raw = eval_ctx(next, depth-1, history, ply+1, ctx, p, -alpha-1, -alpha);
                score = same? raw : -raw;
            }
            if(score>alpha && score<beta){
                raw = eval_ctx(next, depth-1, history, ply+1, ctx, p, -beta, -alpha);
                score = same? raw : -raw;
            }
        }
        delete next;
        if(ctx.stop){ history.pop(key); return 0; }

        if(score > best_score){ best_score = score; best_move = action; }
        if(score > alpha) alpha = score;
        if(alpha >= beta){
            if(quiet){
                store_killer(ply, action);
                g_history[state->player][from_sq(action)][to_sq(action)] += depth*depth;
            }
            break;
        }
        first = false;
        move_count++;
    }

    history.pop(key);
    uint8_t flag = (best_score<=alpha_orig)? TT_UPPER : (best_score>=beta)? TT_LOWER : TT_EXACT;
    tt_store(key, depth, best_score, flag, best_move);
    return best_score;
}

/* ============================================================
 *  search — iterative deepening + aspiration windows
 * ============================================================ */
SearchResult MiniMax::search(State *state, int depth, GameHistory& history, SearchContext& ctx){
    ctx.reset();
    MMParams p = MMParams::from_map(ctx.params);
    g_search_start = std::chrono::steady_clock::now();
    clear_search_tables();

    SearchResult result;
    result.depth = 0;

    if(!state->legal_actions.size()) state->get_legal_actions();
    if(state->legal_actions.empty()){
        result.best_move = INVALID_MOVE; result.score = 0;
        result.nodes = ctx.nodes; result.pv = {result.best_move};
        return result;
    }
    result.best_move = state->legal_actions[0];
    result.score = 0;

    int max_d = (depth>0 && depth<MAX_PLY) ? depth : (MAX_PLY-1);
    int total = (int)state->legal_actions.size();

    for(int d = 1; d <= max_d; ++d){
        /* aspiration window centred on previous score (off for d<=3) */
        int delta = 35;
        int alpha = (d<=3) ? M_MAX : result.score - delta;
        int beta  = (d<=3) ? P_MAX : result.score + delta;

        Move best_m; int best_s; bool ok;

        for(;;){   /* re-search loop on fail-high / fail-low */
            best_m = result.best_move;
            best_s = M_MAX;
            ok = true;
            int a = alpha;
            bool first = true;
            int move_count = 0, idx = 0;

            order_moves(state, state->legal_actions, result.best_move, 0);

            for(auto& action : state->legal_actions){
                State *next = state->next_state(action);
                bool same  = next->same_player_as_parent();
                bool quiet = !is_capture(state, action);
                int raw, score;

                if(first){
                    raw = eval_ctx(next, d-1, history, 1, ctx, p, -beta, -a);
                    score = same? raw : -raw;
                }else{
                    int R = 0;
                    if(d>=3 && quiet && move_count>=3) R = 1 + (move_count>=6?1:0);
                    int dr = d-1-R; if(dr<0) dr=0;
                    raw = eval_ctx(next, dr, history, 1, ctx, p, -a-1, -a);
                    score = same? raw : -raw;
                    if(R>0 && score>a){
                        raw = eval_ctx(next, d-1, history, 1, ctx, p, -a-1, -a);
                        score = same? raw : -raw;
                    }
                    if(score>a && score<beta){
                        raw = eval_ctx(next, d-1, history, 1, ctx, p, -beta, -a);
                        score = same? raw : -raw;
                    }
                }
                delete next;
                if(ctx.stop){ ok = false; break; }

                if(score > best_s){
                    best_s = score; best_m = action;
                    if(p.report_partial && ctx.on_root_update)
                        ctx.on_root_update({best_m, best_s, d, idx+1, total});
                }
                if(score > a) a = score;
                if(a >= beta) break;          /* fail high -> widen & re-search */
                first = false;
                move_count++; idx++;
            }

            if(!ok) break;
            if(best_s <= alpha && alpha != M_MAX){       /* fail low */
                alpha = (alpha - delta <= M_MAX) ? M_MAX : alpha - delta;
                delta *= 2; continue;
            }
            if(best_s >= beta && beta != P_MAX){         /* fail high */
                beta = (beta + delta >= P_MAX) ? P_MAX : beta + delta;
                delta *= 2; continue;
            }
            break;   /* score inside window */
        }

        if(!ok) break;                       /* incomplete depth discarded */
        result.best_move = best_m;
        result.score     = best_s;
        result.depth     = d;

        if(time_up()) break;
        if(best_s >= P_MAX - MAX_PLY || best_s <= M_MAX + MAX_PLY) break;  /* mate */
    }

    result.nodes    = ctx.nodes;
    result.seldepth = ctx.seldepth;
    result.pv       = {result.best_move};
    return result;
}

/* ============================================================
 *  default_params / param_defs
 * ============================================================ */
ParamMap MiniMax::default_params(){
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "false"},
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