#include "utils/option.h"
#include "utils/log.h"
#include "utils/mutex.h"
#include "game/zobrist.h"
#include "game/symmetry.h"
#include "game/types.h"
#include "game/board.h"
#include "pattern/pattern.h"
#include "mcts/lcb.h"
#include "config.h"

#include <limits>
#include <sstream>
#include <fstream>

std::unordered_map<std::string, Option> kOptionsMap;

#define OPTIONS_EXPASSION(T)                        \
template<>                                          \
T GetOption<T>(std::string name) {                  \
    return kOptionsMap.find(name)->second.Get<T>(); \
}                                                   \
                                                    \
template<>                                          \
bool SetOption<T>(std::string name, T val) {        \
    auto res = kOptionsMap.find(name);              \
    if (res != std::end(kOptionsMap)) {             \
        res->second.Set<T>(val);                    \
        return true;                                \
    }                                               \
    return false;                                   \
}

OPTIONS_EXPASSION(std::string)
OPTIONS_EXPASSION(bool)
OPTIONS_EXPASSION(int)
OPTIONS_EXPASSION(float)
OPTIONS_EXPASSION(char)

#undef OPTIONS_EXPASSION

void ArgsParser::InitOptionsMap() const {
    kOptionsMap["help"] << Option::setoption(false);
    kOptionsMap["mode"] << Option::setoption(std::string{"gtp"});
    kOptionsMap["inputs"] << Option::setoption(std::string{});

    // engine options
    kOptionsMap["ponder"] << Option::setoption(false);
    kOptionsMap["reuse_tree"] << Option::setoption(false);
    kOptionsMap["friendly_pass"] << Option::setoption(false);
    kOptionsMap["analysis_verbose"] << Option::setoption(false);
    kOptionsMap["quiet"] << Option::setoption(false);
    kOptionsMap["rollout"] << Option::setoption(false);
    kOptionsMap["no_dcnn"] << Option::setoption(false);
    kOptionsMap["root_dcnn"] << Option::setoption(false);
    kOptionsMap["winograd"] << Option::setoption(true);

    kOptionsMap["search_mode"] << Option::setoption(std::string{});
    kOptionsMap["fixed_nn_boardsize"] << Option::setoption(0);
    kOptionsMap["defualt_boardsize"] << Option::setoption(kDefaultBoardSize);
    kOptionsMap["defualt_komi"] << Option::setoption(kDefaultKomi);

    kOptionsMap["cache_memory_mib"] << Option::setoption(400);
    kOptionsMap["playouts"] << Option::setoption(-1);
    kOptionsMap["ponder_factor"] << Option::setoption(100);
    kOptionsMap["const_time"] << Option::setoption(0);
    kOptionsMap["batch_size"] << Option::setoption(0);
    kOptionsMap["threads"] << Option::setoption(0);

    kOptionsMap["kgs_hint"] << Option::setoption(std::string{});
    kOptionsMap["weights_file"] << Option::setoption(std::string{});
    kOptionsMap["book_file"] << Option::setoption(std::string{});
    kOptionsMap["patterns_file"] << Option::setoption(std::string{});

    kOptionsMap["use_gpu"] << Option::setoption(false);
    kOptionsMap["gpus"] << Option::setoption(std::string{});
    kOptionsMap["gpu_waittime"] << Option::setoption(2);

    kOptionsMap["resign_threshold"] << Option::setoption(0.1f, 1.f, 0.f);

    kOptionsMap["ci_alpha"] << Option::setoption(1e-5f, 1.f, 0.f);
    kOptionsMap["lcb_utility_factor"] << Option::setoption(0.1f);
    kOptionsMap["lcb_reduction"] << Option::setoption(0.02f, 1.f, 0.f);
    kOptionsMap["fpu_reduction"] << Option::setoption(0.25f);
    kOptionsMap["fpu_root_reduction"] << Option::setoption(0.25f);
    kOptionsMap["cpuct_init"] << Option::setoption(0.5f);
    kOptionsMap["cpuct_base_factor"] << Option::setoption(1.0f);
    kOptionsMap["cpuct_base"] << Option::setoption(19652.f);
    kOptionsMap["draw_factor"] << Option::setoption(0.f);
    kOptionsMap["score_utility_factor"] << Option::setoption(0.1f);
    kOptionsMap["score_utility_div"] << Option::setoption(20.f);
    kOptionsMap["expand_threshold"] << Option::setoption(-1);
    kOptionsMap["completed_q_utility_factor"] << Option::setoption(0.0f);

    kOptionsMap["root_policy_temp"] << Option::setoption(1.f, 1.f, 0.f);
    kOptionsMap["policy_temp"] << Option::setoption(1.f, 1.f, 0.f);
    kOptionsMap["lag_buffer"] << Option::setoption(0);
    kOptionsMap["early_symm_cache"] << Option::setoption(false);
    kOptionsMap["symm_pruning"] << Option::setoption(false);
    kOptionsMap["use_stm_winrate"] << Option::setoption(false);

    // self-play options
    kOptionsMap["selfplay_query"] << Option::setoption(std::string{});
    kOptionsMap["random_min_visits"] << Option::setoption(1);
    kOptionsMap["random_moves_factor"] << Option::setoption(0.f);

    kOptionsMap["gumbel_considered_moves"] << Option::setoption(16);
    kOptionsMap["gumbel_playouts"] << Option::setoption(400);
    kOptionsMap["gumbel"] << Option::setoption(false);
    kOptionsMap["always_completed_q_policy"] << Option::setoption(false);

    kOptionsMap["dirichlet_noise"] << Option::setoption(false);
    kOptionsMap["dirichlet_epsilon"] << Option::setoption(0.25f);
    kOptionsMap["dirichlet_init"] << Option::setoption(0.03f);
    kOptionsMap["dirichlet_factor"] << Option::setoption(361.f);

    kOptionsMap["resign_playouts"] << Option::setoption(0);
    kOptionsMap["reduce_playouts"] << Option::setoption(0);
    kOptionsMap["reduce_playouts_prob"] << Option::setoption(0.f, 1.f, 0.f);
    kOptionsMap["first_pass_bonus"] << Option::setoption(false);

    kOptionsMap["num_games"] << Option::setoption(0);
    kOptionsMap["parallel_games"] << Option::setoption(1);
    kOptionsMap["komi_variance"] << Option::setoption(0.f);
    kOptionsMap["target_directory"] << Option::setoption(std::string{});
}

void ArgsParser::InitBasicParameters() const {
    PatternHashAndCoordsInit();
    Board::InitPattern3();
    Zobrist::Initialize();
    Symmetry::Get().Initialize();
    LcbEntries::Get().Initialize(GetOption<float>("ci_alpha"));
    LogOptions::Get().SetQuiet(GetOption<bool>("quiet"));

    bool already_set_thread = GetOption<int>("threads") > 0;
    bool already_set_batchsize = GetOption<int>("batch_size") > 0;
    bool use_gpu = GetOption<bool>("use_gpu");

    const int cores = std::max((int)std::thread::hardware_concurrency(), 1);
    int select_threads = GetOption<int>("threads");
    int select_batchsize = GetOption<int>("batch_size");

    // Try to select a reasonable number for threads and batch
    // size.
    if (!already_set_thread && !already_set_batchsize) {
        select_threads = (1 + (int)use_gpu) * cores;
        select_batchsize = select_threads/2;
    } else if (!already_set_thread && already_set_batchsize) {
        if (use_gpu) {
            select_threads = 2 * select_batchsize; 
        } else {
            select_threads = cores;
        }
    } else if (already_set_thread && !already_set_batchsize) {
        select_batchsize = select_threads/2;
    }

    // The batch size of cpu pipe is always 1. 
    if (!use_gpu) {
        select_batchsize = 1;
    }

    SetOption("threads", std::max(select_threads, 1));
    SetOption("batch_size", std::max(select_batchsize, 1));

    // Try to select a reasonable number for const time and playouts.
    bool already_set_time = GetOption<int>("const_time") > 0;
    bool already_set_playouts = GetOption<int>("playouts") > -1;

    if (!already_set_time && !already_set_playouts) {
        SetOption("const_time", 10); // 10 seconds
    }
    if (!already_set_playouts) {
        SetOption("playouts", std::numeric_limits<int>::max() / 2);
    }

    // Set the root fpu value.
    if (!init_fpu_root_) {
        SetOption("fpu_root_reduction",
                      GetOption<float>("fpu_reduction"));
    }

    // Parse the search mode.
    auto search_mode = GetOption<std::string>("search_mode");
 
    for (char &c : search_mode) {
        if (c == '+') c = ' ';
    }

    auto ss = std::istringstream(search_mode);
    auto get_mode = std::string{};
    while (ss >> get_mode) {
        if (get_mode == "dcnn") {
            SetOption("no_dcnn", false);
        } else if (get_mode == "nodcnn" ||
                       get_mode == "nonet") {
            SetOption("no_dcnn", true);
        } else if (get_mode == "rollout") {
            SetOption("rollout", true);
        } else if (get_mode == "rootdcnn") {
            SetOption("root_dcnn", true);
        }
    }
}

bool IsParameter(const std::string &param) {
    if (param.empty()) {
        return false;
    }
    return param[0] != '-';
};

std::string RemoveComment(std::string line) {
    auto out = std::string{};
    for (auto c : line) {
        if (c == '#') break;
        out += c;
    }
    return out;
}

std::string SplitterToString(Splitter &spt) {
    auto out = std::string{};
    const auto cnt = spt.GetCount();
    for (auto i = size_t{0}; i < cnt; ++i) {
        const auto res = spt.GetWord(i)->Get<>();
        out += (res + " \0"[i+1 == cnt]);
    }
    return out;
}

ArgsParser::ArgsParser(int argc, char** argv) {
    auto spt = Splitter(argc, argv);

    InitOptionsMap();
    inputs_ = std::string{};

    // Remove the name.
    const auto name = spt.RemoveWord(0);
    (void) name;

    auto config = std::string{};

    if (const auto res = spt.FindNext({"--config", "-config"})) {
        if (IsParameter(res->Get<>())) {
            config = res->Get<>();
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (!config.empty()) {
        auto file = std::ifstream{};

        file.open(config);
        if (file.is_open()) {
            auto lines = std::string{};
            auto line = std::string{};

            while(std::getline(file, line)) {
                line = RemoveComment(line);
                if (!line.empty()) {
                    lines += (line + ' ');
                }
            }
            file.close();

            auto cspt = Splitter(lines);
            Parse(cspt);
        }
    }

    Parse(spt);
    SetOption("inputs", inputs_);
}

void ArgsParser::Parse(Splitter &spt) {
    const auto ErrorCommands = [](Splitter & spt) -> bool {
        const auto cnt = spt.GetCount();
        if (cnt == 0) {
            return false;
        }

        LOGGING << "Command(s) Error:" << std::endl;
        for (auto i = size_t{0}; i < cnt; ++i) {
            const auto command = spt.GetWord(i)->Get<>();
            if (!IsParameter(command)) {
                LOGGING << " " << i+1 << ". " << command << std::endl;
            }
        }
        LOGGING << " are not understood." << std::endl;
        return true;
    };

    const auto TransferHint = [](std::string hint) {
        for (auto &c : hint) {
            if (c == '+') {
                c = ' ';
            }
        }
        return hint;
    };

    inputs_ += (SplitterToString(spt) + ' ');

    if (const auto res = spt.FindNext({"--mode", "-m"})) {
        if (IsParameter(res->Get<>())) {
            SetOption("mode", res->Get<>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.Find({"--help", "-h"})) {
        SetOption("help", true);
        spt.RemoveWord(res->Index());
    }

    if (const auto res = spt.Find({"--quiet", "-q"})) {
        SetOption("quiet", true);
        spt.RemoveWord(res->Index());
    }

    if (const auto res = spt.Find("--ponder")) {
        SetOption("ponder", true);
        spt.RemoveWord(res->Index());
    }

    if (const auto res = spt.Find("--reuse-tree")) {
        SetOption("reuse_tree", true);
        spt.RemoveWord(res->Index());
    }

    if (const auto res = spt.Find("--friendly-pass")) {
        SetOption("friendly_pass", true);
        spt.RemoveWord(res->Index());
    }

    if (const auto res = spt.Find("--early-symm-cache")) {
        SetOption("early_symm_cache", true);
        spt.RemoveWord(res->Index());
    }

    if (const auto res = spt.Find("--symm-pruning")) {
        SetOption("symm_pruning", true);
        spt.RemoveWord(res->Index());
    }

    if (const auto res = spt.FindNext("--search-mode")) {
        if (IsParameter(res->Get<>())) {
            SetOption("search_mode", res->Get<>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.Find("--first-pass-bonus")) {
        SetOption("first_pass_bonus", true);
        spt.RemoveWord(res->Index());
    }

    if (const auto res = spt.Find("--use-stm-winrate")) {
        SetOption("use_stm_winrate", true);
        spt.RemoveWord(res->Index());
    }

    if (const auto res = spt.Find("--no-dcnn")) {
        SetOption("no_dcnn", true);
        spt.RemoveWord(res->Index());
    }

    if (const auto res = spt.Find("--no-winograd")) {
        SetOption("winograd", false);
        spt.RemoveWord(res->Index());
    }

    if (const auto res = spt.FindNext({"--resign-threshold", "-r"})) {
        if (IsParameter(res->Get<>())) {
            SetOption("resign_threshold", res->Get<float>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--expand-threshold")) {
        if (IsParameter(res->Get<>())) {
            SetOption("expand_threshold", res->Get<int>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--kgs-hint")) {
        if (IsParameter(res->Get<>())) {
            SetOption("kgs_hint", TransferHint(res->Get<>()));
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        } 
    }

    if (const auto res = spt.Find({"--analysis-verbose", "-a"})) {
        SetOption("analysis_verbose", true);
        spt.RemoveWord(res->Index());
    }

    if (const auto res = spt.Find({"--dirichlet-noise", "--noise", "-n"})) {
        SetOption("dirichlet_noise", true);
        spt.RemoveWord(res->Index());
    }

    if (const auto res = spt.FindNext("--gumbel-considered-moves")) {
        if (IsParameter(res->Get<>())) {
            SetOption("gumbel_considered_moves", res->Get<int>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--gumbel-playouts")) {
        if (IsParameter(res->Get<>())) {
            SetOption("gumbel_playouts", res->Get<int>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.Find("--gumbel")) {
        SetOption("gumbel", true);
        spt.RemoveWord(res->Index());
    }

    if (const auto res = spt.Find("--always-completed-q-policy")) {
        SetOption("always_completed_q_policy", true);
        spt.RemoveWord(res->Index());
    }

    if (const auto res = spt.FindNext("--dirichlet-epsilon")) {
        if (IsParameter(res->Get<>())) {
            SetOption("dirichlet_epsilon", res->Get<float>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--dirichlet-init")) {
        if (IsParameter(res->Get<>())) {
            SetOption("dirichlet_init", res->Get<float>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--dirichlet-factor")) {
        if (IsParameter(res->Get<>())) {
            SetOption("dirichlet_factor", res->Get<float>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--random-moves-factor")) {
        if (IsParameter(res->Get<>())) {
            SetOption("random_moves_factor", res->Get<float>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--gpu-waittime")) {
        if (IsParameter(res->Get<>())) {
            SetOption("gpu_waittime", res->Get<int>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    while (const auto res = spt.FindNext({"--gpu", "-g"})) {
        if (IsParameter(res->Get<>())) {
            auto gpus = GetOption<std::string>("gpus");
            gpus += (res->Get<>() + " ");
            SetOption("gpus", gpus);
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext({"--threads", "-t"})) {
        if (IsParameter(res->Get<>())) {
            SetOption("threads", res->Get<int>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext({"--batch-size", "-b"})) {
        if (IsParameter(res->Get<>())) {
            SetOption("batch_size", res->Get<int>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--cache-memory-mib")) {
        if (IsParameter(res->Get<>())) {
            SetOption("cache_memory_mib", res->Get<int>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext({"--playouts", "-p"})) {
        if (IsParameter(res->Get<>())) {
            SetOption("playouts", res->Get<int>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--ponder-factor")) {
       if (IsParameter(res->Get<>())) {
           SetOption("ponder_factor", res->Get<int>());
           spt.RemoveSlice(res->Index()-1, res->Index()+1);
       }
    }

    if (const auto res = spt.FindNext("--const-time")) {
        if (IsParameter(res->Get<>())) {
            SetOption("const_time", res->Get<int>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext({"--logfile", "-l"})) {
        if (IsParameter(res->Get<>())) {
            auto fname = res->Get<>();
            LogWriter::Get().SetFilename(fname);
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--fixed-nn-boardsize")) {
        if (IsParameter(res->Get<>())) {
            SetOption("fixed_nn_boardsize", res->Get<int>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext({"--board-size", "-s"})) {
        if (IsParameter(res->Get<>())) {
            SetOption("defualt_boardsize", res->Get<int>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext({"--komi", "-k"})) {
        if (IsParameter(res->Get<>())) {
            SetOption("defualt_komi", res->Get<float>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--ci-alpha")) {
        if (IsParameter(res->Get<>())) {
            SetOption("ci_alpha", res->Get<float>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext({"--weights", "-w"})) {
        if (IsParameter(res->Get<>())) {
            SetOption("weights_file", res->Get<>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--book")) {
        if (IsParameter(res->Get<>())) {
            SetOption("book_file", res->Get<>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--patterns")) {
        if (IsParameter(res->Get<>())) {
            SetOption("patterns_file", res->Get<>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--score-utility-factor")) {
        if (IsParameter(res->Get<>())) {
            SetOption("score_utility_factor", res->Get<float>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--score-utility-div")) {
        if (IsParameter(res->Get<>())) {
            SetOption("score_utility_div", res->Get<float>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--completed-q-utility-factor")) {
        if (IsParameter(res->Get<>())) {
            SetOption("completed_q_utility_factor", res->Get<float>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--lcb-reduction")) {
        if (IsParameter(res->Get<>())) {
            SetOption("lcb_reduction", res->Get<float>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--lcb-utility-factor")) {
        if (IsParameter(res->Get<>())) {
            SetOption("lcb_utility_factor", res->Get<float>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--fpu-reduction")) {
        if (IsParameter(res->Get<>())) {
            SetOption("fpu_reduction", res->Get<float>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--fpu-root-reduction")) {
        if (IsParameter(res->Get<>())) {
            init_fpu_root_ = true;
            SetOption("fpu_root_reduction", res->Get<float>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--cpuct-init")) {
        if (IsParameter(res->Get<>())) {
            SetOption("cpuct_init", res->Get<float>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--cpuct-base-factor")) {
        if (IsParameter(res->Get<>())) {
            SetOption("cpuct_base_factor", res->Get<float>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--cpuct-base")) {
        if (IsParameter(res->Get<>())) {
            SetOption("cpuct_base", res->Get<float>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--draw-factor")) {
        if (IsParameter(res->Get<>())) {
            SetOption("draw_factor", res->Get<float>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--root-policy-temp")) {
        if (IsParameter(res->Get<>())) {
            SetOption("root_policy_temp", res->Get<float>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--policy-temp")) {
        if (IsParameter(res->Get<>())) {
            SetOption("policy_temp", res->Get<float>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--resign-playouts")) {
        if (IsParameter(res->Get<>())) {
            SetOption("resign_playouts", res->Get<int>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--reduce-playouts")) {
        if (IsParameter(res->Get<>())) {
            SetOption("reduce_playouts", res->Get<int>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--reduce-playouts-prob")) {
        if (IsParameter(res->Get<>())) {
            SetOption("reduce_playouts_prob", res->Get<float>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--lag-buffer")) {
        if (IsParameter(res->Get<>())) {
            SetOption("lag_buffer", res->Get<int>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--num-games")) {
        if (IsParameter(res->Get<>())) {
            SetOption("num_games", res->Get<int>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--parallel-games")) {
        if (IsParameter(res->Get<>())) {
            SetOption("parallel_games", res->Get<int>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--komi-variance")) {
        if (IsParameter(res->Get<>())) {
            SetOption("komi_variance", res->Get<float>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    if (const auto res = spt.FindNext("--target-directory")) {
        if (IsParameter(res->Get<>())) {
            SetOption("target_directory", res->Get<>());
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

    while (const auto res = spt.FindNext("--selfplay-query")) {
        if (IsParameter(res->Get<>())) {
            auto query = GetOption<std::string>("selfplay_query");
            query += (res->Get<>() + " ");
            SetOption("selfplay_query", query);
            spt.RemoveSlice(res->Index()-1, res->Index()+1);
        }
    }

#ifdef USE_CUDA
    SetOption("use_gpu", true);
#endif

    if (ErrorCommands(spt) || GetOption<bool>("help")) {
        DumpHelper();
    }
    DumpWarning();

    InitBasicParameters();
}

void ArgsParser::DumpHelper() const {
    LOGGING << "Arguments:" << std::endl
                << "\t--quiet, -q\n"
                << "\t\tDisable all diagnostic verbose.\n\n" 

                << "\t--analysis-verbose, -a\n"
                << "\t\tDump the search verbose.\n\n"

                << "\t--ponder\n"
                << "\t\tThinking on opponent's time.\n\n"

                << "\t--reuse-tree\n"
                << "\t\tWill reuse the sub-tree.\n\n"

                << "\t--early-symm-cache\n"
                << "\t\tAccelerate the search on the opening stage.\n\n"

                << "\t--friendly-pass\n"
                << "\t\tDo pass move if the engine wins the game.\n\n"

                << "\t--no-dcnn\n"
                << "\t\tDisable the Neural Network forwarding pipe. Very weak.\n\n"

                << "\t--cache-memory-mib <integer>\n"
                << "\t\tSet the NN cache size in MiB.\n\n"

                << "\t--playouts, -p <integer>\n"
                << "\t\tThe number of maximum playouts.\n\n"

                << "\t--const-time <integer>\n"
                << "\t\tConst time of search in seconds.\n\n"

                << "\t--gpu, -g <integer>\n"
                << "\t\tSelect a specific GPU device. Default is all devices.\n\n"

                << "\t--threads, -t <integer>\n"
                << "\t\tThe number of threads used. Set 0 will select a reasonable number.\n\n"

                << "\t--batch-size, -b <integer>\n"
                << "\t\tThe number of batches for a single evaluation. Set 0 will select a reasonable number.\n\n"

                << "\t--lag-buffer <integer>\n"
                << "\t\tSafety margin for time usage in seconds.\n\n"

                << "\t--score-utility-factor <float>\n"
                << "\t\tScore utility heuristic value.\n\n"

                << "\t--lcb-reduction <float>\n"
                << "\t\tReduce the LCB weights. Set 1 will select the most visits node as the best move in MCTS.\n\n"

                << "\t--resign-threshold, -r <float>\n"
                << "\t\tResign when winrate is less than x. Default is 0.1.\n\n"

                << "\t--weights, -w <weight file name>\n"
                << "\t\tFile with network weights.\n\n"

                << "\t--book <book file name>\n"
                << "\t\tFile with opening book.\n\n"

                << "\t--logfile, -l <log file name>\n"
                << "\t\tFile to log input/output to.\n\n"
          ;
    exit(-1);
}

void ArgsParser::DumpWarning() const {}
