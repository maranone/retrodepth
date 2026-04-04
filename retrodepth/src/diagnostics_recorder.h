#pragma once
#include <filesystem>
#include "game_config.h"

class DiagnosticsRecorder {
public:
    static bool run(const GameConfig& config,
                    const std::filesystem::path& output_path,
                    int duration_seconds = 60,
                    int samples_per_second = 2);
};
