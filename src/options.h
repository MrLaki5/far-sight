#pragma once

#include <chrono>
#include <string>

#include "camera.h"
#include "events.h"
#include "vlm.h"

struct Options {
    Vlm::Options vlm;
    Camera::Options camera;
    EventSink::Options events;

    std::string prompt;
    std::string still_image;  // --image: classify one file and exit

    std::chrono::milliseconds interval{1000};
    std::chrono::milliseconds cooldown{30000};
    std::chrono::milliseconds max_idle{60000};
    float threshold = 0.5f;
    int consecutive = 2;
    double motion_threshold = 2.0;
    int vlm_size = 512;
    int warmup_frames = 5;
    bool once = false;
    bool verbose = false;
};

// Returns false and fills err on invalid input; sets show_help for --help.
bool parse_options(int argc, char** argv, Options& opts, bool& show_help, std::string& err);
void print_usage(const char* argv0);

// Turns an event description into the yes/no question sent to the model.
std::string make_question(const std::string& prompt);
