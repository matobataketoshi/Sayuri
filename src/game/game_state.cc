#include "game/game_state.h"
#include "game/types.h"
#include "utils/parser.h"
#include "utils/log.h"
#include "utils/random.h"
#include "utils/komi.h"
#include "neural/fast_policy.h"

#include <random>

void GameState::Reset(const int boardsize, const float komi) {
    board_.Reset(boardsize);
    SetKomi(komi);
    ko_hash_history_.clear();
    game_history_.clear();
    ko_hash_history_.emplace_back(GetKoHash());
    game_history_.emplace_back(std::make_shared<Board>(board_));

    winner_ = kUndecide;
}

void GameState::ClearBoard() {
    Reset(GetBoardSize(), GetKomi());
}

void GameState::PlayMoveFast(const int vtx, const int color) {
    if (vtx != kResign) {
        board_.PlayMoveAssumeLegal(vtx, color);
    }
}

bool GameState::PlayMove(const int vtx) {
    return PlayMove(vtx, GetToMove());
}

bool GameState::PlayMove(const int vtx, const int color) {
    if (vtx == kResign) {
        if (color == kBlack) {
            winner_ = kWhiteWon;
        } else {
            winner_ = kBlackWon;
        }
        return true;
    }

    if (IsLegalMove(vtx, color)) {
        board_.PlayMoveAssumeLegal(vtx, color);

        // Cut off unused history.
        ko_hash_history_.resize(GetMoveNumber());
        game_history_.resize(GetMoveNumber());

        ko_hash_history_.emplace_back(GetKoHash());
        game_history_.emplace_back(std::make_shared<Board>(board_));

        return true;
    }
    return false;
}

bool GameState::UndoMove() {
    const auto move_number = GetMoveNumber();
    if (move_number >= 1) {
        // Cut off unused history.
        ko_hash_history_.resize(move_number);
        game_history_.resize(move_number);

        board_ = *game_history_[move_number-1];
        return true;
    }
    return false;
}

int GameState::TextToVertex(std::string text) {
    if (text.size() < 2) {
        return kNullVertex;
    }

    if (text == "PASS" || text == "pass") {
        return kPass;
    } else if (text == "RESIGN" || text == "resign") {
        return kResign;
    }

    int x = -1;
    int y = -1;
    if (text[0] >= 'a' && text[0] <= 'z') {
        x = text[0] - 'a';
        if (text[0] >= 'i')
            x--;
    } else if (text[0] >= 'A' && text[0] <= 'Z') {
        x = text[0] - 'A';
        if (text[0] >= 'I')
            x--;
    }
    auto y_str = std::string{};
    auto skip = bool{false};
    std::for_each(std::next(std::begin(text), 1), std::end(text),
                      [&](const auto in) -> void {
                          if (skip) {
                              return;
                          }
                          if (in >= '0' && in <= '9') {
                              y_str += in;
                          } else {
                              y_str = std::string{};
                              skip = true;
                          }
                      });
    if (!y_str.empty()) {
        y = std::stoi(y_str) - 1;
    }

    if (x == -1 || y == -1) {
        return kNullVertex;
    }
    return board_.GetVertex(x, y);
}

std::string GameState::VertexToSgf(const int vtx) {
    assert(vtx != kNullVertex);

    if (vtx == kPass || vtx == kResign) {
        return std::string{};
    }

    auto out = std::ostringstream{};    
    const auto x = GetX(vtx);
    const auto y = GetY(vtx);

    if (x >= 26) {
        out << static_cast<char>(x - 26 + 'A');
    } else {
        out << static_cast<char>(x + 'a');
    }

    if (y >= 26) {
        out << static_cast<char>(y - 26 + 'A');
    } else {
        out << static_cast<char>(y + 'a');
    }

    return out.str();
}

std::string GameState::VertexToText(const int vtx) {
    assert(vtx != kNullVertex);

    if (vtx == kPass) {
        return "pass";
    }
    if (vtx == kResign) {
        return "resign";
    }

    auto out = std::ostringstream{};    
    const auto x = GetX(vtx);
    const auto y = GetY(vtx);

    auto offset = 0;
    if (static_cast<char>(x + 'A') >= 'I') {
        offset = 1;
    }

    out << static_cast<char>(x + offset + 'A');
    out << y+1;

    return out.str();
}

bool GameState::PlayTextMove(std::string input) {
    int color = kInvalid;
    int vertex = kNullVertex;

    auto parser = CommandParser(input);

    if (parser.GetCount() == 2) {
        const auto color_str = parser.GetCommand(0)->Get<std::string>();
        const auto vtx_str = parser.GetCommand(1)->Get<std::string>();

        if (color_str == "B" || color_str == "b" || color_str == "black") {
            color = kBlack;
        } else if (color_str == "W" || color_str == "w" || color_str == "white") {
            color = kWhite;
        }
        vertex = TextToVertex(vtx_str);
    } else if (parser.GetCount() == 1) {
        const auto vtx_str = parser.GetCommand(0)->Get<std::string>();
        color = board_.GetToMove();
        vertex = TextToVertex(vtx_str);
    }

    if (color == kInvalid || vertex == kNullVertex) {
        return false;
    }

    return PlayMove(vertex, color);
}

std::string GameState::GetStateString() const {
    auto out = std::ostringstream{};
    out << "{";
    out << "Next Player: ";
    if (GetToMove() == kBlack) {
        out << "Black";
    } else if (GetToMove() == kWhite) {
        out << "White";
    } else {
        out << "Error";
    }
    out << ", ";
    out << "Move Number: " << GetMoveNumber() << ", ";
    out << "Komi: " << GetKomi() << ", ";
    out << "Board Size: " << GetBoardSize() << ", ";
    out << "Handicap: " << GetHandicap();

    out << "}" << std::endl;
    return out.str();
}


void GameState::ShowBoard() const {
    ERROR << board_.GetBoardString(board_.GetLastMove(), true);
    ERROR << GetStateString();
}

void GameState::SetWinner(GameResult result) {
    winner_ = result;
}

void GameState::SetFinalScore(float score) {
    black_score_ = score;
}

void GameState::SetKomi(float komi) {
    bool negative = komi < 0.f;
    if (negative) {
        komi = -komi;
    }

    int integer_part = static_cast<int>(komi);
    float float_part = komi - static_cast<float>(integer_part);

    if (IsSameKomi(float_part, 0.f)) {
        komi_half_ = false;
    } else if (IsSameKomi(float_part, 0.5f)) {
        komi_half_ = true;
    } else {
        ERROR << "Only accept for integer komi or half komi." << std::endl;
        return;
    }

    komi_negative_ = negative;
    komi_integer_ = integer_part;

    komi_hash_ = Zobrist::kKomi[komi_integer_];
    if (komi_negative_) {
        komi_hash_ ^= Zobrist::kNegativeKomi;
    }
    if (komi_half_) {
        komi_hash_ ^= Zobrist::kHalfKomi;
    }
}

void GameState::SetToMove(const int color) {
    board_.SetToMove(color);
}

void GameState::SetHandicap(int handicap) {
    handicap_ = handicap;
}

bool GameState::IsGameOver() const {
    return winner_ != kUndecide || GetPasses() >= 2;
}

bool GameState::IsSuperko() const {
    auto first = std::crbegin(ko_hash_history_);
    auto last = std::crend(ko_hash_history_);

    auto res = std::find(++first, last, GetKoHash());

    return res != last;
}

bool GameState::IsLegalMove(const int vertex) const {
    return board_.IsLegalMove(vertex, GetToMove());
}

bool GameState::IsLegalMove(const int vertex, const int color) const {
    return board_.IsLegalMove(vertex, color);
}

bool GameState::IsLegalMove(const int vertex, const int color,
                            std::function<bool(int, int)> AvoidToMove) const {
    return board_.IsLegalMove(vertex, color, AvoidToMove);
}

bool GameState::SetFixdHandicap(int handicap) {
    if (board_.SetFixdHandicap(handicap)) {
        SetHandicap(handicap);

        ko_hash_history_.clear();
        game_history_.clear();

        ko_hash_history_.emplace_back(GetKoHash());
        game_history_.emplace_back(std::make_shared<Board>(board_));
        return true;
    }
    return false;
}

bool GameState::SetFreeHandicap(std::vector<std::string> movelist) {
    auto movelist_vertex = std::vector<int>(movelist.size());
    std::transform(std::begin(movelist), std::end(movelist), std::begin(movelist_vertex),
                       [this](auto text){
                           return TextToVertex(text);
                       }
                   );

    if (board_.SetFreeHandicap(movelist_vertex)) {
        SetHandicap(movelist.size());

        ko_hash_history_.clear();
        game_history_.clear();

        ko_hash_history_.emplace_back(GetKoHash());
        game_history_.emplace_back(std::make_shared<Board>(board_));
        return true;
    }
    return false;
}

std::vector<int> GameState::PlaceFreeHandicap(int handicap) {
    auto stone_list = std::vector<int>{};
    if (board_.SetFixdHandicap(handicap)) {
        SetHandicap(handicap);

        for (auto vtx = 0; vtx < board_.GetNumVertices(); ++vtx) {
            if (GetState(vtx) == kBlack) {
                stone_list.emplace_back(vtx);
            }
        }

        ko_hash_history_.clear();
        game_history_.clear();

        ko_hash_history_.emplace_back(GetKoHash());
        game_history_.emplace_back(std::make_shared<Board>(board_));
    }
    return stone_list;
}

std::vector<int> GameState::GetOwnership() const {
    auto res = std::vector<int>(GetNumIntersections(), kInvalid);

    board_.ComputePassAliveOwnership(res);

    return res;
}

// FillRandomMove assume that both players think the game is end. Now we try to remove the
// dead string.
void GameState::FillRandomMove() {
    const int color = GetToMove();
    const int empty_cnt = board_.GetEmptyCount();
    const int rand = Random<RandomType::kXoroShiro128Plus>::Get().Generate() % empty_cnt;
    int select_move = kPass;

    auto safe_area = std::vector<bool>(GetNumIntersections(), false);
    board_.ComputeSafeArea(safe_area);

    for (int i = 0; i < empty_cnt; ++i) {
        const auto rand_pick = (rand + i) % empty_cnt;
        const auto vtx = board_.GetEmpty(rand_pick);

        if (!IsLegalMove(vtx, color)) {
            continue;
        }
        auto x = GetX(vtx);
        auto y = GetY(vtx);

        if (safe_area[GetIndex(x, y)]) {
            continue;
        }

        if (board_.IsCaptureMove(vtx, color)) {
            select_move = vtx;
            break;
        }
    }

    for (int i = 0; i < empty_cnt; ++i) {
        if (select_move != kPass) break;

        const auto rand_pick = (rand + i) % empty_cnt;
        const auto vtx = board_.GetEmpty(rand_pick);

        if (!IsLegalMove(vtx, color)) {
            continue;
        }

        if (board_.IsRealEye(vtx, color)) {
            continue;
        }

        auto x = GetX(vtx);
        auto y = GetY(vtx);

        if (safe_area[GetIndex(x, y)]) {
            continue;
        }

        if (board_.IsSimpleEye(vtx, color) &&
                !board_.IsCaptureMove(vtx, color) &&
                !board_.IsEscapeMove(vtx, color)) {
            continue;
        }

        select_move = vtx;
    }

    PlayMoveFast(select_move, color);
}

void GameState::PlayRandomMove() {
    auto policy = std::vector<float>{};
    const auto board_size = GetBoardSize();

    policy = FastPolicy::Get().Forward(*this);

    const auto random_prob_move = [](std::vector<std::pair<float, int>> &list) {
        float acc = 0.f;

        for (auto &e : list) {
            acc += e.first;
            e.first = acc;
        }

        auto size = list.size();
        auto rng = Random<RandomType::kXoroShiro128Plus>::Get();
        auto dis = std::uniform_real_distribution<float>(0, acc);
        auto p = dis(rng);

        for (size_t i = 1; i < size; ++i) {
            auto low = list[i-1].first;
            auto high = list[i].first;
          
            if (p >= low && p < high) {
                return list[i].second;
            }
        }
        return list[0].second;
    };

    auto simple_ownership = GetOwnership();

    int color = GetToMove();
    auto movelist = std::vector<std::pair<float, int>>{};
    auto acc_prob = 0.f;

    for (int idx = 0; idx < GetNumIntersections(); ++idx) {
        const auto prob = policy[idx];
        const auto x = idx % board_size;
        const auto y = idx / board_size;
        const auto vtx = GetVertex(x, y);

        if (!IsLegalMove(vtx, color)) {
            continue;
        }

        if (board_.IsRealEye(vtx, color)) {
            continue;
        }

        acc_prob += prob;
        movelist.emplace_back(prob, vtx);
    }

    int select_move = kPass;
    if (!movelist.empty()) {
        if (acc_prob == 0.0f) {
            auto new_prob = 1.f / (float)movelist.size();
            for (auto &m : movelist) {
                m.first = new_prob;
            }
        }

        auto move = random_prob_move(movelist);
        select_move = move;
    }
    PlayMove(select_move, color);
}

std::vector<int> GameState::GetOwnershipAndRemovedDeadStrings(int playouts) const {
    auto fork_state = *this;
    fork_state.RemoveDeadStrings(playouts);
    return fork_state.GetOwnership();
}

std::vector<int> GameState::MarKDeadStrings(int playouts) const {
    auto num_intersections = GetNumIntersections();
    auto buffer = std::vector<int>(num_intersections, 0);

    static constexpr int kMaxPlayoutsCount = 32 * 16384;

    playouts = std::min(playouts, kMaxPlayoutsCount);

    for (int p = 0; p < playouts; ++p) {
        int moves = 0;
        auto state = *this;
        if (p%2==0) {
            state.board_.SetToMove(!GetToMove());
        }
        while(true) {
            state.FillRandomMove();

            if (state.GetPasses() >= 4) {
                break;
            }

            if (moves++ >= 2 * num_intersections) {
                // Too many moves.
                break;
            }
        }

        auto final_ownership = state.GetOwnership();

        for (int idx = 0; idx < num_intersections; ++idx) {
            auto owner = final_ownership[idx];
            if (owner == kBlack) {
                buffer[idx] += 1;
            } else if (owner == kWhite) {
                buffer[idx] -= 1;
            }
        }
    }

    const auto board_size = GetBoardSize();
    const auto thes = (int)(0.7 * playouts);
    auto dead = std::vector<int>{};

    for (int idx = 0; idx < num_intersections; ++idx) {
        const auto x = idx % board_size;
        const auto y = idx / board_size;
        const auto state = GetState(x, y);
        if (buffer[idx] >= thes) {
            // It mean that this area belongs to black.
            if (state == kWhite) dead.emplace_back(GetVertex(x, y));
        } else if (buffer[idx] <= -thes) {
            // It mean that this area belongs to white.
            if (state == kBlack) dead.emplace_back(GetVertex(x, y));
        } 
    }

    return dead;
}

void GameState::RemoveDeadStrings(int playouts) {
    auto dead = MarKDeadStrings(playouts);
    board_.RemoveMarkedStrings(dead);
}

float GameState::GetFinalScore(float bonus) const {
    return board_.ComputeSimpleFinalScore(GetKomi() + GetHandicap() - bonus);
}

int GameState::GetVertex(const int x, const int y) const {
    return board_.GetVertex(x, y);
}

int GameState::GetIndex(const int x, const int y) const {
    return board_.GetIndex(x, y);
}

int GameState::GetX(const int vtx) const {
    return board_.GetX(vtx);
}

int GameState::GetY(const int vtx) const {
    return board_.GetY(vtx);
}

float GameState::GetKomi() const {
    float komi = static_cast<float>(komi_integer_) +
                     static_cast<float>(komi_half_) * 0.5f;
    if (komi_negative_) {
        komi = -komi;
    }
    return komi;
}

int GameState::GetWinner() const {
    return winner_;
}

int GameState::GetHandicap() const {
    return handicap_;
}

int GameState::GetPrisoner(const int color) const {
    return board_.GetPrisoner(color);
}

int GameState::GetMoveNumber() const {
    return board_.GetMoveNumber();
}

int GameState::GetBoardSize() const {
    return board_.GetBoardSize();
}

int GameState::GetNumIntersections() const {
    return board_.GetNumIntersections();
}

int GameState::GetToMove() const {
    return board_.GetToMove();
}

int GameState::GetLastMove() const {
    return board_.GetLastMove();
}

int GameState::GetKoMove() const {
    return board_.GetKoMove();
}

int GameState::GetPasses() const {
    return board_.GetPasses();
}

int GameState::GetKoHash() const {
    return board_.GetKoHash();
}

int GameState::GetHash() const {
    return board_.GetHash() ^ komi_hash_;
}

int GameState::GetState(const int vtx) const {
    return board_.GetState(vtx);
}

int GameState::GetState(const int x, const int y) const {
    return board_.GetState(x, y);
}

int GameState::GetLiberties(const int vtx) const {
    return board_.GetLiberties(vtx);
}

std::shared_ptr<const Board> GameState::GetPastBoard(unsigned int p) const {
    assert(p <= (unsigned)GetMoveNumber());
    return game_history_[(unsigned)GetMoveNumber() - p];  
}

const std::vector<std::shared_ptr<const Board>>& GameState::GetHistory() const {
    return game_history_;
}

std::vector<int> GameState::GetStringList(const int vtx) const {
    return board_.GetStringList(vtx);
}
