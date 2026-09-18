#pragma once

// Dependency-free runtime for the published legged-robot policy shapes: a pure MLP actor, or one
// LSTM layer followed by an ELU MLP actor. Weights come from a flat float32 file in the order
// documented below, so deploying a policy needs neither ONNX Runtime nor a protobuf dependency.
// This exists because the vcpkg ONNX Runtime terminates on graphs that contain initializers; it is
// also a better fit for hardware-portable deployments.

#include <string>
#include <vector>

namespace bud::rl {

    class DensePolicy {
    public:
        enum class Kind {
            None,
            Mlp,
            LstmMlp,
        };

        struct Layout {
            Kind kind = Kind::None;
            int input_size = 0;
            int output_size = 0;
            // Ml p: [in, h1, h2, ..., out]; every layer except the last applies ELU.
            std::vector<int> layer_sizes;
            // LstmMlp
            int hidden_size = 0;
            int actor_hidden = 0;
            int layers = 1;
        };

        static Layout make_mlp(std::vector<int> layer_sizes);
        static Layout make_lstm_mlp(int input_size, int hidden_size, int actor_hidden, int output_size);

        // Weight file layout (little-endian float32, no header):
        //   Ml p:     for each layer: weight [out x in], bias [out]
        //   LstmMl p: w_ih [4H x in], w_hh [4H x H], b_ih [4H], b_hh [4H],
        //             actor_w0 [A x H], actor_b0 [A], actor_w2 [out x A], actor_b2 [out]
        // LSTM gate order is PyTorch's: input, forget, cell, output.
        bool load(const std::string& weights_path, const Layout& layout, std::string& error);

        bool is_loaded() const { return loaded; }
        const Layout& get_layout() const { return layout; }
        // Opaque recurrent state (LSTM hidden then cell); zero for a pure MLP.
        int state_size() const { return layout.kind == Kind::LstmMlp ? 2 * layout.layers * layout.hidden_size : 0; }

        // obs/action are sized layout.input_size / layout.output_size. state is a flat buffer of
        // state_size() floats, updated in place (may be null for a pure MLP).
        bool run(const float* obs, float* state, float* action, std::string& error);

    private:
        Layout layout;
        bool loaded = false;

        // MLP
        std::vector<std::vector<float>> layer_w;
        std::vector<std::vector<float>> layer_b;
        std::vector<float> layer_scratch_a;
        std::vector<float> layer_scratch_b;

        // LSTM
        std::vector<float> w_ih;
        std::vector<float> w_hh;
        std::vector<float> b_ih;
        std::vector<float> b_hh;
        std::vector<float> actor_w0;
        std::vector<float> actor_b0;
        std::vector<float> actor_w2;
        std::vector<float> actor_b2;
        std::vector<float> gates;
        std::vector<float> hidden;

        bool run_mlp(const float* obs, float* action);
        bool run_lstm(const float* obs, float* state, float* action);
    };

} // namespace bud::rl
