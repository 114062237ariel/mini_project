#include "submission.hpp"

SearchResult Submission::search(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    return MiniMax::search(state, depth, history, ctx);
}

ParamMap Submission::default_params(){
    return MiniMax::default_params();
}

std::vector<ParamDef> Submission::param_defs(){
    return MiniMax::param_defs();
}