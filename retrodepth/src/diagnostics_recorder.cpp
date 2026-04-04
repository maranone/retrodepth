#include "diagnostics_recorder.h"
#include "shmem_reader.h"
#include "palette_control.h"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <queue>
#include <set>
#include <unordered_map>

using json = nlohmann::json;

namespace {

struct ThumbStats {
    int nonblack_pixels = 0;
    int unique_colors = 0;
    int min_x = 32;
    int min_y = 32;
    int max_x = -1;
    int max_y = -1;
    double centroid_x = 0.0;
    double centroid_y = 0.0;
};

struct OwnerAggregate {
    int pixels = 0;
    int top_half = 0;
    int bottom_half = 0;
    int left_half = 0;
    int right_half = 0;
    int region_count = 0;
    int largest_region = 0;
    std::set<std::string> layers;
};

ThumbStats analyze_thumb(const uint32_t* thumb) {
    ThumbStats s;
    std::set<uint32_t> unique;
    double sum_x = 0.0, sum_y = 0.0;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            uint32_t c = thumb[y * 32 + x];
            uint8_t b = (uint8_t)(c & 0xFF);
            uint8_t g = (uint8_t)((c >> 8) & 0xFF);
            uint8_t r = (uint8_t)((c >> 16) & 0xFF);
            uint8_t a = (uint8_t)((c >> 24) & 0xFF);
            if (a == 0 || (r == 0 && g == 0 && b == 0)) continue;
            ++s.nonblack_pixels;
            unique.insert(c);
            s.min_x = (std::min)(s.min_x, x);
            s.min_y = (std::min)(s.min_y, y);
            s.max_x = (std::max)(s.max_x, x);
            s.max_y = (std::max)(s.max_y, y);
            sum_x += x;
            sum_y += y;
        }
    }
    s.unique_colors = (int)unique.size();
    if (s.nonblack_pixels > 0) {
        s.centroid_x = sum_x / s.nonblack_pixels;
        s.centroid_y = sum_y / s.nonblack_pixels;
    } else {
        s.min_x = s.min_y = 0;
        s.max_x = s.max_y = -1;
    }
    return s;
}

std::vector<std::array<int, 2>> compute_runs(const std::vector<int>& active) {
    std::vector<std::array<int, 2>> runs;
    if (active.empty()) return runs;
    int start = active.front();
    int prev = start;
    for (size_t i = 1; i < active.size(); ++i) {
        if (active[i] == prev + 1) {
            prev = active[i];
            continue;
        }
        runs.push_back({ start, prev });
        start = prev = active[i];
    }
    runs.push_back({ start, prev });
    return runs;
}

json build_sample_json(const std::vector<LayerFrame>& frames,
                       const uint32_t (*palettes)[16],
                       const uint32_t (*thumbs)[32 * 32],
                       int sample_index,
                       int time_ms) {
    json sample;
    sample["sample_index"] = sample_index;
    sample["time_ms"] = time_ms;

    std::unordered_map<int, OwnerAggregate> owners;
    json layers = json::array();

    for (const auto& f : frames) {
        json layer;
        layer["id"] = f.id;
        layer["width"] = f.width;
        layer["height"] = f.height;
        layer["depth_meters"] = f.depth_meters;
        layer["quad_width_meters"] = f.quad_width_meters;
        layer["copy_count"] = (int)f.copies.size();

        int opaque = 0, owner_pixels = 0, top_half = 0, bottom_half = 0, left_half = 0, right_half = 0;
        std::unordered_map<int, int> owner_hist;
        std::vector<uint8_t> visited((size_t)f.width * f.height, 0);

        for (int y = 0; y < f.height; ++y) {
            for (int x = 0; x < f.width; ++x) {
                size_t pos = (size_t)y * f.width + x;
                uint8_t a = f.rgba[pos * 4 + 3];
                if (a) {
                    ++opaque;
                    if (y < f.height / 2) ++top_half; else ++bottom_half;
                    if (x < f.width / 2) ++left_half; else ++right_half;
                }

                if (!f.owner_ids.empty() && f.owner_ids[pos] != LayerFrame::OWNER_NONE) {
                    int owner = (int)f.owner_ids[pos];
                    ++owner_pixels;
                    ++owner_hist[owner];
                    auto& agg = owners[owner];
                    ++agg.pixels;
                    if (y < f.height / 2) ++agg.top_half; else ++agg.bottom_half;
                    if (x < f.width / 2) ++agg.left_half; else ++agg.right_half;
                    agg.layers.insert(f.id);

                    if (!visited[pos]) {
                        int region_area = 0;
                        std::queue<int> q;
                        q.push((int)pos);
                        visited[pos] = 1;
                        while (!q.empty()) {
                            int p = q.front();
                            q.pop();
                            ++region_area;
                            int sx = p % f.width;
                            int sy = p / f.width;
                            const int neigh[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
                            for (const auto& n : neigh) {
                                int nx = sx + n[0], ny = sy + n[1];
                                if (nx < 0 || ny < 0 || nx >= f.width || ny >= f.height) continue;
                                int np = ny * f.width + nx;
                                if (visited[np]) continue;
                                if (f.owner_ids[np] != owner) continue;
                                visited[np] = 1;
                                q.push(np);
                            }
                        }
                        ++owners[owner].region_count;
                        owners[owner].largest_region = (std::max)(owners[owner].largest_region, region_area);
                    }
                }
            }
        }

        layer["opaque_pixels"] = opaque;
        layer["coverage_ratio"] = (f.width > 0 && f.height > 0) ? (double)opaque / (double)(f.width * f.height) : 0.0;
        layer["owner_pixels"] = owner_pixels;
        layer["unique_owner_count"] = (int)owner_hist.size();
        layer["top_half_pixels"] = top_half;
        layer["bottom_half_pixels"] = bottom_half;
        layer["left_half_pixels"] = left_half;
        layer["right_half_pixels"] = right_half;

        json owner_hist_json = json::object();
        for (const auto& kv : owner_hist)
            owner_hist_json[std::to_string(kv.first)] = kv.second;
        layer["owner_histogram"] = std::move(owner_hist_json);
        layers.push_back(std::move(layer));
    }
    sample["layers"] = std::move(layers);

    std::vector<int> active_palettes;
    json palette_rows = json::array();
    for (int p = 0; p < 256; ++p) {
        ThumbStats ts = analyze_thumb(thumbs[p]);
        bool active = ts.nonblack_pixels > 0;
        if (active) active_palettes.push_back(p);

        json row;
        row["palette"] = p;
        row["active"] = active;

        json colors = json::array();
        int nonzero_entries = 0;
        for (int c = 0; c < 16; ++c) {
            uint32_t v = palettes[p][c];
            colors.push_back(v);
            if ((v & 0x00FFFFFFu) != 0) ++nonzero_entries;
        }
        row["colors_argb"] = std::move(colors);
        row["nonzero_color_entries"] = nonzero_entries;
        row["thumb_nonblack_pixels"] = ts.nonblack_pixels;
        row["thumb_unique_colors"] = ts.unique_colors;
        row["thumb_bbox"] = { ts.min_x, ts.min_y, ts.max_x, ts.max_y };
        row["thumb_centroid"] = { ts.centroid_x, ts.centroid_y };

        auto it = owners.find(p);
        if (it != owners.end()) {
            row["screen_pixels"] = it->second.pixels;
            row["region_count"] = it->second.region_count;
            row["largest_region"] = it->second.largest_region;
            row["top_half_pixels"] = it->second.top_half;
            row["bottom_half_pixels"] = it->second.bottom_half;
            row["left_half_pixels"] = it->second.left_half;
            row["right_half_pixels"] = it->second.right_half;
            json layer_names = json::array();
            for (const auto& name : it->second.layers) layer_names.push_back(name);
            row["layers_seen"] = std::move(layer_names);
        } else {
            row["screen_pixels"] = 0;
            row["region_count"] = 0;
            row["largest_region"] = 0;
            row["top_half_pixels"] = 0;
            row["bottom_half_pixels"] = 0;
            row["left_half_pixels"] = 0;
            row["right_half_pixels"] = 0;
            row["layers_seen"] = json::array();
        }

        palette_rows.push_back(std::move(row));
    }

    auto runs = compute_runs(active_palettes);
    json runs_json = json::array();
    json gaps_json = json::array();
    for (size_t i = 0; i < runs.size(); ++i) {
        runs_json.push_back({ runs[i][0], runs[i][1] });
        if (i + 1 < runs.size())
            gaps_json.push_back(runs[i + 1][0] - runs[i][1] - 1);
    }

    sample["active_palette_indices"] = active_palettes;
    sample["active_palette_count"] = (int)active_palettes.size();
    sample["active_palette_runs"] = std::move(runs_json);
    sample["active_palette_gaps"] = std::move(gaps_json);
    sample["palettes"] = std::move(palette_rows);
    return sample;
}

} // namespace

bool DiagnosticsRecorder::run(const GameConfig& config,
                              const std::filesystem::path& output_path,
                              int duration_seconds,
                              int samples_per_second) {
    std::cerr << "[diagnostics] A: enter run\n";
    ShmemReader shmem;
    std::cerr << "[diagnostics] B: shmem constructed\n";
    PaletteRouteWriter ctrl;
    std::cerr << "[diagnostics] C: ctrl constructed\n";
    ctrl.write(config.palette_route, true);
    std::cerr << "[diagnostics] D: ctrl write done\n";

    const int total_samples = duration_seconds * samples_per_second;
    const DWORD sample_interval_ms = 1000 / (samples_per_second > 0 ? samples_per_second : 1);
    std::cerr << "[diagnostics] E: timing initialized\n";

    ULONGLONG start = GetTickCount64();
    ULONGLONG deadline = start + (ULONGLONG)duration_seconds * 1000ull;
    ULONGLONG next_sample = start;
    ULONGLONG next_wait_log = start + 2000ull;
    int sample_index = 0;
    std::cerr << "[diagnostics] F: counters initialized\n";

    std::vector<LayerFrame> last_frames;
    std::cerr << "[diagnostics] G: last_frames ready\n";
    json samples = json::array();
    std::cerr << "[diagnostics] H: samples json ready\n";

    std::cerr << "[diagnostics] recording up to " << total_samples
              << " samples for " << config.rom_name
              << " over " << duration_seconds << " seconds\n";

    while (GetTickCount64() < deadline && sample_index < total_samples) {
        auto new_frames = shmem.poll(config);
        if (!new_frames.empty())
            last_frames = std::move(new_frames);

        ULONGLONG now = GetTickCount64();
        if (now >= next_sample) {
            const auto* palettes = shmem.get_palettes();
            const auto* thumbs = shmem.get_thumbs();
            if (!last_frames.empty() && palettes && thumbs) {
                int elapsed_ms = (int)(now - start);
                samples.push_back(build_sample_json(last_frames, palettes, thumbs, sample_index, elapsed_ms));
                ++sample_index;
                std::cerr << "[diagnostics] sample " << sample_index << "/" << total_samples << "\n";
            } else if (now >= next_wait_log) {
                std::cerr << "[diagnostics] waiting for usable frame data"
                          << " frames=" << (!last_frames.empty() ? "yes" : "no")
                          << " palettes=" << (palettes ? "yes" : "no")
                          << " thumbs=" << (thumbs ? "yes" : "no") << "\n";
                next_wait_log = now + 2000ull;
            }
            next_sample += sample_interval_ms;
        }

        Sleep(10);
    }

    ctrl.write(config.palette_route, false);

    json root;
    root["game"] = config.game;
    root["rom_name"] = config.rom_name;
    root["duration_seconds"] = duration_seconds;
    root["samples_per_second"] = samples_per_second;
    root["sample_count_requested"] = total_samples;
    root["sample_count_captured"] = sample_index;
    root["output_version"] = 1;
    root["virtual_screen"] = { {"width", config.virtual_width}, {"height", config.virtual_height} };
    root["frames"] = std::move(samples);

    std::filesystem::create_directories(output_path.parent_path());
    std::ofstream f(output_path);
    if (!f.is_open()) {
        std::cerr << "[diagnostics] failed to open output file: " << output_path.string() << "\n";
        return false;
    }
    f << root.dump(2) << "\n";
    std::cerr << "[diagnostics] wrote " << output_path.string() << "\n";
    return true;
}
