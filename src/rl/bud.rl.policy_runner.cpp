#include "src/rl/bud.rl.policy_runner.hpp"

#include <algorithm>
#include <filesystem>
#include <vector>

#include <onnxruntime/onnxruntime_cxx_api.h>

#include "src/core/bud.logger.hpp"

namespace bud::rl {

    namespace {
        // One environment for the process: creating an Ort::Env per policy is expensive and
        // unnecessary because session options are what carry the per-model settings.
        Ort::Env& shared_env() {
            static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "bud_rl");
            return env;
        }

        bool flatten_shape(const std::vector<int64_t>& shape, int& out_size, std::string& error) {
            out_size = 1;
            for (int64_t dim : shape) {
                if (dim <= 0) {
                    error = "the policy has a dynamic tensor dimension; export it with fixed shapes";
                    return false;
                }
                out_size *= static_cast<int>(dim);
            }
            return true;
        }
    } // namespace

    struct PolicyRunner::Impl {
        Ort::Session session{ nullptr };
        std::vector<std::string> input_names;
        std::vector<std::string> output_names;
        std::vector<const char*> input_name_ptrs;
        std::vector<const char*> output_name_ptrs;
        std::vector<std::vector<int64_t>> input_shapes;
        std::vector<std::vector<int64_t>> output_shapes;
        std::vector<int> input_sizes;  // flattened element counts
        std::vector<int> output_sizes;
        int input_size = 0;            // first input = observation
        int output_size = 0;           // first output = action
        int state_size = 0;            // extra inputs/outputs = opaque recurrent state
    };

    PolicyRunner::PolicyRunner() : impl(std::make_unique<Impl>()) {}

    PolicyRunner::~PolicyRunner() = default;

    bool PolicyRunner::is_loaded() const {
        return impl && impl->session;
    }

    int PolicyRunner::input_size() const {
        return impl ? impl->input_size : 0;
    }

    int PolicyRunner::output_size() const {
        return impl ? impl->output_size : 0;
    }

    int PolicyRunner::state_size() const {
        return impl ? impl->state_size : 0;
    }

    bool PolicyRunner::load(const std::string& path, std::string& error) {
        impl = std::make_unique<Impl>();

        std::vector<char> model_bytes;
        {
            std::ifstream in(path, std::ios::binary | std::ios::ate);
            if (!in) {
                error = "policy file does not exist or cannot be read: " + path;
                return false;
            }
            const std::streamsize size = in.tellg();
            in.seekg(0, std::ios::beg);
            model_bytes.resize(static_cast<size_t>(size));
            if (!in.read(model_bytes.data(), size)) {
                error = "could not read the policy file: " + path;
                return false;
            }
        }

        try {
            Ort::SessionOptions options;
            // The engine drives its own threads; letting ONNX Runtime spin up an intra-op pool would
            // fight the physics/render threads for cores.
            options.SetIntraOpNumThreads(1);
            options.SetInterOpNumThreads(1);
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

            impl->session =
                Ort::Session(shared_env(), model_bytes.data(), model_bytes.size(), options);

            Ort::AllocatorWithDefaultOptions allocator;
            const size_t input_count = impl->session.GetInputCount();
            const size_t output_count = impl->session.GetOutputCount();
            if (input_count == 0 || output_count == 0) {
                error = "the policy has no input or no output tensor";
                return false;
            }
            if (output_count != input_count) {
                error = "recurrent policies must expose as many outputs as inputs (extra tensors are "
                        "read as the new state)";
                return false;
            }

            for (size_t i = 0; i < input_count; ++i) {
                auto name = impl->session.GetInputNameAllocated(i, allocator);
                impl->input_names.emplace_back(name.get());
                impl->input_shapes.push_back(
                    impl->session.GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape());
            }
            for (size_t i = 0; i < output_count; ++i) {
                auto name = impl->session.GetOutputNameAllocated(i, allocator);
                impl->output_names.emplace_back(name.get());
                impl->output_shapes.push_back(
                    impl->session.GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape());
            }
            for (const std::string& name : impl->input_names)
                impl->input_name_ptrs.push_back(name.c_str());
            for (const std::string& name : impl->output_names)
                impl->output_name_ptrs.push_back(name.c_str());

            impl->input_sizes.resize(input_count);
            impl->output_sizes.resize(output_count);
            for (size_t i = 0; i < input_count; ++i) {
                if (!flatten_shape(impl->input_shapes[i], impl->input_sizes[i], error))
                    return false;
            }
            for (size_t i = 0; i < output_count; ++i) {
                if (!flatten_shape(impl->output_shapes[i], impl->output_sizes[i], error))
                    return false;
            }

            impl->input_size = impl->input_sizes[0];
            impl->output_size = impl->output_sizes[0];
            for (size_t i = 1; i < input_count; ++i)
                impl->state_size += impl->input_sizes[i];

            int output_state_size = 0;
            for (size_t i = 1; i < output_count; ++i)
                output_state_size += impl->output_sizes[i];
            if (output_state_size != impl->state_size) {
                error = "the policy's state inputs and outputs do not have matching sizes";
                return false;
            }
        } catch (const std::exception& e) {
            error = std::string("ONNX Runtime: ") + e.what();
            return false;
        } catch (...) {
            error = "ONNX Runtime threw a non-standard exception while loading the policy";
            return false;
        }
        return true;
    }

    bool PolicyRunner::run(const float* obs, float* action, std::string& error) {
        if (state_size() != 0) {
            error = "this policy is recurrent; call run_stepped with a state buffer";
            return false;
        }
        return run_stepped(obs, nullptr, action, error);
    }

    bool PolicyRunner::run_stepped(const float* obs, float* state, float* action, std::string& error) {
        if (!is_loaded()) {
            error = "policy is not loaded";
            return false;
        }
        if (obs == nullptr || action == nullptr) {
            error = "policy input/output buffers are null";
            return false;
        }
        if (impl->state_size > 0 && state == nullptr) {
            error = "this policy needs a recurrent state buffer";
            return false;
        }

        try {
            Ort::MemoryInfo memory_info =
                Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

            std::vector<Ort::Value> inputs;
            inputs.reserve(impl->input_names.size());
            inputs.push_back(Ort::Value::CreateTensor<float>(
                memory_info, const_cast<float*>(obs), static_cast<size_t>(impl->input_sizes[0]),
                impl->input_shapes[0].data(), impl->input_shapes[0].size()));
            int state_offset = 0;
            for (size_t i = 1; i < impl->input_names.size(); ++i) {
                inputs.push_back(Ort::Value::CreateTensor<float>(
                    memory_info, state + state_offset, static_cast<size_t>(impl->input_sizes[i]),
                    impl->input_shapes[i].data(), impl->input_shapes[i].size()));
                state_offset += impl->input_sizes[i];
            }

            auto outputs = impl->session.Run(
                Ort::RunOptions{ nullptr }, impl->input_name_ptrs.data(), inputs.data(),
                inputs.size(), impl->output_name_ptrs.data(), impl->output_name_ptrs.size());
            if (outputs.size() != impl->output_names.size()) {
                error = "the policy returned an unexpected number of tensors";
                return false;
            }

            const size_t action_count = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();
            if (action_count != static_cast<size_t>(impl->output_size)) {
                error = "policy action size changed at run time";
                return false;
            }
            const float* action_data = outputs[0].GetTensorData<float>();
            std::copy(action_data, action_data + action_count, action);

            int out_state_offset = 0;
            for (size_t i = 1; i < outputs.size(); ++i) {
                const size_t count = outputs[i].GetTensorTypeAndShapeInfo().GetElementCount();
                if (count != static_cast<size_t>(impl->output_sizes[i])) {
                    error = "policy state size changed at run time";
                    return false;
                }
                const float* state_data = outputs[i].GetTensorData<float>();
                std::copy(state_data, state_data + count, state + out_state_offset);
                out_state_offset += impl->output_sizes[i];
            }
        } catch (const std::exception& e) {
            error = std::string("ONNX Runtime: ") + e.what();
            return false;
        } catch (...) {
            error = "ONNX Runtime threw a non-standard exception while running the policy";
            return false;
        }
        return true;
    }

} // namespace bud::rl
