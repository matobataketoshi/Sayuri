#include "pattern/mm_trainer.h"
#include "game/sgf.h"
#include "game/iterator.h"
#include "utils/format.h"
#include "utils/log.h"

#include <algorithm>
#include <functional>

constexpr int MmTrainer::kMmMaxPatternDist;
constexpr int MmTrainer::kMmMinPatternDist;

MmTrainer& MmTrainer::Get() {
    static MmTrainer mm_trainer;
    return mm_trainer;
}

void MmTrainer::Run(std::string sgf_name, std::string out_name, int min_count) {
    auto sgfs = SgfParser::Get().ChopAll(sgf_name);

    num_patterns_ = 0;
    const int num_features = kMmMaxPatternDist + Board::GetMaxFeatures() + 1;

    feature_spat_dicts_.resize(num_features);
    feature_orders_.resize(num_features);
    feature_order_dicts_.resize(num_features);
    feature_counters_.resize(num_features);

    // Gather the mm patterns.
    for (const auto &sgf_string : sgfs) {
        FillPatterns(sgf_string);
    }

    if (num_patterns_ == 0) {
        return;
    }

    FilterPatterns(min_count);

    InitMm();

    // Fill the mm participant.
    for (const auto &sgf_string : sgfs) {
       FillMmParticipant(sgf_string);
    }

    // Start training...
    mm_->StartTraining();

    // Save the training file.
    SaveResult(out_name);

    // Release the training data.
    mm_.reset(nullptr);
}

void MmTrainer::InitMm() {
    auto size = feature_counters_.size();
    auto features = std::vector<int>{};

    for (int i = 0; i < (int)size; ++i) {
        FeatureConuter &counter = feature_counters_[i];
        features.emplace_back(counter.size());
    }

    auto names = std::vector<std::string>{};

    names.emplace_back("NA");
    names.emplace_back("NA");

    names.emplace_back("s2");
    names.emplace_back("s3");
    names.emplace_back("s4");
    names.emplace_back("s5");
    names.emplace_back("s6");
    names.emplace_back("s7");
    names.emplace_back("s8");
    names.emplace_back("s9");
    names.emplace_back("s10");
    names.emplace_back("border");
    names.emplace_back("dist");
    names.emplace_back("dist2");
    names.emplace_back("capture");
    names.emplace_back("atari");
    names.emplace_back("self-atari");

    mm_ = std::make_unique<MinorizationMaximization>();
    mm_->Initialize(features, names);
}

void MmTrainer::FilterPatterns(int select_min_count) {
    constexpr int kMinCount = 3;
    constexpr int kMaxSize = 30 * 1000;

    select_min_count = std::max(kMinCount, select_min_count);

    std::vector<FeatureSpatDict>  filtered_feature_spat_dicts;
    std::vector<FeatureOrder>     filtered_feature_orders;
    std::vector<FeatureOrderDict> filtered_feature_order_dicts;
    std::vector<FeatureConuter>   filtered_feature_counters;

    // compute features
    auto size = feature_counters_.size();
    auto num_features_list = std::vector<int>{};

    for (int i = 0; i < (int)size; ++i) {
        FeatureConuter &counter = feature_counters_[i];
        num_features_list.emplace_back(counter.size());
    }

    // compute min count
    auto all_counts = std::vector<int>{};
    for (int i = 0; i < (int)size; ++i) {
        auto &counter = feature_counters_[i];
        for (auto c : counter) {
            all_counts.emplace_back(c);
        }
    }
    std::sort(std::begin(all_counts), std::end(all_counts), std::greater<int>());

    const int max_size = std::min<int>(kMaxSize, all_counts.size());
    const int min_count = std::max<int>(all_counts[max_size - 1], select_min_count);

    // resize filtered buffer
    filtered_feature_spat_dicts.resize(size);
    filtered_feature_orders.resize(size);
    filtered_feature_order_dicts.resize(size);
    filtered_feature_counters.resize(size);

    // filter them
    for (int i = 0; i < (int)size; ++i) {
        const auto fsize = num_features_list[i];
        auto &spat_dict  = feature_spat_dicts_[i];
        auto &order      = feature_orders_[i];
        // auto &order_dict = feature_order_dicts_[i];
        auto &counter    = feature_counters_[i];

        if (fsize <= 0) continue;

        for (int index = 0; index < fsize; ++index) {
            if (counter[index] >= min_count) {
                auto &filtered_spat_dict  = filtered_feature_spat_dicts[i];
                auto &filtered_order      = filtered_feature_orders[i];
                auto &filtered_order_dict = filtered_feature_order_dicts[i];
                auto &filtered_counter    = filtered_feature_counters[i];

                const auto new_index = filtered_counter.size();
                const auto hash = order[index];
                const auto spat = spat_dict.find(hash)->second;
                const auto cnt = counter[index];

                filtered_spat_dict.insert({hash, spat});
                filtered_order.emplace_back(hash);
                filtered_order_dict.insert({hash, new_index});
                filtered_counter.emplace_back(cnt);
            }
        }
    }

    std::swap(filtered_feature_spat_dicts, feature_spat_dicts_);
    std::swap(filtered_feature_orders, feature_orders_);
    std::swap(filtered_feature_order_dicts, feature_order_dicts_);
    std::swap(filtered_feature_counters, feature_counters_);
}

bool MmTrainer::PatternMatch(const Board& board,
                                 int feature, int dist,
                                 int vertex, int color,
                                 std::uint64_t &mhash) const {
    const FeatureSpatDict &spat_dict = feature_spat_dicts_[feature];
    bool matched = false;

    for (int symm = 0; symm < 8; ++symm) {
        auto hash = board.GetSymmetryPatternHash(vertex, color, dist, symm);
        auto it = spat_dict.find(hash);

        if (it != std::end(spat_dict)) {
            // The pattern was already in the dictionary.
            matched = true;
            mhash = hash;
            break;
        }
        if (matched) break;
    }
    return matched;
}

void MmTrainer::FillPatterns(std::string sgfstring) {
    GameState state;

    try {
        state = Sgf::Get().FromString(sgfstring, 9999);
    } catch (const char *err) {
        LOGGING << "Fail to load the SGF file! Discard it." << std::endl
                    << Format("\tCause: %s.", err) << std::endl;
        return;
    }

    auto game_ite = GameStateIterator(state);

    if (game_ite.MaxMoveNumber() == 0) {
        return;
    }

    // Remove the double pass moves in the middle.
    game_ite.RemoveUnusedDoublePass();

    do {
        // gather patterns
        const auto vtx = game_ite.GetVertex();
        if (vtx == kPass) {
            continue;
        }
        auto color = game_ite.GetToMove();
        Board& board = game_ite.GetState().board_;

        for (int pattern_dist = kMmMinPatternDist;
                 pattern_dist <= kMmMaxPatternDist; ++pattern_dist) {

            FeatureSpatDict &spat_dict = feature_spat_dicts_[pattern_dist];
            FeatureOrder &order = feature_orders_[pattern_dist];
            FeatureOrderDict &order_dict = feature_order_dicts_[pattern_dist];
            FeatureConuter &counter = feature_counters_[pattern_dist];

            std::uint64_t mhash;
            bool matched = PatternMatch(board, pattern_dist,
                                            pattern_dist, vtx, color, mhash);

            if (matched) {
                const auto hash = mhash;
                const auto index = order_dict.find(hash)->second;
                counter[index] += 1;
            } else {
                const auto hash = board.GetPatternHash(vtx, kBlack, pattern_dist);
                const auto spat = board.GetPatternSpat(vtx, kBlack, pattern_dist);
                const auto index = order.size();

                spat_dict.insert({hash, spat});
                order.emplace_back(hash);
                order_dict.insert({hash, index});
                counter.emplace_back(1);
                num_patterns_ += 1;
            }
        }

        // gather board features
        const auto ProcessWrapper = [this](Board &board,
                                               int bf, int mvtx, int color, int feature_idx) {
            std::uint64_t mhash;
            if (board.GetFeatureWrapper(bf, mvtx, color, mhash)) {
                FeatureSpatDict &spat_dict = feature_spat_dicts_[feature_idx];
                FeatureOrder &order = feature_orders_[feature_idx];
                FeatureOrderDict &order_dict = feature_order_dicts_[feature_idx];
                FeatureConuter &counter = feature_counters_[feature_idx];

                bool matched = spat_dict.find(mhash) != std::end(spat_dict);

                if (matched) {
                    const auto index = order_dict.find(mhash)->second;
                    counter[index] += 1;
                } else {
                    const auto index = order.size();

                    spat_dict.insert({mhash, std::to_string(mhash)});
                    order.emplace_back(mhash);
                    order_dict.insert({mhash, index});
                    counter.emplace_back(1);
                    num_patterns_ += 1;
                }
            }
        };

        const auto offset = kMmMaxPatternDist+1;
        for (int i = 0; i < Board::GetMaxFeatures(); ++i) {
            ProcessWrapper(board, i, vtx, color, offset+i);
        }
    } while (game_ite.Next());
}

void MmTrainer::FillMmParticipant(std::string sgfstring) {
    GameState state;

    try {
        state = Sgf::Get().FromString(sgfstring, 9999);
    } catch (const char *err) {
        LOGGING << "Fail to load the SGF file! Discard it." << std::endl
                    << Format("\tCause: %s.", err) << std::endl;
        return;
    }

    auto game_ite = GameStateIterator(state);

    if (game_ite.MaxMoveNumber() == 0) {
        return;
    }

    // Remove the double pass moves in the middle.
    game_ite.RemoveUnusedDoublePass();

    do {
        ParticipantGroup part;
        part.winner_team_idx = -1;

        auto winner_vtx = game_ite.GetVertex();
        if (winner_vtx == kPass) {
            continue;
        }

        auto color = game_ite.GetToMove();
        Board& board = game_ite.GetState().board_;

        const int empty_cnt = board.GetEmptyCount();
        for (int i = 0; i < empty_cnt; ++i) {
            const auto vtx = board.GetEmpty(i);
            if (board.IsLegalMove(vtx, color)) {
                ParticipantGroup::GammasTeam team;

                // gather patterns
                for (int pattern_dist = kMmMinPatternDist;
                         pattern_dist <= kMmMaxPatternDist; ++pattern_dist) {
                    std::uint64_t mhash;
                    bool matched = PatternMatch(board, pattern_dist,
                                                    pattern_dist, vtx, color, mhash);

                    if (matched) {
                        auto &order_dict = feature_order_dicts_[pattern_dist];
                        int matched_index = order_dict.find(mhash)->second;

                        ParticipantGroup::GammaLoc gloc(pattern_dist, matched_index);
                        team.emplace_back(gloc);
                    }
                }

                // gather board features
                const auto ProcessWrapper = [this](Board &board,
                                                       int bf, int mvtx, int color, int feature_idx,
                                                       ParticipantGroup::GammasTeam &team) {
                    std::uint64_t mhash;
                    if (board.GetFeatureWrapper(bf, mvtx, color, mhash)) {
                        auto &order_dict = feature_order_dicts_[feature_idx];
                        auto it = order_dict.find(mhash);
                        bool matched = it != std::end(order_dict);

                        if (matched) {
                            int matched_index = order_dict.find(mhash)->second;

                            ParticipantGroup::GammaLoc gloc(feature_idx, matched_index);
                            team.emplace_back(gloc);
                        }
                    }
                };

                const auto offset = kMmMaxPatternDist+1;
                for (int i = 0; i < Board::GetMaxFeatures(); ++i) {
                    ProcessWrapper(board, i, vtx, color, offset+i, team);
                }

                if (!team.empty()) {
                    part.all_teams.emplace_back(team);
                    if (vtx == winner_vtx) {
                        // It is the winner team.
                        part.winner_team_idx = 0;
                        int last = part.all_teams.size() - 1;
                        std::swap(part.all_teams[part.winner_team_idx], part.all_teams[last]);
                    }
                }
            }
        }
        if (part.winner_team_idx >= 0) {
            mm_->AppendParticipantGroup(part);
        }
    } while (game_ite.Next());
}

void MmTrainer::SaveResult(std::string filename) {
    std::ofstream file(filename, std::ofstream::out);
    if (!file.is_open()) {
        return;
    }

    for (int feature = 0; feature < (int)feature_orders_.size(); ++feature) {
        FeatureSpatDict &spat_dict = feature_spat_dicts_[feature];
        FeatureOrder &order = feature_orders_[feature];

        for (int index = 0; index < (int)order.size(); ++index) {
            float gamma = mm_->GetMmGamma(feature, index).gamma;
            std::uint64_t hash = order[index];
            std::string spat = spat_dict.find(hash)->second;
            int dist = feature;

            if (dist <= kMmMaxPatternDist) {
                file << gamma << ' ' << dist << ' ' << spat << '\n';
            } else {
                // not patterns
                file << gamma << ' ' << 0 << ' ' << spat << '\n';
            }
        }
    }
    file.close();
}
