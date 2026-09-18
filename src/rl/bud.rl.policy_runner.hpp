#pragma once

// Thin ONNX Runtime wrapper for a locomotion policy: one float input tensor in, one float output
// tensor out. The engine stays free of ONNX headers through the pimpl.

#include <memory>
#include <string>

namespace bud::rl {

    class PolicyRunner {
    public:
        PolicyRunner();
        ~PolicyRunner();
        PolicyRunner(const PolicyRunner&) = delete;
        PolicyRunner& operator=(const PolicyRunner&) = delete;

        bool load(const std::string& path, std::string& error);
        bool is_loaded() const;
        int input_size() const;
        int output_size() const;

        // Extra inputs/outputs beyond the first are treated as an opaque recurrent state (the layout
        // is whatever the exported graph declares), so LSTM/GRU policies work without the engine
        // knowing their internals. state_size() == 0 means the policy is feed-forward.
        int state_size() const;

        // obs must hold input_size() floats, action output_size().
        bool run(const float* obs, float* action, std::string& error);
        // Feed-forward policies ignore state; recurrent ones read and update it in place.
        bool run_stepped(const float* obs, float* state, float* action, std::string& error);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

} // namespace bud::rl
