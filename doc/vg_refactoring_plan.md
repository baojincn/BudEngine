# Virtual Geometry Refactoring Plan

## 概述

本次重构目标：
1. 引入 Task/Mesh Shader 路径替代 VG 渲染管线，消除 LOD 过渡闪白
2. 移除 `render_config.enable_gpu_driven` 配置项，默认全 GPU Driven
3. 删除 `bud.graphics.passes.cpp`（已拆分完成）
4. 保留 Compute Shader 路径作为旧硬件回退

---

## 一、移除 `render_config.enable_gpu_driven`

### 改动范围

| 文件 | 改动 |
|------|------|
| `src/graphics/bud.graphics.types.hpp` | 删除 `RenderConfig::enable_gpu_driven` 字段 |
| `src/graphics/bud.graphics.renderer.cpp` | 所有 `if (render_config.enable_gpu_driven)` 改为无条件执行；删除 `gpu_driven_available` 变量 |
| `src/graphics/passes/bud.pass.virtual_geometry.cpp` | 各 Pass 中 `if (!config.enable_gpu_driven \|\| ...)` 删除前半部分 |
| `src/graphics/bud.graphics.passes.hpp` | 移除相关的条件判断 |

### 影响

- 所有 `HierarchyTraversalPass`、`PageEmitPass`、`ClusterCullPass` 等不再检查 enable_gpu_driven
- `RenderConfig` 结构体减少一个字段
- 简化 `render()` 主流程

---

## 二、删除 `bud.graphics.passes.cpp`

`passes.cpp` 已经拆分完成，各 Pass 实现已独立到 `src/graphics/passes/` 目录下。只需：

1. 删除 `src/graphics/bud.graphics.passes.cpp`
2. 更新 `CMakeLists.txt` 移除该文件

**完成后**：所有 Pass 实现统一在 `src/graphics/passes/` 目录下。

---

## 三、架构决策说明

### 3.1 Resolve Pass 与 Main Pass 的混合

Resolve Pass 使用 **硬件光栅化（全屏 Quad）**，而不是软件光栅化（Compute Shader）。

**原因**：我们的 cluster 是常规三角形（128 顶点/cluster），投影到屏幕通常 > 1 像素。硬件光栅化器对常规三角形是最优的，不需要像 Nanite 那样用软光栅。

**混合方式**：
- Resolve Pass 绑定 Phase 2 的 depth buffer（只读，不写深度）
- 有 VG 实例的像素 → 写入 color buffer
- 无 VG 实例的像素 → discard
- Main Pass 使用同一 depth buffer 做深度测试，传统物体正确遮挡 VG

**半透明物体**：半透明物体在 Main Pass 中渲染，使用 Alpha Blending，不写深度。如果半透明物体在 VG 前面，混合叠加 VG color；如果在 VG 后面，深度测试被挡，不写入。

### 3.2 Visibility Pass 必须写入 Depth Buffer

**关键问题**：AO 需要完整的深度缓冲。如果 Visibility Pass 不写深度，Phase 2 Depth Prepass 又跳过 VG 物体，AO 拿到的深度就不全。

**解决方案**：**Visibility Pass 同时输出两个目标**：

```
Visibility Pass Fragment Shader:
  layout(location = 0) out uvec2 out_visibility;  // R32G32_UINT → visibility buffer
  // 同时写入 gl_FragDepth（硬件自动从 depth test 获得）
```

**Phase 2 Depth Prepass 退化为只写传统物体**：

```
Phase 2 Depth Prepass:
  → 跳过 VG 物体（因为 Visibility Pass 已经写了它们的深度）
  → 只写传统动态物体 depth
  → 最终 depth buffer = VG depth (from Visibility Pass) + 传统 depth (from Phase 2)
  → AO 拿到完整 depth
```

### 3.3 硬件光栅化 vs 软光栅化（Nanite 对比）

| 方面 | Nanite（软光栅） | 我们（硬件光栅化） |
|------|-----------------|-------------------|
| **三角形粒度** | 微多边形（< 1 像素） | 常规三角形（> 4 像素） |
| **Cluster 容量** | 128 三角形 | 128 三角形 |
| **LOD 粒度** | 像素级（微多边形细分） | Cluster 级（预处理简化） |
| **光栅化方式** | Compute Shader 软光栅 | 硬件光栅化器 |
| **原因** | 数百万微多边形，硬件光栅化器是瓶颈 | 常规三角形，硬件光栅化器足够 |

### 3.4 微多边形化（未来方向）

**当前**：LOD 在预处理阶段完成，用 `meshopt_simplify` 生成多级简化版本，每个级别作为一个 cluster 存入 hierarchy tree。渲染时通过 Hierarchy Traversal 选择 LOD level，cluster 粒度的 LOD 切换。

**未来**：最终目标是在 Task/Mesh Shader 中实现运行时微多边形化：

```
→ Hierarchy Traversal 遍历 cluster
→ 如果 cluster 投影 > 1 像素的三角形数量：
    → 在 Mesh Shader 中动态细分三角形
    → 直到每个子三角形投影 ≤ 1 像素
→ 写入 Visibility Buffer
```

这样做的好处：
- **LOD 粒度从 cluster 级降到像素级**，消除 LOD 切换边界
- **自然连续的 LOD 过渡**，不需要 dithering
- **彻底消除闪白**（因为不存在 LOD 切换）

**当前不做的原因**：实现复杂，Cluster 粒度的 LOD 配合 Visibility Buffer 已能解决闪白问题。

---

## 四、新管线架构

### 当前管线

```
Phase 1 Depth Prepass (occluder only → Hi-Z)
  → Pyramid Mip (Hi-Z)
  → Hierarchy Traversal (compute)
  → Page Emit (compute)
  → Cluster Cull (compute)
  → Instance Culling (compute, 传统动态网格)
  → Phase 2 Depth Prepass (补全所有物体深度 → AO 用)
  → AO Pass
  → Main Pass (VG + 传统动态 + 半透明, 写深度)
  → CSM Shadow Pass
```

### 新管线

```
Phase 1 Depth Prepass (occluder only → Hi-Z)
  → Pyramid Mip (Hi-Z)
  → Hierarchy Traversal (compute)
  → Instance Culling (compute, 传统动态网格)
  →
  → [Mesh Shader 可用]
  │   → Visibility Pass (task/mesh, VG only)
  │       写入 visibility buffer (R32G32_UINT) + depth buffer (D32_FLOAT)
  │
  → [Mesh Shader 不可用]
  │   → Page Emit (compute) → Cluster Cull (compute)
  │   → Main Pass (vertex/fragment, VG, 写 depth + color)
  │
  → Phase 2 Depth Prepass (传统动态物体 ONLY, 补全 AO depth)
  → AO Pass (读取完整 depth buffer)
  →
  → [Mesh Shader 可用]
  │   → Resolve Pass (全屏 Quad, 读取 visibility buffer, 着色 VG)
  │
  → Main Pass (传统动态物体 + 半透明, 深度测试使用完整 depth buffer)
  → CSM Shadow Pass
```

### 关键变化

1. **Visibility Pass 写入 visibility buffer + depth buffer**，Phase 2 不再写 VG 深度
2. **Main Pass 退化为**只处理传统动态物体 + 半透明物体
3. **Phase 2 Depth Prepass 退化**为只写传统物体深度，补全给 AO 使用
4. **光照计算** 抽取到 `lighting.glsl`，Resolve Pass 和 Main Pass 共用

---

## 五、Task/Mesh Shader 路径（新硬件）

### 检测方式

在 `Renderer` 初始化时检查：

```cpp
VkPhysicalDeviceMeshShaderFeaturesEXT mesh_features{};
mesh_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
vkGetPhysicalDeviceFeatures2(device, &features2);
bool has_mesh_shader = mesh_features.taskShader && mesh_features.meshShader;
```

### 新增 Shader

| 文件 | 说明 |
|------|------|
| `src/shaders/visibility.task` | 读取 `visible_pages` buffer，按 page 分派，遍历 cluster，frustum cull + occlusion cull，放大到 mesh shader |
| `src/shaders/visibility.mesh` | 解码 VG 顶点（`vg_read_bits` 从 `main.vert` 移植），输出三角形 |
| `src/shaders/visibility.frag` | 输出 `R32G32_UINT`（instance_id, cluster_id）+ 写入 `gl_FragDepth` |
| `src/shaders/resolve.vert` | 全屏 Quad |
| `src/shaders/resolve.frag` | 读取 visibility buffer，解码 VG 顶点，调用 `lighting.glsl` 着色 |
| `src/shaders/lighting.glsl` | 从 `main.frag` 提取光照计算函数 |

### Visibility Buffer 格式

`R32G32_UINT` 纹理，屏幕分辨率：

```
bit 0-15:   instance_id  (65536 个实例)
bit 16-31:  cluster_id   (65536 个 cluster)
bit 32-55:  depth        (24-bit float encoding)
bit 56-63:  flags        (保留)
```

### 新增 Pass

| Pass | 文件 | 说明 |
|------|------|------|
| `VisibilityPass` | `src/graphics/passes/bud.pass.visibility.cpp` | 绑定 task/mesh shader 管线，写入 visibility buffer + depth buffer |
| `ResolvePass` | `src/graphics/passes/bud.pass.resolve.cpp` | 全屏 Quad，解析 visibility buffer 并着色 |

### LOD 过渡处理

在 Visibility Pass 的 Fragment Shader 中：

```glsl
// 低 LOD = blend_factor > 0
// 高 LOD = blend_factor = 0
if (blend_factor > 0.0 && blend_factor < 1.0) {
    float noise = golden_noise(gl_FragCoord.xy);
    if (noise < blend_factor)
        discard; // 低 LOD 像素丢弃，高 LOD 自然胜出
}
```

深度测试在同一 Pass 内完成，低 LOD discard 后高 LOD 自动可见，无闪白。

---

## 六、Compute Shader 回退路径（旧硬件）

保留现有 pipeline 作为降级方案：

```
Hierarchy Traversal (compute)
  → visible_pages buffer
  → Page Emit (compute)
  → visible_clusters buffer
  → Cluster Cull (compute)
  → indirect_draw + dynamic_instances buffer
  → Main Pass (vertex/fragment, 解码 VG 顶点)
```

### 条件

- 仅在 `!has_mesh_shader` 时使用
- 维持现有逻辑不变（包括 LOD dithering + depth prepass 跳过逻辑）

---

## 七、完整管线流程

```
Phase 1 Depth Prepass (occluder only → Hi-Z)
  → Pyramid Mip (Hi-Z)
  → Hierarchy Traversal (compute)
  → Instance Culling (compute, 传统动态网格)
  →
  → [has_mesh_shader]
  │   → Visibility Pass (task/mesh, VG only)
  │       ├─ 写入 visibility buffer (R32G32_UINT)
  │       └─ 写入 depth buffer (D32_FLOAT, 同 frame)
  │
  → [!has_mesh_shader]
  │   → Page Emit (compute) → Cluster Cull (compute)
  │   → Main Pass (vertex/fragment, VG, 写 depth + color)
  │
  → Phase 2 Depth Prepass (传统动态物体 ONLY)
  │   → 读取 depth buffer (已有 VG depth)
  │   → 写入 depth buffer (追加传统物体 depth)
  │
  → AO Pass (读取完整 depth buffer)
  │
  → [has_mesh_shader]
  │   → Resolve Pass (全屏 Quad, 不写 depth)
  │       ├─ 读取 visibility buffer
  │       ├─ 解码 VG 顶点, 着色
  │       └─ 写入 color buffer
  │
  → Main Pass (传统动态 + 半透明)
  │   → 不透明物体: 深度测试(完整 depth), 写入 color
  │   → 半透明物体: Alpha Blending, 不写 depth
  │
  → CSM Shadow Pass
```

```
Phase 1 Depth Prepass (occluder only → Hi-Z)
  → Pyramid Mip (Hi-Z)
  → Hierarchy Traversal (compute)
  → Instance Culling (compute, 传统动态网格)
  →
  → [has_mesh_shader]
  │   → Visibility Pass (task/mesh) → Resolve Pass
  │
  → [!has_mesh_shader]
      → Page Emit (compute) → Cluster Cull (compute) → Main Pass (VG)
  →
  → Phase 2 Depth Prepass (补全深度 → AO)
  → AO Pass
  → Main Pass (传统动态物体 + 半透明)
  → CSM Shadow Pass
```
---

## 八、改动清单

### 7.1 新增文件

| 文件 | 说明 |
|------|------|
| `src/shaders/visibility.task` | Task Shader |
| `src/shaders/visibility.mesh` | Mesh Shader |
| `src/shaders/visibility.frag` | Visibility Fragment Shader |
| `src/shaders/resolve.vert` | Resolve Vertex Shader |
| `src/shaders/resolve.frag` | Resolve Fragment Shader |
| `src/shaders/lighting.glsl` | 光照计算共享库 |
| `src/graphics/passes/bud.pass.visibility.cpp` | Visibility Pass 实现 |
| `src/graphics/passes/bud.pass.resolve.cpp` | Resolve Pass 实现 |

### 7.2 修改文件

| 文件 | 改动 |
|------|------|
| `src/graphics/bud.graphics.types.hpp` | 删除 `enable_gpu_driven` |
| `src/graphics/bud.graphics.renderer.hpp` | 加 `has_mesh_shader` 标志，声明新 Pass |
| `src/graphics/bud.graphics.renderer.cpp` | 移除 `enable_gpu_driven` 条件，插入 Visibility/Resolve Pass |
| `src/graphics/bud.graphics.passes.hpp` | 声明新 Pass 类 |
| `src/graphics/passes/bud.pass.virtual_geometry.cpp` | 移除 `enable_gpu_driven` 检查，加回退路径 |
| `src/graphics/passes/bud.pass.main.cpp` | 移除 VG 逻辑，只保留传统动态 + 半透明 |
| `src/graphics/passes/bud.pass.depth.cpp` | 恢复原样（移除 blend_factor） |
| `src/shaders/main.vert` | 移除 VG 解码逻辑 |
| `src/shaders/main.frag` | 移除 VG 相关，调用 `lighting.glsl` |
| `src/shaders/depth_only.vert` | 恢复原样 |
| `src/shaders/depth_only.frag` | 恢复原样 |
| `CMakeLists.txt` | 加新 shader 文件，移除 `passes.cpp` |

### 7.3 删除文件

| 文件 |
|------|
| `src/graphics/bud.graphics.passes.cpp` |

---

## 九、执行顺序

| 阶段 | 内容 | 估计 | 依赖 |
|------|------|------|------|
| 1 | 移除 `enable_gpu_driven` | 1 天 | 无 |
| 2 | 删除 `bud.graphics.passes.cpp` | 0.5 天 | 无 |
| 3 | 提取 `lighting.glsl` | 1 天 | 无 |
| 4 | 新建 Visibility/Resolve Shader | 2 天 | 无 |
| 5 | 新建 Visibility/Resolve Pass | 1 天 | 4 |
| 6 | 简化 `main.vert/frag`（移除 VG 逻辑） | 1 天 | 3 |
| 7 | 恢复 `depth_only.vert/frag` | 0.5 天 | 无 |
| 8 | 添加 Compute Shader 回退路径 | 1 天 | 5 |
| 9 | 集成测试 + 调优 | 2 天 | 1-8 |
| **合计** | | **~10.5 天** | |

---

## 十、风险

| 风险 | 影响 | 缓解 |
|------|------|------|
| Visibility Buffer 显存占用（4K = 64MB） | 显存增加 | 可降采样至半分辨率 |
| Mesh Shader 解码性能 | 帧率下降 | 测试后优化，必要时回退到 Compute 路径 |
| 传统网格兼容性 | 现有网格渲染异常 | 分开处理 VG 和传统网格 |
| `passes.cpp` 删除引入回归 | 编译失败 | 编译验证后删除 |

---

## 十一、未来方向：微多边形化

当前方案使用 **预处理简化 + Cluster 粒度 LOD**，配合 Visibility Buffer 解决闪白问题。

**未来**：在 Task/Mesh Shader 中实现运行时微多边形化：

```
→ Hierarchy Traversal 遍历 cluster
→ 如果 cluster 投影 > 1 像素的三角形数量：
    → 在 Mesh Shader 中动态细分三角形
    → 直到每个子三角形投影 ≤ 1 像素
→ 写入 Visibility Buffer
```

**优势**：
- LOD 粒度从 cluster 级降到像素级，消除 LOD 切换边界
- 自然连续的 LOD 过渡，不需要 dithering
- 彻底消除闪白（因为不存在 LOD 切换）

**路线图**：
1. ✅ 当前阶段：Cluster 粒度 LOD + Visibility Buffer（解决闪白）
2. 🔜 下一阶段：运行时微多边形化（消除 LOD 切换）