#include "bud.asset.processor.hpp"
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cfloat>
#include <limits>
#include <map>
#include <unordered_map>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <meshoptimizer.h>
#include <optional>
#include <cmath>
#include <chrono>

#include "../bud_tool_support/bud_tool_support.hpp"
#if defined(__has_include)
# if __has_include(<spirv_reflect.h>)
#  ifndef SPIRV_REFLECT_USE_SYSTEM_SPIRV_H
#   define SPIRV_REFLECT_USE_SYSTEM_SPIRV_H 1
#  endif
#  include <spirv_reflect.h>
#  define BUD_HAVE_SPIRV_REFLECT 1
# endif
#endif
#include <filesystem>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <thread>
#include <future>
#include <atomic>
#include <mutex>
#include <functional>
// note: avoid depending on bud::io; use local helpers above

#if defined(BUD_HAVE_SPIRV_REFLECT)
static bool compile_shader_with_glslc(const std::filesystem::path& src, const std::filesystem::path& out_spv);
static bool reflect_and_validate_spv(const std::filesystem::path& spv_path);
#include <sstream>

static bool compile_shader_with_glslc(const std::filesystem::path& src, const std::filesystem::path& out_spv) {
    // Use process runner to capture output instead of manual redirection
    std::ostringstream cmd;
    cmd << "glslc " << '"' << src.string() << '"' << " -o " << '"' << out_spv.string() << '"';
    auto res = bud::tool_support::run_process_capture(cmd.str());
    if (!res.stderr_str.empty()) {
        // write compiler log next to spv
        std::filesystem::path log_path = out_spv;
        log_path += ".log";
        bud::tool_support::write_text_file_atomic(log_path, res.stderr_str);
    }
    return res.exit_code == 0;
}

static bool reflect_and_validate_spv(const std::filesystem::path& spv_path) {
    // Minimal reflection validation: attempt to create and destroy a module
    std::ifstream in(spv_path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return false;
    auto size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<char> data(size);
    in.read(data.data(), size);
    SpvReflectShaderModule module;
    SpvReflectResult res = spvReflectCreateShaderModule(data.size(), data.data(), &module);
    if (res != SPV_REFLECT_RESULT_SUCCESS) return false;
    spvReflectDestroyShaderModule(&module);
    return true;
}
#endif

namespace {

    // ====================================================================
    // Nanite cluster DAG builder (UE5-aligned)
    //
    // 自底向上构建 per-mesh cluster 层次 DAG：
    //   - Level 0 = 原始网格 meshlet 化（<=128 顶点 / 128 三角形）
    //   - 上层 = 下层 groups 合并几何的简化父 cluster
    //   - cluster -> group（父，更粗）；group -> children groups（更细）
    //   - children 范围保持连续：上层 clusters 按父 group 顺序生成（空间
    //     近似有序），无需再次全局排序即可保证连续
    // ====================================================================

    struct NaniteClusterBuild {
        uint32_t material_index = 0;
        uint32_t level = 0;
        uint32_t group_index = bud::asset::INVALID_INDEX;   // 所属（更粗）group
        uint32_t source_group = bud::asset::INVALID_INDEX;  // 来源（更细）group，构建期用
        float lod_error = 0.0f;          // 相对父 cluster 的误差
        float parent_lod_error = 0.0f;   // 父 cluster 的 lod_error（页回退用）
        std::vector<bud::asset::Vertex> vertices;   // 对象空间顶点（局部）
        std::vector<uint32_t> indices;              // 局部三角形列表
        float bounds_min[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
        float bounds_max[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        float lod_bounds_center[3] = { 0.0f, 0.0f, 0.0f };
        float lod_bounds_radius = 0.0f;
        float cone_axis[3] = { 0.0f, 0.0f, 1.0f };
        float cone_cutoff = 0.0f;
        // 页分配/序列化结果（后续步骤填充）
        uint32_t page_index = bud::asset::INVALID_INDEX;
        uint32_t page_vertex_offset = 0;
        uint32_t page_index_offset = 0;
    };

    struct NaniteGroupBuild {
        uint32_t level = 0;
        uint32_t cluster_start = 0;
        uint32_t cluster_count = 0;
        uint32_t children_start = bud::asset::INVALID_INDEX;
        uint32_t children_count = 0;
        float lod_bounds_center[3] = { 0.0f, 0.0f, 0.0f };
        float lod_bounds_radius = 0.0f;
        float lod_error = 0.0f;
    };

    // 简化目标误差：meshopt 的 target_error 语义为“相对输入网格尺寸的比例”
    // （如 0.01 = 1% 变形，见 meshopt SimplifyErrorAbsolute 注释）。这里传纯相对值。
    // 注意：不能把值放得过大，否则 meshopt 的 performEdgeCollapses 会因“目标按
    // 每折叠 1-2 三角形计数、实际按退化三角形移除远超目标”而级联折叠到 0 条索引
    // （实测 4.0×radius 会返回 0）。0.02 = 2% 网格尺寸是较合理的 LOD 简化预算。
    constexpr float NANITE_SIMPLIFY_RELATIVE_ERROR = 0.02f;

    // 每个父 cluster 合并的子 cluster 数（空间相邻同材质为一组）。
    // 二分 DAG（2 个一组）对齐 UE5 MaxClustersPerGroup=2；父 cluster 简化时锁定
    // 边界顶点，保证跨 LOD 无缝。
    constexpr uint32_t NANITE_GROUP_SIZE = 2;

    // 层级构建对齐 UE5：自底向上逐层合并/简化直到单个根 cluster（不再人为截断层级）。
    // 仅当一层无法减少任何 cluster（几何不可继续简化）时回滚终止，防止无限循环。

    // 3D Morton code（10 bit/轴，基于网格 AABB 归一化）
    static uint32_t morton3(const float* p, const float origin[3], const float scale[3]) {
        auto part1by2 = [](uint32_t v) -> uint32_t {
            v &= 0x3ffu;
            v = (v | (v << 16)) & 0x030000FFu;
            v = (v | (v << 8)) & 0x0300F00Fu;
            v = (v | (v << 4)) & 0x030C30C3u;
            v = (v | (v << 2)) & 0x09249249u;
            return v;
        };
        auto q = [&](float v, int a) -> uint32_t {
            float t = std::clamp((v - origin[a]) * scale[a], 0.0f, 1023.0f);
            return (uint32_t)t;
        };
        return (part1by2(q(p[0], 0)) | (part1by2(q(p[1], 1)) << 1) | (part1by2(q(p[2], 2)) << 2));
    }

    // 计算 cluster 的 AABB / LOD 包围球 / 法锥
    static void compute_cluster_bounds(NaniteClusterBuild& cb) {
        for (int k = 0; k < 3; ++k) {
            cb.bounds_min[k] = FLT_MAX;
            cb.bounds_max[k] = -FLT_MAX;
        }
        for (const auto& v : cb.vertices) {
            for (int k = 0; k < 3; ++k) {
                cb.bounds_min[k] = std::min(cb.bounds_min[k], v.position[k]);
                cb.bounds_max[k] = std::max(cb.bounds_max[k], v.position[k]);
            }
        }
        float c[3] = { 0.0f, 0.0f, 0.0f };
        for (int k = 0; k < 3; ++k)
            c[k] = (cb.bounds_min[k] + cb.bounds_max[k]) * 0.5f;
        float r2 = 0.0f;
        for (int k = 0; k < 3; ++k) {
            float d = std::max(cb.bounds_max[k] - c[k], c[k] - cb.bounds_min[k]);
            r2 += d * d;
        }
        const float r = std::sqrt(r2);
        for (int k = 0; k < 3; ++k)
            cb.lod_bounds_center[k] = c[k];
        cb.lod_bounds_radius = r + cb.lod_error;

        if (!cb.indices.empty()) {
            meshopt_Bounds mb = meshopt_computeClusterBounds(
                cb.indices.data(), cb.indices.size(),
                &cb.vertices[0].position[0], cb.vertices.size(), sizeof(bud::asset::Vertex));
            cb.cone_axis[0] = mb.cone_axis_s8[0] / 127.0f;
            cb.cone_axis[1] = mb.cone_axis_s8[1] / 127.0f;
            cb.cone_axis[2] = mb.cone_axis_s8[2] / 127.0f;
            cb.cone_cutoff = mb.cone_cutoff_s8 / 127.0f;
        }
    }

    // LOD0：原始网格 meshlet 化
    static void generate_level0_clusters(const std::vector<bud::asset::Vertex>& vertices,
                                         const std::vector<uint32_t>& indices,
                                         uint32_t material_index,
                                         std::vector<NaniteClusterBuild>& out) {
        const uint32_t max_vertices = bud::asset::NANITE_MAX_CLUSTER_VERTICES;
        const uint32_t max_triangles = bud::asset::NANITE_MAX_CLUSTER_TRIANGLES;
        // meshopt_buildMeshlets（1.0.1）按空间范围生长 meshlet，对密集网格会生成
        // “散装云团”（每三角形 ~3 个独立顶点，~42 三角形就触顶 128 顶点上限），
        // 导致后续合并/简化输入退化。改用基于连接性的 buildMeshletsScan：
        // 先做顶点缓存优化，再按索引流连接性切出紧凑 meshlet（填满 128 三角形）。
        std::vector<unsigned int> cached_indices(indices.size());
        meshopt_optimizeVertexCache(cached_indices.data(), indices.data(), indices.size(), vertices.size());
        size_t max_meshlets = meshopt_buildMeshletsBound(cached_indices.size(), max_vertices, max_triangles);
        std::vector<meshopt_Meshlet> meshlets(max_meshlets);
        std::vector<unsigned int> mv(max_meshlets * max_vertices);
        std::vector<unsigned char> mt(max_meshlets * max_triangles * 3);
        size_t count = meshopt_buildMeshletsScan(meshlets.data(), mv.data(), mt.data(),
                                                 cached_indices.data(), cached_indices.size(),
                                                 vertices.size(),
                                                 max_vertices, max_triangles);
        for (size_t i = 0; i < count; ++i) {
            auto& m = meshlets[i];
            meshopt_optimizeMeshlet(&mv[m.vertex_offset], &mt[m.triangle_offset], m.triangle_count, m.vertex_count);
            NaniteClusterBuild cb;
            cb.material_index = material_index;
            cb.level = 0;
            cb.vertices.reserve(m.vertex_count);
            cb.indices.reserve(m.triangle_count * 3);
            for (uint32_t v = 0; v < m.vertex_count; ++v)
                cb.vertices.push_back(vertices[mv[m.vertex_offset + v]]);
            for (uint32_t t = 0; t < m.triangle_count * 3; ++t)
                cb.indices.push_back(mt[m.triangle_offset + t]);
            compute_cluster_bounds(cb);
            out.push_back(std::move(cb));
        }
    }

    // 构建诊断统计（每组走的路径：直接合并 / 简化 / 拆分 / 跳过）
    struct NaniteBuildStats {
        uint64_t direct = 0;
        uint64_t simplified = 0;
        uint64_t split = 0;
        uint64_t skipped = 0;
    };

    // 合并 group 内子 cluster 几何并简化成一个（或拆分多个）父 cluster
    static std::vector<NaniteClusterBuild> simplify_group(const NaniteGroupBuild& g,
                                                          const std::vector<NaniteClusterBuild>& clusters,
                                                          uint32_t level, uint32_t material_index,
                                                          NaniteBuildStats& stats) {
        std::vector<bud::asset::Vertex> merged_v;
        std::vector<uint32_t> merged_i;
        size_t base = 0;
        for (uint32_t k = 0; k < g.cluster_count; ++k) {
            const auto& c = clusters[g.cluster_start + k];
            for (const auto& v : c.vertices)
                merged_v.push_back(v);
            for (uint32_t idx : c.indices)
                merged_i.push_back((uint32_t)(base + idx));
            base += c.vertices.size();
        }

        // 防御：子 cluster 几何为空时无法构建父 cluster，跳过该组（父 cluster 缺失
        // 时该组子 clusters 保持无父，DAG 收敛检查会将其归入根层）。
        if (merged_v.empty() || merged_i.empty()) {
            stats.skipped++;
            return {};
        }

        NaniteClusterBuild cb;
        cb.material_index = material_index;
        cb.level = level;

        // 组内子 cluster 的累计 LOD 误差（相对 LOD0）——父 cluster 的误差 = 子
        // 误差 + 本次简化新增误差。
        float max_child_lod_error = 0.0f;
        for (uint32_t k = 0; k < g.cluster_count; ++k)
            max_child_lod_error = std::max(max_child_lod_error, clusters[g.cluster_start + k].lod_error);

        // 两个子 cluster 的顶点是拼接的（边界顶点重复），先按全部属性去重，再根据
        // 去重结果决定是直接作为父 cluster 还是简化。不先去重会导致父 cluster 顶点
        // 数超过 128 上限、层级无法收敛。
        meshopt_Stream streams[4];
        // meshopt_Stream::size 单位是字节（“Each element takes size bytes”），
        // 传 float 个数会只比较属性前几个字节，导致不同顶点被误判为相同。
        streams[0].data = &merged_v[0].position[0];
        streams[0].size = sizeof(float) * 3;
        streams[0].stride = sizeof(bud::asset::Vertex);
        streams[1].data = &merged_v[0].normal[0];
        streams[1].size = sizeof(float) * 3;
        streams[1].stride = sizeof(bud::asset::Vertex);
        streams[2].data = &merged_v[0].uv[0];
        streams[2].size = sizeof(float) * 2;
        streams[2].stride = sizeof(bud::asset::Vertex);
        streams[3].data = &merged_v[0].tangent[0];
        streams[3].size = sizeof(float) * 4;
        streams[3].stride = sizeof(bud::asset::Vertex);
        std::vector<unsigned int> remap(merged_v.size(), ~0u);
        size_t dedup_count = meshopt_generateVertexRemapMulti(remap.data(), merged_i.data(), merged_i.size(),
                                                              merged_v.size(), streams, 4);
        std::vector<bud::asset::Vertex> deduped_v(dedup_count);
        for (size_t i = 0; i < merged_v.size(); ++i)
            if (remap[i] != ~0u)
                deduped_v[remap[i]] = merged_v[i];
        std::vector<uint32_t> deduped_i(merged_i.size());
        meshopt_remapIndexBuffer(deduped_i.data(), merged_i.data(), merged_i.size(), remap.data());

        // 边界顶点锁定（防跨 LOD 裂缝）：合并补丁中只被 1 个三角形引用的边是边界
        // 边，其端点是与相邻 group 共享的边界顶点。简化父 cluster 时锁定这些顶点，
        // 使父 cluster 的外轮廓与邻居（其他 LOD0 cluster / 其他父 cluster）保持完全
        // 一致的边和位置，跨 LOD 切换不会出现 T 型接缝/裂缝（对齐 UE5 Nanite）。
        std::vector<unsigned char> vertex_lock(deduped_v.size(), 0);
        {
            std::unordered_map<uint64_t, uint32_t> edge_count;
            edge_count.reserve(deduped_i.size());
            auto add_edge = [&](unsigned int x, unsigned int y) {
                const uint64_t key = (uint64_t)(x < y ? x : y) << 32 | (x < y ? y : x);
                ++edge_count[key];
            };
            for (size_t t = 0; t + 2 < deduped_i.size(); t += 3) {
                const unsigned int a = deduped_i[t], b = deduped_i[t + 1], c = deduped_i[t + 2];
                add_edge(a, b);
                add_edge(b, c);
                add_edge(c, a);
            }
            for (const auto& [key, cnt] : edge_count) {
                if (cnt == 1) {
                    const unsigned int x = (unsigned int)(key >> 32);
                    const unsigned int y = (unsigned int)key;
                    if (x < deduped_v.size())
                        vertex_lock[x] = 1;
                    if (y < deduped_v.size())
                        vertex_lock[y] = 1;
                }
            }
        }

        const uint32_t max_indices = bud::asset::NANITE_MAX_CLUSTER_TRIANGLES * 3;
        if (deduped_i.size() <= max_indices && deduped_v.size() <= bud::asset::NANITE_MAX_CLUSTER_VERTICES) {
            // 去重后已满足 cluster 上限：直接作为父 cluster（不再简化，误差继承子 cluster 最大值）
            cb.vertices = std::move(deduped_v);
            cb.indices = std::move(deduped_i);
            cb.lod_error = max_child_lod_error;
            stats.direct++;
        } else {
            // 需要简化。目标 = 减半（上限 128 三角形，下限 1 三角形），保证父 cluster
            // 同时满足三角形与顶点上限。若结果仍超限会在重试中收紧目标。
            std::vector<unsigned int> simplified(deduped_i.size());
            std::vector<float> attrs(deduped_v.size() * 5);
            for (size_t i = 0; i < deduped_v.size(); ++i) {
                attrs[i * 5 + 0] = deduped_v[i].uv[0];
                attrs[i * 5 + 1] = deduped_v[i].uv[1];
                attrs[i * 5 + 2] = deduped_v[i].normal[0];
                attrs[i * 5 + 3] = deduped_v[i].normal[1];
                attrs[i * 5 + 4] = deduped_v[i].normal[2];
            }
            const float weights[5] = { 1e-2f, 1e-2f, 1e-3f, 1e-3f, 1e-3f };
            float group_radius = 0.0f;
            for (uint32_t k = 0; k < g.cluster_count; ++k)
                group_radius = std::max(group_radius, clusters[g.cluster_start + k].lod_bounds_radius);
            // meshopt target_error 是相对输入网格尺寸的比例（不乘 radius）。
            // 误差预算随父级层级增长（对齐 UE5：越粗的 LOD 允许越多简化误差），
            // 否则高层级组（合并 2×128 三角形）会卡在 0.02 的误差墙，无法减到
            // 能让“边界顶点+内部顶点”落进 128 顶点上限的目标。
            const float level_scale = 1.0f + 0.5f * (float)(level - 1);
            const float base_error = NANITE_SIMPLIFY_RELATIVE_ERROR * level_scale;

            // 简化后按全部属性重新去重（位置去重会破坏 UV/法线，需按属性流去重）。
            // 注意 streams 指向 merged_v（去重前的拼接缓冲区），而简化后的索引引用
            // 的是 deduped_v（去重后的顶点集），因此这里必须对 deduped_v 重新建流。
            meshopt_Stream streams2[4];
            // 同上：size 单位是字节
            streams2[0].data = &deduped_v[0].position[0];
            streams2[0].size = sizeof(float) * 3;
            streams2[0].stride = sizeof(bud::asset::Vertex);
            streams2[1].data = &deduped_v[0].normal[0];
            streams2[1].size = sizeof(float) * 3;
            streams2[1].stride = sizeof(bud::asset::Vertex);
            streams2[2].data = &deduped_v[0].uv[0];
            streams2[2].size = sizeof(float) * 2;
            streams2[2].stride = sizeof(bud::asset::Vertex);
            streams2[3].data = &deduped_v[0].tangent[0];
            streams2[3].size = sizeof(float) * 4;
            streams2[3].stride = sizeof(bud::asset::Vertex);

            // 简化 + 去重，带重试：边界锁定会保留大量边界顶点，若结果仍超
            // 128 顶点/128 三角形上限，收紧目标重试（而不是直接拆分），保证父
            // cluster 是单个合法 cluster，层级每层真正减半、能一直构建到单根。
            // 高层级组的合并片大（2×128 三角形），需多次收紧目标（×2/3，最多
            // 8 次）才能让“边界顶点 + 内部顶点”落进 128 顶点上限。
            size_t target = std::max<size_t>(3, std::min<size_t>(max_indices, deduped_i.size() / 2));
            float result_error = 0.0f;
            size_t simplified_count = 0;
            for (unsigned int attempt = 0; attempt < 8; ++attempt) {
                // 误差预算随目标减幅放大：目标越紧（减幅越大）允许越多误差。
                // 目标 = 减半时用 base_error，目标再收紧按比例放宽。
                const float attempt_error = base_error * (float)deduped_i.size()
                    / (2.0f * (float)std::max<size_t>(3, target));
                // 边界顶点锁定（防跨 LOD 裂缝）：全层级按 Group 外部边界锁定（对齐 UE5 Nanite）。
                // 若前 4 次尝试在严格锁下无法收缩至 128 顶点/128 三角形，后 4 次松开锁定以保层级收敛。
                const unsigned char* lock_ptr = (attempt < 4) ? vertex_lock.data() : nullptr;
                simplified_count = meshopt_simplifyWithAttributes(
                    simplified.data(), deduped_i.data(), deduped_i.size(),
                    &deduped_v[0].position[0], deduped_v.size(), sizeof(bud::asset::Vertex),
                    attrs.data(), sizeof(float) * 5, weights, 5,
                    lock_ptr,
                    target, attempt_error, 0,
                    &result_error);

                std::vector<unsigned int> remap2(deduped_v.size(), ~0u);
                const size_t new_count = meshopt_generateVertexRemapMulti(
                    remap2.data(), simplified.data(), simplified_count, deduped_v.size(), streams2, 4);
                cb.vertices.resize(new_count);
                for (size_t i = 0; i < deduped_v.size(); ++i)
                    if (remap2[i] != ~0u)
                        cb.vertices[remap2[i]] = deduped_v[i];
                cb.indices.resize(simplified_count);
                meshopt_remapIndexBuffer(cb.indices.data(), simplified.data(), simplified_count, remap2.data());

                if (cb.indices.size() >= 3 && cb.vertices.size() >= 3 &&
                    cb.indices.size() <= max_indices && cb.vertices.size() <= bud::asset::NANITE_MAX_CLUSTER_VERTICES)
                    break;
                target = std::max<size_t>(3, target * 2 / 3);
            }
            // 累计误差 = 子误差 + 本次简化误差（相对 LOD0）。
            // meshopt 的 result_error 是相对输入网格尺寸的（相对值），需要乘上本组
            // 包围盒直径（约 2*group_radius）换算回对象空间绝对误差。
            cb.lod_error = max_child_lod_error + result_error * (2.0f * group_radius);
            if (cb.indices.size() < 3 || cb.vertices.size() < 3) {
                // 简化过度（退化/零面积区域被 meshopt 全折叠）：退回未简化但已去重的
                // 原几何，由末尾的 meshlet 拆分兜底，避免产生 0 三角形的空 cluster。
                cb.vertices = std::move(deduped_v);
                cb.indices = std::move(deduped_i);
                cb.lod_error = max_child_lod_error;
            }
            stats.simplified++;
        }

        std::vector<NaniteClusterBuild> result;
        if (cb.indices.size() >= 3 &&
            cb.indices.size() <= bud::asset::NANITE_MAX_CLUSTER_TRIANGLES * 3 &&
            cb.vertices.size() <= bud::asset::NANITE_MAX_CLUSTER_VERTICES) {
            // 仅在 cluster 合法（≤128 三角形/128 顶点）时才计算 bounds——
            // meshopt_computeClusterBounds 要求 index_count/3 <= 255（固定数组），
            // 超限的中间结果必须先拆分再计算。
            compute_cluster_bounds(cb);
            result.push_back(std::move(cb));
        } else if (cb.indices.size() >= 3) {
            stats.split++;
            // 仍超上限（三角形或顶点 >128）：按 meshlet 拆分，各块同属一个 group
            const uint32_t max_vertices = bud::asset::NANITE_MAX_CLUSTER_VERTICES;
            const uint32_t max_triangles = bud::asset::NANITE_MAX_CLUSTER_TRIANGLES;
            size_t max_meshlets = meshopt_buildMeshletsBound(cb.indices.size(), max_vertices, max_triangles);
            std::vector<meshopt_Meshlet> meshlets(max_meshlets);
            std::vector<unsigned int> mv(max_meshlets * max_vertices);
            std::vector<unsigned char> mt(max_meshlets * max_triangles * 3);
            size_t count = meshopt_buildMeshlets(meshlets.data(), mv.data(), mt.data(),
                                                 cb.indices.data(), cb.indices.size(),
                                                 &cb.vertices[0].position[0], cb.vertices.size(),
                                                 sizeof(bud::asset::Vertex),
                                                 max_vertices, max_triangles, 0.5f);
            for (size_t i = 0; i < count; ++i) {
                auto& m = meshlets[i];
                meshopt_optimizeMeshlet(&mv[m.vertex_offset], &mt[m.triangle_offset], m.triangle_count, m.vertex_count);
                NaniteClusterBuild sub;
                sub.material_index = material_index;
                sub.level = level;
                sub.lod_error = cb.lod_error;
                sub.vertices.reserve(m.vertex_count);
                sub.indices.reserve(m.triangle_count * 3);
                for (uint32_t v = 0; v < m.vertex_count; ++v)
                    sub.vertices.push_back(cb.vertices[mv[m.vertex_offset + v]]);
                for (uint32_t t = 0; t < m.triangle_count * 3; ++t)
                    sub.indices.push_back(mt[m.triangle_offset + t]);
                compute_cluster_bounds(sub);
                result.push_back(std::move(sub));
            }
        }
        return result;
    }

    // 构建一个 mesh 的 Nanite 层次 DAG（LOD0..根）
    static void build_mesh_nanite_dag(
        const std::vector<bud::asset::Vertex>& vertices,
        const std::vector<uint32_t>& indices,
        uint32_t material_index,
        std::vector<NaniteClusterBuild>& out_clusters,
        std::vector<NaniteGroupBuild>& out_groups,
        std::vector<bud::asset::NaniteHierarchyLevel>& out_levels) {

        // 输入 AABB（Morton 空间归一化范围）
        float origin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
        float ext[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (const auto& v : vertices) {
            for (int k = 0; k < 3; ++k) {
                origin[k] = std::min(origin[k], v.position[k]);
                ext[k] = std::max(ext[k], v.position[k]);
            }
        }
        float scale[3];
        for (int k = 0; k < 3; ++k)
            scale[k] = (ext[k] > origin[k]) ? (1023.0f / (ext[k] - origin[k])) : 0.0f;

        // Level 0
        const uint32_t l0_cluster_start = (uint32_t)out_clusters.size();
        generate_level0_clusters(vertices, indices, material_index, out_clusters);

        uint32_t level = 0;
        uint32_t level_cluster_start = l0_cluster_start;
        while (true) {
            const uint32_t ccount = (uint32_t)out_clusters.size() - level_cluster_start;

            // Level 0 按空间排序；上层按父 group 顺序生成（空间近似有序），
            // 直接分组即可保证 children 范围连续。
            if (level == 0 && ccount > 1) {
                std::sort(out_clusters.begin() + level_cluster_start, out_clusters.end(),
                    [&](const NaniteClusterBuild& a, const NaniteClusterBuild& b) {
                        return morton3(a.lod_bounds_center, origin, scale) <
                               morton3(b.lod_bounds_center, origin, scale);
                    });
            }

            // 分组（每 NANITE_GROUP_SIZE 个相邻同材质 cluster 一组；材质不同则拆开）
            const uint32_t group_start = (uint32_t)out_groups.size();
            for (uint32_t i = 0; i < ccount;) {
                NaniteGroupBuild g;
                g.level = level;
                g.cluster_start = level_cluster_start + i;
                const uint32_t mat = out_clusters[g.cluster_start].material_index;
                g.cluster_count = 1;
                while (g.cluster_count < NANITE_GROUP_SIZE &&
                       i + g.cluster_count < ccount &&
                       out_clusters[level_cluster_start + i + g.cluster_count].material_index == mat)
                    g.cluster_count++;
                out_groups.push_back(g);
                i += g.cluster_count;
            }
            const uint32_t group_count = (uint32_t)out_groups.size() - group_start;

            // cluster -> group 链接
            for (uint32_t g = 0; g < group_count; ++g) {
                auto& grp = out_groups[group_start + g];
                for (uint32_t k = 0; k < grp.cluster_count; ++k)
                    out_clusters[grp.cluster_start + k].group_index = group_start + g;
            }

            // group bounds + children（level>0 时指向更细层 groups，连续）
            for (uint32_t g = 0; g < group_count; ++g) {
                auto& grp = out_groups[group_start + g];
                uint32_t first_sg = bud::asset::INVALID_INDEX;
                uint32_t last_sg = bud::asset::INVALID_INDEX;
                for (uint32_t k = 0; k < grp.cluster_count; ++k) {
                    const auto& c = out_clusters[grp.cluster_start + k];
                    if (c.source_group != bud::asset::INVALID_INDEX) {
                        if (first_sg == bud::asset::INVALID_INDEX)
                            first_sg = c.source_group;
                        last_sg = c.source_group;
                    }
                    for (int kk = 0; kk < 3; ++kk)
                        grp.lod_bounds_center[kk] += c.lod_bounds_center[kk];
                    grp.lod_bounds_radius = std::max(grp.lod_bounds_radius, c.lod_bounds_radius);
                    grp.lod_error = std::max(grp.lod_error, c.lod_error);
                }
                for (int kk = 0; kk < 3; ++kk)
                    grp.lod_bounds_center[kk] /= (float)grp.cluster_count;
                if (level > 0 && first_sg != bud::asset::INVALID_INDEX) {
                    // 子 groups 为成员 cluster 的来源 groups（空间相邻，索引连续）
                    grp.children_start = first_sg;
                    grp.children_count = (last_sg != bud::asset::INVALID_INDEX)
                                             ? (last_sg - first_sg + 1)
                                             : 1u;
                }
            }

            // 记录本层范围
            out_levels.push_back({ level_cluster_start, ccount, group_start, group_count });

            // 检查是否还能合并：本层需存在至少一个 >1-cluster group。
            // 多材质网格中相邻 meshlet 材质不同时分组退化为单 cluster 组，
            // 此时若不终止会无限循环。根层因此允许多个 cluster（对齐 UE5：
            // 每个材质/子树各有一个根 cluster）。
            bool can_merge = false;
            for (uint32_t g = 0; g < group_count; ++g)
                if (out_groups[group_start + g].cluster_count > 1)
                    can_merge = true;
            if (ccount == 1 || !can_merge)
                break;

            // 生成下一层：每个 group 合并简化成父 cluster
            NaniteBuildStats stats = {};
            const uint32_t next_cluster_start = (uint32_t)out_clusters.size();
            for (uint32_t gi = group_start; gi < group_start + group_count; ++gi) {
                const auto& grp = out_groups[gi];
                const uint32_t mat = out_clusters[grp.cluster_start].material_index;
                auto parents = simplify_group(grp, out_clusters, level + 1, mat, stats);
                for (auto& p : parents) {
                    p.source_group = gi;
                    out_clusters.push_back(std::move(p));
                }
                // 子 clusters 的 parent_lod_error = 父 cluster 的 lod_error（累计，相对根）
                if (!parents.empty()) {
                    for (uint32_t k = 0; k < grp.cluster_count; ++k)
                        out_clusters[grp.cluster_start + k].parent_lod_error = parents[0].lod_error;
                }
            }
            std::cout << "    level " << level << "->" << level + 1 << " groups=" << group_count
                      << " direct=" << stats.direct << " simplified=" << stats.simplified
                      << " split=" << stats.split << " skipped=" << stats.skipped << std::endl;

            // 收敛检查（对齐 UE5 构建到单根）：仅当本层完全没有减少 cluster 数时
            // 回滚终止（几何不可继续简化），否则继续向根构建。允许拆分导致的减幅
            // 低于 50%，层数近似 O(log N)。
            const uint32_t next_count = (uint32_t)out_clusters.size() - next_cluster_start;
            if (next_count >= ccount) {
                out_clusters.resize(next_cluster_start);
                break;
            }

            level++;
            level_cluster_start = next_cluster_start;
        }

        // 根层（本 mesh 最高层）无更粗父簇：parent_lod_error = 自身 lod_error
        //（语义：无可用回退，页常驻）。多 mesh 场景每个 mesh 都有自己的根层，
        // 必须在构建器内逐个设置，序列化侧无法从全局 levels 区分 mesh 边界。
        if (!out_levels.empty()) {
            const auto& root = out_levels.back();
            for (uint32_t k = 0; k < root.cluster_count; ++k) {
                auto& c = out_clusters[root.cluster_start + k];
                c.parent_lod_error = c.lod_error;
            }
        }
    }

    // ====================================================================
    // Per-resource page assignment (UE5-aligned)
    // 把 DAG 序的 clusters 贪心打包进 128KB 页：
    //   - 页内顶点为页局部序号（u16 索引引用），位置/属性流按 cluster 连续
    //   - 每页记录 dependency_page_id = 该页 clusters 的父（更粗）cluster
    //     所在页中 level 最小者；根 cluster 所在页常驻
    // ====================================================================

    struct NanitePageAssign {
        std::vector<uint32_t> clusters;      // 页内 cluster 全局索引（DAG 序）
        uint32_t dependency_page_id = bud::asset::INVALID_INDEX;
        uint32_t vertex_count = 0;           // 页内顶点数
        uint32_t index_count = 0;            // 页内三角形数
        uint32_t size_in_bytes = 0;          // 页数据总字节（含页头）
        bool is_root = false;                // 含根 cluster，常驻
    };

    // 原生 GPU 页容量（对齐 UE5 Nanite 128KB 页位流）
    constexpr uint32_t NANITE_PAGE_RAW_CAPACITY = bud::asset::NANITE_PAGE_SIZE; // 128 KB

    static void assign_nanite_pages(
        const std::vector<NaniteClusterBuild>& clusters,
        const std::vector<NaniteGroupBuild>& groups,
        std::vector<NanitePageAssign>& out_pages,
        std::vector<uint32_t>& out_cluster_page,
        std::vector<uint32_t>& out_cluster_page_vertex_offset,
        std::vector<uint32_t>& out_cluster_page_index_offset) {

        const uint32_t header_size = sizeof(bud::asset::NanitePageDataHeader);
        const uint32_t capacity = NANITE_PAGE_RAW_CAPACITY;

        out_pages.clear();
        out_cluster_page.assign(clusters.size(), bud::asset::INVALID_INDEX);
        out_cluster_page_vertex_offset.assign(clusters.size(), 0);
        out_cluster_page_index_offset.assign(clusters.size(), 0);

        uint32_t page_id = bud::asset::INVALID_INDEX;
        uint32_t page_vertex_count = 0;
        uint32_t page_tri_count = 0;
        std::vector<uint32_t> page_clusters;

        auto calc_raw_bytes = [&](uint32_t verts, uint32_t tris) -> uint32_t {
            const uint32_t bits = bud::asset::NANITE_POSITION_BITS;
            const uint32_t pos_bytes = (uint32_t)(((uint64_t)verts * bits * 3 + 7) / 8);
            const uint32_t pos_bytes_aligned = (pos_bytes + 3u) & ~3u;
            const uint32_t attr_bytes = verts * (uint32_t)sizeof(bud::asset::NanitePackedVertex);
            const uint32_t idx_bytes = tris * 3u * (uint32_t)sizeof(uint16_t);
            return header_size + pos_bytes_aligned + attr_bytes + idx_bytes;
        };

        auto close_page = [&]() {
            if (page_id == bud::asset::INVALID_INDEX)
                return;
            out_pages[page_id].clusters = std::move(page_clusters);
            out_pages[page_id].vertex_count = page_vertex_count;
            out_pages[page_id].index_count = page_tri_count;
            out_pages[page_id].size_in_bytes = calc_raw_bytes(page_vertex_count, page_tri_count);
        };

        for (uint32_t ci = 0; ci < (uint32_t)clusters.size(); ++ci) {
            const uint32_t cv = (uint32_t)clusters[ci].vertices.size();
            const uint32_t ct = (uint32_t)clusters[ci].indices.size() / 3;

            const uint32_t cand_verts = page_vertex_count + cv;
            const uint32_t cand_tris = page_tri_count + ct;
            const uint32_t cand_bytes = calc_raw_bytes(cand_verts, cand_tris);

            if (page_id == bud::asset::INVALID_INDEX || cand_bytes > capacity) {
                close_page();
                out_pages.push_back({});
                page_id = (uint32_t)out_pages.size() - 1;
                page_vertex_count = 0;
                page_tri_count = 0;
                page_clusters.clear();
            }
            out_cluster_page[ci] = page_id;
            out_cluster_page_vertex_offset[ci] = page_vertex_count;
            out_cluster_page_index_offset[ci] = page_tri_count;
            page_clusters.push_back(ci);
            page_vertex_count += cv;
            page_tri_count += ct;
        }
        close_page();

        // cluster -> 父 cluster（更粗）映射：level>0 的 cluster 的 source_group
        // 即其子 clusters 所在的更细 group；该 group 的 clusters 的父 = 它。
        std::vector<uint32_t> cluster_parent(clusters.size(), bud::asset::INVALID_INDEX);
        for (uint32_t ci = 0; ci < (uint32_t)clusters.size(); ++ci) {
            const uint32_t sg = clusters[ci].source_group;
            if (sg == bud::asset::INVALID_INDEX || sg >= groups.size())
                continue;
            for (uint32_t k = 0; k < groups[sg].cluster_count; ++k) {
                const uint32_t child = groups[sg].cluster_start + k;
                if (child < clusters.size())
                    cluster_parent[child] = ci;
            }
        }

        // 依赖解析 + 根常驻
        for (uint32_t pi = 0; pi < (uint32_t)out_pages.size(); ++pi) {
            auto& page = out_pages[pi];
            uint32_t dep = bud::asset::INVALID_INDEX;
            uint32_t dep_level = 0xFFFFFFFFu;
            bool has_root = false;
            for (uint32_t ci : page.clusters) {
                const uint32_t pc = cluster_parent[ci];
                if (pc == bud::asset::INVALID_INDEX) {
                    // 无父 cluster = 根 cluster；根 cluster 所在页必须常驻且无依赖
                    has_root = true;
                    continue;
                }
                const uint32_t pcp = out_cluster_page[pc];
                // 父 cluster 必须在其他页（本页内既有父又有子的页不可作为依赖）；
                // 取 level 最小（最粗）的父所在页作为回退依赖。
                if (pcp != bud::asset::INVALID_INDEX && pcp != pi && clusters[pc].level < dep_level) {
                    dep = pcp;
                    dep_level = clusters[pc].level;
                }
            }
            page.is_root = has_root;
            page.dependency_page_id = has_root ? bud::asset::INVALID_INDEX : dep;
        }
    }

    // ====================================================================
    // Page serialization (UE5-aligned)
    //   页数据 = 64B 页头 + 页级量化位置位流 + 打包属性流 + u16 页局部索引流
    //   位置按“页”量化（页头存 position_bits/offset/extent，对齐 UE5 页级
    //   PositionPrecision/PositionOffset），页内所有顶点统一量化、连续位流
    // ====================================================================

    // 把整页顶点位置按页 AABB 量化写入连续位流（字节输出）
    static void write_page_quantized_positions(const std::vector<bud::asset::Vertex>& page_vertices,
                                               const float offset[3], const float extent[3],
                                               uint32_t bits, std::vector<uint8_t>& out) {
        const uint32_t max_q = (1u << bits) - 1;
        const size_t total_bits = page_vertices.size() * 3ull * bits;
        out.assign((total_bits + 7) / 8, 0);
        size_t bit_pos = 0;
        for (const auto& v : page_vertices) {
            for (int k = 0; k < 3; ++k) {
                const float span = extent[k];
                const float t = (span > 0.0f) ? ((v.position[k] - offset[k]) / span) : 0.0f;
                const uint32_t q = (uint32_t)(std::clamp(t, 0.0f, 1.0f) * (float)max_q + 0.5f);
                for (uint32_t b = 0; b < bits; ++b) {
                    if (q & (1u << b)) {
                        const size_t byte = bit_pos >> 3;
                        const size_t bit = bit_pos & 7;
                        out[byte] |= (uint8_t)(1u << bit);
                    }
                    ++bit_pos;
                }
            }
        }
    }

    // 打包顶点属性（对齐 UE5 FPackedNormal / FPackedRGBA16N）
    static bud::asset::NanitePackedVertex pack_vertex_attributes(const bud::asset::Vertex& v) {
        bud::asset::NanitePackedVertex p = {};
        auto pack_snorm8 = [](float x) -> uint8_t {
            return (uint8_t)(int8_t)(std::clamp(x, -1.0f, 1.0f) * 127.0f);
        };
        auto pack_u16 = [](float x) -> uint16_t {
            return meshopt_quantizeHalf(x);
        };
        p.normal[0] = pack_snorm8(v.normal[0]);
        p.normal[1] = pack_snorm8(v.normal[1]);
        p.normal[2] = pack_snorm8(v.normal[2]);
        p.normal[3] = 0;
        p.tangent[0] = pack_snorm8(v.tangent[0]);
        p.tangent[1] = pack_snorm8(v.tangent[1]);
        p.tangent[2] = pack_snorm8(v.tangent[2]);
        p.tangent[3] = pack_snorm8(v.tangent[3]);
        p.uv[0] = pack_u16(v.uv[0]);
        p.uv[1] = pack_u16(v.uv[1]);
        p.color = 0;
        return p;
    }

    // 写 .budmesh 文件
    static bool write_nanite_file(
        const std::string& output_path,
        const std::vector<NaniteClusterBuild>& clusters,
        const std::vector<NaniteGroupBuild>& groups,
        const std::vector<bud::asset::NaniteHierarchyLevel>& levels,
        const std::vector<NanitePageAssign>& pages,
        const std::vector<uint32_t>& cluster_page,
        const std::vector<uint32_t>& cluster_page_vertex_offset,
        const std::vector<uint32_t>& cluster_page_index_offset,
        const std::vector<bud::asset::MaterialDescriptor>& materials,
        const std::vector<std::string>& texture_paths) {

        // ---- 序列化常驻表（字段布局对齐 UE5 FCluster）----
        std::vector<bud::asset::NaniteCluster> ser_clusters(clusters.size());
        for (uint32_t ci = 0; ci < (uint32_t)clusters.size(); ++ci) {
            const auto& cb = clusters[ci];
            auto& sc = ser_clusters[ci];
            sc.num_verts = (uint32_t)cb.vertices.size();
            sc.num_tris = (uint32_t)cb.indices.size() / 3;
            sc.material_index = cb.material_index;
            sc.position_offset = cluster_page_vertex_offset[ci];
            sc.position_page_offset = cluster_page[ci];
            sc.index_offset = cluster_page_index_offset[ci];
            sc.index_page_offset = cluster_page[ci];
            sc.group_index = cb.group_index;
            // LOD 误差：f32 -> u32 有序编码（对齐 UE5，保持单调序）。
            // 每个 mesh 根簇的 parent_lod_error 已在 build_mesh_nanite_dag 设为自身。
            sc.lod_error = bud::asset::nanite_encode_lod_error(cb.lod_error);
            sc.parent_lod_error = bud::asset::nanite_encode_lod_error(cb.parent_lod_error);
            for (int k = 0; k < 3; ++k) {
                sc.position_bounds_center[k] = (cb.bounds_min[k] + cb.bounds_max[k]) * 0.5f;
                sc.position_bounds_extent[k] = (cb.bounds_max[k] - cb.bounds_min[k]) * 0.5f;
                sc.lod_bounds_center[k] = cb.lod_bounds_center[k];
            }
            sc.lod_bounds_radius = cb.lod_bounds_radius;
            sc.cone_axis[0] = cb.cone_axis[0];
            sc.cone_axis[1] = cb.cone_axis[1];
            sc.cone_axis[2] = cb.cone_axis[2];
            sc.cone_cutoff = cb.cone_cutoff;
        }

        std::vector<bud::asset::NaniteClusterGroup> ser_groups(groups.size());
        for (uint32_t gi = 0; gi < (uint32_t)groups.size(); ++gi) {
            const auto& gb = groups[gi];
            auto& sg = ser_groups[gi];
            uint32_t pmin = ~0u, pmax = 0;
            for (uint32_t k = 0; k < gb.cluster_count; ++k) {
                const uint32_t p = cluster_page[gb.cluster_start + k];
                pmin = std::min(pmin, p);
                pmax = std::max(pmax, p);
            }
            sg.page_index_start = (pmin == ~0u) ? bud::asset::INVALID_INDEX : pmin;
            sg.page_index_num = (pmin == ~0u) ? 0 : (pmax - pmin + 1);
            sg.children_start = gb.children_start;
            sg.children_num = gb.children_count;
            sg.lod_bounds_center[0] = gb.lod_bounds_center[0];
            sg.lod_bounds_center[1] = gb.lod_bounds_center[1];
            sg.lod_bounds_center[2] = gb.lod_bounds_center[2];
            sg.lod_bounds_radius = gb.lod_bounds_radius;
            sg.lod_error = gb.lod_error;
        }

        std::vector<bud::asset::NanitePageStreamingState> ser_pages(pages.size());
        for (uint32_t pi = 0; pi < (uint32_t)pages.size(); ++pi) {
            // 页级布局：64B 页头 + 页级量化位置位流 + 打包属性 + u16 页局部索引
            const uint32_t bits = bud::asset::NANITE_POSITION_BITS;
            const uint32_t pos_bytes = (uint32_t)(((uint64_t)pages[pi].vertex_count * bits * 3 + 7) / 8);
            const uint32_t pos_bytes_aligned = (pos_bytes + 3u) & ~3u;
            const uint32_t attr_bytes = pages[pi].vertex_count * (uint32_t)sizeof(bud::asset::NanitePackedVertex);
            const uint32_t idx_bytes = pages[pi].index_count * 3u * (uint32_t)sizeof(uint16_t);
            auto& sp = ser_pages[pi];
            sp.raw_vertex_offset = sizeof(bud::asset::NanitePageDataHeader);
            sp.raw_vertex_count = pages[pi].vertex_count;
            sp.raw_index_offset = sizeof(bud::asset::NanitePageDataHeader) + pos_bytes_aligned + attr_bytes;
            sp.raw_index_count = pages[pi].index_count;
            sp.imposter_offset = 0;
            sp.imposter_count = 0;
            sp.flags = pages[pi].is_root ? 1u : 0u;
            // --- BudEngine 扩展 ---
            sp.dependency_page_id = pages[pi].dependency_page_id;
            sp.size_in_bytes = (uint32_t)sizeof(bud::asset::NanitePageDataHeader) + pos_bytes_aligned + attr_bytes + idx_bytes;
        }

        std::vector<bud::asset::NanitePageDependency> ser_deps;
        for (uint32_t pi = 0; pi < (uint32_t)pages.size(); ++pi) {
            const uint32_t dep = pages[pi].dependency_page_id;
            if (dep == bud::asset::INVALID_INDEX || dep >= pages.size())
                continue;
            uint32_t gs = ~0u, ge = 0;
            for (uint32_t ci : pages[dep].clusters) {
                const uint32_t g = ser_clusters[ci].group_index;
                if (g != bud::asset::INVALID_INDEX) {
                    gs = std::min(gs, g);
                    ge = std::max(ge, g);
                }
            }
            if (gs != ~0u) {
                bud::asset::NanitePageDependency d = {};
                d.page_id = dep;
                d.start_group_index = gs;
                d.num_groups = ge - gs + 1;
                ser_deps.push_back(d);
            }
        }

        // ---- 文件布局 ----
        std::ofstream out(output_path, std::ios::binary);
        if (!out.is_open()) {
            std::cerr << "[BudNanite] Failed to open output: " << output_path << std::endl;
            return false;
        }

        uint64_t off = sizeof(bud::asset::NaniteHeader);
        const uint64_t cluster_offset = off;
        off += ser_clusters.size() * sizeof(bud::asset::NaniteCluster);
        const uint64_t group_offset = off;
        off += ser_groups.size() * sizeof(bud::asset::NaniteClusterGroup);
        const uint64_t hierarchy_offset = off;
        off += levels.size() * sizeof(bud::asset::NaniteHierarchyLevel);
        const uint64_t page_state_offset = off;
        off += ser_pages.size() * sizeof(bud::asset::NanitePageStreamingState);
        const uint64_t dependency_offset = off;
        off += ser_deps.size() * sizeof(bud::asset::NanitePageDependency);
        const uint64_t material_offset = off;
        off += materials.size() * sizeof(bud::asset::MaterialDescriptor);
        const uint64_t texture_offset = off;
        for (const auto& p : texture_paths)
            off += p.length() + 1;
        const uint64_t page_data_offset = off;

        // 全局 AABB
        float aabb_min[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
        float aabb_max[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (const auto& cb : clusters) {
            for (int k = 0; k < 3; ++k) {
                aabb_min[k] = std::min(aabb_min[k], cb.bounds_min[k]);
                aabb_max[k] = std::max(aabb_max[k], cb.bounds_max[k]);
            }
        }

        bud::asset::NaniteHeader h = {};
        h.magic = bud::asset::NANITE_MAGIC;
        h.version = bud::asset::NANITE_VERSION;
        h.flags = 0;
        h.cluster_count = (uint32_t)ser_clusters.size();
        h.group_count = (uint32_t)ser_groups.size();
        h.hierarchy_level_count = (uint32_t)levels.size();
        h.page_count = (uint32_t)ser_pages.size();
        h.dependency_count = (uint32_t)ser_deps.size();
        h.material_count = (uint32_t)materials.size();
        h.texture_count = (uint32_t)texture_paths.size();
        h.cluster_offset = cluster_offset;
        h.group_offset = group_offset;
        h.hierarchy_offset = hierarchy_offset;
        h.page_state_offset = page_state_offset;
        h.dependency_offset = dependency_offset;
        h.material_offset = material_offset;
        h.texture_offset = texture_offset;
        h.page_data_offset = page_data_offset;
        for (int k = 0; k < 3; ++k) {
            h.aabb_min[k] = aabb_min[k];
            h.aabb_max[k] = aabb_max[k];
        }

        out.write(reinterpret_cast<const char*>(&h), sizeof(h));
        if (!ser_clusters.empty())
            out.write(reinterpret_cast<const char*>(ser_clusters.data()), ser_clusters.size() * sizeof(bud::asset::NaniteCluster));
        if (!ser_groups.empty())
            out.write(reinterpret_cast<const char*>(ser_groups.data()), ser_groups.size() * sizeof(bud::asset::NaniteClusterGroup));
        if (!levels.empty())
            out.write(reinterpret_cast<const char*>(levels.data()), levels.size() * sizeof(bud::asset::NaniteHierarchyLevel));
        if (!ser_pages.empty())
            out.write(reinterpret_cast<const char*>(ser_pages.data()), ser_pages.size() * sizeof(bud::asset::NanitePageStreamingState));
        if (!ser_deps.empty())
            out.write(reinterpret_cast<const char*>(ser_deps.data()), ser_deps.size() * sizeof(bud::asset::NanitePageDependency));
        if (!materials.empty())
            out.write(reinterpret_cast<const char*>(materials.data()), materials.size() * sizeof(bud::asset::MaterialDescriptor));
        for (const auto& p : texture_paths)
            out.write(p.c_str(), (std::streamsize)p.length() + 1);

        // ---- 页数据区 ----
        // 先在内存中组装每页的 页级量化位置位流 / 打包属性 / 页局部 u16 索引 三条流，
        // 再整块写入，避免逐元素小写拖慢导出。
        for (uint32_t pi = 0; pi < (uint32_t)pages.size(); ++pi) {
            // 收集页内全部顶点（按 cluster 顺序），并求页级 AABB（量化原点/范围）
            std::vector<bud::asset::Vertex> page_vertices;
            page_vertices.reserve(pages[pi].vertex_count);
            float poff[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
            float pext[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
            for (uint32_t ci : pages[pi].clusters) {
                const auto& cb = clusters[ci];
                for (const auto& v : cb.vertices) {
                    page_vertices.push_back(v);
                    for (int k = 0; k < 3; ++k) {
                        poff[k] = std::min(poff[k], v.position[k]);
                        pext[k] = std::max(pext[k], v.position[k]);
                    }
                }
            }
            for (int k = 0; k < 3; ++k) {
                pext[k] = (pext[k] > poff[k]) ? (pext[k] - poff[k]) : 1.0f;
            }

            const uint32_t bits = bud::asset::NANITE_POSITION_BITS;
            std::vector<uint8_t> pos_stream;
            write_page_quantized_positions(page_vertices, poff, pext, bits, pos_stream);

            std::vector<bud::asset::NanitePackedVertex> attr_stream;
            attr_stream.reserve(pages[pi].vertex_count);
            for (uint32_t ci : pages[pi].clusters)
                for (const auto& v : clusters[ci].vertices)
                    attr_stream.push_back(pack_vertex_attributes(v));

            std::vector<uint16_t> idx_stream;
            idx_stream.reserve(pages[pi].index_count * 3u);
            for (uint32_t ci : pages[pi].clusters) {
                const auto& cb = clusters[ci];
                const uint32_t vbase = cluster_page_vertex_offset[ci];
                for (uint32_t idx : cb.indices)
                    idx_stream.push_back((uint16_t)(vbase + idx));
            }

            const uint32_t pos_bytes = (uint32_t)pos_stream.size();
            const uint32_t pos_bytes_aligned = (pos_bytes + 3u) & ~3u;
            const uint32_t attr_bytes = (uint32_t)(attr_stream.size() * sizeof(bud::asset::NanitePackedVertex));
            const uint32_t idx_bytes = (uint32_t)(idx_stream.size() * sizeof(uint16_t));

            const uint64_t pg_start = (uint64_t)out.tellp();
            bud::asset::NanitePageDataHeader pd = {};
            pd.magic = bud::asset::NANITE_PAGE_DATA_MAGIC;
            pd.version = bud::asset::NANITE_VERSION;
            pd.cluster_count = (uint32_t)pages[pi].clusters.size();
            pd.vertex_count = pages[pi].vertex_count;
            pd.index_count = pages[pi].index_count;
            pd.vertex_stream_offset = sizeof(bud::asset::NanitePageDataHeader);
            pd.index_stream_offset = sizeof(bud::asset::NanitePageDataHeader) + pos_bytes_aligned + attr_bytes;
            pd.total_size = pages[pi].size_in_bytes;
            pd.flags = pages[pi].is_root ? 1u : 0u;
            pd.position_bits = bits;
            for (int k = 0; k < 3; ++k) {
                pd.position_offset[k] = poff[k];
                pd.position_extent[k] = pext[k];
            }
            out.write(reinterpret_cast<const char*>(&pd), sizeof(pd));
            if (!pos_stream.empty()) {
                out.write(reinterpret_cast<const char*>(pos_stream.data()), (std::streamsize)pos_bytes);
                if (pos_bytes_aligned > pos_bytes) {
                    static const uint32_t pad = 0;
                    out.write(reinterpret_cast<const char*>(&pad), pos_bytes_aligned - pos_bytes);
                }
            }
            if (!attr_stream.empty())
                out.write(reinterpret_cast<const char*>(attr_stream.data()), (std::streamsize)attr_bytes);
            if (!idx_stream.empty())
                out.write(reinterpret_cast<const char*>(idx_stream.data()), (std::streamsize)idx_bytes);

            const uint64_t pg_size = (uint64_t)out.tellp() - pg_start;
            if (pg_size != pages[pi].size_in_bytes) {
                std::cerr << "[BudNanite] Page " << pi << " size mismatch: wrote=" << pg_size
                          << " expected=" << pages[pi].size_in_bytes << std::endl;
                return false;
            }
        }
        out.close();
        return true;
    }

} // namespace

namespace bud::tool {

    bool AssetProcessor::process_gltf_to_budmesh(const std::string& input_path, const std::string& output_path,
                                                 size_t max_vertices, size_t max_triangles, float cone_weight,
                                                 size_t page_size) {
        // v5 Nanite format is the standard format for .budmesh
        return process_gltf_to_budnanite(input_path, output_path);
    }


#if 0 // Legacy v4 exporter disabled; v5 Nanite format is standard for .budmesh
            // Try to query two-sided and opacity from Assimp material (best-effort)
            int two_sided = 0;
            float opacity = 1.0f;
            if (mat->Get(AI_MATKEY_TWOSIDED, two_sided) == AI_SUCCESS) {
                md.double_sided = two_sided ? 1 : 0;
            }
            if (mat->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS) {
                if (opacity < 1.0f) {
                    // If an explicit opacity map exists, treat as MASK; otherwise BLEND
                    aiString op_tex;
                    if (mat->GetTexture(aiTextureType_OPACITY, 0, &op_tex) == AI_SUCCESS) {
						md.alpha_mode = static_cast<uint8_t>(asset::AlphaMode::Mask);
                    } else {
						md.alpha_mode = static_cast<uint8_t>(asset::AlphaMode::Blend);
                    }
                    md.alpha_cutoff = 0.5f;
                }
            }

            uint32_t mat_out_idx = (uint32_t)materials.size();
            materials.push_back(md);
            mat_to_mat_idx[i] = mat_out_idx;

            // Keep a mapping for diffuse texture for backwards compat if needed
            mat_to_tex_idx[i] = base_tex;
        }

        struct MeshInstance {
            unsigned int mesh_index;
            aiMatrix4x4 transform;
            std::string node_name;
        };

        std::vector<MeshInstance> instances;
        std::function<void(aiNode*, aiMatrix4x4, int)> collect_instances = [&](aiNode* node, aiMatrix4x4 parent_transform, int depth) {
            aiMatrix4x4 current_transform = parent_transform * node->mTransformation;
            for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
                instances.push_back({ node->mMeshes[i], current_transform, node->mName.C_Str() });
            }
            for (unsigned int i = 0; i < node->mNumChildren; ++i) {
                collect_instances(node->mChildren[i], current_transform, depth + 1);
            }
        };

        aiMatrix4x4 root_transform = aiMatrix4x4(); 
        collect_instances(scene->mRootNode, root_transform, 0);

        std::cout << "[BudAssetTool] Processing " << instances.size() << " instances..." << std::endl;
        for (size_t i = 0; i < instances.size(); ++i) {
            const auto& instance = instances[i];
            const aiMesh* mesh = scene->mMeshes[instance.mesh_index];
            unsigned int mat_idx = mesh->mMaterialIndex;
            uint32_t mapped_mat_idx = mat_to_mat_idx[mat_idx];

            std::vector<asset::Vertex> group_vertices;
            std::vector<uint32_t> group_indices;

            uint32_t base_v = 0;
            for (unsigned int v_idx = 0; v_idx < mesh->mNumVertices; ++v_idx) {
                asset::Vertex v = {};
                aiVector3D pos = instance.transform * mesh->mVertices[v_idx];
                v.position[0] = pos.x;
                v.position[1] = pos.y;
                v.position[2] = pos.z;

                if (mesh->HasNormals()) {
                    aiMatrix3x3 normal_matrix(instance.transform);
                    aiVector3D norm = normal_matrix * mesh->mNormals[v_idx];
                    norm.Normalize();
                    v.normal[0] = norm.x;
                    v.normal[1] = norm.y;
                    v.normal[2] = norm.z;
                }
                if (mesh->HasTextureCoords(0)) {
                    v.uv[0] = mesh->mTextureCoords[0][v_idx].x;
                    v.uv[1] = mesh->mTextureCoords[0][v_idx].y;
                }
                group_vertices.push_back(v);
            }

            for (unsigned int f_idx = 0; f_idx < mesh->mNumFaces; ++f_idx) {
                const aiFace& face = mesh->mFaces[f_idx];
                if (face.mNumIndices != 3) continue;
                group_indices.push_back(face.mIndices[0]);
                group_indices.push_back(face.mIndices[1]);
                group_indices.push_back(face.mIndices[2]);
            }

            if (group_indices.empty()) continue;

            // Global stats for this group relative to file
            uint32_t group_base_vertex = (uint32_t)all_vertices.size();
            uint32_t group_base_index = (uint32_t)all_indices.size();
            uint32_t group_base_meshlet = (uint32_t)all_meshlets.size();

            // Meshoptimizer processing
            std::vector<uint32_t> optimized_indices(group_indices.size());
            meshopt_optimizeVertexCache(optimized_indices.data(), group_indices.data(), group_indices.size(), group_vertices.size());

            // Append to global buffers MUST BE AFTER OPTIMIZATION AND USE OPTIMIZED_INDICES
            for (const auto& v : group_vertices) all_vertices.push_back(v);
            for (auto idx : optimized_indices) all_indices.push_back(group_base_vertex + idx);

            // --- Nanite-style multi-level LOD generation ---
            // LOD 0 = original meshlet set; LOD 1.. = meshopt_simplify then
            // rebuild meshlets. Every cluster keeps its LOD level and the
            // accumulated simplification error (object-space, relative to LOD0).
            const uint32_t LOD_COUNT = 3; // LOD0 (full), LOD1, LOD2
            const float lod_target_errors[LOD_COUNT] = { 0.0f, 1e-3f, 5e-3f };

            // Per-LOD meshlet generation: produce meshlets from a triangle list.
            // meshlet_sets[level] = (meshlets, vertices, triangles)
            std::vector<std::vector<meshopt_Meshlet>> lod_meshlets;
            std::vector<std::vector<unsigned int>> lod_meshlet_vertices;
            std::vector<std::vector<unsigned char>> lod_meshlet_triangles;

            // Simplification result per level (indices into group_vertices).
            std::vector<unsigned int> lod_indices = optimized_indices;
            float lod_error = 0.0f;

            for (uint32_t lod = 0; lod < LOD_COUNT; ++lod) {
                // For LOD>0, simplify the previous level's indices.
                if (lod > 0) {
                    const float target = lod_target_errors[lod];
                    float target_error = target;
                    size_t target_index_count = lod_indices.size() / 2; // 50% reduction per level
                    if (target_index_count < 3) target_index_count = 3;
                    std::vector<unsigned int> simplified(lod_indices.size());
                    float result_error = 0.0f;
                    size_t simplified_count = meshopt_simplify(
                        simplified.data(), lod_indices.data(), lod_indices.size(),
                        &group_vertices[0].position[0], group_vertices.size(), sizeof(asset::Vertex),
                        target_index_count, target_error, 0, &result_error);
                    simplified.resize(simplified_count);
                    lod_indices.swap(simplified);
                    lod_error = result_error;
                }

                size_t max_meshlets = meshopt_buildMeshletsBound(lod_indices.size(), max_vertices, max_triangles);
                std::vector<meshopt_Meshlet> local_meshlets(max_meshlets);
                std::vector<unsigned int> local_meshlet_vertices(max_meshlets * max_vertices);
                std::vector<unsigned char> local_meshlet_triangles(max_meshlets * max_triangles * 3);

                size_t meshlet_count = meshopt_buildMeshlets(local_meshlets.data(), local_meshlet_vertices.data(), local_meshlet_triangles.data(),
                                                             lod_indices.data(), lod_indices.size(), &group_vertices[0].position[0], group_vertices.size(), sizeof(asset::Vertex),
                                                             max_vertices, max_triangles, cone_weight);
                local_meshlets.resize(meshlet_count);
                lod_meshlets.push_back(std::move(local_meshlets));
                lod_meshlet_vertices.push_back(std::move(local_meshlet_vertices));
                lod_meshlet_triangles.push_back(std::move(local_meshlet_triangles));
            }

            // Total clusters across all LOD levels.
            uint32_t total_clusters = 0;
            for (const auto& set : lod_meshlets) total_clusters += (uint32_t)set.size();

            asset::SubMeshDescriptor sub_desc = {};
            sub_desc.index_start = group_base_index;
            sub_desc.index_count = (uint32_t)group_indices.size();
            sub_desc.meshlet_start = group_base_meshlet;
            sub_desc.meshlet_count = total_clusters;
            sub_desc.material_id = mapped_mat_idx;
            
            // Compute SubMesh AABB
            sub_desc.aabb_min[0] = sub_desc.aabb_min[1] = sub_desc.aabb_min[2] = std::numeric_limits<float>::max();
            sub_desc.aabb_max[0] = sub_desc.aabb_max[1] = sub_desc.aabb_max[2] = -std::numeric_limits<float>::max();
            for (const auto& v : group_vertices) {
                sub_desc.aabb_min[0] = std::min(sub_desc.aabb_min[0], v.position[0]);
                sub_desc.aabb_min[1] = std::min(sub_desc.aabb_min[1], v.position[1]);
                sub_desc.aabb_min[2] = std::min(sub_desc.aabb_min[2], v.position[2]);
                sub_desc.aabb_max[0] = std::max(sub_desc.aabb_max[0], v.position[0]);
                sub_desc.aabb_max[1] = std::max(sub_desc.aabb_max[1], v.position[1]);
                sub_desc.aabb_max[2] = std::max(sub_desc.aabb_max[2], v.position[2]);
            }
            submeshes.push_back(sub_desc);

            // Emit clusters for every LOD level. cluster_error is the accumulated
            // simplification error (object-space) for that level; parent_error
            // chains to the coarser level (level+1) so the GPU can transition.
            for (uint32_t lod = 0; lod < LOD_COUNT; ++lod) {
                float this_error = (lod == 0) ? 0.0f : lod_error;
                float parent_error = (lod + 1 < LOD_COUNT) ? lod_target_errors[lod + 1] : lod_target_errors[LOD_COUNT - 1];
                auto& local_meshlets = lod_meshlets[lod];
                auto& local_meshlet_vertices = lod_meshlet_vertices[lod];
                auto& local_meshlet_triangles = lod_meshlet_triangles[lod];

                for (size_t i = 0; i < local_meshlets.size(); ++i) {
                    meshopt_Meshlet& m = local_meshlets[i];
                    meshopt_optimizeMeshlet(&local_meshlet_vertices[m.vertex_offset], &local_meshlet_triangles[m.triangle_offset], m.triangle_count, m.vertex_count);

                    asset::MeshletDescriptor desc = {};
                    desc.vertex_offset = (uint32_t)all_meshlet_vertices.size();
                    desc.vertex_count = m.vertex_count;
                    desc.triangle_offset = (uint32_t)all_meshlet_triangles.size();
                    desc.triangle_count = m.triangle_count;
                    all_meshlets.push_back(desc);
                    all_cluster_lod.push_back(lod);
                    all_cluster_error.push_back(this_error);
                    all_cluster_mesh.push_back((uint32_t)i); // mesh (instance) ownership

                    for (uint32_t v_idx = 0; v_idx < m.vertex_count; ++v_idx) {
                        all_meshlet_vertices.push_back(group_base_vertex + local_meshlet_vertices[m.vertex_offset + v_idx]);
                    }
                    for (uint32_t t_idx = 0; t_idx < m.triangle_count * 3; ++t_idx) {
                        all_meshlet_triangles.push_back(local_meshlet_triangles[m.triangle_offset + t_idx]);
                    }

                    meshopt_Bounds mbounds = meshopt_computeMeshletBounds(&local_meshlet_vertices[m.vertex_offset], &local_meshlet_triangles[m.triangle_offset],
                                                                        m.triangle_count, &group_vertices[0].position[0], group_vertices.size(), sizeof(asset::Vertex));
                    asset::MeshletCullData cull = {};
                    cull.bounding_sphere[0] = mbounds.center[0];
                    cull.bounding_sphere[1] = mbounds.center[1];
                    cull.bounding_sphere[2] = mbounds.center[2];
                    cull.bounding_sphere[3] = mbounds.radius;
                    cull.cone_axis[0] = mbounds.cone_axis_s8[0];
                    cull.cone_axis[1] = mbounds.cone_axis_s8[1];
                    cull.cone_axis[2] = mbounds.cone_axis_s8[2];
                    cull.cone_cutoff = mbounds.cone_cutoff_s8;
                    all_cull_data.push_back(cull);
                }
            }
        }

        // 2. Serialize to .budmesh
        std::ofstream out(output_path, std::ios::binary);
        if (!out.is_open()) return false;

        static_assert(sizeof(asset::BudMeshHeader) == asset::MESH_HEADER_SIZE, "BudMeshHeader size mismatch!");
        static_assert(offsetof(asset::BudMeshHeader, vertex_offset) == asset::MESH_HEADER_VERTEX_OFFSET, "BudMeshHeader alignment mismatch!");
        static_assert(offsetof(asset::BudMeshHeader, submesh_count) == asset::MESH_HEADER_SUBMESH_COUNT_OFFSET, "BudMeshHeader submesh_count offset mismatch!");
        static_assert(sizeof(asset::SubMeshDescriptor) == asset::SUBMESH_DESCRIPTOR_SIZE, "SubMeshDescriptor size mismatch!");

        asset::BudMeshHeader header = {};
        header.magic = asset::MESH_MAGIC;
        header.version = asset::MESH_VERSION;
        header.total_vertices = (uint32_t)all_vertices.size();
        header.total_indices = (uint32_t)all_indices.size();
        header.meshlet_count = (uint32_t)all_meshlets.size();
        header.submesh_count = (uint32_t)submeshes.size();

        // Textures already processed at the start
        header.texture_count = (uint32_t)texture_paths.size();
        header.material_count = (uint32_t)materials.size();

        header.aabb_min[0] = header.aabb_min[1] = header.aabb_min[2] = std::numeric_limits<float>::max();
        header.aabb_max[0] = header.aabb_max[1] = header.aabb_max[2] = -std::numeric_limits<float>::max();
        for (const auto& v : all_vertices) {
            header.aabb_min[0] = std::min(header.aabb_min[0], v.position[0]);
            header.aabb_min[1] = std::min(header.aabb_min[1], v.position[1]);
            header.aabb_min[2] = std::min(header.aabb_min[2], v.position[2]);
            header.aabb_max[0] = std::max(header.aabb_max[0], v.position[0]);
            header.aabb_max[1] = std::max(header.aabb_max[1], v.position[1]);
            header.aabb_max[2] = std::max(header.aabb_max[2], v.position[2]);
        }

        size_t current_offset = sizeof(header);
        header.vertex_offset = current_offset;
        current_offset += all_vertices.size() * sizeof(asset::Vertex);
        header.index_offset = current_offset;
        current_offset += all_indices.size() * sizeof(uint32_t);
        header.meshlet_offset = current_offset;
        current_offset += all_meshlets.size() * sizeof(asset::MeshletDescriptor);
        header.vertex_index_offset = current_offset;
        current_offset += all_meshlet_vertices.size() * sizeof(uint32_t);
        header.meshlet_index_offset = current_offset;
        current_offset += all_meshlet_triangles.size() * sizeof(uint32_t);
        header.cull_data_offset = current_offset;
        current_offset += all_cull_data.size() * sizeof(asset::MeshletCullData);
        header.submesh_offset = current_offset;
        current_offset += submeshes.size() * sizeof(asset::SubMeshDescriptor);
        header.material_offset = current_offset;
        current_offset += materials.size() * sizeof(asset::MaterialDescriptor);
        header.texture_offset = current_offset;
        // Total size of all strings including null terminators
        for (const auto& path : texture_paths) {
            current_offset += path.length() + 1;
        }

        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(all_vertices.data()), all_vertices.size() * sizeof(asset::Vertex));
        out.write(reinterpret_cast<const char*>(all_indices.data()), all_indices.size() * sizeof(uint32_t));
        out.write(reinterpret_cast<const char*>(all_meshlets.data()), all_meshlets.size() * sizeof(asset::MeshletDescriptor));
        out.write(reinterpret_cast<const char*>(all_meshlet_vertices.data()), all_meshlet_vertices.size() * sizeof(uint32_t));
        out.write(reinterpret_cast<const char*>(all_meshlet_triangles.data()), all_meshlet_triangles.size() * sizeof(uint32_t));
        out.write(reinterpret_cast<const char*>(all_cull_data.data()), all_cull_data.size() * sizeof(asset::MeshletCullData));
        out.write(reinterpret_cast<const char*>(submeshes.data()), submeshes.size() * sizeof(asset::SubMeshDescriptor));
        // Write material table
        if (!materials.empty()) {
            out.write(reinterpret_cast<const char*>(materials.data()), materials.size() * sizeof(asset::MaterialDescriptor));
        }

        for (const auto& path : texture_paths) {
            out.write(path.c_str(), path.length() + 1);
        }

        if (page_size > 0) {
            out.close();

            // LOD constants for page metadata (must match the instance loop).
            constexpr uint32_t LOD_COUNT = 3;
            constexpr float lod_target_errors[LOD_COUNT] = { 0.0f, 1e-3f, 5e-3f };

            std::string json_path = output_path;
            std::string bin_path = output_path;
            if (json_path.find(".budmesh") != std::string::npos) {
                json_path = json_path.substr(0, json_path.rfind('.')) + ".budmesh.json";
                bin_path  = bin_path.substr(0, bin_path.rfind('.')) + ".budmesh.bin";
            }

            // Build page table (Nanite-style: spatial BVH subtrees become pages).
            // Reserve 64B header + up to 47B alignment + 1 vertex margin per page.
            constexpr uint64_t page_reserve = 64 + 47 + 48;
            const uint32_t meshlet_total = (uint32_t)all_meshlets.size();

            // meshlet_ordering maps the spatial-BVH traversal order back to the
            // original meshlet index. The binary page data (vertices/indices/
            // descriptors) below is written page by page in this new order, so
            // page_starts/page_counts index into the REORDERED meshlet sequence.
            std::vector<uint32_t> meshlet_ordering(meshlet_total);
            for (uint32_t i = 0; i < meshlet_total; ++i) meshlet_ordering[i] = i;

            // Page model v3 (Nanite-like hierarchy):
            //   - Every BVH node becomes a page. Leaf pages keep the full
            //     LOD0..LOD2 cluster set of their subtree (existing behavior).
            //   - Internal (coarse) pages aggregate the subtree's LOD2
            //     clusters so an unloaded leaf can be replaced by its parent's
            //     coarse geometry while streaming (no holes).
            // page_cluster_ids[page] holds the original meshlet ids of that
            // page, ordered by LOD then by the BVH layout.
            std::vector<std::vector<uint32_t>> page_cluster_ids;
            std::vector<uint32_t> page_parent;       // INVALID_INDEX for root
            std::vector<std::vector<uint32_t>> page_children;
            std::vector<uint8_t> page_is_coarse;

            auto new_page = [&](std::vector<uint32_t> cluster_ids, uint32_t parent, bool coarse) -> uint32_t {
                uint32_t pid = (uint32_t)page_cluster_ids.size();
                page_cluster_ids.push_back(std::move(cluster_ids));
                page_parent.push_back(parent);
                page_children.emplace_back();
                page_is_coarse.push_back(coarse ? 1u : 0u);
                if (parent != asset::INVALID_INDEX)
                    page_children[parent].push_back(pid);
                return pid;
            };

            // meshlet_size[i] in bytes for the given original meshlet index.
            // NOTE: pages store PageClusterDesc (28 B in v3, was 24 in v2), so
            // the page-size estimation must use the v3 descriptor stride or
            // leaf pages overflow page_size and corrupt the binary layout.
            auto meshlet_size = [&](uint32_t mi) -> uint64_t {
                return all_meshlets[mi].vertex_count * sizeof(asset::Vertex)
                     + all_meshlets[mi].triangle_count * 3 * sizeof(uint32_t)
                     + asset::PAGE_CLUSTER_DESC_STRIDE + sizeof(asset::MeshletCullData);
            };
            // Total byte size of meshlets in the ordering range [begin, end).
            auto range_size = [&](const std::vector<uint32_t>& ord, size_t begin, size_t end) -> uint64_t {
                uint64_t s = 0;
                for (size_t k = begin; k < end; ++k) s += meshlet_size(ord[k]);
                return s;
            };

            // Recursively split [begin,end) of meshlet_ordering by the meshlet
            // bounding-sphere center along the largest-extent axis (median
            // split). Leaf pages hold the subtree's full LOD set; internal
            // nodes additionally become coarse pages aggregating LOD2.
            std::function<uint32_t(size_t, size_t, uint32_t)> bvh_build =
                [&](size_t begin, size_t end, uint32_t parent) -> uint32_t {
                size_t count = end - begin;
                if (count == 0) return asset::INVALID_INDEX;
                uint64_t sz = range_size(meshlet_ordering, begin, end);
                if (count == 1 || sz + page_reserve <= page_size) {
                    // Leaf page: full LOD set of this subtree.
                    // NOTE: must use iterator range construction; (begin, end)
                    // are size_t indices, and vector(count, value) would fill
                    // 'begin' copies of 'end' -> out-of-bounds all_cluster_lod.
                    std::vector<uint32_t> ids(meshlet_ordering.begin() + begin, meshlet_ordering.begin() + end);
                    // Order by LOD (LOD0..LOD2) for contiguous per-LOD ranges.
                    std::stable_sort(ids.begin(), ids.end(), [&](uint32_t a, uint32_t b) {
                        return all_cluster_lod[a] < all_cluster_lod[b];
                    });
                    return new_page(std::move(ids), parent, false);
                }
                // Compute AABB over the meshlet bounding-sphere centers.
                float cmin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
                float cmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
                for (size_t k = begin; k < end; ++k) {
                    uint32_t mi = meshlet_ordering[k];
                    const float* c = all_cull_data[mi].bounding_sphere;
                    for (int a = 0; a < 3; ++a) {
                        cmin[a] = std::min(cmin[a], c[a]);
                        cmax[a] = std::max(cmax[a], c[a]);
                    }
                }
                float ext[3] = { cmax[0]-cmin[0], cmax[1]-cmin[1], cmax[2]-cmin[2] };
                int axis = (ext[0] >= ext[1] && ext[0] >= ext[2]) ? 0 : (ext[1] >= ext[2] ? 1 : 2);
                size_t mid = begin + count / 2;
                std::nth_element(meshlet_ordering.begin() + begin, meshlet_ordering.begin() + mid, meshlet_ordering.begin() + end,
                    [&](uint32_t a, uint32_t b) {
                        return all_cull_data[a].bounding_sphere[axis] < all_cull_data[b].bounding_sphere[axis];
                    });

                // Recurse children first.
                uint32_t left = bvh_build(begin, mid, asset::INVALID_INDEX);
                uint32_t right = bvh_build(mid, end, asset::INVALID_INDEX);

                // Internal node -> coarse page. Per-mesh DAG rule: a coarse
                // page must only group clusters of a SINGLE mesh (they are one
                // simplification chain). Aggregate the LOD2 clusters of the
                // mesh that has the most clusters in this subtree; other meshes'
                // LOD2 stays on their own leaf pages (no cross-mesh stitching).
                std::unordered_map<uint32_t, size_t> mesh_cluster_count;
                for (size_t k = begin; k < end; ++k) {
                    uint32_t mi = meshlet_ordering[k];
                    if (all_cluster_lod[mi] == 2)
                        mesh_cluster_count[all_cluster_mesh[mi]]++;
                }
                uint32_t dominant_mesh = asset::INVALID_INDEX;
                size_t dominant_count = 0;
                for (const auto& [mesh_idx, cnt] : mesh_cluster_count) {
                    if (cnt > dominant_count) { dominant_mesh = mesh_idx; dominant_count = cnt; }
                }
                std::vector<uint32_t> coarse_ids;
                if (dominant_mesh != asset::INVALID_INDEX) {
                    for (size_t k = begin; k < end; ++k) {
                        uint32_t mi = meshlet_ordering[k];
                        if (all_cluster_lod[mi] == 2 && all_cluster_mesh[mi] == dominant_mesh)
                            coarse_ids.push_back(mi);
                    }
                }
                // Coarse pages must fit page_size (v3 descriptor is 28 B, so a
                // large subtree's LOD2 aggregate can overflow). If it does, split
                // the coarse set recursively until each coarse page fits.
                // Returns the topmost coarse page created for this subtree.
                std::function<uint32_t(std::vector<uint32_t>, uint32_t, uint32_t)> build_coarse =
                    [&](std::vector<uint32_t> ids, uint32_t par, uint32_t top) -> uint32_t {
                    uint64_t s = 0;
                    for (uint32_t mi : ids) s += meshlet_size(mi);
                    if (ids.size() == 1 || s + page_reserve <= page_size) {
                        uint32_t pid = new_page(std::move(ids), par, true);
                        return (top == asset::INVALID_INDEX) ? pid : top;
                    }
                    float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
                    for (uint32_t mi : ids) {
                        const float* c = all_cull_data[mi].bounding_sphere;
                        for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], c[a]); hi[a] = std::max(hi[a], c[a]); }
                    }
                    float ex[3] = { hi[0]-lo[0], hi[1]-lo[1], hi[2]-lo[2] };
                    int ax = (ex[0] >= ex[1] && ex[0] >= ex[2]) ? 0 : (ex[1] >= ex[2] ? 1 : 2);
                    size_t m = ids.size() / 2;
                    std::nth_element(ids.begin(), ids.begin() + m, ids.end(),
                        [&](uint32_t a, uint32_t b) { return all_cull_data[a].bounding_sphere[ax] < all_cull_data[b].bounding_sphere[ax]; });
                    std::vector<uint32_t> la(ids.begin(), ids.begin() + m);
                    std::vector<uint32_t> ra(ids.begin() + m, ids.end());
                    // Both halves are coarse pages under `par`; keep the first
                    // created coarse page as the subtree's coarse representative.
                    uint32_t t1 = build_coarse(std::move(la), par, top);
                    uint32_t t2 = build_coarse(std::move(ra), par, t1);
                    return t2;
                };
                if (!coarse_ids.empty()) {
                    uint32_t cpid = build_coarse(std::move(coarse_ids), parent, asset::INVALID_INDEX);
                    // cpid is always a valid coarse page id now (coarse_ids was
                    // non-empty). Reparent the (leaf) children under it.
                    if (cpid == asset::INVALID_INDEX) cpid = asset::INVALID_INDEX; // defensive
                    if (left != asset::INVALID_INDEX && cpid != asset::INVALID_INDEX) { page_parent[left] = cpid; page_children[cpid].push_back(left); }
                    if (right != asset::INVALID_INDEX && cpid != asset::INVALID_INDEX) { page_parent[right] = cpid; page_children[cpid].push_back(right); }
                    return cpid == asset::INVALID_INDEX ? (left != asset::INVALID_INDEX ? left : right) : cpid;
                }
                // No LOD2 in this subtree: propagate the parent up.
                if (left != asset::INVALID_INDEX) page_parent[left] = parent;
                if (right != asset::INVALID_INDEX) page_parent[right] = parent;
                return parent == asset::INVALID_INDEX
                    ? (left != asset::INVALID_INDEX ? left : right)
                    : parent;
            };

            uint32_t root_page = bvh_build(0, meshlet_total, asset::INVALID_INDEX);

            // Per-meshlet base color texture index (from the owning submesh's material)
            std::vector<uint32_t> meshlet_tex_index(all_meshlets.size(), 0);
            for (const auto& sub : submeshes) {
                uint32_t tex = materials[sub.material_id].base_color_texture;
                for (uint32_t m = sub.meshlet_start; m < sub.meshlet_start + sub.meshlet_count; ++m)
                    meshlet_tex_index[m] = tex;
            }

            // JSON
            nlohmann::json j;
            j["magic"] = "BUDM"; j["version"] = asset::MESH_VERSION;
            j["data_uri"] = std::filesystem::path(bin_path).filename().string();
            j["pages"] = nlohmann::json::array();
            for (uint32_t i = 0; i < (uint32_t)page_cluster_ids.size(); ++i) {
                const auto& ids = page_cluster_ids[i];
                const size_t mc = ids.size();
                // Split the page into contiguous per-(LOD, material) submeshes.
                // ids are already LOD-ordered (LOD0..LOD2) so each LOD level is
                // a contiguous run and submesh ranges map 1:1 to the index data.
                nlohmann::json subs = nlohmann::json::array();
                uint32_t cum_idx = 0;      // running offset into the page index data
                uint32_t cum_clusters = 0; // running cluster offset within the page
                int cur_lod = -1;
                uint32_t cur_tex = 0;
                uint32_t run_count = 0;
                uint32_t run_clusters = 0;
                uint32_t run_start = 0;
                uint32_t run_cluster_start = 0;
                std::vector<uint32_t> lod_range_start(LOD_COUNT, 0);
                std::vector<uint32_t> lod_range_count(LOD_COUNT, 0);

                for (uint32_t k = 0; k < (uint32_t)mc; ++k) {
                    uint32_t m = ids[k]; // original meshlet index
                    uint32_t lod = all_cluster_lod[m];
                    uint32_t tex = meshlet_tex_index[m];
                    uint32_t tris = all_meshlets[m].triangle_count * 3;
                    if ((int)lod != cur_lod || tex != cur_tex) {
                        if (run_count > 0)
                            subs.push_back({{"index_start", run_start}, {"index_count", run_count},
                                            {"material_id", cur_tex}, {"lod_level", cur_lod},
                                            {"cluster_start", run_cluster_start}, {"cluster_count", run_clusters}});
                        cur_lod = (int)lod; cur_tex = tex;
                        run_start = cum_idx; run_cluster_start = cum_clusters;
                        run_count = 0; run_clusters = 0;
                    }
                    if (lod_range_count[lod] == 0)
                        lod_range_start[lod] = cum_idx; // first cluster of this LOD
                    run_count += tris;
                    run_clusters += 1;
                    cum_idx += tris;
                    cum_clusters += 1;
                    lod_range_count[lod] = cum_idx - lod_range_start[lod];
                }
                if (run_count > 0)
                    subs.push_back({{"index_start", run_start}, {"index_count", run_count},
                                    {"material_id", cur_tex}, {"lod_level", cur_lod},
                                    {"cluster_start", run_cluster_start}, {"cluster_count", run_clusters}});

                // Per-LOD index ranges [start, count] within the page index data.
                nlohmann::json lod_ranges = nlohmann::json::array();
                for (uint32_t lod = 0; lod < LOD_COUNT; ++lod)
                    lod_ranges.push_back({lod_range_start[lod], lod_range_count[lod]});

                // Per-page AABB (for distance-based on-demand streaming).
                float amin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
                float amax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
                for (size_t m = 0; m < mc; ++m) {
                    uint32_t mi = ids[m]; // original meshlet index
                    const auto& md = all_meshlets[mi];
                    for (uint32_t v = 0; v < md.vertex_count; ++v) {
                        const auto& vt = all_vertices[all_meshlet_vertices[md.vertex_offset + v]];
                        for (int k = 0; k < 3; ++k) {
                            amin[k] = std::min(amin[k], vt.position[k]);
                            amax[k] = std::max(amax[k], vt.position[k]);
                        }
                    }
                }

                nlohmann::json children = nlohmann::json::array();
                for (uint32_t c : page_children[i]) children.push_back(c);

                j["pages"].push_back({
                    {"page_id", i},
                    {"file_offset", (uint64_t)i * page_size},
                    {"capacity", page_size},
                    {"cluster_count", (uint32_t)mc},
                    {"parent_page_id", page_parent[i] == asset::INVALID_INDEX ? nlohmann::json(nullptr) : nlohmann::json(page_parent[i])},
                    {"is_coarse", page_is_coarse[i] != 0u},
                    {"children", children},
                    {"material_id", ids.empty() ? 0u : meshlet_tex_index[ids[0]]},
                    {"aabb_min", {amin[0], amin[1], amin[2]}},
                    {"aabb_max", {amax[0], amax[1], amax[2]}},
                    {"lod_ranges", lod_ranges},
                    {"submeshes", subs}
                });
            }
            j["textures"] = nlohmann::json(texture_paths);
            std::ofstream jf(json_path); if (jf.is_open()) jf << j.dump(4);

            // Binary
            std::ofstream bf(bin_path, std::ios::binary);
            if (bf.is_open()) {
                for (uint32_t pi = 0; pi < (uint32_t)page_cluster_ids.size(); ++pi) {
                    const auto& ids = page_cluster_ids[pi];
                    const uint32_t mc = (uint32_t)ids.size();
                    uint64_t pg_start = bf.tellp();

                    float amin[3]={FLT_MAX,FLT_MAX,FLT_MAX}, amax[3]={-FLT_MAX,-FLT_MAX,-FLT_MAX};
                    uint32_t vcnt = 0;
                    for (uint32_t m = 0; m < mc; ++m) {
                        uint32_t mi = ids[m]; // original meshlet index
                        const auto& md = all_meshlets[mi];
                        vcnt += md.vertex_count;
                        for (uint32_t v=0; v<md.vertex_count; ++v) {
                            const auto& vt = all_vertices[all_meshlet_vertices[md.vertex_offset+v]];
                            for (int k=0;k<3;++k){amin[k]=std::min(amin[k],vt.position[k]);amax[k]=std::max(amax[k],vt.position[k]);}
                        }
                    }
                    uint32_t ce = asset::PAGE_HEADER_SIZE + mc*asset::PAGE_CLUSTER_DESC_STRIDE + mc*asset::PAGE_CULL_DATA_STRIDE;
                    uint32_t vo = ce, io = vo + vcnt*asset::PAGE_VERTEX_STRIDE;

                    // Build page-local vertex remap: global vertex id -> page-local vertex id
                    std::unordered_map<uint32_t,uint32_t> page_remap;
                    std::vector<uint32_t> page_vertex_ids; // page-local vertex id -> global vertex id
                    std::vector<uint32_t> meshlet_local_vert_off; // per meshlet: page-local vertex offset
                    std::vector<uint32_t> meshlet_local_tri_off;  // per meshlet: page-local triangle offset

                    uint32_t tri_total = 0;
                    for (uint32_t m=0;m<mc;++m) {
                        uint32_t mi = ids[m];
                        const auto& md = all_meshlets[mi];
                        meshlet_local_vert_off.push_back((uint32_t)page_vertex_ids.size());
                        for(uint32_t v=0;v<md.vertex_count;++v) {
                            uint32_t gvid = all_meshlet_vertices[md.vertex_offset+v];
                            if (page_remap.find(gvid) == page_remap.end()) {
                                page_remap[gvid] = (uint32_t)page_vertex_ids.size();
                                page_vertex_ids.push_back(gvid);
                            }
                        }
                        meshlet_local_tri_off.push_back(tri_total);
                        tri_total += md.triangle_count*3;
                    }

                    asset::PageBinaryHeader h={};
                    h.magic=asset::PageBinaryHeader::MAGIC; h.version=asset::PageBinaryHeader::VERSION;
                    h.cluster_count=mc; h.parent_page_id = page_parent[pi] == asset::INVALID_INDEX ? asset::INVALID_INDEX : page_parent[pi];
                    h.vertex_count=(uint32_t)page_vertex_ids.size(); h.index_count=tri_total;
                    std::memcpy(h.aabb_min,amin,sizeof(amin)); std::memcpy(h.aabb_max,amax,sizeof(amax));
                    // Align vertex data offset to 48-byte stride so draw vertexOffset stays integer-exact
                    uint32_t vo2 = (ce + asset::PAGE_VERTEX_STRIDE - 1) / asset::PAGE_VERTEX_STRIDE * asset::PAGE_VERTEX_STRIDE;
                    uint32_t io2 = vo2 + (uint32_t)page_vertex_ids.size()*asset::PAGE_VERTEX_STRIDE;
                    h.vertex_data_offset=vo2; h.index_data_offset=io2;
                    h.max_error = lod_target_errors[LOD_COUNT-1]; // coarsest level error (for threshold scale)
                    h.padding[0] = (page_is_coarse[pi] ? 1u : 0u) | (ids.empty() ? 0u : (meshlet_tex_index[ids[0]] << 1));
                    // NOTE: padding has exactly one element (byte 60..63, struct is 64 bytes);
                    // writing padding[1] would overflow past the end of the struct.
                    bf.write((const char*)&h,sizeof(h));

                    for (uint32_t m=0;m<mc;++m) {
                        uint32_t mi = ids[m];
                        const auto& md = all_meshlets[mi];
                        asset::PageClusterDesc pd = {};
                        pd.vertex_offset = meshlet_local_vert_off[m];
                        pd.vertex_count = md.vertex_count;
                        pd.triangle_offset = meshlet_local_tri_off[m];
                        pd.triangle_count = md.triangle_count;
                        pd.lod_level = all_cluster_lod[mi];
                        pd.cluster_error = all_cluster_error[mi];
                        uint32_t lod = all_cluster_lod[mi];
                        pd.parent_error = (lod + 1 < LOD_COUNT) ? lod_target_errors[lod + 1] : lod_target_errors[LOD_COUNT - 1];
                        bf.write((const char*)&pd,sizeof(pd));
                    }
                    for (uint32_t m=0;m<mc;++m) bf.write((const char*)&all_cull_data[ids[m]],sizeof(asset::MeshletCullData));
                    // Pad to aligned vertex_data_offset
                    {
                        uint64_t cur = (uint64_t)bf.tellp() - pg_start;
                        if (cur < vo2) { std::vector<char> pad((size_t)(vo2-cur),0); bf.write(pad.data(),pad.size()); }
                    }
                    for (uint32_t gvid : page_vertex_ids) bf.write((const char*)&all_vertices[gvid],sizeof(asset::Vertex));
                    for (uint32_t m=0;m<mc;++m){
                        const auto& md = all_meshlets[ids[m]];
                        for(uint32_t t=0;t<md.triangle_count*3;++t) {
                            uint32_t local_vi = all_meshlet_triangles[md.triangle_offset+t];
                            uint32_t gvid = all_meshlet_vertices[md.vertex_offset+local_vi];
                            uint32_t pvid = page_remap[gvid];
                            bf.write((const char*)&pvid,sizeof(uint32_t));
                        }
                    }
                    uint64_t wr = (uint64_t)bf.tellp()-pg_start;
                    if (wr < page_size) { std::vector<char> pad((size_t)(page_size-wr),0); bf.write(pad.data(),pad.size()); }
                }
            }
            std::cout << "[BudAssetTool] Exported " << all_meshlets.size() << " meshlets in " << page_cluster_ids.size() << " pages to " << json_path << std::endl;
            return true;
        }

        std::cout << "[BudAssetTool] Successfully exported " << header.submesh_count << " submeshes, " << header.meshlet_count << " meshlets, " << header.material_count << " materials and " << header.texture_count << " textures to " << output_path << std::endl;
        return true;
    }
#endif // Legacy v4 exporter disabled

} // namespace bud::tool

#include <string>

#if !defined(BUD_HAVE_SPIRV_REFLECT)
bool bud::tool::AssetProcessor::validate_shaders_in_directory(const std::string& shader_dir, const std::string& report_path, unsigned int /*max_workers*/) {
    (void)shader_dir; (void)report_path;
    std::cerr << "[BudAssetTool] SPIRV-Reflect not available in this build. Install spirv-reflect via vcpkg to enable validation." << std::endl;
    return false;
}
#else
bool bud::tool::AssetProcessor::validate_shaders_in_directory(const std::string& shader_dir, const std::string& report_path, unsigned int max_workers) {
    namespace fs = std::filesystem;
    fs::path dir(shader_dir);
    // Ensure tmp dir exists under repo for temporary compiler outputs
    fs::path tmp_dir = std::filesystem::current_path() / "tmp";
    std::error_code tmp_ec;
    fs::create_directories(tmp_dir, tmp_ec);
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        std::cerr << "[BudAssetTool] Shader directory does not exist: " << shader_dir << std::endl;
        return false;
    }

    std::vector<std::string> exts = { ".vert", ".frag", ".comp", ".geom", ".tesc", ".tese" };
    nlohmann::json report_json;
    report_json["shaders"] = nlohmann::json::array();

    // Collect shader files
    std::vector<fs::path> shader_files;
    for (auto& p : fs::recursive_directory_iterator(dir)) {
        if (!p.is_regular_file()) continue;
        fs::path path = p.path();
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (std::find(exts.begin(), exts.end(), ext) == exts.end()) continue;
        shader_files.push_back(path);
    }

    std::atomic<bool> all_ok{true};
    // Determine worker count: prefer explicit parameter, then env var, then hardware concurrency
    unsigned int workers = max_workers;
    if (workers == 0) {
        // check env var BUD_SHADER_WORKERS or BUD_ASSET_TOOL_WORKERS
        const char* env = std::getenv("BUD_SHADER_WORKERS");
        if (!env) env = std::getenv("BUD_ASSET_TOOL_WORKERS");
        if (env) {
            try { workers = std::stoul(env); }
            catch (...) { workers = 0; }
        }
    }
    if (workers == 0) {
        unsigned int hw = std::thread::hardware_concurrency();
        workers = hw == 0 ? 1u : hw;
    }
    unsigned int max_workers_final = workers;
    std::cout << "[BudAssetTool] Using " << max_workers_final << " parallel workers for shader validation." << std::endl;

    // Launch tasks in parallel using a simple batch/future approach
    std::vector<std::future<nlohmann::json>> futures;
    futures.reserve(shader_files.size());

    for (const auto& path : shader_files) {
        futures.push_back(std::async(std::launch::async, [path, tmp_dir]() -> nlohmann::json {
            nlohmann::json entry;
            entry["path"] = path.string();
            entry["compiled"] = false;
            entry["warnings"] = nlohmann::json::array();
            entry["errors"] = nlohmann::json::array();
            entry["bindings"] = nlohmann::json::array();
            entry["inputs"] = nlohmann::json::array();
            entry["outputs"] = nlohmann::json::array();
            entry["push_constants"] = nlohmann::json::array();

            try {
                std::cout << "[BudAssetTool] Validating shader: " << path << std::endl;
                fs::path out_spv = tmp_dir / (path.filename().string() + std::string(".spv"));
                bool compiled = compile_shader_with_glslc(path, out_spv);
                entry["compiled"] = compiled;

                fs::path log_path = tmp_dir / (path.filename().string() + std::string(".spv.log"));
                if (auto log_data = bud::tool_support::read_binary_file(log_path)) {
                    std::string compiler_output(log_data->begin(), log_data->end());
                    entry["compiler_output"] = compiler_output;
                }

                if (!compiled) {
                    std::string msg = std::string("Failed to compile shader with glslc: ") + path.string();
                    std::cerr << "[BudAssetTool] " << msg << std::endl;
                    entry["errors"].push_back(msg);
                    // cleanup
                    std::error_code ec; fs::remove(out_spv, ec); fs::remove(log_path, ec);
                    return entry;
                }

                auto spv_data_opt = bud::tool_support::read_binary_file(out_spv);
                if (!spv_data_opt) {
                    std::string msg = std::string("Failed to read compiled SPV: ") + out_spv.string();
                    std::cerr << "[BudAssetTool] " << msg << std::endl;
                    entry["errors"].push_back(msg);
                    return entry;
                }
                std::vector<char> data = *spv_data_opt;

                SpvReflectShaderModule module;
                SpvReflectResult res = spvReflectCreateShaderModule(data.size(), data.data(), &module);
                if (res != SPV_REFLECT_RESULT_SUCCESS) {
                    std::string msg = std::string("SPIRV-Reflect: failed to create module for ") + path.string();
                    std::cerr << "[BudAssetTool] " << msg << std::endl;
                    entry["errors"].push_back(msg);
                    return entry;
                }

                entry["stage"] = module.shader_stage;

                uint32_t set_count = 0;
                res = spvReflectEnumerateDescriptorSets(&module, &set_count, nullptr);
                if (res == SPV_REFLECT_RESULT_SUCCESS && set_count > 0) {
                    std::vector<SpvReflectDescriptorSet*> sets(set_count);
                    res = spvReflectEnumerateDescriptorSets(&module, &set_count, sets.data());
                    if (res == SPV_REFLECT_RESULT_SUCCESS) {
                        for (uint32_t si = 0; si < set_count; ++si) {
                            SpvReflectDescriptorSet* set = sets[si];
                            nlohmann::json set_json;
                            set_json["set"] = set->set;
                            set_json["bindings"] = nlohmann::json::array();
                            for (uint32_t bi = 0; bi < set->binding_count; ++bi) {
                                const SpvReflectDescriptorBinding* binding = set->bindings[bi];
                                nlohmann::json b;
                                b["set"] = set->set;
                                b["binding"] = binding->binding;
                                b["descriptor_type"] = binding->descriptor_type;
                                b["array_dims"] = binding->array.dims_count > 0 ? binding->array.dims[0] : 0;
                                b["name"] = binding->name ? binding->name : "";
                                set_json["bindings"].push_back(b);
                            }
                            entry["bindings"].push_back(set_json);
                        }
                    }
                }

                uint32_t input_count = 0;
                if (spvReflectEnumerateInputVariables(&module, &input_count, nullptr) == SPV_REFLECT_RESULT_SUCCESS && input_count > 0) {
                    std::vector<SpvReflectInterfaceVariable*> inputs(input_count);
                    if (spvReflectEnumerateInputVariables(&module, &input_count, inputs.data()) == SPV_REFLECT_RESULT_SUCCESS) {
                        for (uint32_t ii = 0; ii < input_count; ++ii) {
                            SpvReflectInterfaceVariable* v = inputs[ii];
                            nlohmann::json iv;
                            iv["location"] = v->location;
                            iv["name"] = v->name ? v->name : "";
                            iv["built_in"] = v->built_in;
                            iv["format"] = v->format;
                            entry["inputs"].push_back(iv);
                        }
                    }
                }

                uint32_t output_count = 0;
                if (spvReflectEnumerateOutputVariables(&module, &output_count, nullptr) == SPV_REFLECT_RESULT_SUCCESS && output_count > 0) {
                    std::vector<SpvReflectInterfaceVariable*> outputs(output_count);
                    if (spvReflectEnumerateOutputVariables(&module, &output_count, outputs.data()) == SPV_REFLECT_RESULT_SUCCESS) {
                        for (uint32_t oi = 0; oi < output_count; ++oi) {
                            SpvReflectInterfaceVariable* v = outputs[oi];
                            nlohmann::json ov;
                            ov["location"] = v->location;
                            ov["name"] = v->name ? v->name : "";
                            ov["built_in"] = v->built_in;
                            ov["format"] = v->format;
                            entry["outputs"].push_back(ov);
                        }
                    }
                }

                uint32_t pcb_count = 0;
                if (spvReflectEnumeratePushConstantBlocks(&module, &pcb_count, nullptr) == SPV_REFLECT_RESULT_SUCCESS && pcb_count > 0) {
                    std::vector<SpvReflectBlockVariable*> pcbs(pcb_count);
                    if (spvReflectEnumeratePushConstantBlocks(&module, &pcb_count, pcbs.data()) == SPV_REFLECT_RESULT_SUCCESS) {
                        for (uint32_t pi = 0; pi < pcb_count; ++pi) {
                            SpvReflectBlockVariable* b = pcbs[pi];
                            nlohmann::json pjson;
                            pjson["size"] = b->size;
                            pjson["name"] = b->name ? b->name : "";
                            entry["push_constants"].push_back(pjson);
                        }
                    }
                }

                spvReflectDestroyShaderModule(&module);
                // remove temp spv
                std::error_code ec; fs::remove(out_spv, ec);
            }
            catch (const std::exception& e) {
                entry["errors"].push_back(std::string("Exception: ") + e.what());
            }
            return entry;
        }));
        // If too many outstanding futures, wait for some
        while (futures.size() > max_workers) {
            auto f = std::move(futures.front());
            futures.erase(futures.begin());
            nlohmann::json e = f.get();
            if (e.contains("compiled") && !e["compiled"].get<bool>()) all_ok.store(false);
            report_json["shaders"].push_back(e);
        }
    }

    // Collect remaining futures
    for (auto& fut : futures) {
        nlohmann::json e = fut.get();
        if (e.contains("compiled") && !e["compiled"].get<bool>()) all_ok.store(false);
        report_json["shaders"].push_back(e);
    }

    if (!report_path.empty()) {
        std::ofstream out(report_path);
        if (out.is_open()) {
            out << report_json.dump(2);
            out.close();
        } else {
            std::cerr << "[BudAssetTool] Failed to write report to " << report_path << std::endl;
        }
    }

    return all_ok.load();
}
#endif

bool bud::tool::AssetProcessor::process_gltf_to_budnanite(const std::string& input_path, const std::string& output_path) {
    Assimp::Importer importer;
    // 注意：不要用 aiProcess_CalcTangentSpace / aiProcess_GenNormals——它们会在 UV/法线
    // 接缝处拆分顶点，把原本共享的网格拆成每面独立顶点（1.5M 顶点），导致后续
    // meshlet 化/简化全部退化。OBJ/glTF 自带的法线直接使用即可。
    const aiScene* scene = importer.ReadFile(input_path,
        aiProcess_Triangulate |
        aiProcess_FlipUVs |
        aiProcess_JoinIdenticalVertices);

    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
        std::cerr << "[Assimp Error]: " << importer.GetErrorString() << std::endl;
        return false;
    }
    if (scene->mNumMeshes == 0) {
        std::cerr << "[BudAssetTool] No meshes found in file." << std::endl;
        return false;
    }

    // 材质与纹理表（与 .budmesh 路径一致，按 aiMaterial 索引对齐）
    std::vector<asset::MaterialDescriptor> materials;
    std::vector<std::string> texture_paths;
	const std::string default_texture_path = "Content/Textures/default.png";
    texture_paths.push_back(default_texture_path);
    {
        std::string input_path_str = std::string(input_path);
        std::string base_dir = "";
        size_t last_slash = input_path_str.find_last_of("\\/");
        if (last_slash != std::string::npos)
            base_dir = input_path_str.substr(0, last_slash + 1);
        for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
            aiMaterial* mat = scene->mMaterials[i];
            asset::MaterialDescriptor md = {};
            aiString tex_path;
            if (mat->GetTexture(aiTextureType_DIFFUSE, 0, &tex_path) == AI_SUCCESS) {
                std::string p = tex_path.C_Str();
                if (p.find(":") == std::string::npos && p.find("/") != 0 && p.find("\\") != 0)
                    p = base_dir + p;
                md.base_color_texture = (uint32_t)texture_paths.size();
                texture_paths.push_back(p);
            } else {
                md.base_color_texture = 0;
            }
            md.alpha_mode = static_cast<uint8_t>(asset::AlphaMode::Opaque);
            md.double_sided = 0;
            md.alpha_cutoff = 0.5f;
            materials.push_back(md);
        }
    }

    // 收集 mesh instances（世界变换，保证 cluster 在对象空间）
    struct MeshInstanceXf {
        unsigned int mesh_index;
        aiMatrix4x4 transform;
    };
    std::vector<MeshInstanceXf> instances;
    std::function<void(aiNode*, aiMatrix4x4)> collect_instances_xf = [&](aiNode* node, aiMatrix4x4 parent) {
        aiMatrix4x4 cur = parent * node->mTransformation;
        for (unsigned int i = 0; i < node->mNumMeshes; ++i)
            instances.push_back({ node->mMeshes[i], cur });
        for (unsigned int i = 0; i < node->mNumChildren; ++i)
            collect_instances_xf(node->mChildren[i], cur);
    };
    collect_instances_xf(scene->mRootNode, aiMatrix4x4());

    // 全局构建结果（多 mesh 合并，偏移按追加顺序调整）
    std::vector<NaniteClusterBuild> all_clusters;
    std::vector<NaniteGroupBuild> all_groups;
    std::vector<asset::NaniteHierarchyLevel> all_levels;
    uint32_t cluster_base = 0;
    uint32_t group_base = 0;

    auto t_start = std::chrono::steady_clock::now();
    std::cout << "[BudNanite] processing " << instances.size() << " instances..." << std::endl;

    for (const auto& inst : instances) {
        const aiMesh* mesh = scene->mMeshes[inst.mesh_index];
        std::vector<asset::Vertex> verts(mesh->mNumVertices);
        for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
            aiVector3D pos = inst.transform * mesh->mVertices[v];
            verts[v].position[0] = pos.x;
            verts[v].position[1] = pos.y;
            verts[v].position[2] = pos.z;
            if (mesh->HasNormals()) {
                aiMatrix3x3 nm(inst.transform);
                aiVector3D n = nm * mesh->mNormals[v];
                n.Normalize();
                verts[v].normal[0] = n.x;
                verts[v].normal[1] = n.y;
                verts[v].normal[2] = n.z;
            }
            if (mesh->HasTextureCoords(0)) {
                verts[v].uv[0] = mesh->mTextureCoords[0][v].x;
                verts[v].uv[1] = mesh->mTextureCoords[0][v].y;
            }
        }
        std::vector<uint32_t> idx;
        for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
            if (mesh->mFaces[f].mNumIndices != 3)
                continue;
            idx.push_back(mesh->mFaces[f].mIndices[0]);
            idx.push_back(mesh->mFaces[f].mIndices[1]);
            idx.push_back(mesh->mFaces[f].mIndices[2]);
        }
        if (idx.empty())
            continue;

        std::vector<NaniteClusterBuild> mesh_clusters;
        std::vector<NaniteGroupBuild> mesh_groups;
        std::vector<asset::NaniteHierarchyLevel> mesh_levels;
        build_mesh_nanite_dag(verts, idx, mesh->mMaterialIndex, mesh_clusters, mesh_groups, mesh_levels);

        // 偏移调整（局部 -> 全局）
        for (auto& g : mesh_groups)
            if (g.children_start != asset::INVALID_INDEX)
                g.children_start += group_base;
        for (auto& c : mesh_clusters) {
            if (c.group_index != asset::INVALID_INDEX)
                c.group_index += group_base;
            if (c.source_group != asset::INVALID_INDEX)
                c.source_group += group_base;
        }
        for (auto& lv : mesh_levels) {
            lv.cluster_start += cluster_base;
            lv.group_start += group_base;
        }
        for (auto& c : mesh_clusters)
            all_clusters.push_back(std::move(c));
        for (auto& g : mesh_groups)
            all_groups.push_back(std::move(g));
        for (auto& lv : mesh_levels)
            all_levels.push_back(std::move(lv));

        cluster_base = (uint32_t)all_clusters.size();
        group_base = (uint32_t)all_groups.size();
    }

    const auto t_dag = std::chrono::steady_clock::now();
    std::cout << "[BudNanite] DAG build took "
              << std::chrono::duration_cast<std::chrono::milliseconds>(t_dag - t_start).count()
              << " ms" << std::endl;

    // 统计输出（验证 DAG 构建）
    std::cout << "[BudNanite] instances=" << instances.size()
              << " clusters=" << all_clusters.size()
              << " groups=" << all_groups.size()
              << " levels=" << all_levels.size() << std::endl;
    for (size_t li = 0; li < all_levels.size(); ++li) {
        const auto& lv = all_levels[li];
        std::cout << "  level " << li << ": clusters=" << lv.cluster_count
                  << " groups=" << lv.group_count << std::endl;
    }

    // 页分配（DAG 序贪心打包 128KB 页）
    std::vector<NanitePageAssign> pages;
    std::vector<uint32_t> cluster_page;
    std::vector<uint32_t> cluster_page_vertex_offset;
    std::vector<uint32_t> cluster_page_index_offset;
    const auto t_pages = std::chrono::steady_clock::now();
    assign_nanite_pages(all_clusters, all_groups, pages,
                        cluster_page, cluster_page_vertex_offset, cluster_page_index_offset);
    std::cout << "[BudNanite] page assignment took "
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t_pages).count()
              << " ms" << std::endl;

    std::cout << "[BudNanite] pages=" << pages.size()
              << " (page_size=" << bud::asset::NANITE_PAGE_SIZE << ")" << std::endl;
    uint32_t total_data = 0;
    for (size_t pi = 0; pi < pages.size(); ++pi) {
        const auto& p = pages[pi];
        total_data += p.size_in_bytes;
        std::cout << "  page " << pi << ": clusters=" << p.clusters.size()
                  << " verts=" << p.vertex_count << " tris=" << p.index_count
                  << " bytes=" << p.size_in_bytes
                  << " dep=" << (p.dependency_page_id == asset::INVALID_INDEX ? -1 : (int)p.dependency_page_id)
                  << (p.is_root ? " [ROOT]" : "") << std::endl;
    }
    std::cout << "[BudNanite] total page data=" << total_data << " bytes" << std::endl;

    // 序列化 .budmesh
    const auto t_write = std::chrono::steady_clock::now();
    const bool ok = write_nanite_file(output_path, all_clusters, all_groups, all_levels,
                                      pages, cluster_page,
                                      cluster_page_vertex_offset, cluster_page_index_offset,
                                      materials, texture_paths);
    std::cout << "[BudNanite] serialization took "
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t_write).count()
              << " ms" << std::endl;
    if (ok)
        std::cout << "[BudNanite] wrote " << output_path << std::endl;
    return ok;
}


// end of file


