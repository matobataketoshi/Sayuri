#include "selfplay/engine.h"
#include "utils/threadpool.h"
#include "utils/random.h"
#include "utils/komi.h"
#include "game/sgf.h"
#include "config.h"

#include <sstream>

void Engine::Initialize() {
    parallel_games_ = GetOption<int>("parallel_games");

    if (!network_) {
        network_ = std::make_unique<Network>();
    }
    network_->Initialize(GetOption<std::string>("weights_file"));

    game_pool_.clear();
    for (int i = 0; i < parallel_games_; ++i) {
        game_pool_.emplace_back(GameState{});
        game_pool_[i].Reset(GetOption<int>("defualt_boardsize"),
                                GetOption<float>("defualt_komi"));
    }

    search_pool_.clear();
    for (int i = 0; i < parallel_games_; ++i) {
        search_pool_.emplace_back(std::make_unique<Search>(game_pool_[i], *network_));
    }

    ThreadPool::Get(GetOption<int>("threads") * parallel_games_);
    ParseQueries();
}

void Engine::ParseQueries() {
    auto queries = GetOption<std::string>("selfplay_query");
    std::istringstream iss{queries};
    std::string query;

    int max_bsize = -1;
    float acc_prob = 0.f;

    while (iss >> query) {
        for (char &c : query) {
            if (c == ':') {
                c = ' ';
            }
        }

        std::istringstream tiss{query};
        std::string token;
        std::vector<std::string> tokens;

        while (tiss >> token) {
            tokens.emplace_back(token);
        }

        if (tokens[0] == "bkp") { 
            // boardsize-komi-probabilities
            // "bkp:19:7.5:20"

            // Assume the query is valid.
            ProbQuery pq;

            pq.board_size = std::stoi(tokens[1]);
            pq.komi = std::stof(tokens[2]);
            pq.prob = std::stof(tokens[3]);

            prob_queries_.emplace_back(pq);
            acc_prob += pq.prob;
        }
    }

    for (auto &pd : prob_queries_) {
        pd.prob /= acc_prob;
        max_bsize = std::max(pd.board_size, max_bsize);
    }
    if (prob_queries_.empty()) {
        ProbQuery pq;
        pq.board_size = GetOption<int>("defualt_boardsize");
        pq.komi = GetOption<float>("defualt_komi");
        pq.prob = 1.0f;
        prob_queries_.emplace_back(pq);

        max_bsize = pq.board_size;
    }

    // Adjust matched NN size.
    network_->Reload(max_bsize);
}

void Engine::SaveSgf(std::string filename, int g) {
    Handel(g);
    Sgf::Get().ToFile(filename, game_pool_[g]);
}

void Engine::GatherTrainingData(std::vector<Training> &chunk, int g) {
    Handel(g);
    search_pool_[g]->GatherTrainingBuffer(chunk, game_pool_[g]);
}

void Engine::PrepareGame(int g) {
    Handel(g);
    auto &state = game_pool_[g];

    state.ClearBoard();

    SetNormalGame(g);
}

void Engine::Selfplay(int g) {
    Handel(g);
    auto &state = game_pool_[g];
    while (!state.IsGameOver()) {
        state.PlayMove(search_pool_[g]->GetSelfPlayMove());
    }
}

void Engine::SetNormalGame(int g) {
    constexpr std::uint32_t kRange = 1000000;
    std::uint32_t rand = Random<kXoroShiro128Plus>::Get().RandFix<kRange>();

    float acc_prob = 0.f;
    int select_bk_idx = 0;

    for (int i = 0; i < (int)prob_queries_.size(); ++i) {
        acc_prob += prob_queries_[i].prob;
        if (rand <= kRange * acc_prob) {
            select_bk_idx = i;
            break;
        }
    }
    int query_boardsize = prob_queries_[select_bk_idx].board_size;
    float query_komi = prob_queries_[select_bk_idx].komi;

    float variance = GetOption<float>("komi_variance");
    auto dist = std::normal_distribution<float>(0.f, variance);
    float bonus = dist(Random<kXoroShiro128Plus>::Get());

    game_pool_[g].Reset(query_boardsize,
                            AdjustKomi<float>(query_komi + bonus));
}

void Engine::SetHandicapGame(int g) {
    auto &state = game_pool_[g];

    int handicap = Random<kXoroShiro128Plus>::Get().RandFix<4>() + 1;
    state.SetFixdHandicap(handicap);

    SetFairKomi(g);
}

void Engine::SetFairKomi(int g) {
    auto &state = game_pool_[g];
    auto result = search_pool_[g]->Computation(400, Search::kNullTag);
    auto komi = state.GetKomi();
    auto final_score = result.root_final_score;

    if (state.GetToMove() == kBlack) {
        final_score = 0.0f - final_score;
    }

    state.SetKomi(AdjustKomi<int>(final_score + komi));
}

int Engine::GetParallelGames() const {
    return parallel_games_;
}

void Engine::Handel(int g) {
    if (g < 0 || g >= parallel_games_) {
        throw std::runtime_error("Selection is out of array.");
    }
}
