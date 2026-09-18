#include "src/rl/bud.rl.dense_policy.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>

namespace bud::rl {

    namespace {
        float sigmoid(float x) {
            return 1.0f / (1.0f + std::exp(-x));
        }

        float elu(float x) {
            return x > 0.0f ? x : std::expm1(x);
        }
    } // namespace

    DensePolicy::Layout DensePolicy::make_mlp(std::vector<int> layer_sizes) {
        Layout out;
        out.kind = Kind::Mlp;
        out.layer_sizes = std::move(layer_sizes);
        if (!out.layer_sizes.empty()) {
            out.input_size = out.layer_sizes.front();
            out.output_size = out.layer_sizes.back();
        }
        return out;
    }

    DensePolicy::Layout DensePolicy::make_lstm_mlp(int input_size, int hidden_size, int actor_hidden,
                                                   int output_size) {
        Layout out;
        out.kind = Kind::LstmMlp;
        out.input_size = input_size;
        out.output_size = output_size;
        out.hidden_size = hidden_size;
        out.actor_hidden = actor_hidden;
        out.layers = 1;
        return out;
    }

    bool DensePolicy::load(const std::string& weights_path, const Layout& in_layout, std::string& error) {
        layout = in_layout;
        loaded = false;
        layer_w.clear();
        layer_b.clear();

        if (layout.kind == Kind::None) {
            error = "dense policy layout has no kind";
            return false;
        }

        // Expected element counts per layout.
        std::vector<size_t> chunk_sizes;
        switch (layout.kind) {
            case Kind::Mlp: {
                if (layout.layer_sizes.size() < 2) {
                    error = "an MLP needs at least an input and an output layer";
                    return false;
                }
                for (size_t i = 0; i + 1 < layout.layer_sizes.size(); ++i) {
                    const size_t in = static_cast<size_t>(layout.layer_sizes[i]);
                    const size_t out = static_cast<size_t>(layout.layer_sizes[i + 1]);
                    if (in == 0 || out == 0) {
                        error = "an MLP layer has a zero dimension";
                        return false;
                    }
                    chunk_sizes.push_back(out * in);
                    chunk_sizes.push_back(out);
                }
                break;
            }
            case Kind::LstmMlp: {
                if (layout.input_size <= 0 || layout.hidden_size <= 0 || layout.actor_hidden <= 0 ||
                    layout.output_size <= 0) {
                    error = "dense policy layout has a non-positive dimension";
                    return false;
                }
                if (layout.layers != 1) {
                    error = "the dense policy loader currently supports exactly one LSTM layer";
                    return false;
                }
                const size_t gate_count = static_cast<size_t>(4 * layout.hidden_size);
                chunk_sizes = {
                    gate_count * static_cast<size_t>(layout.input_size),
                    gate_count * static_cast<size_t>(layout.hidden_size),
                    gate_count,
                    gate_count,
                    static_cast<size_t>(layout.actor_hidden) * layout.hidden_size,
                    static_cast<size_t>(layout.actor_hidden),
                    static_cast<size_t>(layout.output_size) * layout.actor_hidden,
                    static_cast<size_t>(layout.output_size),
                };
                break;
            }
            case Kind::None:
                break;
        }

        size_t expected = 0;
        for (size_t size : chunk_sizes)
            expected += size;

        std::ifstream in(weights_path, std::ios::binary | std::ios::ate);
        if (!in) {
            error = "cannot open policy weights: " + weights_path;
            return false;
        }
        const std::streamsize bytes = in.tellg();
        in.seekg(0, std::ios::beg);
        if (bytes < 0 || static_cast<size_t>(bytes) != expected * sizeof(float)) {
            error = "policy weights have the wrong size: expected " + std::to_string(expected) +
                    " floats, file holds " +
                    std::to_string(bytes / static_cast<std::streamsize>(sizeof(float)));
            return false;
        }

        std::vector<float> all(expected);
        if (!in.read(reinterpret_cast<char*>(all.data()), bytes)) {
            error = "could not read policy weights: " + weights_path;
            return false;
        }

        size_t offset = 0;
        auto take = [&](std::vector<float>& out, size_t count) {
            out.assign(all.begin() + static_cast<std::ptrdiff_t>(offset),
                       all.begin() + static_cast<std::ptrdiff_t>(offset + count));
            offset += count;
        };

        if (layout.kind == Kind::Mlp) {
            for (size_t layer = 0; layer + 1 < layout.layer_sizes.size(); ++layer) {
                const size_t in_dim = static_cast<size_t>(layout.layer_sizes[layer]);
                const size_t out_dim = static_cast<size_t>(layout.layer_sizes[layer + 1]);
                layer_w.emplace_back();
                layer_b.emplace_back();
                take(layer_w.back(), out_dim * in_dim);
                take(layer_b.back(), out_dim);
            }
            const size_t max_dim = static_cast<size_t>(*std::max_element(layout.layer_sizes.begin(),
                                                                        layout.layer_sizes.end()));
            layer_scratch_a.assign(max_dim, 0.0f);
            layer_scratch_b.assign(max_dim, 0.0f);
        } else {
            take(w_ih, chunk_sizes[0]);
            take(w_hh, chunk_sizes[1]);
            take(b_ih, chunk_sizes[2]);
            take(b_hh, chunk_sizes[3]);
            take(actor_w0, chunk_sizes[4]);
            take(actor_b0, chunk_sizes[5]);
            take(actor_w2, chunk_sizes[6]);
            take(actor_b2, chunk_sizes[7]);
            gates.assign(static_cast<size_t>(4 * layout.hidden_size), 0.0f);
            hidden.assign(static_cast<size_t>(layout.actor_hidden), 0.0f);
        }

        loaded = true;
        error.clear();
        return true;
    }

    bool DensePolicy::run(const float* obs, float* state, float* action, std::string& error) {
        if (!loaded) {
            error = "dense policy is not loaded";
            return false;
        }
        if (obs == nullptr || action == nullptr) {
            error = "dense policy input/output buffers are null";
            return false;
        }
        if (layout.kind == Kind::Mlp) {
            if (state != nullptr) {
                error = "a pure MLP policy does not take a recurrent state";
                return false;
            }
            return run_mlp(obs, action);
        }
        if (state == nullptr) {
            error = "this policy needs a recurrent state buffer";
            return false;
        }
        return run_lstm(obs, state, action);
    }

    bool DensePolicy::run_mlp(const float* obs, float* action) {
        // Two scratch buffers alternate so a layer never writes over the activations it is reading.
        float* scratch[2] = { layer_scratch_a.data(), layer_scratch_b.data() };
        const float* input = obs;
        const size_t layer_count = layout.layer_sizes.size() - 1;
        for (size_t layer = 0; layer < layer_count; ++layer) {
            const size_t in_dim = static_cast<size_t>(layout.layer_sizes[layer]);
            const size_t out_dim = static_cast<size_t>(layout.layer_sizes[layer + 1]);
            const bool is_last = layer + 1 == layer_count;
            float* output = is_last ? action : scratch[layer % 2];
            for (size_t row = 0; row < out_dim; ++row) {
                const float* weight_row = &layer_w[layer][row * in_dim];
                float sum = layer_b[layer][row];
                for (size_t i = 0; i < in_dim; ++i)
                    sum += weight_row[i] * input[i];
                output[row] = is_last ? sum : elu(sum);
            }
            input = output;
        }
        return true;
    }

    bool DensePolicy::run_lstm(const float* obs, float* state, float* action) {
        const int H = layout.hidden_size;
        const int in = layout.input_size;
        const int A = layout.actor_hidden;
        const int out = layout.output_size;

        const float* h = state;
        const float* c = state + H;

        for (int row = 0; row < 4 * H; ++row) {
            float sum = b_ih[static_cast<size_t>(row)] + b_hh[static_cast<size_t>(row)];
            const float* w_ih_row = &w_ih[static_cast<size_t>(row) * in];
            for (int i = 0; i < in; ++i)
                sum += w_ih_row[i] * obs[i];
            const float* w_hh_row = &w_hh[static_cast<size_t>(row) * H];
            for (int k = 0; k < H; ++k)
                sum += w_hh_row[k] * h[k];
            gates[static_cast<size_t>(row)] = sum;
        }

        float* h_out = state;
        float* c_out = state + H;
        for (int k = 0; k < H; ++k) {
            const float i_gate = sigmoid(gates[static_cast<size_t>(k)]);
            const float f_gate = sigmoid(gates[static_cast<size_t>(H + k)]);
            const float g_gate = std::tanh(gates[static_cast<size_t>(2 * H + k)]);
            const float o_gate = sigmoid(gates[static_cast<size_t>(3 * H + k)]);
            const float c_new = f_gate * c[k] + i_gate * g_gate;
            c_out[k] = c_new;
            h_out[k] = o_gate * std::tanh(c_new);
        }

        for (int a = 0; a < A; ++a) {
            float sum = actor_b0[static_cast<size_t>(a)];
            const float* row = &actor_w0[static_cast<size_t>(a) * H];
            for (int k = 0; k < H; ++k)
                sum += row[k] * h_out[k];
            hidden[static_cast<size_t>(a)] = elu(sum);
        }
        for (int o = 0; o < out; ++o) {
            float sum = actor_b2[static_cast<size_t>(o)];
            const float* row = &actor_w2[static_cast<size_t>(o) * A];
            for (int a = 0; a < A; ++a)
                sum += row[a] * hidden[static_cast<size_t>(a)];
            action[o] = sum;
        }
        return true;
    }

} // namespace bud::rl
