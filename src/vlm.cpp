#include "vlm.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>

#include "llama.h"
#include "mtmd-helper.h"
#include "mtmd.h"

namespace {

bool g_verbose = false;

void log_cb(ggml_log_level level, const char* text, void*) {
    if (g_verbose || level >= GGML_LOG_LEVEL_WARN) std::fputs(text, stderr);
}

// First token of each spelling, so " Yes" and "yes" both count regardless of tokenizer.
std::vector<int> first_tokens(const llama_vocab* vocab, std::initializer_list<const char*> words) {
    std::vector<int> out;
    for (const char* w : words) {
        llama_token tok[8];
        const int n = llama_tokenize(vocab, w, static_cast<int32_t>(std::strlen(w)), tok, 8, false, false);
        if (n > 0 && std::find(out.begin(), out.end(), tok[0]) == out.end()) out.push_back(tok[0]);
    }
    return out;
}

double log_sum_exp(const float* logits, const std::vector<int>& ids) {
    double mx = -std::numeric_limits<double>::infinity();
    for (int id : ids) mx = std::max(mx, static_cast<double>(logits[id]));
    double s = 0.0;
    for (int id : ids) s += std::exp(logits[id] - mx);
    return mx + std::log(s);
}

}  // namespace

Vlm::~Vlm() {
    if (mctx_) mtmd_free(mctx_);
    if (ctx_) llama_free(ctx_);
    if (model_) llama_model_free(model_);
}

bool Vlm::load(const Options& opts, std::string& err) {
    g_verbose = opts.verbose;
    llama_log_set(log_cb, nullptr);
    mtmd_helper_log_set(log_cb, nullptr);
    llama_backend_init();

    const int threads = opts.n_threads > 0
                            ? opts.n_threads
                            : static_cast<int>(std::max(1u, std::min(8u, std::thread::hardware_concurrency() / 2)));

    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = opts.n_gpu_layers < 0 ? 999 : opts.n_gpu_layers;
    model_ = llama_model_load_from_file(opts.model_path.c_str(), mp);
    if (!model_) {
        err = "failed to load model " + opts.model_path;
        return false;
    }

    // Non-causal vision models need a whole image in one ubatch, so keep batch == ubatch and large.
    n_batch_ = 2048;
    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = static_cast<uint32_t>(opts.n_ctx);
    cp.n_batch = static_cast<uint32_t>(n_batch_);
    cp.n_ubatch = static_cast<uint32_t>(n_batch_);
    cp.n_threads = threads;
    cp.n_threads_batch = threads;
    ctx_ = llama_init_from_model(model_, cp);
    if (!ctx_) {
        err = "failed to create llama context";
        return false;
    }

    mtmd_context_params mtp = mtmd_context_params_default();
    mtp.use_gpu = opts.n_gpu_layers != 0;
    mtp.n_threads = threads;
    mtp.print_timings = opts.verbose;
    mctx_ = mtmd_init_from_file(opts.mmproj_path.c_str(), model_, mtp);
    if (!mctx_) {
        err = "failed to load multimodal projector " + opts.mmproj_path;
        return false;
    }
    if (!mtmd_support_vision(mctx_)) {
        err = "projector " + opts.mmproj_path + " has no vision support";
        return false;
    }

    const llama_vocab* vocab = llama_model_get_vocab(model_);
    yes_tokens_ = first_tokens(vocab, {"yes", "Yes", " yes", " Yes", "YES"});
    no_tokens_ = first_tokens(vocab, {"no", "No", " no", " No", "NO"});
    if (yes_tokens_.empty() || no_tokens_.empty()) {
        err = "could not tokenize yes/no answers";
        return false;
    }
    return true;
}

std::string Vlm::build_prompt(const std::string& question) const {
    const std::string content = std::string(mtmd_default_marker()) + "\n" + question;
    const char* tmpl = llama_model_chat_template(model_, nullptr);
    llama_chat_message msg{"user", content.c_str()};

    std::vector<char> buf(content.size() * 2 + 512);
    int n = llama_chat_apply_template(tmpl, &msg, 1, true, buf.data(), static_cast<int32_t>(buf.size()));
    if (n > static_cast<int>(buf.size())) {
        buf.resize(static_cast<size_t>(n));
        n = llama_chat_apply_template(tmpl, &msg, 1, true, buf.data(), static_cast<int32_t>(buf.size()));
    }
    if (n < 0) return "USER: " + content + "\nASSISTANT:";  // model has no template llama.cpp understands
    return std::string(buf.data(), static_cast<size_t>(n));
}

bool Vlm::ask_yes_no(const Image& img, const std::string& question, float& p_yes, std::string& err) {
    llama_memory_clear(llama_get_memory(ctx_), true);

    const std::string prompt = build_prompt(question);
    mtmd_bitmap* bmp = mtmd_bitmap_init(static_cast<uint32_t>(img.width), static_cast<uint32_t>(img.height),
                                        img.rgb.data());
    mtmd_input_chunks* chunks = mtmd_input_chunks_init();
    mtmd_input_text text{prompt.c_str(), prompt.size(), true, true};
    const mtmd_bitmap* bitmaps[] = {bmp};

    bool ok = false;
    if (int rc = mtmd_tokenize(mctx_, chunks, &text, bitmaps, 1); rc != 0) {
        err = "mtmd_tokenize failed (" + std::to_string(rc) + ")";
    } else if (llama_pos n_past = 0;
               mtmd_helper_eval_chunks(mctx_, ctx_, chunks, 0, 0, n_batch_, true, &n_past) != 0) {
        err = "failed to evaluate prompt (context too small? try a smaller --vlm-size)";
    } else {
        const float* logits = llama_get_logits_ith(ctx_, -1);
        const double ly = log_sum_exp(logits, yes_tokens_);
        const double ln = log_sum_exp(logits, no_tokens_);
        p_yes = static_cast<float>(1.0 / (1.0 + std::exp(ln - ly)));
        ok = true;
    }

    mtmd_input_chunks_free(chunks);
    mtmd_bitmap_free(bmp);
    return ok;
}
