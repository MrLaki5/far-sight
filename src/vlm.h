#pragma once

#include <string>
#include <vector>

#include "image.h"

struct llama_model;
struct llama_context;
struct mtmd_context;

// Wraps llama.cpp + libmtmd to answer a yes/no question about one image.
class Vlm {
public:
    struct Options {
        std::string model_path;
        std::string mmproj_path;
        int n_gpu_layers = -1;  // negative = offload everything the backend can take
        int n_threads = 0;      // 0 = auto
        int n_ctx = 4096;
        bool verbose = false;
    };

    Vlm() = default;
    ~Vlm();
    Vlm(const Vlm&) = delete;
    Vlm& operator=(const Vlm&) = delete;

    bool load(const Options& opts, std::string& err);

    // p_yes is P(yes) / (P(yes) + P(no)) over the first answer token.
    bool ask_yes_no(const Image& img, const std::string& question, float& p_yes, std::string& err);

private:
    std::string build_prompt(const std::string& question) const;

    llama_model* model_ = nullptr;
    llama_context* ctx_ = nullptr;
    mtmd_context* mctx_ = nullptr;
    int n_batch_ = 0;
    std::vector<int> yes_tokens_;
    std::vector<int> no_tokens_;
};
