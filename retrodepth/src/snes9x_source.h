#pragma once
#include <cstdint>
#include <string>
#include "shmem_reader.h"

// Forward-declare COM interfaces so this header needs no Windows headers
struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

class Snes9xSource : public IFrameSource {
public:
    explicit Snes9xSource(const std::string& rom_path);
    ~Snes9xSource() override;

    std::vector<LayerFrame> poll(const GameConfig& config) override;
    bool is_connected() override { return m_game_loaded; }

private:
    // Static libretro callbacks — dispatch via s_instance
    static bool    s_env_cb(unsigned cmd, void* data);
    static void    s_video_cb(const void* data, unsigned w, unsigned h, size_t pitch);
    static void    s_audio_cb(int16_t l, int16_t r);
    static size_t  s_audio_batch_cb(const int16_t* data, size_t frames);
    static void    s_input_poll_cb();
    static int16_t s_input_state_cb(unsigned port, unsigned device,
                                    unsigned index, unsigned id);

    bool        handle_environment(unsigned cmd, void* data);
    void        handle_video_frame(const void* data, unsigned w, unsigned h, size_t pitch);
    void        push_audio(const int16_t* data, int frames);
    void        submit_audio();
    void        init_xaudio(int sample_rate);
    std::string extract_archive(const std::string& path);
    std::vector<LayerFrame> build_layer_frames(const GameConfig& cfg);
    uint32_t    joypad_mask() const;

    static Snes9xSource* s_instance;

    std::string m_rom_path;
    std::string m_temp_dir;
    bool        m_core_initialized       = false;
    bool        m_game_loaded            = false;
    bool        m_variables_dirty        = false;
    uint64_t    m_video_frame_count      = 0;
    bool        m_last_frame_had_visible = false;
    bool        m_video_has_visible      = false;
    bool        m_logged_frame_stats     = false;
    int         m_pixel_format           = 0;   // 0 = RGB565
    unsigned    m_width                  = 256;
    unsigned    m_height                 = 224;
    const void* m_last_video_data        = nullptr;
    size_t      m_last_video_pitch       = 0;
    std::vector<uint8_t> m_last_video_bgra;

    IXAudio2*               m_xaudio       = nullptr;
    IXAudio2MasteringVoice* m_master_voice = nullptr;
    IXAudio2SourceVoice*    m_source_voice = nullptr;

    static constexpr int kPoolSlots  = 4;
    static constexpr int kSlotFrames = 2400;
    int16_t m_audio_pool[kPoolSlots][kSlotFrames * 2] = {};
    int     m_pool_slot                                = 0;
    int     m_pool_fill                                = 0;

    uint32_t m_gamepad_buttons = 0;
};
