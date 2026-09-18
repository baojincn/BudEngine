# BudEngine 双模式物理与仿真环境碰撞演进路线图

## 一、 架构定位与背景

BudEngine 采用面向游戏与具身智能机器人并重的双模式物理架构：

| 模式 | 运行后端 | 核心物理步频 | 适用目标 |
| :--- | :--- | :--- | :--- |
| **标准游戏模式 (Game Mode)** | Jolt Physics | 60 Hz ~ 120 Hz | 玩家角色控制器、大世界场景流式加载、海量散落动态刚体、GPU XPBD 布料物理、交互道具 |
| **机器人仿真模式 (Simulation Mode)** | MuJoCo / 自研 Luoma Physics | 500 Hz ~ 1000 Hz | 29-DOF 拟人全身运动学与动力学、电机参数辨识、高保真足底接触力、强化学习 (RL) 控制策略与 Sim2Real |

### 1.1 为什么游戏模式与仿真模式的场景碰撞范式完全不同？

在游戏模式中，场景静态碰撞的主流方案是**层次包围盒三角网格（Triangle Mesh BVH）**。这种方案在游戏低频（60Hz）下配合粗糙的玩家角色胶囊体（Capsule Character Controller）运作良好。

但在仿真模式（500Hz+）下，**直接使用三角面网格是导致仿真崩溃和非物理行为的核心诱因**：
1. **接缝奇点与卡边（Mesh Edge Snagging）**：
   三角面由离散的边与顶点构成。机器人细小的足端或连杆划过网格接缝时，表面法线会发生阶跃突变。
2. **500Hz 刚性冲击爆炸**：
   高频 LCP/PGS 微分方程求解器为了纠正微小的穿透，会在顶点接缝处产生极大冲击力（$10^4\text{ N}$ 级虚拟反作用力），直接导致机器人跌倒或炸飞。
3. **穿透深度计算不可微**：
   凹面三角网格无法快速、稳定地计算处处连续的穿透深度梯度 $\nabla \phi$。

因此，国际先进机器人仿真平台（NVIDIA Omniverse / Isaac Lab、DeepMind MuJoCo 3+、Genesis World）在仿真模式下的统一标准做法均为：**放弃复杂的非凸三角网格，采用 HField（高度场）与 Mesh SDF（有向距离场）作为静态场景碰撞的数学底座**。

---

## 二、 仿真模式环境碰撞的两大核心支柱

```
[场景几何分类]
       |
       +---> 室外地表 / 台阶 / 起伏山地 ---> HField (高度场)
       |                                      |--> 2.5D 高程映射 z = f(x,y)
       |                                      |--> O(1) 连续高程与平滑法线
       |
       +---> 室内建筑 / Sponza 宫殿 / 家具 ---> Mesh SDF (有向距离场)
                                              |--> 3D 体素距离场 phi(x,y,z)
                                              |--> 连续穿透梯度，无接缝奇点
```

### 2.1 支柱一：高度场（HField / Elevation Map）
- **职责**：野外地表、斜坡、碎石路面、台阶、楼梯、路沿。
- **数学本质**：连续 2.5D 映射 $z = f(x, y)$。
- **核心优势**：
  - 查询时间复杂度为 $O(1)$，内存开销极小。
  - 空间梯度与法线处处连续可微，足底踩踏永远不会卡入三角形缝隙。
  - 原生支持强化学习高程扫描图（Elevation Heightmap Scan）的快速采样。
- **底层映射**：
  - MuJoCo 阶段：`mjGEOM_HFIELD`。
  - Luoma Physics 阶段：`TerrainHField` 模块。

### 2.2 支柱二：有向距离场（Mesh SDF / Signed Distance Fields）
- **职责**：Sponza 宫殿建筑、室内多层房间、走廊立柱、拱门、门框、桌椅家具。
- **数学本质**：3D 体素化网格，每个体素存储到最近几何表面的带符号欧氏距离 $\phi(\mathbf{x})$：
  - $\phi(\mathbf{x}) > 0$：空间外部自由区；
  - $\phi(\mathbf{x}) = 0$：几何表面；
  - $\phi(\mathbf{x}) < 0$：几何内部（穿透量）；
  - 空间梯度 $\nabla \phi(\mathbf{x})$：精确、连续的表面接触法线。
- **核心优势**：
  - 彻底解决凹面建筑（如 Sponza）被算法粗暴退化为实心巨石凸包的问题。
  - 机器人身体连杆碰撞检测转化为直接读取 $\phi$ 与 $\nabla \phi$，极其快速且动力学稳定。
- **底层映射**：
  - MuJoCo 阶段：`mjGEOM_SDF`（MuJoCo 3.0+ 原生特性）。
  - Luoma Physics 阶段：`SceneSDF` 模块。

---

## 三、 三阶段演进路线图

### 阶段 1：Sponza 关键障碍物代理体 (Proxy Primitives)
- **状态**：[READY] 待实施
- **目标**：在短时间内使当前 MuJoCo 模式下的 G1 机器人在 Sponza 场景中具备真实的实体碰撞阻挡，不穿柱、不穿外墙。
- **实施内容**：
  1. 梳理 Sponza 场景的主结构网格（柱列、四面外围主墙体）。
  2. 在场景初始化时提取对应的 AABB 或定向包围盒，生成对应的 `ShapeType::Box` 或 `ShapeType::Capsule` 描述。
  3. 调用现已完全支持的 [MujocoPhysicsWorld::add_rigid_body](file:///d:/PersonalProjects/BudEngine/src/physics/bud.physics.world.mujoco.cpp#L259) 注册到 MuJoCo 的 `world_body`。
  4. 保持地面平面 `mjGEOM_PLANE` 负责平地站立，立柱/墙面负责侧向排斥与阻挡。

### 阶段 2：仿真资产烘焙管线与 MuJoCo HField / Mesh SDF 对接
- **状态**：[PLANNED] 规划中
- **目标**：打通环境模型在 BudAsset 资产系统中的离线仿真数据烘焙，全面接入 MuJoCo 原生 HField 与 Mesh SDF。
- **实施内容**：
  1. **资产编译器扩展 (`BudAssetCompiler`)**：
     - 为环境模型引入 `AssetChunkType::SimulationHField` 与 `AssetChunkType::SimulationSDF` 块。
     - 地表网格自动光栅化为 2.5D 高程数组（HField）。
     - 3D 复杂建筑网格离线体素化为 Signed Distance Field（网格分辨率如 2cm ~ 5cm）。
  2. **MuJoCo 后端对接 (`MujocoPhysicsWorld`)**：
     - 扩展 `add_rigid_body()`，新增 `ShapeType::HeightField` 与 `ShapeType::SDF` 分支。
     - 对接 MuJoCo 的 `mjs_addHField`、`mjGEOM_HFIELD` 与 `mjGEOM_SDF`。
  3. **成果**：机器人可以自然踩上台阶、倚靠真实凹凸墙壁，完全消除简化盒带来的视觉与物理缝隙。

### 阶段 3：自研 Luoma Physics 物理后端与无缝承接
- **状态**：[FUTURE] 远期规划
- **目标**：自研机器人专用多刚体动力学求解器 Luoma Physics，替代 MuJoCo，实现全链路自主可控。
- **架构设计原则**：
  1. **核心动力学**：
     - 采用广义坐标（Generalized Coordinates），实现 Featherstone 羽石算法（ABA 前向递推正动力学 + CRBA 联合惯量复合刚体算法）。
     - 相比最大坐标（笛卡尔 6-DOF + 关节约束），广义坐标从数学上天然杜绝关节脱臼与拉伸。
  2. **环境碰撞核心**：
     - **不实现** 传统的游戏三角面 BVH 碰撞树。
     - 原生设计两个专职模块：
       - `TerrainHField`：地表高程模块，支持动态形变地貌与台阶。
       - `SceneSDF`：三维静态空间距离场模块，直接消费阶段 2 烘焙的 `SimulationSDF` 资产块。
  3. **统一上层抽象**：
     - 上层应用（如 `RobotAvatarController`、RL Policy Runner）完全面向 `PhysicsWorldBase` 接口，无感切换 Jolt、MuJoCo 与 Luoma Physics。

---

## 四、 感知层（Perception Layer）与控制协同

物理环境具备了 HField 和 SDF 后，控制层也需要同步由**盲走**演进到**感知交互**：

```
[环境物理场]                    [机器人感知层]                    [RL 控制策略]
  HField (高程)  -------->   足底高度图采样 (16x16)   -------->   抬腿跨步 / 调整步高
  Mesh SDF (距离) -------->   前向深度图 / 射线雷达   -------->   主动避障 / 绕行柱子
```

1. **当前状态**：
   现有的 G1 步态策略为纯盲走本体感知（Proprioceptive-only: 关节角、角速度、IMU），撞到柱子会产生被动物理阻挡，但策略自身仍会尝试向前迈步。
2. **后续目标**：
   在感知循环中挂载虚拟深度相机或从 HField 中实时采样足底高度网格，输入到感知运动策略中，实现真正的避障与楼梯跨越。
