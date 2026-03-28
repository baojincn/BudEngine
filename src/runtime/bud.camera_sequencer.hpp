#pragma once

#include <vector>
#include <string>
#include <cstddef>
#include <chrono>
#include <format>
#include <filesystem>

#include "src/core/bud.math.hpp"

// Forward declare to avoid pulling io headers into every translation unit
namespace bud::io {
    class AssetManager;
    class VirtualFileSystem;
}

namespace bud::scene {
    class Camera;
}

namespace bud::scene {

    struct CameraKeyframe {
        float                  time;      // Seconds since recording start
        bud::math::vec3        position;
        bud::math::quaternion  rotation;
    };

    enum class SequencerState {
        IDLE,
        RECORDING,
        PLAYING
    };

    class CameraSequencer {
    public:
        // Recording interval: one keyframe every 0.1 s (10 Hz)
        static constexpr float default_record_interval = 0.1f;

        explicit CameraSequencer(bud::io::AssetManager*       asset_manager = nullptr,
                                  bud::io::VirtualFileSystem* vfs           = nullptr);

        // --- State control ---
        void start_recording(Camera& camera);
        void stop_recording();              // Triggers async save automatically
        void start_playback(bool loop = false);  // One-shot playback (loop=false) or looping
        void stop_playback();
        void toggle_pause();

        // --- Main tick (call from fixed-logic step, every frame) ---
        void update(float dt, Camera& inout_camera);

        // --- Serialization ---
        bool save_to_file(const std::string& filepath) const;      // async via AssetManager
        bool save_to_file_sync(const std::string& filepath) const;  // sync, safe at shutdown
        bool load_from_file(const std::string& filepath);

        // Scans replay_dir, loads the newest camera_track_*.json automatically.
        // Call this after engine init so F9 plays the last recorded session immediately.
        // Returns the path that was loaded, or empty string if nothing found.
        std::string load_latest();

        // --- Shutdown flush ---
        // Must be called before AssetManager is destroyed. If recording is in progress,
        // stops it and writes the track synchronously so data is never lost on exit.
        void flush();

        // --- Accessors ---
        SequencerState get_state()            const { return m_state; }
        bool           is_playing()           const { return m_state == SequencerState::PLAYING; }
        bool           is_recording()         const { return m_state == SequencerState::RECORDING; }
        bool           is_playback_finished() const;
        bool           is_looping()           const { return m_looping; }
        bool           is_paused()            const { return m_paused; }
        void           set_looping(bool loop)       { m_looping = loop; }
        size_t         get_keyframe_count()   const { return m_track.size(); }
        size_t         get_playback_index()   const { return m_playback_index; }

        static constexpr std::string_view replay_dir = "data/replays";

        // Returns a unique timestamped path, e.g. data/replays/camera_track_20260328_170501.json
        static std::string make_timestamped_path();

    private:
        void record_tick(float dt, Camera& camera);
        void playback_tick(float dt, Camera& inout_camera);

        SequencerState               m_state            = SequencerState::IDLE;
        std::vector<CameraKeyframe>  m_track;

        float   m_timer              = 0.0f;
        float   m_record_interval    = default_record_interval;
        float   m_last_record_time   = 0.0f;
        size_t  m_playback_index     = 0;

        bool           m_looping          = false;
        bool           m_paused           = false;
        bud::io::AssetManager*      m_asset_manager = nullptr;
        bud::io::VirtualFileSystem* m_vfs           = nullptr;
    };

} // namespace bud::scene
