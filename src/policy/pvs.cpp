#include <utility>
#include "state.hpp"
#include "pvs.hpp"


/*============================================================
 * PVS — eval_ctx
 *============================================================*/
int PVS::eval_ctx(
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
    if(ply > ctx.seldepth){
        ctx.seldepth = ply;
    }
    if(ctx.stop){
        return 0;
    }

    /* === Lazy move generation (sets game_state) === */
    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }

    /* === Terminal / leaf checks === */

    // [ Hackathon TODO 3-1 ]
    // return the score for a winning terminal state
    // Hint: prefer faster wins by using ply.
    if(state->game_state == WIN){
        return P_MAX-ply; 
    }

    if(state->game_state == DRAW){
        return 0;
    }

    /* === Repetition check (game-specific) === */
    int rep_score;
    if(state->check_repetition(history, rep_score)){
        return rep_score;
    }
    history.push(state->hash());

    if(depth <= 0){
        int score = quiescence(
            state,
            history,
            ctx,
            p,
            alpha,
            beta
        );

        history.pop(state->hash());
        return score;
    }

    /* === Negamax loop === */
    int best_score = M_MAX;
    bool first = true;
    for(auto& action : state->legal_actions){
        // [ Hackathon TODO 3-2 ]
        // create the child state after applying action
        State *next = state->next_state(action); 

        bool same = next->same_player_as_parent();

        // [Hackathon TODO 3-3]
        // search the child one level deeper
        int raw;

        if(first){

            raw = eval_ctx(
                next,
                depth-1,
                history,
                ply+1,
                ctx,
                p,
                -beta,
                -alpha
            );

            first = false;
        }
        else{

            raw = eval_ctx(
                next,
                depth-1,
                history,
                ply+1,
                ctx,
                p,
                -alpha-1,
                -alpha
            );

            int score_test = same ? raw : -raw;

            if(score_test > alpha && score_test < beta){

                raw = eval_ctx(
                    next,
                    depth-1,
                    history,
                    ply+1,
                    ctx,
                    p,
                    -beta,
                    -alpha
                );
            }
        }
        
        // [Hackathon TODO 3-4]
        // convert raw to the current player's perspective.
        int score = same? raw:-raw; 
        delete next; //avoid memory leak
        // [ Hackathon TODO 3-5 ]
        // update best_score if this child is better.
        if(score>best_score){
            best_score = score;
        }
        if(score>alpha){
            alpha = score;
        }
        if(alpha>=beta){
            break;  
        }

    }

    history.pop(state->hash());
    return best_score;
}
/*============================================================
 * PVS — quiescence
 *============================================================*/
int PVS::quiescence(
    State *state,
    GameHistory& history,
    SearchContext& ctx,
    const MMParams& p,
    int alpha,
    int beta
){
    ctx.nodes++;

    int stand_pat = state->evaluate(
        p.use_kp_eval,
        p.use_eval_mobility,
        &history
    );

    if(stand_pat >= beta){
        return beta;
    }

    if(stand_pat > alpha){
        alpha = stand_pat;
    }

    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }

    int opp = 1 - state->player;

    for(auto& action : state->legal_actions){
        int tr = action.second.first;
        int tc = action.second.second;

        // 只搜尋吃子
        if(state->board.board[opp][tr][tc] == 0){
            continue;
        }

        State *next = state->next_state(action);

        int raw = quiescence(
            next,
            history,
            ctx,
            p,
            -beta,
            -alpha
        );

        int score = -raw;

        delete next;

        if(score >= beta){
            return beta;
        }

        if(score > alpha){
            alpha = score;
        }
    }

    return alpha;
}

/*============================================================
 * MiniMax — search
 *
 * Iterate legal moves, call eval_ctx, return SearchResult.
 *============================================================*/

SearchResult PVS::search(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){ 
    int alpha = M_MAX;
    int beta = P_MAX;
    ctx.reset();
    MMParams p = MMParams::from_map(ctx.params);
    SearchResult result;
    result.depth = depth;

    if(!state->legal_actions.size()){
        state->get_legal_actions();
    }


    int best_score = M_MAX;
    int move_index = 0;
    int total_moves = (int)state->legal_actions.size();

    for(auto& action : state->legal_actions){
        /* [ Hackathon TODO 4-1 ]
         * search this move like TODO 3, but starting from the root */
            State *next = state->next_state(action);
            bool same = next->same_player_as_parent();
            int raw = eval_ctx(next,depth-1,history,1,ctx,p,alpha,beta);//root,ply->1
            int score = same? raw:-raw;
            delete next;
            if(score > best_score){
                // [ Hackathon TODO 4-2 ]
                // keep this move if it is the best so far
                result.best_move = action;
                best_score = score;

                if(p.report_partial && ctx.on_root_update){
                   ctx.on_root_update({result.best_move, best_score, depth, move_index + 1, total_moves});
                }
            }  
            if(score>alpha){
                alpha = score;
            }
        move_index++;
        
    }

    // [ Hackathon TODO 4-3 ]
    // update result and return

        result.score = best_score;
        result.nodes = ctx.nodes;
        result.seldepth = ctx.seldepth;
        result.pv = {result.best_move};

        return result;
} 


/*============================================================
 * PVS — default_params / param_defs
 *============================================================*/
ParamMap PVS::default_params(){
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
    };
}

std::vector<ParamDef> PVS::param_defs(){
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
    };
}
