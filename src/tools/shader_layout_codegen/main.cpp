#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <set>
#include <nlohmann/json.hpp>

static std::string basename_no_ext(const std::string& path) {
    std::filesystem::path p(path);
    return p.stem().string();
}

static std::string sanitize_identifier(const std::string& s) {
    std::string out;
    for (char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out.push_back(c);
        else out.push_back('_');
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "Usage: shader_layout_codegen --input <report.json> --output <header.h>\n";
        return 1;
    }

    std::string input;
    std::string output;
    for (int i = 1; i + 1 < argc; ++i) {
        std::string a = argv[i];
        if (a == "--input") { input = argv[++i]; }
        else if (a == "--output") { output = argv[++i]; }
    }

    if (input.empty() || output.empty()) {
        std::cerr << "Missing --input or --output\n";
        return 1;
    }

    std::ifstream in(input);
    if (!in.is_open()) { std::cerr << "Failed to open input: " << input << "\n"; return 2; }

    nlohmann::json j;
    try { in >> j; }
    catch (const std::exception& e) { std::cerr << "JSON parse error: " << e.what() << "\n"; return 3; }

    std::ofstream out(output, std::ios::trunc);
    if (!out.is_open()) { std::cerr << "Failed to open output: " << output << "\n"; return 4; }

    out << "// Generated shader layouts header - do not edit.\n";
    out << "#pragma once\n\n";
    out << "#include <cstdint>\n#include <cstddef>\n\n";
    out << "namespace generated {\n\n";

    if (!j.contains("shaders") || !j["shaders"].is_array()) {
        std::cerr << "Input JSON missing 'shaders' array\n";
        return 5;
    }

    for (auto& s : j["shaders"]) {
        std::string path = s.value("path", "");
        std::string name = basename_no_ext(path);
        // Determine stage suffix (vert/frag/comp/other)
        int stage_val = s.value("stage", 0);
        std::string stage_suffix;
        switch (stage_val) {
            case 1: stage_suffix = "vert"; break;   // Vertex
            case 16: stage_suffix = "frag"; break;  // Fragment
            case 32: stage_suffix = "comp"; break;  // Compute
            default: stage_suffix = "stage"; break;
        }
        std::string ns = sanitize_identifier(name + "_" + stage_suffix);
        out << "// ===== shader: " << path << " =====\n";
        out << "namespace " << ns << " {\n";

        // Descriptor sets / bindings
        if (s.contains("bindings") && s["bindings"].is_array()) {
            // Track emitted constants to avoid duplicates
            std::set<std::string> emitted_consts;
            for (auto& setjson : s["bindings"]) {
                if (!setjson.is_object()) continue;
                int set_index = setjson.value("set", 0);
                if (setjson.contains("bindings") && setjson["bindings"].is_array()) {
                    for (auto& b : setjson["bindings"]) {
                        int binding = b.value("binding", -1);
                        std::string bname = b.value("name", "");
                        if (bname.empty()) bname = std::string("binding_") + std::to_string(binding);
                        std::string id = sanitize_identifier(bname);

                        // Determine resource type prefix from descriptor_type when available
                        int dtype = b.value("descriptor_type", -1);
                        std::string prefix = "binding"; // fallback
                        switch (dtype) {
                            case 6: prefix = "uniform"; break; // UNIFORM_BUFFER
                            case 7: prefix = "storage"; break; // STORAGE_BUFFER
                            case 1: prefix = "sampler"; break; // COMBINED_IMAGE_SAMPLER
                            case 3: prefix = "storage_image"; break; // STORAGE_IMAGE
                            default: prefix = "binding"; break;
                        }

                        // Emit set constant per variable (named) to avoid generic SET_ conflicts
                        std::string set_const = prefix + "_" + id + "_set";
                        if (emitted_consts.find(set_const) == emitted_consts.end()) {
                            out << "    inline constexpr uint32_t " << set_const << " = " << set_index << ";\n";
                            emitted_consts.insert(set_const);
                        }

                        // Emit binding constant per variable
                        std::string bind_const = prefix + "_" + id + "_binding";
                        if (emitted_consts.find(bind_const) == emitted_consts.end()) {
                            out << "    inline constexpr uint32_t " << bind_const << " = " << binding << "; // set=" << set_index << "\n";
                            emitted_consts.insert(bind_const);
                        }
                    }
                }
            }
        }

        // Inputs (interface variables)
        if (s.contains("inputs") && s["inputs"].is_array()) {
            for (auto& iv : s["inputs"]) {
                std::string iname = iv.value("name", "");
                uint32_t loc = iv.value("location", UINT32_MAX);
                int built_in = iv.value("built_in", -1);
                if (iname.empty()) continue;
                std::string id = sanitize_identifier(iname);
                // Always emit constant; if location is UINT32_MAX leave value and mark TODO or built-in
                if (loc == UINT32_MAX) {
                    if (built_in != -1) {
                        // Built-in variables should not be emitted as constants; comment instead
                        out << "    // built-in input: " << id << " (" << iname << ")\n";
                    } else {
                        out << "    inline constexpr uint32_t in_" << id << "_location = " << loc << "; // TODO: no explicit location\n";
                    }
                } else {
                    out << "    inline constexpr uint32_t in_" << id << "_location = " << loc << ";\n";
                }
            }
        }

        // Outputs
        if (s.contains("outputs") && s["outputs"].is_array()) {
            for (auto& ov : s["outputs"]) {
                std::string oname = ov.value("name", "");
                uint32_t loc = ov.value("location", UINT32_MAX);
                int built_in = ov.value("built_in", -1);
                if (oname.empty()) continue;
                std::string id = sanitize_identifier(oname);
                if (loc == UINT32_MAX) {
                    if (built_in != -1) {
                        // Built-in variables should not be emitted as constants; comment instead
                        out << "    // built-in output: " << id << " (" << oname << ")\n";
                    } else {
                        out << "    inline constexpr uint32_t out_" << id << "_location = " << loc << "; // TODO: no explicit location\n";
                    }
                } else {
                    out << "    inline constexpr uint32_t out_" << id << "_location = " << loc << ";\n";
                }
            }
        }

        // Push constants
        if (s.contains("push_constants") && s["push_constants"].is_array()) {
            for (auto& pc : s["push_constants"]) {
                std::string pname = pc.value("name", "push_const");
                size_t psz = pc.value("size", 0);
                std::string id = sanitize_identifier(pname);
                out << "    inline constexpr size_t pushconst_" << id << "_size = " << psz << ";\n";
            }
        }

        out << "}\n\n";
    }

    out << "} // namespace generated::shaders\n";
    out.close();

    std::cout << "Generated header: " << output << "\n";
    return 0;
}
