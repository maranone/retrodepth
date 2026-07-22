#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xaudio2.h>
#include <xinput.h>

#include "snes9x_source.h"
#include "snes9x_layer_capture.h"
#include "libretro.h"

#include <cstdarg>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

// Declare the snes9x libretro core API (defined in snes9x_libretro static lib)
extern "C" {
void retro_set_environment(retro_environment_t);
void retro_set_video_refresh(retro_video_refresh_t);
void retro_set_audio_sample(retro_audio_sample_t);
void retro_set_audio_sample_batch(retro_audio_sample_batch_t);
void retro_set_input_poll(retro_input_poll_t);
void retro_set_input_state(retro_input_state_t);
void retro_init();
void retro_deinit();
bool retro_load_game(const struct retro_game_info*);
void retro_unload_game();
void retro_run();
void retro_get_system_av_info(struct retro_system_av_info*);
void retro_set_controller_port_device(unsigned, unsigned);
}

Snes9xSource* Snes9xSource::s_instance = nullptr;

namespace fs = std::filesystem;

static bool executable_exists_on_path(const char* exe_name) {
    char buf[MAX_PATH] = {};
    DWORD len = SearchPathA(nullptr, exe_name, nullptr, MAX_PATH, buf, nullptr);
    return len > 0 && len < MAX_PATH;
}

// ---- log callback ----------------------------------------------------------

static void snes9x_log_cb(retro_log_level level, const char* fmt, ...) {
    if (level == RETRO_LOG_DEBUG) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

// ---- static libretro callbacks ---------------------------------------------

bool Snes9xSource::s_env_cb(unsigned cmd, void* data) {
    return s_instance ? s_instance->handle_environment(cmd, data) : false;
}

void Snes9xSource::s_video_cb(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (s_instance) s_instance->handle_video_frame(data, w, h, pitch);
}

void Snes9xSource::s_audio_cb(int16_t l, int16_t r) {
    if (!s_instance) return;
    const int16_t buf[2] = {l, r};
    s_instance->push_audio(buf, 1);
}

size_t Snes9xSource::s_audio_batch_cb(const int16_t* data, size_t frames) {
    if (s_instance) s_instance->push_audio(data, (int)frames);
    return frames;
}

void Snes9xSource::s_input_poll_cb() {}

int16_t Snes9xSource::s_input_state_cb(unsigned port, unsigned device,
                                        unsigned index, unsigned id) {
    if (!s_instance || port != 0 || device != RETRO_DEVICE_JOYPAD || index != 0) return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
        return (int16_t)s_instance->joypad_mask();
    const uint32_t btn = s_instance->m_gamepad_buttons;
    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_B:      return (btn & XINPUT_GAMEPAD_A)              ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_A:      return (btn & XINPUT_GAMEPAD_B)              ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_Y:      return (btn & XINPUT_GAMEPAD_X)              ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_X:      return (btn & XINPUT_GAMEPAD_Y)              ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_L:      return (btn & XINPUT_GAMEPAD_LEFT_SHOULDER)  ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_R:      return (btn & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_SELECT: return (btn & XINPUT_GAMEPAD_BACK)           ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_START:  return (btn & XINPUT_GAMEPAD_START)          ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_UP:     return (btn & XINPUT_GAMEPAD_DPAD_UP)        ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_DOWN:   return (btn & XINPUT_GAMEPAD_DPAD_DOWN)      ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_LEFT:   return (btn & XINPUT_GAMEPAD_DPAD_LEFT)      ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return (btn & XINPUT_GAMEPAD_DPAD_RIGHT)     ? 1 : 0;
    default: return 0;
    }
}

// ---- environment handler ---------------------------------------------------

bool Snes9xSource::handle_environment(unsigned cmd, void* data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        static_cast<retro_log_callback*>(data)->log = snes9x_log_cb;
        return true;

    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *static_cast<const char**>(data) = ".";
        return true;

    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        const int fmt = (int)*static_cast<const retro_pixel_format*>(data);
        if (fmt != RETRO_PIXEL_FORMAT_RGB565 && fmt != RETRO_PIXEL_FORMAT_XRGB8888)
            return false;
        m_pixel_format = fmt;
        return true;
    }

    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        return true;

    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *static_cast<bool*>(data) = m_variables_dirty;
        m_variables_dirty = false;
        return true;

    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto* var = static_cast<retro_variable*>(data);
        if (!var || !var->key) return false;
        if (strcmp(var->key, "snes9x_auto_frame_skip") == 0) {
            var->value = "disabled";
            return true;
        }
        if (strcmp(var->key, "snes9x_overclock_superfx") == 0) {
            var->value = "100";
            return true;
        }
        return false;
    }

    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        *static_cast<bool*>(data) = true;
        return true;

    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
        *static_cast<int*>(data) = 3;   // bit 0 = video, bit 1 = audio
        return true;

    default:
        return false;
    }
}

// ---- video callback --------------------------------------------------------

void Snes9xSource::handle_video_frame(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (!data || data == (void*)(intptr_t)-1) return;
    ++m_video_frame_count;
    if (w > 0) m_width  = w;
    if (h > 0) m_height = h;
    m_last_video_data  = data;
    m_last_video_pitch = pitch;

    bool has_visible = false;
    m_last_video_bgra.resize((size_t)w * h * 4);
    uint8_t* dst = m_last_video_bgra.data();
    if (m_pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) {
        const auto* px = static_cast<const uint32_t*>(data);
        const size_t pitch_px = pitch / 4;
        for (unsigned y = 0; y < h; ++y) {
            for (unsigned x = 0; x < w; ++x) {
                const uint32_t src = px[y * pitch_px + x];
                if (src & 0x00FFFFFFu) has_visible = true;
                dst[0] = (uint8_t)(src & 0xFF);
                dst[1] = (uint8_t)((src >> 8) & 0xFF);
                dst[2] = (uint8_t)((src >> 16) & 0xFF);
                dst[3] = 255u;
                dst += 4;
            }
        }
    } else {
        const auto* px = static_cast<const uint16_t*>(data);
        const size_t pitch_px = pitch / 2;
        for (unsigned y = 0; y < h; ++y) {
            for (unsigned x = 0; x < w; ++x) {
                const uint16_t src = px[y * pitch_px + x];
                if (src) has_visible = true;
                dst[0] = (uint8_t)((src & 0x1F) * 255 / 31);
                dst[1] = (uint8_t)(((src >> 5) & 0x3F) * 255 / 63);
                dst[2] = (uint8_t)(((src >> 11) & 0x1F) * 255 / 31);
                dst[3] = 255u;
                dst += 4;
            }
        }
    }
    m_last_frame_had_visible = has_visible;
    if (has_visible)
        m_video_has_visible = true;
}

// ---- audio -----------------------------------------------------------------

void Snes9xSource::push_audio(const int16_t* data, int frames) {
    if (!m_source_voice) return;
    int remaining = frames;
    const int16_t* src = data;
    while (remaining > 0) {
        const int space = kSlotFrames - m_pool_fill;
        const int copy  = (remaining < space) ? remaining : space;
        memcpy(m_audio_pool[m_pool_slot] + m_pool_fill * 2, src, (size_t)copy * 4);
        m_pool_fill += copy;
        src         += copy * 2;
        remaining   -= copy;
        if (m_pool_fill >= kSlotFrames) submit_audio();
    }
}

void Snes9xSource::submit_audio() {
    if (!m_source_voice || m_pool_fill == 0) return;
    XAUDIO2_VOICE_STATE state = {};
    m_source_voice->GetState(&state);
    if ((int)state.BuffersQueued >= kPoolSlots - 1) {
        m_pool_fill = 0;
        return;
    }
    XAUDIO2_BUFFER buf = {};
    buf.AudioBytes = (UINT32)(m_pool_fill * 4);
    buf.pAudioData = reinterpret_cast<const BYTE*>(m_audio_pool[m_pool_slot]);
    m_source_voice->SubmitSourceBuffer(&buf);
    m_pool_slot = (m_pool_slot + 1) % kPoolSlots;
    m_pool_fill = 0;
}

void Snes9xSource::init_xaudio(int sample_rate) {
    if (FAILED(XAudio2Create(&m_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
        fprintf(stderr, "[Snes9xSource] XAudio2Create failed\n");
        return;
    }
    if (FAILED(m_xaudio->CreateMasteringVoice(&m_master_voice))) {
        fprintf(stderr, "[Snes9xSource] CreateMasteringVoice failed\n");
        return;
    }
    WAVEFORMATEX wfx    = {};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = (DWORD)sample_rate;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = 4;
    wfx.nAvgBytesPerSec = (DWORD)sample_rate * 4;
    if (FAILED(m_xaudio->CreateSourceVoice(&m_source_voice, &wfx))) {
        fprintf(stderr, "[Snes9xSource] CreateSourceVoice failed\n");
        return;
    }
    m_source_voice->Start(0);
}

// ---- archive extraction ----------------------------------------------------

std::string Snes9xSource::extract_archive(const std::string& path) {
    char tmp_root[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tmp_root);
    fs::path temp_path = fs::path(tmp_root) / ("snes9x_rd_" + std::to_string(GetCurrentProcessId()));
    std::string temp_dir = temp_path.string();

    std::error_code ec;
    fs::remove_all(temp_path, ec);
    fs::create_directories(temp_path, ec);
    if (ec) {
        fprintf(stderr, "[Snes9xSource] cannot create temp extraction dir: %s\n", temp_dir.c_str());
        return {};
    }

    auto shell_quote = [](const std::string& value) {
        std::string quoted = "'";
        for (char c : value) {
            if (c == '\'') quoted += "''";
            else quoted += c;
        }
        quoted += "'";
        return quoted;
    };

    std::string ext;
    const auto dot = path.rfind('.');
    if (dot != std::string::npos) {
        ext = path.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
    }

    std::string cmd;
    if (ext == ".zip") {
        cmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
              "\"Expand-Archive -LiteralPath " + shell_quote(path) +
              " -DestinationPath " + shell_quote(temp_dir) + " -Force\"";
    } else if (ext == ".7z" && !executable_exists_on_path("7z.exe") &&
               !executable_exists_on_path("7za.exe") &&
               !executable_exists_on_path("7zr.exe")) {
        cmd = "tar -xf \"" + path + "\" -C \"" + temp_dir + "\"";
    } else {
        cmd = "7z e -y -o\"" + temp_dir + "\" \"" + path + "\" >nul 2>&1";
    }
    if (system(cmd.c_str()) != 0) {
        fprintf(stderr, "[Snes9xSource] archive extraction failed: %s\n", path.c_str());
        return {};
    }
    m_temp_dir = temp_dir;

    for (const auto& entry : fs::recursive_directory_iterator(temp_dir, fs::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file())
            continue;
        std::string rom_ext = entry.path().extension().string();
        std::transform(rom_ext.begin(), rom_ext.end(), rom_ext.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (rom_ext == ".sfc" || rom_ext == ".smc") {
            return entry.path().string();
        }
    }
    fprintf(stderr, "[Snes9xSource] no .sfc/.smc in archive: %s\n", path.c_str());
    return {};
}

// ---- joypad mask -----------------------------------------------------------

uint32_t Snes9xSource::joypad_mask() const {
    const uint32_t btn = m_gamepad_buttons;
    uint32_t mask = 0;
    if (btn & XINPUT_GAMEPAD_A)              mask |= 1u << RETRO_DEVICE_ID_JOYPAD_B;
    if (btn & XINPUT_GAMEPAD_B)              mask |= 1u << RETRO_DEVICE_ID_JOYPAD_A;
    if (btn & XINPUT_GAMEPAD_X)              mask |= 1u << RETRO_DEVICE_ID_JOYPAD_Y;
    if (btn & XINPUT_GAMEPAD_Y)              mask |= 1u << RETRO_DEVICE_ID_JOYPAD_X;
    if (btn & XINPUT_GAMEPAD_LEFT_SHOULDER)  mask |= 1u << RETRO_DEVICE_ID_JOYPAD_L;
    if (btn & XINPUT_GAMEPAD_RIGHT_SHOULDER) mask |= 1u << RETRO_DEVICE_ID_JOYPAD_R;
    if (btn & XINPUT_GAMEPAD_BACK)           mask |= 1u << RETRO_DEVICE_ID_JOYPAD_SELECT;
    if (btn & XINPUT_GAMEPAD_START)          mask |= 1u << RETRO_DEVICE_ID_JOYPAD_START;
    if (btn & XINPUT_GAMEPAD_DPAD_UP)        mask |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
    if (btn & XINPUT_GAMEPAD_DPAD_DOWN)      mask |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
    if (btn & XINPUT_GAMEPAD_DPAD_LEFT)      mask |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
    if (btn & XINPUT_GAMEPAD_DPAD_RIGHT)     mask |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
    return mask;
}

// ---- layer frame building --------------------------------------------------

static const char* kLayerIds[SNES9X_LAYER_COUNT] = {
    "bg1_low",     // capture[0] = snes9x BG0 = SNES hardware BG1
    "bg2_low",     // capture[1] = snes9x BG1 = SNES hardware BG2
    "bg3",         // capture[2] = snes9x BG2 = SNES hardware BG3
    "background",  // capture[3] = snes9x BG3 = SNES hardware BG4 / backdrop
    "sprites_low", // capture[4] = OBJ
};

std::vector<LayerFrame> Snes9xSource::build_layer_frames(const GameConfig& cfg) {
    std::vector<LayerFrame> result;
    result.reserve(SNES9X_LAYER_COUNT);
    size_t total_opaque = 0;
    size_t total_colored = 0;

    auto find_layer_config = [&](const char* id) -> const LayerConfig* {
        for (const auto& l : cfg.layers)
            if (l.id == id) return &l;
        return nullptr;
    };

    for (int li = 0; li < SNES9X_LAYER_COUNT; ++li) {
        const char* id = kLayerIds[li];

        const LayerConfig* lc = find_layer_config(id);
        if (!lc) continue;

        unsigned stride = 0;
        const uint16_t* pixels = snes9x_get_layer_pixels(li, &stride);
        const uint8_t*  mask   = snes9x_get_layer_mask(li, nullptr);
        if (!pixels || !mask || stride == 0) continue;

        LayerFrame frame;
        frame.id                = lc->id;
        frame.depth_meters      = lc->depth_meters;
        frame.quad_width_meters = lc->quad_width_meters;
        frame.copies            = lc->copies;
        frame.width             = (int)m_width;
        frame.height            = (int)m_height;
        frame.rgba.resize((size_t)m_width * m_height * 4);

        uint8_t* dst = frame.rgba.data();
        for (unsigned y = 0; y < m_height; ++y) {
            for (unsigned x = 0; x < m_width; ++x) {
                const uint16_t px   = pixels[y * stride + x];
                const uint8_t  opaq = mask[y * stride + x];
                const uint8_t  r    = (uint8_t)(((px >> 11) & 0x1F) * 255 / 31);
                const uint8_t  g    = (uint8_t)(((px >>  5) & 0x3F) * 255 / 63);
                const uint8_t  b    = (uint8_t)( (px        & 0x1F) * 255 / 31);
                // LayerFrame.rgba is BGRA byte order
                dst[0] = b;
                dst[1] = g;
                dst[2] = r;
                dst[3] = opaq ? 255u : 0u;
                if (opaq) ++total_opaque;
                if (opaq && px != 0) ++total_colored;
                dst += 4;
            }
        }
        result.push_back(std::move(frame));
    }

    size_t video_colored = 0;
    if (m_last_frame_had_visible && !m_last_video_bgra.empty()) {
        for (size_t i = 0; i + 3 < m_last_video_bgra.size(); i += 4) {
            if ((m_last_video_bgra[i] | m_last_video_bgra[i + 1] | m_last_video_bgra[i + 2]) != 0)
                ++video_colored;
        }
    }

    bool use_fallback = (total_opaque == 0 || total_colored == 0) &&
                        !m_last_video_bgra.empty() && m_last_frame_had_visible;
    if (!m_logged_frame_stats && m_last_frame_had_visible) {
        fprintf(stderr,
                "[Snes9xSource] first visible frame stats: layers=%zu opaque=%zu colored=%zu video_colored=%zu fallback=%d\n",
                result.size(), total_opaque, total_colored, video_colored, (int)use_fallback);
        m_logged_frame_stats = true;
    }

    if (use_fallback) {
        const LayerConfig* lc = find_layer_config("bg1_low");
        if (!lc && !cfg.layers.empty())
            lc = &cfg.layers.front();
        if (!lc)
            return result;

        LayerFrame frame;
        frame.id                = lc->id;
        frame.depth_meters      = lc->depth_meters;
        frame.quad_width_meters = lc->quad_width_meters;
        frame.copies            = lc->copies;
        frame.width             = (int)m_width;
        frame.height            = (int)m_height;
        frame.rgba = m_last_video_bgra;

        result.clear();
        result.push_back(std::move(frame));
    }
    return result;
}

// ---- constructor -----------------------------------------------------------

Snes9xSource::Snes9xSource(const std::string& rom_path)
    : m_rom_path(rom_path)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    s_instance = this;

    // Resolve ROM: extract archive if needed
    std::string actual_path = rom_path;
    {
        const auto dot = rom_path.rfind('.');
        if (dot != std::string::npos) {
            std::string ext = rom_path.substr(dot);
            for (char& c : ext) c = (char)tolower((unsigned char)c);
            if (ext == ".7z" || ext == ".zip")
                actual_path = extract_archive(rom_path);
        }
    }
    if (actual_path.empty()) {
        fprintf(stderr, "[Snes9xSource] archive extraction yielded no ROM\n");
        CoUninitialize();
        s_instance = nullptr;
        throw std::runtime_error("SNES archive extraction failed: " + rom_path);
    }

    snes9x_set_layer_capture_mask((1u << SNES9X_LAYER_COUNT) - 1u);
    retro_set_environment(s_env_cb);
    retro_set_video_refresh(s_video_cb);
    retro_set_audio_sample(s_audio_cb);
    retro_set_audio_sample_batch(s_audio_batch_cb);
    retro_set_input_poll(s_input_poll_cb);
    retro_set_input_state(s_input_state_cb);
    retro_init();
    retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    retro_set_controller_port_device(1, RETRO_DEVICE_JOYPAD);
    m_core_initialized = true;

    // Load ROM bytes
    std::ifstream f(actual_path, std::ios::binary | std::ios::ate);
    if (!f) {
        fprintf(stderr, "[Snes9xSource] cannot open ROM: %s\n", actual_path.c_str());
        CoUninitialize();
        s_instance = nullptr;
        throw std::runtime_error("Cannot open SNES ROM: " + actual_path);
    }
    const auto sz = f.tellg();
    std::vector<uint8_t> rom_bytes((size_t)sz);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(rom_bytes.data()), sz);
    f.close();

    retro_game_info gi = {};
    gi.path = actual_path.c_str();
    gi.data = rom_bytes.data();
    gi.size = rom_bytes.size();
    if (!retro_load_game(&gi)) {
        fprintf(stderr, "[Snes9xSource] retro_load_game failed: %s\n", actual_path.c_str());
        retro_deinit();
        m_core_initialized = false;
        if (!m_temp_dir.empty()) {
            std::error_code ec;
            fs::remove_all(m_temp_dir, ec);
            m_temp_dir.clear();
        }
        CoUninitialize();
        s_instance = nullptr;
        throw std::runtime_error("Snes9x failed to load ROM: " + actual_path);
    }
    m_game_loaded = true;

    retro_system_av_info av = {};
    retro_get_system_av_info(&av);
    if (av.geometry.base_width  > 0) m_width  = av.geometry.base_width;
    if (av.geometry.base_height > 0) m_height = av.geometry.base_height;
    const int sample_rate = (av.timing.sample_rate > 0.0) ? (int)av.timing.sample_rate : 32000;
    fprintf(stderr, "[Snes9xSource] loaded %ux%u @ %.4f Hz, audio %d Hz\n",
            m_width, m_height, av.timing.fps, sample_rate);
    init_xaudio(sample_rate);

    // Warmup: run up to a few seconds until we see visible pixels. SuperFX
    // games often stay black longer than regular LoROM games during boot.
    for (int i = 0; i < 180; ++i) {
        retro_run();
        submit_audio();
        if (m_video_frame_count > 0 && m_last_frame_had_visible) break;
    }
    fprintf(stderr, "[Snes9xSource] warmup done: %llu frames, visible=%d\n",
            (unsigned long long)m_video_frame_count, (int)m_last_frame_had_visible);
}

// ---- destructor ------------------------------------------------------------

Snes9xSource::~Snes9xSource() {
    if (m_source_voice) { m_source_voice->DestroyVoice(); m_source_voice = nullptr; }
    if (m_master_voice) { m_master_voice->DestroyVoice(); m_master_voice = nullptr; }
    if (m_xaudio)       { m_xaudio->Release();            m_xaudio       = nullptr; }

    if (m_core_initialized) {
        if (m_game_loaded) retro_unload_game();
        retro_deinit();
        m_core_initialized = false;
    }
    if (!m_temp_dir.empty()) {
        std::error_code ec;
        fs::remove_all(m_temp_dir, ec);
        m_temp_dir.clear();
    }
    if (s_instance == this) s_instance = nullptr;
    CoUninitialize();
}

// ---- poll ------------------------------------------------------------------

std::vector<LayerFrame> Snes9xSource::poll(const GameConfig& config) {
    if (!m_game_loaded) return {};

    XINPUT_STATE xi = {};
    if (XInputGetState(0, &xi) == ERROR_SUCCESS)
        m_gamepad_buttons = xi.Gamepad.wButtons;

    retro_run();
    submit_audio();
    return build_layer_frames(config);
}
