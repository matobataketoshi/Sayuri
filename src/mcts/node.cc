#include "mcts/node.h"
#include "mcts/lcb.h"
#include "mcts/rollout.h"
#include "utils/atomic.h"
#include "utils/random.h"
#include "utils/format.h"
#include "game/symmetry.h"

#include <cassert>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <stack>

#define VIRTUAL_LOSS_COUNT (3)

Node::Node(std::int16_t vertex, float policy) {
    vertex_ = vertex;
    policy_ = policy;
}

Node::~Node() {
    assert(GetThreads() == 0);
    ReleaseAllChildren();
}

bool Node::PrepareRootNode(Network &network,
                               GameState &state,
                               NodeEvals &node_evals,
                               AnalysisConfig &config) {
    const auto is_root = true;
    const auto success = ExpandChildren(network, state, node_evals, config, is_root);
    assert(HaveChildren());

    InflateAllChildren();
    if (param_->dirichlet_noise) {
        // Generate the dirichlet noise and gather it.
        const auto legal_move = children_.size();
        const auto factor = param_->dirichlet_factor;
        const auto init = param_->dirichlet_init;
        const auto alpha = init * factor / static_cast<float>(legal_move);

        ApplyDirichletNoise(alpha);
    }

    // Remove all superkos at the root. In the most case,
    // it will help simplify the state.
    KillRootSuperkos(state);

    // Reset the bouns.
    SetScoreBouns(0.f);
    for (auto &child : children_) {
        auto node = child.Get();
        if (param_->first_pass_bonus &&
                child.GetVertex() == kPass) {
            // Half komi bouns may efficiently end the game.
            node->SetScoreBouns(0.5f);
        } else {
            node->SetScoreBouns(0.f);
        }
    }

    return success;
}

bool Node::ExpandChildren(Network &network,
                              GameState &state,
                              NodeEvals &node_evals,
                              AnalysisConfig &config,
                              const bool is_root) {
    // The node must be the first time to expand and is not the terminate node.
    assert(state.GetPasses() < 2);
    if (HaveChildren()) {
        return false;
    }

    // Try to acquire the owner.
    if (!AcquireExpanding()) {
        return false;
    }

    // Get network computation result.
    const float temp = is_root ? param_->root_policy_temp : param_->policy_temp;

    auto raw_netlist = Network::Result{};
    color_ = state.GetToMove();

    if (param_->no_dcnn &&
            !(param_->root_dcnn && is_root)) {
        ApplyNoDcnnPolicy(state, color_, raw_netlist);
    } else {
        raw_netlist = network.GetOutput(state, Network::kRandom, temp);
    }

    // Store the network reuslt.
    ApplyNetOutput(state, raw_netlist, node_evals, color_);

    // For children...
    auto nodelist = std::vector<Network::PolicyVertexPair>{};
    auto allow_pass = true;
    auto legal_accumulate = 0.0f;

    const auto board_size = state.GetBoardSize();
    const auto num_intersections = state.GetNumIntersections();
    const auto safe_area = state.GetStrictSafeArea();

    // For symmetry pruning.
    bool apply_symm_pruning = param_->symm_pruning &&
                                  board_size >= state.GetMoveNumber();
    auto moves_hash = std::vector<std::uint64_t>{};
    auto symm_base_hash = std::vector<std::uint64_t>(Symmetry::kNumSymmetris, 0ULL);

    for (int symm = Symmetry::kIdentitySymmetry;
             apply_symm_pruning && symm < Symmetry::kNumSymmetris; ++symm) {
        symm_base_hash[symm] = state.ComputeSymmetryHash(symm);
    }

    // Prune the illegal moves or some bad move.
    for (int idx = 0; idx < num_intersections; ++idx) {
        const auto x = idx % board_size;
        const auto y = idx / board_size;
        const auto vtx = state.GetVertex(x, y);
        const auto policy = raw_netlist.probabilities[idx];

        // Prune the illegal, unwise and forbidden move.
        int movenum = state.GetMoveNumber();
        if (!state.IsLegalMove(vtx, color_,
                [movenum, &config](int vtx, int color){
                    return !config.IsLegal(vtx, color, movenum);
                }) 
                    || safe_area[idx]) {
            continue;
        }

        // Prune the symmetry moves. May reduce some perfomance.
        if (apply_symm_pruning) {
            bool hash_found = false;
            for (int symm = Symmetry::kIdentitySymmetry+1;
                     symm < Symmetry::kNumSymmetris && !hash_found; ++symm) {
                const auto symm_vtx = Symmetry::Get().TransformVertex(board_size, symm, vtx);
                const auto symm_hash = symm_base_hash[symm] ^ state.GetMoveHash(symm_vtx, color_);
                hash_found = std::end(moves_hash) !=
                                 std::find(std::begin(moves_hash),
                                               std::end(moves_hash), symm_hash);
            }

            if (!hash_found) {
                // Get next game state hash. Is is not correct if the
                // move is capture move. It is ok because we only need
                // move hash in the opening stage. The capture move is
                // unusual in the opening stage.
                moves_hash.emplace_back(
                    state.GetHash() ^ state.GetMoveHash(vtx, color_));
            } else {
                // The pruned node is a legal move. We need accumulate
                // the all legal moves policy.
                legal_accumulate += policy;
                continue;
            }
        }

        nodelist.emplace_back(policy, vtx);
        legal_accumulate += policy;
    }

    // There ara too many legal moves. Disable the pass move.
    if ((int)nodelist.size() > 3*num_intersections/4) {
        allow_pass = false;
    }

    // The pass is always legal. If there is no legal move except for pass, forcing
    // to open the pass node.
    if (allow_pass || nodelist.empty()) {
        nodelist.emplace_back(raw_netlist.pass_probability, kPass);
        legal_accumulate += raw_netlist.pass_probability;
    }

    if (legal_accumulate < 1e-8f) {
        // It will be happened if the policy focuses on the illegal moves.
        for (auto &node : nodelist) {
            node.first = 1.f/nodelist.size();
        }
    } else {
        for (auto &node : nodelist) {
            // Adjust the policy.
            node.first /= legal_accumulate;
        }
    }

    // Extend the nodes.
    LinkNodeList(nodelist);

    // Release the owner.
    ExpandDone();

    return true;
}

void Node::LinkNodeList(std::vector<Network::PolicyVertexPair> &nodelist) {
    // Besure that the best policy is on the top.
    std::stable_sort(std::rbegin(nodelist), std::rend(nodelist));

    for (const auto &node : nodelist) {
        const auto vertex = (std::uint16_t)node.second;
        const auto policy = node.first;
        children_.emplace_back(vertex, policy);
    }
    assert(!children_.empty());
}

void Node::ApplyNetOutput(GameState &state,
                              const Network::Result &raw_netlist,
                              NodeEvals& node_evals, const int color) {
    auto black_ownership = std::array<float, kNumIntersections>{};
    auto black_fs = float(0.f);
    auto draw =raw_netlist.wdl[1];

    // Compute the black side to move evals.
    auto wl = float(0.5f);

    if (param_->use_stm_winrate) {
        wl = raw_netlist.stm_winrate;
    } else {
        wl = (raw_netlist.wdl[0] - raw_netlist.wdl[2] + 1) / 2;
    }

    auto final_score = raw_netlist.final_score;

    if (color == kWhite) {
        wl = 1.0f - wl;
        final_score = 0.0f - final_score;
    }

    black_wl_ = wl;
    black_fs = final_score;

    for (int idx = 0; idx < kNumIntersections; ++idx) {
        auto owner = raw_netlist.ownership[idx];
        if (color == kWhite) {
            owner = 0.f - owner;
        }
        black_ownership[idx] = owner;
        avg_black_ownership_[idx] = 0.f;
    }

    // Do rollout if we disable the DCNN or the DCNN does not
    // support the ownership.
    if (param_->use_rollout || param_->no_dcnn) {
        float mc_black_rollout_score;
        float mc_black_rollout_res = GetBlackRolloutResult(
                                         state,
                                         black_ownership.data(),
                                         mc_black_rollout_score);
        if (param_->no_dcnn) {
            black_wl_ = mc_black_rollout_res;
            black_fs = mc_black_rollout_score;
        }
    }

    // Store the network evals.
    node_evals.black_wl = black_wl_;
    node_evals.draw = draw;
    node_evals.black_final_score = black_fs;

    for (int idx = 0; idx < kNumIntersections; ++idx) {
        node_evals.black_ownership[idx] = black_ownership[idx];
    }
}

void Node::ApplyNoDcnnPolicy(GameState &state, const int color,
                                 Network::Result &raw_netlist) const {
    const auto num_intersections = state.GetNumIntersections();
    auto policy = state.GetGammasPolicy(color);

    for (int idx = 0; idx < num_intersections; ++idx) {
        raw_netlist.probabilities[idx] = policy[idx];
        raw_netlist.ownership[idx] = 0.f; // set zero...
    }

    raw_netlist.board_size = state.GetBoardSize();
    raw_netlist.komi = state.GetKomi();

    // Give the pass move a little value in order to avoid the 
    // bug if there is no legal moves.
    raw_netlist.pass_probability = 0.1f/num_intersections;
    raw_netlist.final_score = 0.f; // set zeros...
    raw_netlist.wdl = {0.5f, 0, 0.5f}; // set draw value...
    raw_netlist.wdl_winrate = 0.5f; // set draw value...
    raw_netlist.stm_winrate = 0.5f; // set draw value...
}

bool Node::SetTerminal() {
    if (!AcquireExpanding()) {
        return false;
    }

    color_ = kInvalid; // no children

    ExpandDone();
    return true;
}

float Node::ComputeKlDivergence() {
    const auto vtx = GetBestMove();
    int parentvisits = 0;
    int best_visits = 0;

    for (const auto &child : children_) {
        const auto node = child.Get();
        if (node && node->IsActive()) {
            const auto visits = node->GetVisits();

            parentvisits += visits;
            if (node->GetVertex() == vtx) {
                best_visits = visits;
            }
        }
    }

    if (parentvisits == best_visits) {
        return 0;
    }
    if (parentvisits == 0 || best_visits == 0) {
        return -1;
    }

    return -std::log((float)best_visits / parentvisits);
}

float Node::ComputeTreeComplexity() {
    const auto visits = GetVisits();
    if (visits <= 1) {
        return 0;
    }

    const auto variance = GetLcbVariance(1.0f, visits);
    const auto stddev = std::sqrt(100 * variance);

    return stddev;
}

Node *Node::ProbSelectChild() {
    WaitExpanded();
    assert(HaveChildren());

    Edge* best_node = nullptr;
    float best_prob = std::numeric_limits<float>::lowest();

    for (auto &child : children_) {
        const auto node = child.Get();
        const bool is_pointer = node != nullptr;

        // The node is pruned or invalid. Skip it.
        if (is_pointer && !node->IsActive()) {
            continue;
        }

        auto prob = child.GetPolicy();

        // The node is expanding. Give it very bad value.
        if (is_pointer && node->IsExpanding()) {
            prob = -1.0f + prob;
        }

        if (prob > best_prob) {
            best_prob = prob;
            best_node = &child;
        }
    }

    Inflate(*best_node);
    return best_node->Get();
}

Node *Node::PuctSelectChild(const int color, const bool is_root) {
    WaitExpanded();
    assert(HaveChildren());
    // assert(color == color_);

    // Apply the Gumbel-Top-k trick here. Mix it with PUCT
    // search. Use the PUCT directly if there is already
    // enough visits (playouts).
    if (is_root && ShouldApplyGumbel()) {
        return GumbelSelectChild(color, false);
    }

    // Gather all parent's visits.
    int parentvisits = 0;
    float total_visited_policy = 0.0f;
    for (auto &child : children_) {
        const auto node = child.Get();
        const bool is_pointer = node != nullptr;

        if (is_pointer && node->IsValid()) {
            // The node status is pruned or active.
            const auto visits = node->GetVisits();
            parentvisits += visits;
            if (visits > 0) {
                total_visited_policy += child.GetPolicy();
            }
        }
    }

    const auto cpuct_init           = param_->cpuct_init;
    const auto cpuct_base_factor    = param_->cpuct_base_factor;
    const auto cpuct_base           = param_->cpuct_base;
    const auto draw_factor          = param_->draw_factor;
    const auto score_utility_factor = param_->score_utility_factor;
    const auto score_utility_div    = param_->score_utility_div;
    const auto noise                = is_root ? param_->dirichlet_noise  : false;
    const auto fpu_reduction_factor = is_root ? param_->fpu_root_reduction : param_->fpu_reduction;

    const float cpuct         = cpuct_init + cpuct_base_factor *
                                                 std::log((float(parentvisits) + cpuct_base + 1) / cpuct_base);
    const float numerator     = std::sqrt(float(parentvisits));
    const float fpu_reduction = fpu_reduction_factor * std::sqrt(total_visited_policy);
    const float fpu_value     = GetNetWL(color) - fpu_reduction;
    const float parent_score  = GetFinalScore(color);

    Edge* best_node = nullptr;
    float best_value = std::numeric_limits<float>::lowest();

    for (auto &child : children_) {
        const auto node = child.Get();
        const bool is_pointer = node != nullptr;

        // The node is pruned or invalid. Skip it.
        if (is_pointer && !node->IsActive()) {
            continue;
        }

        // Apply First Play Urgency (FPU). We should think the value of the 
        // unvisited nodes are same as parent. But the NN-based MCTS tends to
        // search the visited node. So give the unvisited node a little bad
        // value (FPU reduction).
        float q_value = fpu_value;

        float denom = 1.0f;
        float utility = 0.0f; // the utility value

        if (is_pointer) {
            const auto visits = node->GetVisits();

            if (node->IsExpanding()) {
                // Like virtual loss, give it a bad value because there are other
                // threads in this node.
                q_value = -1.0f - fpu_reduction;
            } else if (visits > 0) {
                // Transfer win-draw-loss to side-to-move value (Q value).
                const float eval = node->GetWL(color);
                const float draw_value = node->GetDraw() * draw_factor;
                q_value = eval + draw_value;

                // Heuristic value for score lead.
                utility += score_utility_factor *
                               node->GetScoreUtility(
                                   color, score_utility_div, parent_score);
            }
            denom += visits;
        }


        // PUCT algorithm
        const float psa = GetSearchPolicy(child, noise);
        const float puct = cpuct * psa * (numerator / denom);
        const float value = q_value + puct + utility;
        assert(value > std::numeric_limits<float>::lowest());

        if (value > best_value) {
            best_value = value;
            best_node = &child;
        }
    }

    Inflate(*best_node);
    return best_node->Get();
}

Node *Node::UctSelectChild(const int color, const bool is_root, const GameState &state) {
    WaitExpanded();
    assert(HaveChildren());
    // assert(color == color_);

    (void) is_root;

    int parentvisits = 0;
    const float cpuct = param_->cpuct_init;
    const float parent_qvalue = GetWL(color, false);

    std::vector<Edge*> edge_buf;

    for (auto &child : children_) {
        const auto node = child.Get();
        const bool is_pointer = node != nullptr;
        if (is_pointer && node->IsValid()) {
            // The node status is pruned or active.
            parentvisits += node->GetVisits();
        }
        edge_buf.emplace_back(&child);
    }
    const float numerator = std::log((float)parentvisits + 1);

    Edge* best_node = nullptr;
    float best_value = std::numeric_limits<float>::lowest();

    int width = std::max(ComputeWidth(parentvisits), 1);
    int i = 0;

    //TODO: Sort the 'edge_buf' according to priority value.

    for (auto edge_ptr : edge_buf) {
        auto &child = *edge_ptr;

        if (state.board_.IsCaptureMove(edge_ptr->GetVertex(), color)) {
            width += 1;
        }

        if (++i > width) {
            break;
        }

        const auto node = child.Get();
        const bool is_pointer = node != nullptr;

        // The node is pruned or invalid. Skip it.
        if (is_pointer && !node->IsActive()) {
            continue;
        }

        float q_value = parent_qvalue;
        int visits = 0;

        if (is_pointer) {
            visits = node->GetVisits();

            if (node->IsExpanding()) {
                q_value = -1.0f; // Give it a bad value.
            } else if (visits > 0) {
                q_value = node->GetWL(color);
            }
        }

        // UCT algorithm
        const float denom = 1.0f + visits;
        const float psa = child.GetPolicy();
        const float bouns = 1.0f * std::sqrt(1000.f / ((float)parentvisits + 1000.f)) * psa;
        const float uct = cpuct * std::sqrt(numerator / denom);
        float value = q_value + uct + bouns;
        assert(value > std::numeric_limits<float>::lowest());

        if (value > best_value) {
            best_value = value;
            best_node = edge_ptr;
        }
    }

    Inflate(*best_node);
    return best_node->Get();
}

int Node::RandomizeFirstProportionally(float temp, int min_visits) {
    auto select_vertex = kNullVertex;
    auto accum = float{0.0f};
    auto accum_vector = std::vector<std::pair<float, int>>{};

    for (const auto &child : children_) {
        auto node = child.Get();
        const auto visits = node->GetVisits();
        const auto vertex = node->GetVertex();
        if (visits > min_visits) {
            accum += std::pow((float)visits, (1.0 / temp));
            accum_vector.emplace_back(std::pair<float, int>(accum, vertex));
        }
    }

    if (accum_vector.empty()) {
        if (min_visits > 0) {
            return RandomizeFirstProportionally(temp, 0);
        } else {
            // There is no visits. Reture the best policy move.
            return GetBestMove();
        }
    }

    auto distribution = std::uniform_real_distribution<float>{0.0, accum};
    auto pick = distribution(Random<>::Get());
    auto size = accum_vector.size();

    for (auto idx = size_t{0}; idx < size; ++idx) {
        if (pick < accum_vector[idx].first) {
            select_vertex = accum_vector[idx].second;
            break;
        }
    }

    return select_vertex;
}

void Node::Update(const NodeEvals *evals) {
    auto WelfordDelta = [](double eval,
                               double old_acc_eval,
                               int old_visits) {
        const double old_delta = old_visits > 0 ? eval - old_acc_eval / old_visits : 0.0f;
        const double new_delta = eval - (old_acc_eval + eval) / (old_visits+1);
        const double delta = old_delta * new_delta;
        return delta;
    };

    // type casting
    const double eval = evals->black_wl;
    const double draw = evals->draw;
    const double black_final_score = evals->black_final_score;
    const double old_acc_eval = accumulated_black_wl_.load(std::memory_order_relaxed);

    const int old_visits = visits_.load(std::memory_order_relaxed);

    // TODO: According to Kata Go, It is not necessary to use
    //       Welford's online algorithm. The accuracy of simplify
    //       algorithm is enough.
    // Welford's online algorithm for calculating variance.
    const double delta = WelfordDelta(eval, old_acc_eval, old_visits);

    visits_.fetch_add(1, std::memory_order_relaxed);
    AtomicFetchAdd(squared_eval_diff_   , delta);
    AtomicFetchAdd(accumulated_black_wl_, eval);
    AtomicFetchAdd(accumulated_draw_    , draw);
    AtomicFetchAdd(accumulated_black_fs_, black_final_score);

    {
        std::lock_guard<std::mutex> lock(os_mtx_);
        for (int idx = 0; idx < kNumIntersections; ++idx) {
            const double eval_owner = evals->black_ownership[idx];
            const double avg_owner  = avg_black_ownership_[idx];
            const double diff_owner = (eval_owner - avg_owner) / (old_visits+1);

            avg_black_ownership_[idx] += diff_owner;
        }
    }
}

void Node::ApplyEvals(const NodeEvals *evals) {
    black_wl_ = evals->black_wl;
}

std::array<float, kNumIntersections> Node::GetOwnership(int color) {
    std::lock_guard<std::mutex> lock(os_mtx_);

    auto out = std::array<float, kNumIntersections>{};
    for (int idx = 0; idx < kNumIntersections; ++idx) {
        auto owner = avg_black_ownership_[idx];
        if (color == kWhite) {
            owner = 0.f - owner;
        }
        out[idx] = owner;
    }
    return out;
}

float Node::GetScoreUtility(const int color,
                            float div,
                            float parent_score) const {
    const auto score =
        GetFinalScore(color) + score_bouns_;
    return std::tanh((score - parent_score)/div);
}

float Node::GetLcbVariance(const float default_var, const int visits) const {
    return visits > 1 ?
               squared_eval_diff_.load(std::memory_order_relaxed) / (visits - 1) :
               default_var;
}

float Node::GetLcb(const int color) const {
    // The Lower confidence bound of winrate.
    // See the LCB issues here: https://github.com/leela-zero/leela-zero/pull/2290

    const auto visits = GetVisits();
    if (visits <= 1) {
        // We can not get the variance in the first visit. Return
        // the large negative value.
        return GetPolicy() - 1e6f;
    }

    const auto mean = GetWL(color, false);
    const auto variance = GetLcbVariance(1.0f, visits);
    const auto stddev = std::sqrt(variance / float(visits));
    const auto z = LcbEntries::Get().CachedTQuantile(visits - 1);

    return mean - z * stddev;
}

std::string Node::ToVerboseString(GameState &state, const int color) {
    auto out = std::ostringstream{};
    const auto lcblist = GetLcbUtilityList(color);
    const auto parentvisits = GetVisits() - 1; // One is root visit.

    if (lcblist.empty()) {
         out << " * Search List: N/A" << std::endl;
        return out.str();
    }

    const auto space1 = 7;
    out << " * Search List:" << std::endl;
    out << std::setw(6) << "move"
            << std::setw(10) << "visits"
            << std::setw(space1) << "WL(%)"
            << std::setw(space1) << "LCB(%)"
            << std::setw(space1) << "D(%)"
            << std::setw(space1) << "P(%)"
            << std::setw(space1) << "N(%)"
            << std::setw(space1) << "S"
            << std::endl;

    for (auto &lcb_pair : lcblist) {
        const auto lcb = lcb_pair.first > 0.0f ? lcb_pair.first : 0.0f;
        const auto vertex = lcb_pair.second;

        auto child = GetChild(vertex);
        const auto visits = child->GetVisits();
        const auto pobability = child->GetPolicy();
        assert(visits != 0);

        const auto final_score = child->GetFinalScore(color);
        const auto eval = child->GetWL(color, false);
        const auto draw = child->GetDraw();

        const auto pv_string = state.VertexToText(vertex) + ' ' + child->GetPvString(state);

        const auto visit_ratio = static_cast<float>(visits) / (parentvisits);
        out << std::fixed << std::setprecision(2)
                << std::setw(6) << state.VertexToText(vertex)  // move
                << std::setw(10) << visits                     // visits
                << std::setw(space1) << eval * 100.f           // win loss eval
                << std::setw(space1) << lcb * 100.f            // LCB eval
                << std::setw(space1) << draw * 100.f           // draw probability
                << std::setw(space1) << pobability * 100.f     // move probability
                << std::setw(space1) << visit_ratio * 100.f    // visits ratio
                << std::setw(space1) << final_score            // score lead
                << std::setw(6) << "| PV:" << ' ' << pv_string // principal variation
                << std::endl;
    }

    auto nodes = size_t{0};
    auto edges = size_t{0};
    ComputeNodeCount(nodes, edges);

    const auto node_mem = sizeof(Node) + sizeof(Edge);
    const auto edge_mem = sizeof(Edge);

    // There is some error to compute memory used. It is because that
    // we may not collect all node conut. 
    const auto mem_used = static_cast<double>(nodes * node_mem + edges * edge_mem) / (1024.f * 1024.f);

    const auto space2 = 10;
    out << " * Tree Status:" << std::endl
            << std::fixed << std::setprecision(4)
            << std::setw(space2) << "root KL:" << ' ' << ComputeKlDivergence() << std::endl
            << std::setw(space2) << "root C:"  << ' ' << ComputeTreeComplexity() << std::endl
            << std::setw(space2) << "nodes:"   << ' ' << nodes    << std::endl
            << std::setw(space2) << "edges:"   << ' ' << edges    << std::endl
            << std::setw(space2) << "memory:"  << ' ' << mem_used << ' ' << "(MiB)" << std::endl;

    return out.str();
}

std::string Node::OwnershipToString(GameState &state, const int color, std::string name, Node *node) {
    auto out = std::ostringstream{};
    const auto board_size = state.GetBoardSize();

    auto ownership = node->GetOwnership(color);

    out << name << ' ';
    for (int y = board_size-1; y >= 0; --y) {
        for (int x = 0; x < board_size; ++x) {
            out << Format("%.6f ", ownership[state.GetIndex(x,y)]);
        }
    }

    return out.str();
}

std::string Node::ToAnalysisString(GameState &state,
                                       const int color,
                                       AnalysisConfig &config) {
    // Gather the analysis string. You can see the detail here
    // https://github.com/SabakiHQ/Sabaki/blob/master/docs/guides/engine-analysis-integration.md

    auto out = std::ostringstream{};
    const auto lcblist = GetLcbUtilityList(color);

    if (lcblist.empty()) {
        return std::string{};
    }

    const auto root_visits = static_cast<float>(GetVisits() - 1);

    bool is_sayuri = config.is_sayuri;
    bool is_kata = config.is_kata;
    bool use_ownership = config.ownership;
    bool use_moves_ownership = config.moves_ownership;

    int order = 0;
    for (auto &lcb_pair : lcblist) {
        if (order+1 > config.max_moves) {
            break;
        }

        const auto lcb = lcb_pair.first > 0.0f ? lcb_pair.first : 0.0f;
        const auto vertex = lcb_pair.second;

        auto child = GetChild(vertex);
        const auto final_score = child->GetFinalScore(color);
        const auto winrate = child->GetWL(color, false);
        const auto visits = child->GetVisits();
        const auto prior = child->GetPolicy();
        const auto pv_string = state.VertexToText(vertex) + ' ' + child->GetPvString(state);

        if (param_->no_dcnn &&
                visits/root_visits < 0.01f) { // cut off < 1% children...
            continue;
        }

        if (is_sayuri) {
            const auto kl = child->ComputeKlDivergence();
            const auto complexity = child->ComputeTreeComplexity();
            out << Format("info move %s visits %d winrate %.6f scorelead %.6f prior %.6f lcb %.6f kl %.6f complexity %.6f order %d pv %s",
                             state.VertexToText(vertex).c_str(),
                             visits,
                             winrate,
                             final_score,
                             prior,
                             lcb,
                             kl,
                             complexity,
                             order,
                             pv_string.c_str()
                         );
        } else if (is_kata) {
            out << Format("info move %s visits %d winrate %.6f scoreLead %.6f prior %.6f lcb %.6f order %d pv %s",
                             state.VertexToText(vertex).c_str(),
                             visits,
                             winrate,
                             final_score,
                             prior,
                             lcb,
                             order,
                             pv_string.c_str()
                         );
        } else {
            out << Format("info move %s visits %d winrate %d scoreLead %.6f prior %d lcb %d order %d pv %s",
                             state.VertexToText(vertex).c_str(),
                             visits,
                             std::min(10000, (int)(10000 * winrate)),
                             final_score,
                             std::min(10000, (int)(10000 * prior)),
                             std::min(10000, (int)(10000 * lcb)),
                             order,
                             pv_string.c_str()
                         );
        }
        if (use_moves_ownership) {
            if (is_sayuri) {
                out << OwnershipToString(state, color, "movesownership", child);
            } else {
                out << OwnershipToString(state, color, "movesOwnership", child);
            }
        }
        order += 1;
    }

    if (use_ownership) {
        out << OwnershipToString(state, color, "ownership", this->Get());
    }

    out << std::endl;

    return out.str();
}

std::string Node::GetPvString(GameState &state) {
    auto pvlist = std::vector<int>{};
    auto *next = this;
    while (next->HaveChildren()) {
        const auto vtx = next->GetBestMove();
        pvlist.emplace_back(vtx);
        next = next->GetChild(vtx);
    }
  
    auto res = std::string{};
    for (const auto &vtx : pvlist) {
        res += state.VertexToText(vtx);
        res += " ";
    }
    return res;
}

Node *Node::Get() {
    return this;
}

Node *Node::GetChild(const int vertex) {
    for (auto & child : children_) {
        if (vertex == child.GetVertex()) {
            Inflate(child);
            return child.Get();
        }
    }
    return nullptr;
}

Node *Node::PopChild(const int vertex) {
    auto node = GetChild(vertex);
    if (node) {
        auto ite = std::remove_if(std::begin(children_), std::end(children_),
                                  [node](Edge &ele) {
                                      return ele.Get() == node;
                                  });
        children_.erase(ite, std::end(children_));
    }
    return node;
}

std::vector<std::pair<float, int>> Node::GetLcbUtilityList(const int color) {
    WaitExpanded();
    assert(HaveChildren());

    const auto lcb_utility_factor = std::max(0.f, param_->lcb_utility_factor);
    const auto lcb_reduction = std::min(
                                   std::max(0.f, param_->lcb_reduction), 1.f);
    int parentvisits = 0;
    const auto score = GetFinalScore(color);
    const auto score_utility_div = param_->score_utility_div;

    auto list = std::vector<std::pair<float, int>>{};

    for (const auto & child : children_) {
        const auto node = child.Get();
        const bool is_pointer = node != nullptr;

        if (is_pointer && node->IsActive()) {
            parentvisits += node->GetVisits();
        }
    }

    for (const auto & child : children_) {
        const auto node = child.Get();
        const bool is_pointer = node != nullptr;

        // The node is uninflated, pruned or invalid. Skip it.
        if (!is_pointer || !node->IsActive()) {
            continue;
        }

        const auto visits = node->GetVisits();
        if (visits > 0) {
            auto lcb = node->GetLcb(color);
            auto utility = lcb_utility_factor *
                               node->GetScoreUtility(
                                   color, score_utility_div, score);
            const auto ulcb = (lcb + utility) * (1.f - lcb_reduction) + 
                                  lcb_reduction * ((float)visits/parentvisits);
            list.emplace_back(ulcb, node->GetVertex());
        }
    }

    std::stable_sort(std::rbegin(list), std::rend(list));
    return list;
}

int Node::GetBestMove() {
    WaitExpanded();
    assert(HaveChildren());

    auto lcblist = GetLcbUtilityList(color_);
    float best_value = std::numeric_limits<float>::lowest();
    int best_move = kNullVertex;

    for (auto &entry : lcblist) {
        const auto lcb = entry.first;
        const auto vtx = entry.second;
        if (lcb > best_value) {
            best_value = lcb;
            best_move = vtx;
        }
    }

    if (lcblist.empty() && HaveChildren()) {
        best_move = ProbSelectChild()->GetVertex();
    }

    assert(best_move != kNullVertex);
    return best_move;
}

const std::vector<Node::Edge> &Node::GetChildren() const {
    return children_;
}

void Node::SetParameters(Parameters * param) {
    param_ = param;
}

int Node::GetVirtualLoss() const {
    return VIRTUAL_LOSS_COUNT *
               running_threads_.load(std::memory_order_relaxed);
}

int Node::GetVertex() const {
    return vertex_;
}

float Node::GetPolicy() const {
    return policy_;
}

int Node::GetVisits() const {
    return visits_.load(std::memory_order_relaxed);
}

float Node::GetFinalScore(const int color) const {
    auto score = accumulated_black_fs_.load(std::memory_order_relaxed) / GetVisits();

    if (color == kBlack) {
        return score;
    }
    return 0.0f - score;
}

float Node::GetDraw() const {
    return accumulated_draw_.load(std::memory_order_relaxed) / GetVisits();
}

float Node::GetNetWL(const int color) const {
    if (color == kBlack) {
        return black_wl_;
    }
    return 1.0f - black_wl_;
}

float Node::GetWL(const int color, const bool use_virtual_loss) const {
    auto virtual_loss = 0;

    if (use_virtual_loss) {
        // Punish the node if there are some threads in this 
        // sub-tree.
        virtual_loss = GetVirtualLoss();
    }

    auto visits = GetVisits() + virtual_loss;
    auto accumulated_wl = accumulated_black_wl_.load(std::memory_order_relaxed);
    if (color == kWhite && use_virtual_loss) {
        accumulated_wl += static_cast<double>(virtual_loss);
    }
    auto eval = accumulated_wl / visits;

    if (color == kBlack) {
        return eval;
    }
    return 1.0f - eval;
}

void Node::InflateAllChildren() {
    for (auto &child : children_) {
         Inflate(child);
    }
}

void Node::ReleaseAllChildren() {
    for (auto &child : children_) {
         Release(child);
    }
}

void Node::Inflate(Edge& child) {
    if (child.Inflate()) {
        child.Get()->SetParameters(param_);
    }
}

void Node::Release(Edge& child) {
    if (child.Release()) {
        // do nothing...
    }
}

bool Node::HaveChildren() const { 
    return color_ != kInvalid;
}

void Node::IncrementThreads() {
    running_threads_.fetch_add(1, std::memory_order_relaxed);
}

void Node::DecrementThreads() {
    running_threads_.fetch_sub(1, std::memory_order_relaxed);
}

void Node::SetActive(const bool active) {
    if (IsValid()) {
        StatusType v = active ? StatusType::kActive : StatusType::kPruned;
        status_.store(v, std::memory_order_relaxed);
    }
}

void Node::Invalidate() {
    if (IsValid()) {
        status_.store(StatusType::kInvalid, std::memory_order_relaxed);
    }
}

bool Node::IsPruned() const {
    return status_.load(std::memory_order_relaxed) == StatusType::kPruned;
}

bool Node::IsActive() const {
    return status_.load(std::memory_order_relaxed) == StatusType::kActive;
}

bool Node::IsValid() const {
    return status_.load(std::memory_order_relaxed) != StatusType::kInvalid;
}

bool Node::AcquireExpanding() {
    auto expected = ExpandState::kInitial;
    auto newval = ExpandState::kExpanding;
    return expand_state_.compare_exchange_strong(expected, newval, std::memory_order_acquire);
}

void Node::ExpandDone() {
    auto v = expand_state_.exchange(ExpandState::kExpanded, std::memory_order_release);
#ifdef NDEBUG
    (void) v;
#endif
    assert(v == ExpandState::kExpanding);
}

void Node::ExpandCancel() {
    auto v = expand_state_.exchange(ExpandState::kInitial, std::memory_order_release);
#ifdef NDEBUG
    (void) v;
#endif
    assert(v == ExpandState::kExpanding);
}

void Node::WaitExpanded() const {
    while (true) {
        auto v = expand_state_.load(std::memory_order_acquire);
        if (v == ExpandState::kExpanded) {
            break;
        }

        // Wait some time to avoid busy waiting.
        std::this_thread::yield();
    }
}

bool Node::Expandable() const {
    return expand_state_.load(std::memory_order_relaxed) == ExpandState::kInitial;
}

bool Node::IsExpanding() const {
    return expand_state_.load(std::memory_order_relaxed) == ExpandState::kExpanding;
}

bool Node::IsExpanded() const {
    return expand_state_.load(std::memory_order_relaxed) == ExpandState::kExpanded;
}

void Node::ApplyDirichletNoise(const float alpha) {
    auto child_cnt = children_.size();
    auto buffer = std::vector<float>(child_cnt);
    auto gamma = std::gamma_distribution<float>(alpha, 1.0f);

    std::generate(std::begin(buffer), std::end(buffer),
                      [&gamma] () { return gamma(Random<>::Get()); });

    auto sample_sum =
        std::accumulate(std::begin(buffer), std::end(buffer), 0.0f);

    auto &dirichlet = param_->dirichlet_buffer;
    dirichlet.fill(0.0f);

    // If the noise vector sums to 0 or a denormal, then don't try to
    // normalize.
    if (sample_sum < std::numeric_limits<float>::min()) {
        std::fill(std::begin(buffer), std::end(buffer), 0.0f);
        return;
    }

    for (auto &v : buffer) {
        v /= sample_sum;
    }

    for (auto i = size_t{0}; i < child_cnt; ++i) {
        const auto vertex = children_[i].GetVertex();
        dirichlet[vertex] = buffer[i];
    }
}

float Node::GetSearchPolicy(Node::Edge& child, bool noise) {
    auto policy = child.GetPolicy();
    if (noise) {
        const auto vertex = child.GetVertex();
        const auto epsilon = param_->dirichlet_epsilon;
        const auto eta_a = param_->dirichlet_buffer[vertex];
        policy = policy * (1 - epsilon) + epsilon * eta_a;
    }
    return policy;
}

void Node::SetVisits(int v) {
    visits_.store(v, std::memory_order_relaxed);
}

void Node::SetPolicy(float p) {
    policy_ = p;
}

void Node::ComputeNodeCount(size_t &nodes, size_t &edges) {
    // Use DFS to search all nodes.
    auto stk = std::stack<Node *>{};

    // Start search from this node.
    stk.emplace(Get());
    nodes++;

    while (!stk.empty()) {
        Node * node = stk.top();
        stk.pop();

        const auto &children = node->GetChildren();

        // Because we want compute the memory used, collect
        // all types of nodes. Including pruned and invalid node.
        for (const auto &child : children) {
            node = child.Get();
            const bool is_pointer = node != nullptr;

            if (is_pointer) {
                if (!(node->IsExpanding())) {
                    // If the node is expanding, skip the
                    // the node.
                    stk.emplace(node);
                }
                nodes++;
            } else {
                edges++;
            }
        }
    }
}

float Node::GetGumbelQValue(int color, float parent_score) const {
    // Get non-normalized complete Q value. In the original
    // paper, it is Q value. We mixe Q value and score lead
    // in order to optimize the move probabilities if one side
    // has won the game.
    const auto score_utility_div = param_->score_utility_div;
    const auto completed_q_utility_factor = param_->completed_q_utility_factor;
    const auto mixed_q = GetWL(color, false) +
                             completed_q_utility_factor *
                                 GetScoreUtility(
                                     color, score_utility_div, parent_score);
    return mixed_q;
}

float Node::NormalizeCompletedQ(const float completed_q,
                                    const int max_visits) const {
    // The transformation progressively increases the scale for
    // Q value and reduces the effect of the prior policy.
    return (50.f + max_visits) * 0.1f * completed_q;
}

std::vector<float> Node::GetProbLogitsCompletedQ(GameState &state) {
    const auto num_intersections = state.GetNumIntersections();
    auto prob = std::vector<float>(num_intersections+1, 0.f);
    float acc = 0.f;

    for (auto & child : children_) {
        const auto vtx = child.GetVertex();
        int idx = num_intersections; // pass move
        if (vtx != kPass) {
            idx = state.GetIndex(
                      state.GetX(vtx), state.GetY(vtx));
        }
        acc += child.GetPolicy();
        prob[idx] = child.GetPolicy(); 
    }

    for (auto &v : prob) {
        v /= acc;
    }

    MixLogitsCompletedQ(state, prob);
    return prob;
}

void Node::MixLogitsCompletedQ(GameState &state, std::vector<float> &prob) {
    const auto num_intersections = state.GetNumIntersections();
    const auto color = state.GetToMove();

    if (num_intersections+1 != (int)prob.size()) {
        return;
    }

    const auto parent_score = GetFinalScore(color);
    auto logits_q = std::vector<float>(num_intersections+1, -1e6f);

    int max_visits = 0;
    int parentvisits = 0;
    float weighted_q = 0.f;
    float weighted_pi = 0.f;

    // Gather some basic informations.
    for (auto & child : children_) {
        const auto node = child.Get();
        const bool is_pointer = node != nullptr;

        int visits = 0;
        if (is_pointer && node->IsActive()) {
            visits = node->GetVisits();
        }
        parentvisits += visits;
        max_visits = std::max(max_visits, visits);

       if (visits > 0) {
           weighted_q += child.GetPolicy() * 
                             node->GetGumbelQValue(color, parent_score);
           weighted_pi += child.GetPolicy();
       }
    }

    // Compute the all children's completed Q.
    auto completed_q_list = std::vector<float>{};
    float max_completed_q = std::numeric_limits<float>::min();
    float min_completed_q = std::numeric_limits<float>::max();
    float raw_value = this->GetGumbelQValue(color, parent_score);

    for (auto & child : children_) {
        const auto node = child.Get();
        const bool is_pointer = node != nullptr;

        int visits = 0;
        if (is_pointer && node->IsActive()) {
            visits = node->GetVisits();
        }

        float completed_q;
        if (visits == 0) {
            // Use the mixed value instead of raw value network evaluation. It
            // is a approximate value.
            completed_q = (raw_value + (parentvisits/weighted_pi) *
                               weighted_q) / (1 + parentvisits);
        } else {
            completed_q = node->GetGumbelQValue(color, parent_score);
        }
        completed_q_list.emplace_back(completed_q);

        max_completed_q = std::max(max_completed_q, completed_q);
        min_completed_q = std::min(min_completed_q, completed_q);
    }

    // Rescale the the completed Q.
    for (auto &q : completed_q_list) {
        q = (q - min_completed_q) /
                std::max(max_completed_q - min_completed_q, 1e-8f);
    }

    // Apply the completed Q with policy.
    int completed_q_idx = 0;
    for (auto & child : children_) {
        const auto vtx = child.GetVertex();
        int idx = num_intersections; // pass move
        if (vtx != kPass) {
            idx = state.GetIndex(
                      state.GetX(vtx), state.GetY(vtx));
        }

        const float logits = std::log((double)prob[idx] + 1e-8f);
        const float completed_q = completed_q_list[completed_q_idx++];
        logits_q[idx] = logits + NormalizeCompletedQ(
                                     completed_q, max_visits);
    }
    prob = Network::Softmax(logits_q, 1.f);

    // Prune the bad policy.
    double psize = prob.size();
    double noise_threshold = 1.f/(psize * psize);
    double o = 0.f;
    for (auto &v : prob) {
        if (v < noise_threshold) {
            v = 0;
        } else {
            o += v;
        }
    }

    for (auto &v : prob) {
        v /= o;
    }
}

void Node::ProcessGumbelLogits(std::vector<float> &gumbel_logits,
                                   const int color,
                                   const int root_visits,
                                   const int max_visists,
                                   const int considered_moves, const float mval,
                                   bool only_max_visit) {

    // The Variant of Sequential Halving algorithm. The input N playous
    // is always log2(considered moves) * (considered moves) for each
    // epoch. It is same as Sequential Halving with Gumbel algorithm if 
    // the playous is low.
    //
    // Round 1.
    // distribution -> 1 | 1 | 1 | 1
    // accumulation -> 1 | 1 | 1 | 1
    //
    // Round 2.
    // distribution -> 2 | 2 | 0 | 0
    // accumulation -> 3 | 3 | 1 | 1
    //
    // Round 3.(first epoch is end)
    // distribution -> 4 | 0 | 0 | 0
    // accumulation -> 7 | 3 | 1 | 1
    //
    // Round 4.
    // distribution -> 1 | 1 | 1 | 1
    // accumulation -> 8 | 4 | 2 | 2
    //
    // Round 5.
    // distribution -> 2  | 2 | 0 | 0
    // accumulation -> 10 | 6 | 2 | 2
    //
    // Round 6. (second epoch is end)
    // distribution -> 4  | 0 | 0 | 0
    // accumulation -> 14 | 6 | 2 | 2

    const int n = std::log2(std::max(1,considered_moves)) + 1;
    const int adj_considered_moves = std::pow(2, n-1); // Be sure that it is pow of 2.

    auto table = std::vector<int>(adj_considered_moves, 0);
    for (int i=0, r=1, w=adj_considered_moves; i < n; ++i, w/=2, r*=2) {
        for (int j = 0; j < w; ++j) {
            for (int k = 0; k < r; ++k) {
                table[adj_considered_moves-j-1] += 1;
            }
        }
    }

    const int visits_per_round = n * adj_considered_moves;
    const int rounds = root_visits / visits_per_round;
    const int visits_this_round = root_visits - rounds * visits_per_round;
    const int m = visits_this_round / adj_considered_moves;

    int height = 0;
    int width = adj_considered_moves;
    int offset = 0;
    for (int i = 0, t = 1; i < m; i++, t *= 2) {
        height += t;
        width /= 2;
        offset += width;
    }

    const auto parent_score = GetFinalScore(color);
    const int idx = offset + root_visits%width;
    const int considered_visists =
        only_max_visit ?
            max_visists :
            table[idx] * rounds + height + 
                (visits_this_round - m*adj_considered_moves)/width;

    for (auto &child : children_) {
        const auto node = child.Get();
        const bool is_pointer = node != nullptr;

        if (is_pointer && !node->IsActive()) {
            continue;
        }

        auto visits = node->GetVisits();
        if (visits == considered_visists) {
            if (visits > 0) {
                gumbel_logits[node->GetVertex()] += 
                    NormalizeCompletedQ(
                        node->GetGumbelQValue(color, parent_score), max_visists);
            }
            // Each completed Q value is same if the considered visists is
            // zero. To do nothing is Ok.
        } else {
            gumbel_logits[node->GetVertex()] = mval;
        }
    }
}

bool Node::ShouldApplyGumbel() const {
    // We simply think the parent's visits is
    // current visits. Ignore the pruned node.
    const int visits = GetVisits() - 1;
    return param_->gumbel &&
               param_->gumbel_playouts > visits;
}

Node *Node::GumbelSelectChild(int color, bool only_max_visit) {
    WaitExpanded();
    assert(HaveChildren());

    auto gumbel_type1 = std::extreme_value_distribution<float>(0, 1);
    auto gumbel_logits = std::vector<float>(kNumVertices+10, -1e6f);
    int parentvisits = 0;
    int max_visits = 0;

    // Gather all parent's visits.
    for (auto &child : children_) {
        const auto node = child.Get();
        const bool is_pointer = node != nullptr;

        gumbel_logits[child.GetVertex()] =
            gumbel_type1(Random<>::Get()) +
                std::log((double)(child.GetPolicy()) + 1e-8f);

        if (is_pointer && node->IsValid()) {
            // The node status is pruned or active.
            const auto visits = node->GetVisits();
            parentvisits += visits;
            max_visits = std::max(max_visits, visits);
        }
    }

    const int considered_moves =
        std::min(param_->gumbel_considered_moves, (int)children_.size());
    ProcessGumbelLogits(gumbel_logits, color,
                            parentvisits, max_visits,
                            considered_moves, -1e6f,
                            only_max_visit);

    Edge* best_node = nullptr;
    float best_value = std::numeric_limits<float>::lowest();

    for (auto &child : children_) {
        const auto value = gumbel_logits[child.GetVertex()];
        if (value > best_value) {
            best_value = value;
            best_node = &child;
        }
    }
    Inflate(*best_node);
    return best_node->Get();
}

int Node::GetGumbelMove() {
    WaitExpanded();
    assert(HaveChildren());
    return GumbelSelectChild(color_, true)->GetVertex();
}

void Node::SetScoreBouns(float val) {
    score_bouns_ = val;
}

void Node::KillRootSuperkos(GameState &state) {
    for (const auto &child : children_) {
        const auto vtx = child.GetVertex();

        auto fork_state = state;
        fork_state.PlayMove(vtx);

        if (vtx != kPass &&
                fork_state.IsSuperko()) {
            // Prune the superko move.
            child.Get()->Invalidate();
        }
    }

    auto ite = std::remove_if(std::begin(children_), std::end(children_),
                              [](Edge &ele) {
                                  return !ele.Get()->IsValid();
                              });
    children_.erase(ite, std::end(children_));
}
