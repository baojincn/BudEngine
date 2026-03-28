#include "src/runtime/bud.camera_sequencer.hpp"
#include "src/runtime/bud.scene.hpp"
#include "src/io/bud.io.hpp"
#include "src/core/bud.logger.hpp"

#include <nlohmann/json.hpp>
#include <format>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <filesystem>

namespace bud::scene {

    // -------------------------------------------------------------------------
    // Static helpers
    // -------------------------------------------------------------------------

    std::string CameraSequencer::make_timestamped_path() {
        // Format: data/replays/camera_track_YYYYMMDD_HHMMSS.json
        auto now    = std::chrono::system_clock::now();
        auto now_tt = std::chrono::system_clock::to_time_t(now);
        struct tm local_tm;
#if defined(_WIN32)
        localtime_s(&local_tm, &now_tt);
#else
        localtime_r(&now_tt, &local_tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &local_tm);
        return std::format("{}/camera_track_{}.json", replay_dir, buf);
    }


    CameraSequencer::CameraSequencer(bud::io::AssetManager* asset_manager,
                                       bud::io::VirtualFileSystem* vfs)
        : m_asset_manager(asset_manager), m_vfs(vfs)
    {
    }

    // -------------------------------------------------------------------------
    // State control
    // -------------------------------------------------------------------------

    void CameraSequencer::start_recording(Camera& camera) {
        m_track.clear();
        m_timer             = 0.0f;
        m_last_record_time  = 0.0f;

        // Capture the very first frame immediately so playback starts from
        // the exact position where recording began.
        m_track.push_back({
            0.0f,
            camera.position,
            camera.get_rotation()
        });

        m_state = SequencerState::RECORDING;
        bud::print("[CameraSequencer] Recording started.");
    }

    void CameraSequencer::stop_recording() {
        if (m_state != SequencerState::RECORDING) {
            return;
        }
        m_state = SequencerState::IDLE;
        bud::print("[CameraSequencer] Recording stopped. {} keyframes captured.", m_track.size());

        // Auto-save asynchronously with a timestamped filename
        save_to_file(make_timestamped_path());
    }

    void CameraSequencer::start_playback(bool loop) {
        if (m_track.empty()) {
            bud::print("[CameraSequencer] Cannot start playback: track is empty.");
            return;
        }
        m_looping         = loop;
        m_paused          = false;
        m_timer           = 0.0f;
        m_playback_index  = 0;
        m_state           = SequencerState::PLAYING;
        bud::print("[CameraSequencer] Playback started ({}) — {} keyframes, duration={:.2f}s",
            loop ? "LOOP" : "ONE-SHOT", m_track.size(), m_track.back().time);
    }

    void CameraSequencer::stop_playback() {
        m_state = SequencerState::IDLE;
        m_paused = false;
        bud::print("[CameraSequencer] Playback stopped.");
    }

    void CameraSequencer::toggle_pause() {
        if (m_state == SequencerState::PLAYING) {
            m_paused = !m_paused;
            bud::print("[CameraSequencer] Playback {}", m_paused ? "PAUSED" : "RESUMED");
        }
    }

    // -------------------------------------------------------------------------
    // Main tick
    // -------------------------------------------------------------------------

    void CameraSequencer::update(float dt, Camera& inout_camera) {
        switch (m_state) {
        case SequencerState::RECORDING:
            record_tick(dt, inout_camera);
            break;
        case SequencerState::PLAYING:
            playback_tick(dt, inout_camera);
            break;
        default:
            break;
        }
    }

    void CameraSequencer::record_tick(float dt, Camera& camera) {
        m_timer += dt;
        if (m_timer - m_last_record_time >= m_record_interval) {
            m_track.push_back({
                m_timer,
                camera.position,
                camera.get_rotation()
            });
            m_last_record_time = m_timer;
        }
    }

    void CameraSequencer::playback_tick(float dt, Camera& inout_camera) {
        if (m_track.empty()) {
            return;
        }

        if (!m_paused) {
            m_timer += dt;
        }

        const float total_duration = m_track.back().time;

        // Reached end of track
        if (m_timer >= total_duration) {
            inout_camera.position = m_track.back().position;
            inout_camera.set_rotation(m_track.back().rotation);

            if (m_looping) {
                // Wrap: carry over any overshoot so timing stays accurate
                m_timer         = std::fmod(m_timer, total_duration);
                m_playback_index = 0;
                bud::print("[CameraSequencer] Playback looped.");
            } else {
                m_state = SequencerState::IDLE;
                bud::print("[CameraSequencer] Playback finished.");
            }
            return;
        }

        // Advance cached interval index (mono-increasing timer means we never go back)
        while (m_playback_index < m_track.size() - 2 &&
               m_timer > m_track[m_playback_index + 1].time)
        {
            ++m_playback_index;
        }

        const CameraKeyframe& kf0 = m_track[m_playback_index];
        const CameraKeyframe& kf1 = m_track[m_playback_index + 1];

        const float interval = kf1.time - kf0.time;
        // Guard against degenerate zero-length intervals
        const float t = (interval > 1e-6f) ? ((m_timer - kf0.time) / interval) : 1.0f;
        const float t_clamped = std::clamp(t, 0.0f, 1.0f);

        inout_camera.position = bud::math::lerp(kf0.position, kf1.position, t_clamped);
        inout_camera.set_rotation(bud::math::slerp(kf0.rotation, kf1.rotation, t_clamped));
    }

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------

    bool CameraSequencer::is_playback_finished() const {
        return m_state == SequencerState::IDLE && !m_track.empty();
    }

    // -------------------------------------------------------------------------
    // Serialization
    // -------------------------------------------------------------------------

    bool CameraSequencer::save_to_file(const std::string& filepath) const {
        if (!m_asset_manager) {
            bud::eprint("[CameraSequencer] save_to_file: no AssetManager, cannot save.");
            return false;
        }

        using json = nlohmann::json;

        json j;
        j["duration"]  = m_track.empty() ? 0.0f : m_track.back().time;
        j["keyframes"] = json::array();

        for (const CameraKeyframe& kf : m_track) {
            j["keyframes"].push_back({
                {"time", kf.time},
                {"pos",  {kf.position.x,  kf.position.y,  kf.position.z}},
                {"rot",  {kf.rotation.x,  kf.rotation.y,  kf.rotation.z, kf.rotation.w}}
            });
        }

        m_asset_manager->save_json_async(filepath, j, [filepath](bool ok) {
            if (ok)
                bud::print("[CameraSequencer] Saved to '{}'.", filepath);
            else
                bud::eprint("[CameraSequencer] Failed to save to '{}'.", filepath);
        });

        return true;
    }

    bool CameraSequencer::save_to_file_sync(const std::string& filepath) const {
        if (!m_vfs) {
            bud::eprint("[CameraSequencer] save_to_file_sync: no VirtualFileSystem.");
            return false;
        }

        using json = nlohmann::json;
        json j;
        j["duration"]  = m_track.empty() ? 0.0f : m_track.back().time;
        j["keyframes"] = json::array();
        for (const CameraKeyframe& kf : m_track) {
            j["keyframes"].push_back({
                {"time", kf.time},
                {"pos",  {kf.position.x, kf.position.y, kf.position.z}},
                {"rot",  {kf.rotation.x, kf.rotation.y, kf.rotation.z, kf.rotation.w}}
            });
        }

        // Write directly through VFS — the sync layer. Safe at shutdown when the
        // task scheduler may no longer be available to process async tasks.
        return m_vfs->write_json(filepath, j);
    }

    void CameraSequencer::flush() {
        if (m_state == SequencerState::RECORDING) {
            // Stop without triggering the async save
            m_state = SequencerState::IDLE;
            bud::print("[CameraSequencer] flush: stopping recording ({} keyframes). Saving synchronously...",
                m_track.size());
            save_to_file_sync(make_timestamped_path());
        }
        // If a playback was running, just idle it — no data to save.
        else if (m_state == SequencerState::PLAYING) {
            m_state = SequencerState::IDLE;
        }
    }

    bool CameraSequencer::load_from_file(const std::string& filepath) {
        if (!m_asset_manager) {
            bud::eprint("[CameraSequencer] load_from_file: no AssetManager.");
            return false;
        }

        // Fire-and-forget async load. The callback runs on the main thread when it
        // pumps its task queue (each frame start). No spin-wait — no deadlock.
        m_asset_manager->load_json_async(filepath, [this, filepath](const nlohmann::json& j) {
            try {
                m_track.clear();
                m_timer            = 0.0f;
                m_last_record_time = 0.0f;
                m_playback_index   = 0;

                for (const auto& kf_j : j["keyframes"]) {
                    CameraKeyframe kf;
                    kf.time       = kf_j["time"].get<float>();
                    kf.position.x = kf_j["pos"][0].get<float>();
                    kf.position.y = kf_j["pos"][1].get<float>();
                    kf.position.z = kf_j["pos"][2].get<float>();
                    kf.rotation.x = kf_j["rot"][0].get<float>();
                    kf.rotation.y = kf_j["rot"][1].get<float>();
                    kf.rotation.z = kf_j["rot"][2].get<float>();
                    kf.rotation.w = kf_j["rot"][3].get<float>();
                    m_track.push_back(kf);
                }

                bud::print("[CameraSequencer] Loaded {} keyframes from '{}'. Press F9 to play.",
                    m_track.size(), filepath);
            }
            catch (const std::exception& e) {
                bud::eprint("[CameraSequencer] Failed to parse '{}': {}", filepath, e.what());
                m_track.clear();
            }
        });

        return true; // Async: track will be populated once the callback fires.
    }


    std::string CameraSequencer::load_latest() {
        if (!m_asset_manager) {
            return {};
        }

        // Scan replay_dir for camera_track_*.json files.
        // Since filenames embed YYYYMMDD_HHMMSS, lexicographic max == chronological latest.
        namespace fs = std::filesystem;

        // replay_dir is relative; build absolute path via root_path heuristic:
        // try cwd / replay_dir first, then parent directories.
        fs::path dir(replay_dir);
        if (!dir.is_absolute()) {
            std::error_code ec;
            auto cwd = fs::current_path(ec);
            dir = cwd / dir;
        }

        std::error_code ec;
        if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
            bud::print("[CameraSequencer] load_latest: replay dir '{}' not found, skipping.",
                dir.string());
            return {};
        }

        std::string latest_name;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            const auto& p = entry.path();
            if (p.extension() != ".json") continue;
            const auto stem = p.stem().string(); // "camera_track_YYYYMMDD_HHMMSS"
            if (stem.starts_with("camera_track_")) {
                if (stem > latest_name)
                    latest_name = stem;
            }
        }

        if (latest_name.empty()) {
            bud::print("[CameraSequencer] load_latest: no replay files found in '{}'.",
                dir.string());
            return {};
        }

        // Reconstruct relative path for load_from_file (which will resolve via VFS root)
        const std::string rel_path = std::format("{}/{}.json", replay_dir, latest_name);
        bud::print("[CameraSequencer] load_latest: loading '{}'.", rel_path);

        if (load_from_file(rel_path))
            return rel_path;

        return {};
    }

} // namespace bud::scene
