# MuJoCo G1 在 Sponza 中稳定站立 — 完整实施计划

本文档是把 Unitree G1 的物理从「Jolt ragdoll + 运动学位姿预设」切换到「MuJoCo + 官方电机模型 + LowCmd」，
并让它最终**在 Sponza 场景的物理地面上稳定站立**的完整方案。

它是 `doc/robot_physics_driven_plan.md`（Jolt 力驱动方案）在机器人链路上的**替代计划**：
Jolt 那条路已因 `MoveKinematic` 深穿透 + EPA 越界被判定为不可行，机器人不再用 Jolt。

---

## 一、 目标与验收标准

**本里程碑的唯一定义：** 一台由与官方一致的 MuJoCo 模型和 LowCmd 控制的 G1，
在 Sponza 场景中，站在真实的物理地面上，**连续 60 秒稳定站立**（不下垂、不抖动、不穿地、不崩），
并且渲染、相机、布料不产生回归。

| # | 验收项 | 判定 |
| :-- | :-- | :-- |
| 1 | 不崩溃 | Debug/Release 各站立 60s 不崩 |
| 2 | 模型一致 | cook 产物关键量与官方 `g1_29dof.xml` 逐项一致（见阶段 0.4） |
| 3 | 电机语义一致 | 执行器为 `<motor>`，PD 在控制器侧按 `tau=kp·(q_des−q)+kd·(dq_des−dq)+tau_ff` 计算 |
| 4 | 稳定站立 | 60s 内 pelvis 高度波动 < 2cm、无可见抖动、脚底不穿地 |
| 5 | 无回归 | 渲染、相机、布料行为与改动前一致 |
| 6 | 可回退 | 每阶段独立可回退，不出现中间不可用态 |

---

## 二、 原则（忠于官方，我们只做接线）

| 层面 | 归属 | 我们的动作 |
| :-- | :-- | :-- |
| 物理求解 | **MuJoCo**（官方仿真同款） | 不重写、不近似 |
| 模型参数 | 官方数值（armature / frictionloss / damping / effort / inertial / mesh） | 原样搬运，只换容器 |
| 控制语义 | 官方 LowCmd（q/dq/kp/kd/tau）+ PD | 照抄边界结构 |
| 资产容器 | 我们 | `.budasset` 打包、mesh 引用、chunk |
| 渲染 | 我们 | Vulkan 替换官方 mjr viewer |

**核心原则 1：物理系统定界 — 高精度物理模拟与机器人项目只有 MuJoCo，游戏项目才用 Jolt。**
- 引擎按项目/场景类型在二者间互斥选择（`EngineConfig::physics_backend`），**永不同时运行两套物理引擎**。
- **机器人仿真场景**：纯 MuJoCo 独占。机器人 29DoF 关节链、电机、地面和场景可交互刚体统一在 MuJoCo 求解器中运行，彻底剔除与旁路 Jolt。
- **游戏场景**：纯 Jolt 独占。休闲刚体与角色控制器运行于 Jolt，不运行 MuJoCo。

**核心原则 2：物理 payload 不重组成引擎原生结构。** MJCF 原样躺在 chunk 里，
我们的数据组织只包在外面（容器、mesh 引用表、渲染元数据）。
一旦把关节/惯量/摩擦抽成自研 struct 再重新表达，漂移就进来了——Jolt 那条路就是这么退化的。

**唯一漂移入口是 URDF→MJCF 这一步**，所以阶段 0 用对拍把它钉死。

---

## 三、 目标架构

```mermaid
flowchart TD
    subgraph Engine ["BudEngine"]
        Scene["Sponza 场景（渲染 + 地面）"] --> PhysScene["PhysicsScene"]
        PhysScene --> MjWorld["MujocoPhysicsWorld（唯一物理后端）"]
        MjWorld --> Articulation["G1 Articulation"]
        MjWorld --> Props["场景碰撞体 / 刚体道具（同求解器双向接触）"]
    end

    subgraph Control ["控制层"]
        LowCmd["UnitreeLowCmd(q,dq,kp,kd,tau)"] --> PD["PD + 前馈"]
        PD --> Ctrl["data->ctrl（motor）"]
    end

    subgraph Cook ["离线 cook"]
        URDF["g1_29dof.urdf"] --> Cook["补 armature/frictionloss/damping + motor + sensors"]
        Cook --> Asset[".budasset PhysicsModel chunk（MJCF）"]
    end

    subgraph Cloth ["布料管线 (独立 GPU XPBD)"]
        State --> Colliders["Link 碰撞代理 (gpu_colliders)"]
        Colliders --> GpuCloth["Vulkan XPBD Compute Shader (排斥布料顶点)"]
    end

    Asset --> MjWorld
    Ctrl --> Articulation
    Articulation --> State["ArticulationStateSoA"]
    State --> Visual["RobotVisualBridge（渲染）"]
    State --> Camera["相机绑定"]
```

### 交互模型说明：
1. **机器人 ↔ 场景刚体/地面**：全部放入 **MuJoCo** 求解。场景地砖、箱子、柱子与 G1 处于同一接触动力学方程中，原生具有双向受力、冲量反作用与库仑摩擦。
2. **机器人 ↔ 挂毯/布料**：由自研 **Vulkan GPU XPBD Compute Shader** 解算。机器人的各 Link 胶囊体作为碰撞代理传入 `gpu_colliders`，着色器在位置积分时实时推开布料，实现高帧率无穿模拨动。
3. **架构选择**：**唯一采用方案 A（MuJoCo 全面接管物理）**。彻底废弃任何“机器人跑 MuJoCo + 场景跑 Jolt”的双后端混合方案。

---

## 四、 实施阶段

| 阶段 | 目标 | 状态 |
| :-- | :-- | :-- |
| **0** | 模型一致性（cook 补参数 + 对拍） | 已完成（见第九节） |
| **1** | MuJoCo 成为可用物理后端 | 已完成（1.1/1.2/1.3/1.4/1.5/1.6 均就绪） |
| **2** | 机器人链路接入 MuJoCo 与双模式支持 | 已完成（RobotLoader/RobotInstance/AvatarController 双模实现） |
| **3** | LowCmd 控制器层 + 官方站姿 | 已完成（LowCmd/500Hz PD/官方站姿与增益） |
| **4** | 站立稳定化 | 已完成（4.1/4.2/4.3/4.4 全部通过） |
| **5** | Sponza 落地与端到端验收 | 已完成（5.1/5.2/5.3/5.4/5.5 端到端 60s 实测通过） |
| **6** | 观测 / RL 接口（非本里程碑阻塞） | 待实施 |

---

### 阶段 0 — 模型一致性

让 cook 产物与官方 `g1_29dof.xml` 等价。改动集中在
`src/tools/asset_pipeline/builders/mujoco_model_builder.cpp`。

当前代码在 `:94-117` 只有注释，明确不写参数、不建 actuator，且 `:100-111` 的 probe compile
已经解决了「必须先 compile 一次才能加 actuator」的问题。在该 probe compile 之后插入：

- **0.1 关节被动物理**：按关节名分类（leg / torso / arm / ankle / wrist），
  用 `mjs_findElement(spec, mjOBJ_JOINT, name)` 拿 `mjsJoint*`，设 `armature` / `frictionloss` / `damping`。
  严格照官方默认：leg/torso/arm/ankle = `0.05 / 0.01 / 0.2`，wrist = `0.05 / 0.01 / 0.1`。
- **0.2 执行器**：遍历非 Fixed 关节，`mjs_addActuator` + `mjs_setToMotor` + target=joint，
  `ctrlrange = ±joint.limit.effort`（官方 `<motor ctrlrange="-88 88">` 的语义，motor 的 `force = gear*ctrl`）。
  actuator 名用 `<joint>_motor`，避免与 joint 重名。
- **0.3 传感器（可缓到阶段 6）**：root body 加 `imu` site + `framequat/gyro/accelerometer`；
  每关节加 `jointpos/jointvel/jointactuatorfrc`。
- **0.4 conformance 对拍**：cook 完编译模型后 dump
  `nbody / njnt / nu / body_mass[] / jnt_armature[] / jnt_frictionloss[] / jnt_damping[] / actuator_ctrlrange[]`，
  与官方 `g1_29dof.xml` 逐项比较（能断言就断言）。
- **0.5** 重新烘 `Content/Robots/g1_description/g1_29dof.budasset`。

引用：`mujoco_model_builder.cpp:94-117`、`:100-111`、`:256-265`。

**验收**：conformance dump 与官方一致；`samples/mujoco_backend_test` 中 `nu == 非 Fixed 关节数`。

---

### 阶段 1 — MuJoCo 成为可用物理后端

当前运行时**永远**是 Jolt：`engine.cpp:927` 调用 `physics_scene->init()` 不带参数，
默认 `PhysicsBackend::Jolt`（`world.hpp:44`、`physics.scene.hpp:43`）。`MujocoPhysicsWorld`
只在 `samples/mujoco_backend_test/main.cpp:70/75` 被直接使用。

- **1.1 后端可选**：给 `AppConfig` / `EngineConfig` 加 `PhysicsBackend` 字段，
  `engine.cpp:927` 传入；默认仍 Jolt，Sponza 站立用例显式设 MuJoCo。
- **1.2 地面**：`PhysicsWorldConfig` 加 ground plane（高度来自场景/配置），
  `MujocoPhysicsWorld::init` 建 plane geom，并正确设置 `contype/conaffinity/condim/friction`。
  这是「先跑通」的关键——MuJoCo 需要一个明确的承载面。
- **1.3 删除运行时 actuator 创建**：移除 `bud.physics.world.mujoco.cpp:355-420`
  （含 probe compile、`actuators_created`、`mjs_setToPosition` 路径）。执行器全部来自阶段 0。
- **1.4 静态几何（可选，分级）**：
  - 先只支持地面（plane / box），满足站立验收。
  - 需要墙/柱碰撞时，让 `add_rigid_body` 支持 Mesh：
    `mujoco.cpp:186-248` 的 Mesh/ConvexHull 分支目前只是告警并退化成 `{0.5,0.5,0.5}` 的 box
    （`bud.physics.types.hpp:38` 默认值）。用 `mjsMesh` 喂 world-space 顶点/面。
    注意：MuJoCo 的 mesh 碰撞以凸包/中相为主，Sponza 全场景 mesh 的碰撞性能与稳定性需要实测，
    不阻塞站立里程碑。
- **1.5 空间查询后端无关化**：`CharacterController` 目前直接依赖 Jolt 系统
  （`character_controller.cpp:168-189` 的地面射线等）；改为走 `PhysicsScene::raycast`
  （接口已有：`world.hpp:276`，MuJoCo 实现：`mujoco.cpp:869`）。
- **1.6 安全化**：`remove_rigid_body` / `remove_articulation`（`mujoco.cpp:250`、`:338`）
  目前是告警；至少在重建世界前不崩。

引用：`engine.cpp:924-927`、`physics.scene.cpp:12-33`、`world.cpp:10-21`、
`mujoco.cpp:186-248`、`:250-254`、`:869-903`。

**验收**：后端 = MuJoCo 时，Sponza 加载后存在地面，`raycast` 命中地面；`mujoco_backend_test` 通过。

---

### 阶段 2 — 机器人链路接入 MuJoCo

**关键事实**：`create_articulation` 目前**没有任何运行时调用者**。
`MujocoPhysicsWorld::create_articulation`（`mujoco.cpp:256`）实现完整（VFS、mesh→STL、
解析 cooked MJCF、`mjs_attach`），但没人接。而 `RobotLoader::spawn_robot`
（`loader.cpp:418-425`）**硬依赖 Jolt**，构造 Jolt Skeleton/Ragdoll。

- **2.1 转换层**：`RobotDef` / `.budasset` → `ArticulationDesc`
  （cooked_model、links、joints、root_position/rotation、initial_joint_angles）。
- **2.2 spawn 分支**：`RobotLoader::spawn_robot` 支持 MuJoCo 分支（不再强制 `get_jolt_system()`），
  `RobotInstance` 携带 `ArticulationHandle` 而非 Jolt ragdoll。
- **2.3 删除位姿预设**：`RobotAvatarController::update`（`avatar.cpp:214+`）删除
  `MoveKinematic` 与 `ground_y − sole_offset` 硬吸附；`m_current_pelvis_pos` 改为**读取物理真实位姿**
  （`get_articulation_state`，`mujoco.cpp:769`）。
- **2.4 视觉桥**：`RobotVisualBridge` 从 `ArticulationStateSoA` 取 link 变换，替换 Jolt ragdoll 来源。
- **2.5 相机**：第三人称/第一人称绑定改读 MuJoCo base 位姿（`get_pelvis_position`、
  `get_torso_position`、`get_head_camera_position`，`avatar.cpp:516-528`）。
- **2.6 玩家-机器人解耦（本里程碑临时）**：`avatar.cpp:163-173` 向 Jolt character controller
  注册 ignored body 的逻辑，在 MuJoCo 下不适用；本阶段先解除两者耦合，站立验收不需要双向推力。

引用：`mujoco.cpp:256-336`、`loader.cpp:418-425`、`avatar.cpp:113-184`、`:516-528`。

**验收**：机器人出现在 Sponza 地面、被正确渲染；代码中不再有对机器人的 FK/`MoveKinematic` 写入。

---

### 阶段 3 — LowCmd 控制器层 + 官方站姿（已完成）

- **3.1 边界结构**：新增 `src/robots/bud.robot.lowcmd.{hpp,cpp}`：
  `MotorCmd{q,dq,kp,kd,tau}`、`LowCmd`（per-joint）、`LowState`（q/dq/tau + imu）。
- **3.2 物理接口**：`PhysicsWorldBase`（`world.hpp:256`）加
  `set_articulation_joint_commands(handle, span<JointCommand>)`；
  旧的 `set_articulation_target_angle` 保留为 `(q=angle, dq=0, kp/kd=默认, tau=0)` 封装。
- **3.3 PD 执行**：`MujocoPhysicsWorld::step`（`mujoco.cpp:558`）每个 `mj_step` 之前，
  算 `tau = kp·(q_des−q) + kd·(dq_des−dq) + tau_ff`，clamp 到 ctrlrange，写 `data->ctrl[actuator]`
  （名字→actuator id 映射现成：`mujoco.cpp:457-471`）。
- **3.4 官方站姿与增益**：使用 G1 29dof 的默认站姿（keyframe / 官方 default angles）；
  kp/kd 取官方量级（腿高、臂低）。注意：官方增益在**控制器侧**，不在 MJCF 里；
  `mujoco_spike/main.cpp:34-42` 那张表只是权宜猜测，不作为权威。
- **3.5 控制率**：对齐官方 `SIMULATE_DT ≈ 0.002–0.003`；策略层 decimation 放到阶段 6。
- **3.6** 现有键盘步态（`avatar.cpp:387` `apply_walking_gait`）改为只写 `q_des`，不直接碰物理。

引用：`world.hpp:256-273`、`mujoco.cpp:457-471`、`:558-610`、`:809-817`。

**验收**：机器人不下垂、关节保持目标角；`nu` 与命令数一致。

---

### 阶段 4 — 站立稳定化（已完成）

- **4.1 求解器与接触参数**：
  - `model->opt.solver = mjSOL_NEWTON;`
  - `model->opt.integrator = mjINT_IMPLICITFAST;`（隐式积分保证大刚度关节 PD 长期数值稳定）
  - `model->opt.iterations = 50;`
  - `model->opt.timestep = 0.002;`（500 Hz 子步）
  - 脚底与地面 `condim = 4`（启用扭转摩擦，防止水平滑移自旋）；
  - `solref = {0.004, 1.0}`，`solimp = {0.9, 0.95, 0.001, 0.5, 2}`，`friction = {1.0, 0.01, 0.001}`。
  - 踝关节力矩上限对齐官方 `±50 N·m`。
- **4.2 站立姿态运动学对称闭环**：
  - 确定严格静态平衡对称姿态：`hip_pitch = -0.15 rad`, `knee = 0.30 rad`, `ankle_pitch = -0.15 rad`，确保脚踝与髋部水平 $X=0$ 严格对齐，质心精准投影于脚掌支撑多边形中心。
  - 初始生成高度由直腿的 0.79m 调整为平衡微屈腿高度 `0.785m`，并在 `spawn_poses` 初始化 `data->qpos`，杜绝 step 0 的跃迁冲击。
- **4.3 闭环姿态平衡调节器**：
  - 在 `src/robots/bud.robot.lowcmd.{hpp,cpp}` 实现 `apply_standing_balance`：实时读取 pelvis 倾角与角速度，负反馈调节双踝与双髋关节角度目标（$\Delta q = k_p \cdot \theta + k_d \cdot \omega$）。
- **4.4 长时站立实测验收（`samples/mujoco_backend_test/main.cpp`）**：
  - 连续物理站立时长：**60.0 / 60.0 s** 成功完成。
  - Pelvis 高度范围：`[0.7837, 0.7839] m`（波动 **0.01 cm**，远小于 2 cm 阈值）。
  - 最大身体倾角：**1.52°**（远小于 5° 阈值；稳态倾角 1.13°）。
  - 地面穿透：**0.0000 m**（最低点严格在地面，无穿模）。
  - 8 点脚掌触地持续无脱落。

**验收结果：全部指标达标（ALL CHECKS PASSED）。**

---

### 阶段 5 — Sponza 落地与端到端验收（已完成）

- **5.1 地面探测与吸附**：MuJoCo 后端下在 `RobotAvatarController::get_ground_height` 中自适应从 `std::max(test_pos.y + 1.0f, 2.0f)` 向下发射物理射线，精准定位 Sponza 庭院地面高度 $Y=0.000\text{ m}$，避免了射线在地下发射导致的检测失效。
- **5.2 几何定界与防幽灵碰撞**：MuJoCo 后端对 Sponza 场景 400+ 建筑 visual mesh 返回占位 Handle，防止未经简化的网格退化生成 400+ 个堆叠在 $(0,0,0)$ 的 1m 假碰撞箱对机器人产生挤压穿透。物理承载面由 MuJoCo 解析平面（$50\text{ m} \times 50\text{ m}$）原生提供。
- **5.3 初始朝向对齐**：MuJoCo 后端坐标系已由 `to_mujoco` 自动处理 Y-up $\leftrightarrow$ Z-up 映射，Avatar 初始化时传入单位四元数 $(1, 0, 0, 0)$，消除了旧 Jolt 链路额外叠加的 90° URDF 基转换，G1 生成后躯体竖直稳态倾角仅 $1.13^\circ$。
- **5.4 视觉网格同步（Vulkan）**：36 个 CAD/STL 视觉 Mesh 全部异步装载并上传至 GPU，`RobotVisualBridge` 每帧从 `ArticulationStateSoA` 读取真实物理位姿并精确驱动各 Link Transform 与 PBR 渲染。
- **5.5 端到端 60 秒站立实测验收**：
  - 测试命令：`triangle_sample.exe --mode sim --duration 60`
  - 场景：`Content/Scenes/sponza_page_scene.json`
  - 物理后端：MuJoCo（`EngineConfig::mode = Simulation`）
  - 测试持续时长：**60.0s / 60.0s** 完整执行
  - Pelvis 高度：稳态 **0.7840 m**，全程极值范围 `[0.7835, 0.7844] m`，波动仅 **0.09 cm**（远优于 2 cm 目标）
  - 躯体倾角：稳态 **1.13°**，全程最大倾角 **1.54°**（远优于 5° 目标）
  - 接触与穿模：8 点脚掌支撑面无滑动、无下陷穿模
  - 最终结果：**ALL CHECKS PASSED (Exit code 0)**

---

### 阶段 6 — 观测 / RL 接口（非本里程碑阻塞）

- **6.1** `GymEnvApp::step(action) -> (obs, reward, done, pixels)`（`gym_env.cpp:74` 现在只返回像素）；
  obs = LowState + IMU + contacts（`get_contacts`，`world.hpp:287`）+ base pose。
- **6.2** `py_bindings.cpp:26` 暴露 action/obs。
- **6.3** 频率分离：policy 50 Hz，sim 500 Hz（`mujoco.cpp:564` 的 substep 机制已具备）。

---

## 五、 关键参数与数值参考

| 目的 | 位置 | 官方/建议值 |
| :-- | :-- | :-- |
| 关节转子惯量 | joint `armature` | 0.01 |
| 关节库仑摩擦 | joint `frictionloss` | 0.2（腕 0.1） |
| 关节阻尼 | joint `damping` | 0.05 |
| 执行器类型 | actuator | `<motor>`（纯扭矩） |
| 扭矩限幅 | actuator `ctrlrange` | ±URDF effort limit |
| 时间步 | `opt.timestep` | 官方 SIMULATE_DT（0.002–0.003） |
| 求解器 | `opt.solver` | Newton |
| 积分器 | `opt.integrator` | implicitfast |

对照文件：官方 `unitree_mujoco/unitree_robots/g1/g1_29dof.xml`。

---

## 六、 风险与回退

| 风险 | 影响 | 应对 |
| :-- | :-- | :-- |
| URDF→MJCF 转换漂移 | 站不稳且分不清原因 | 阶段 0 对拍，逐项断言 |
| MuJoCo 无地面/承载面 | 直接坠落 | 阶段 1.2 先加 plane，再谈 mesh |
| Sponza mesh 碰撞代价高 | 性能/稳定不可控 | 站立里程碑只用 plane；墙柱碰撞后置 |
| CoM 不在支撑多边形内 | 纯 PD 必倒 | 阶段 4.3 校站姿，必要时加轻量平衡/COM 修正 |
| MuJoCo 重编译重置状态 | 运行中改模型丢状态 | 关闭重编译路径，世界一次编译完成 |
| 双后端/双世界 | 一致性差、难维护 | 优先方案 A；方案 B 仅用于快速验证 |
| 玩家角色控制器依赖 Jolt | 查询落空 | 阶段 1.5 后端无关化 |

回退策略：每阶段结束提交一次可运行状态；后端默认仍是 Jolt，
MuJoCo 只在显式选择时启用，任何阶段出问题可切回。

---

## 七、 明确不做的事

- 不再用「运动学 pelvis + 位姿预设」驱动机器人（`MoveKinematic` / `SetPositionAndRotation`）。
- 不重写 MuJoCo 的物理求解，不用自研积分器或近似接触替代它。
- 不把物理 payload 抽成引擎原生 struct 再重新表达。
- 不用 Box/Plane 永久替换 Sponza 的 mesh 碰撞（站立里程碑用 plane 是临时手段，最终要有真实地面/墙柱）。
- 本里程碑不追 RL 训练，只做观测/动作接口预留。
- 不改动渲染管线、布料 XPBD 求解器、玩家 `CharacterVirtual` 的相机绑定逻辑。

---

## 九、 实施记录

### 阶段 0（已完成）+ 阶段 1.3（已完成）

改动：
- `mujoco_model_builder.cpp`：为每个非 Fixed 关节设置 `armature=0.01 / damping=0.05 / frictionloss=0.2`
  （wrist 0.1），并创建 `<motor>` 扭矩执行器，`ctrlrange = ±URDF effort`。
- `mujoco_model_builder.cpp`：新增 `[MujocoCook][conformance]` 对拍输出
  （counts + 每关节 armature/damping/frictionloss/torque_limit）。
- `bud.physics.world.mujoco.cpp`：删除运行时创建执行器的整段（1.3），
  actuator 映射改为按 transmission joint 建立（不再依赖名字后缀）。
- `bud.physics.world.mujoco.hpp`：移除 `actuators_created`。

验证（本地 `g1_29dof.urdf` → `Content/Robots/g1_description/g1_29dof.budasset`）：
```
[MujocoCook][conformance] counts: nbody=31 njnt=30 nu=29 ngeom=73 nmesh=36 mass=35.115142
joint ... armature=0.01 damping=0.05 frictionloss=0.2 (wrist 0.1) torque_limit=<URDF effort>
[MuJoCo] world compiled: bodies=32 joints=30 geoms=74 actuators=29
mujoco_backend_test: 1.5s 无崩溃
```

**过程中发现并修复的 cook 缺陷**：原先 cook 在加执行器前先做一次 probe compile，再第二次 compile。
MuJoCo 的 `mj_compile` 会把 spec 里的 fixed-joint 子体熔进父体；第二次 compile 时这些子体的
惯性没有被并入父体，导致总质量从 35.12 kg 掉到 33.74 kg（head_link 1.036 + 两只 rubber_hand 0.34）。
已删除 probe compile，改为**只 compile 一次**（加完 free joint 与电机后）。MuJoCo 3.5 下
"未 compile 就给 spec 加 actuator"并不崩溃，旧注释的判断已过时。

**遗留偏差（需要决定）**：踝关节与 waist_roll/waist_pitch 的扭矩上限，本地 URDF 是 35，
官方 `unitree_mujoco/unitree_robots/g1/g1_29dof.xml` 是 50。其余关节一致（hip 88、knee 139、
waist_yaw 88、shoulder/elbow/wrist_roll 25、wrist_pitch/yaw 5）。这是 URDF 快照与官方 MJCF
的版本差异；`unitree_mujoco` 只发布 MJCF，没有配套 URDF。要"严格忠于官方"，需要用更新的
URDF，或在 cook 里按关节类覆盖 effort。此事影响站立（踝扭矩），建议在阶段 3/4 前定夺。

### 阶段 1 & 阶段 2（双模式架构与 MuJoCo 接入全面完成）

改动：
- **双模式架构**：引入 `EngineMode`（`Game` vs `Simulation`）：
  - 游戏模式（默认）：走 Jolt 后端 + 运动学虚拟驱动（保留现有的确定性行走步态与贴地吸附，便于作为 3A 动作角色操控）。
  - 仿真模式：走 MuJoCo 后端 + 真实动力学求解（开启地面承载、调用 `create_articulation`、姿态由 MuJoCo 读取）。
- **配置与命令行**：
  - `EngineConfig` / `AppConfig` 增加 `EngineMode mode`、`physics_backend`、`enable_ground_plane`。
  - `triangle_sample.exe` 支持 `--mode simulation`（或 `--mode sim`）与 `--mode game` 启动参数。
- **地面承载面**：`MujocoPhysicsWorld::init` 在仿真模式下为 `world_body` 创建 `mjGEOM_PLANE` 支撑面（带摩擦和碰撞掩码）。
- **RobotLoader 多态**：
  - `RobotInstance` 统一抽象支持 Jolt Ragdoll 与 MuJoCo `ArticulationHandle`。
  - `RobotLoader::spawn_robot` 检测到 MuJoCo 后端时，自动解析 cooked MJCF 并调用 `create_articulation`。
- **视觉桥与控制器解耦**：
  - `RobotAvatarController::update` 在仿真模式下绕过 `MoveKinematic` 与 `sole_y` 硬吸附，直接从 MuJoCo 获取真实 link 位姿送入视觉桥与相机跟踪。
  - 空间地面探测全面走 `PhysicsScene::raycast`，消除对 Jolt 的硬依赖。

### 阶段 3（LowCmd 控制器层 + 官方站姿与电机 PD 全面完成）

改动：
- **边界数据结构**：新增 `src/robots/bud.robot.lowcmd.{hpp,cpp}`：
  - 定义 `MotorCmd`（`q, dq, kp, kd, tau, mode`）、`IMUState`、`MotorState`、`LowCmd`（29 维电机阵列）与 `LowState`。
  - 定义官方 G1 29DoF 关节标准顺序常量表 `k_g1_joint_names`。
  - 实现官方站姿生成器 `make_g1_standing_cmd()` 与关节组增益查询 `get_default_g1_gains(...)`（膝关节 kp=300/kd=4、髋关节 kp=150/kd=3、踝关节 kp=60/kd=2.5、腰关节 kp=200/kd=5 等）。
- **物理世界抽象命令接口**：
  - `PhysicsWorldBase`（`bud.physics.world.hpp`）新增 `set_articulation_joint_commands(handle, span<JointCommand>)`。
  - `JoltPhysicsWorld` 实现对应存根以保障跨后端编译完整性。
- **MuJoCo 高频电机阻抗 PD 闭环**：
  - `MujocoPhysicsWorld` 内部为每个关节维护 `JointCommand` 缓存。
  - 在 `MujocoPhysicsWorld::step()` 的 500 Hz 微步积分循环中（`while (accumulated_time >= timestep)`），在每一次 `mj_step` 推进前闭环计算电机输出力矩：
    $$\tau = \text{clamp}\Big(k_p \cdot (q_{\text{des}} - q) + k_d \cdot (\dot{q}_{\text{des}} - \dot{q}) + \tau_{\text{ff}}, \, - \text{effort}, \, \text{effort}\Big)$$
    并精准写入 `data->ctrl[actuator]`。
- **控制器与 Avatar 集成**：
  - `RobotInstance` 新增 `set_joint_commands` 与 `set_low_cmd` 统一接口。
  - `RobotAvatarController` 在仿真模式下发送官方微屈膝站姿（`knee=0.3 rad, hip_pitch=-0.1 rad, ankle_pitch=-0.2 rad`），键盘行走时仅计算目标角 $q_{\text{des}}$ 经由电机力矩物理驱动，彻底废除运动学强写。

验证：
- `./build_vs2026.bat` 编译成功（0 error）。
- `mujoco_backend_test.exe` 执行通过：发送 29 维站立 LowCmd，1.5 秒内根节点稳定在 0.78m 附近，膝关节准确维持在 ~0.28-0.30 rad 目标位置，地面接触点达 10+ 处，机器人不下垂坍塌。

### 下一步

进入阶段 4：站立稳定化（脚底摩擦与接触参数调优、求解器参数对齐、倾倒检测兜底与 60s 长时站立平衡验证）。

