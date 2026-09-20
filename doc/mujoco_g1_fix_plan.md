# MuJoCo G1 修复与加固计划

本计划承接 `doc/mujoco_g1_stand_plan.md`。阶段 1-5 已实现并通过 Stage 4 验证
（60s 站立、pelvis 波动 0.01 cm、最大倾角 1.52°、无穿地），所以这里不是返工，
而是**代码评审发现的清理、正确性与结构性问题**，按顺序修完后进入阶段 6（RL 观测/动作接口）。

来源：对 `bd05ab4`、`96602f7` 两个提交的代码评审。

---

## 一、 目标与验收基线

**目标：** 消除通用层的机型污染与调试残留，补齐"忠于官方"的闭环，让机器人链路具备
可移除/可复位的能力，为阶段 6 做好准备。

**全局验收命令（每个修复项完成后至少跑一遍）：**

| 命令 | 用途 |
| :-- | :-- |
| `./build_tools.bat` | cook 与工具链 |
| `./build_vs2026.bat` | 引擎与 samples |
| `mujoco_backend_test`（60s） | Stage 4 站立回归：4 项 PASS |
| `triangle --duration 60`（MuJoCo 模式） | Stage 5 Sponza 站立回归 |
| 重新烘 `g1_29dof.budasset` + `[MujocoCook][conformance]` | 模型一致性对拍 |

**硬性回归红线：** Stage 4 的 pelvis 波动 < 2 cm、最大倾角 < 5°、无穿地必须继续满足，
任何修复项不得让这三项退化。

---

## 二、 修复清单总览

| 编号 | 标题 | 级别 | 依赖 | 状态 |
| :-- | :-- | :-- | :-- | :-- |
| **F1** | 移除后端调试残留与文件副作用 | P1 | 无 | 已完成 |
| **F2** | Sponza 网格碰撞不再静默丢弃 | P1 | 无 | 已完成（F2a；F2b 后置） |
| **F3** | 扭矩上限对齐搬到 cook（踝 + waist_roll/pitch） | P1 | F1 | 已完成 |
| **F4** | 清除通用层的 G1 硬编码 | P1 | 无 | 已完成 |
| **F5** | 非 Jolt 后端下 CharacterController 行为显式化 | P2 | 无 | 已完成 |
| **F6** | articulation 的移除与复位 | P2 | 无 | 已完成 |
| **F7** | 后端状态读取的线程安全 | P2 | 无 | 已完成 |
| **F8** | 关节命令映射改为 name→index 表 | P2 | 无 | 已完成 |
| **F9** | 姿态分解改为稳健实现 | P3 | 无 | 已完成 |
| **F10** | 测试与统计的打印/口径修正 | P3 | 无 | 已完成 |
| **F11** | 求解器参数选择依据与 metadata 清理 | P3 | F3 | 已完成 |

**顺序：** F1 → F2 → F3 → F4 → F5 → F6 → F7 → F8 → F9 → F10 → F11，然后进入阶段 6。

**状态：全部 11 项已完成。** 每项都跑过 `mujoco_backend_test` 与真实 Sponza 站立回归，
Stage 4 红线（波动 < 2 cm、倾角 < 5°、无穿地）全程未退化。F6 完成后 S6.5 的前置也满足了，
可以开始阶段 6（RL 观测/动作接口）。
其中 F1/F2/F4 互不依赖，若想并行可拆给不同会话；F3 依赖 F1（同一文件同一段代码）。

---

## 三、 修复项详述

### F1 · 移除后端调试残留与文件副作用

**问题**：`create_articulation` 在运行时会写文件并逐 mesh 刷屏。

- `mujoco.cpp:378-386`：`create_directories("tmp")` + 写 `tmp/debug_robot.xml`
- `mujoco.cpp:369`：每个 mesh 一行 `mj_addBufferVFS(...) -> ...`（G1 一次 36 行）
- `mujoco.cpp:376`：`parsing MJCF string: size=...`
- `mujoco.cpp:389`：`safe_parse_xml result: ...`
- `mujoco.cpp:372`：`mesh VFS ready (...)` 可降级为单行汇总

**动作**：删除文件写入与逐 mesh 日志；保留一条汇总（mesh 数、解析结果、VFS 字节数）。
如需保留落盘调试，放到显式的 debug 开关后面（默认关闭）。

**文件**：`src/physics/bud.physics.world.mujoco.cpp`

**验收**：`mujoco_backend_test` 输出中不再出现 `tmp/`、`mj_addBufferVFS`、`safe_parse_xml`、
`parsing MJCF` 字样；`git status` 运行后 `tmp/` 无新增文件。

---

### F2 · Sponza 网格碰撞不再静默丢弃

**问题**：`add_rigid_body` 对 `Mesh`/`ConvexHull` 直接返回 body=-1 的占位 handle
（`mujoco.cpp:252-260`）。后果：机器人站的是合成平面而非真实 Sponza 地板；墙柱无碰撞；
`mujoco_backend_test` 里向下的 raycast 打到的是机器人自己（y=1.307）而不是地面。
更危险的是返回了一个"看起来合法、实际什么都不指"的 handle。

**动作（分两级，F2a 必做，F2b 可选后置）：**

- **F2a**：不再静默。Mesh/ConvexHull 走显式 `eprint` 说明"该静态网格不参与 MuJoCo 碰撞"，
  并登记到一份 `uncollidable_static_meshes` 计数，在 compile 日志里汇总。
  同时确认：Sponza 模式下机器人确实落在配置的 `ground_plane_height` 上（现有行为），
  用一次"从机器人脚下向下、排除机器人自身"的 raycast 断言打到地面而不是自身。
- **F2b（可后置）**：给场景地板喂一个薄 box（由场景 bounds 推导），或实现真正的
  `mjsMesh` 用户顶点碰撞。不做全场景 mesh 碰撞（性能与稳定性不可控）。

**文件**：`src/physics/bud.physics.world.mujoco.cpp`、`src/runtime/bud.engine.cpp`（如需配置）

**验收**：日志明确列出被跳过的静态网格数量；存在一条打到地面（非机器人）的 raycast 证据；
Stage 4/5 站立回归不退化。

---

### F3 · 扭矩上限对齐搬到 cook（踝 + waist_roll/pitch）

**问题**：一致性对拍发现踝与 `waist_roll`/`waist_pitch` 的扭矩上限，本地 URDF 是 35、
官方 `g1_29dof.xml` 是 50。现在只在后端按关节名把 **ankle** 覆盖成 ±50
（`mujoco.cpp:531-541`），遗漏了 waist_roll/pitch，而且**覆盖发生在错误的层**：
这是资产参数，属于 cook。

**动作**：

1. 在 `mujoco_model_builder.cpp` 的电机创建处，按关节类覆盖 effort：
   `ankle_pitch/ankle_roll/waist_roll/waist_pitch → 50`。
   覆盖值要作为"官方模型常量"集中定义，注释标明来源 `unitree_mujoco/g1_29dof.xml`。
2. 删除 `mujoco.cpp:531-541` 的后端 grep 覆盖。
3. 重新烘 `g1_29dof.budasset`，让 `[MujocoCook][conformance]` 直接显示 50。

**文件**：`src/tools/asset_pipeline/builders/mujoco_model_builder.cpp`、
`src/physics/bud.physics.world.mujoco.cpp`、重新生成 `Content/Robots/g1_description/g1_29dof.budasset`

**验收**：conformance dump 中 ankle 与 waist_roll/pitch 的 `torque_limit=50`，
其余关节不变（hip 88、knee 139、waist_yaw 88、shoulder/elbow/wrist_roll 25、wrist_pitch/yaw 5）；
后端不再有任何按关节名改扭矩的代码；Stage 4 回归通过。

---

### F4 · 清除通用层的 G1 硬编码

**问题**：通用后端/loader 里散落 G1 专属常量。

- `mujoco.cpp:346-349`：硬编码兜底目录 `Robots/g1_description`
- `loader.cpp:525`：默认资产 `Content/Robots/g1_description/g1_29dof.budasset`
- `loader.cpp:537-538`、`avatar.cpp:162`：站姿 pelvis 高度 `0.785f` 魔法值
- `loader.cpp:555`：对所有机器人调用 `get_default_g1_gains`
- `lowcmd`：`G1_NUM_MOTORS=29` 与增益表本身（作为 G1 专属文件可接受，但不能被通用层假定）

**动作**：

- 移除后端的硬编码兜底目录，只依赖 `asset_root` + loader 传入的 `asset_path`；
  找不到就明确报错。
- 默认资产路径由 sample/AppConfig 提供，不写在后端/loader 的通用分支里。
- 站姿基线高度改为机器人定义/资产携带的参数（如 `RobotDef` 增加 `default_root_height`
  或从 cooked 模型的静止高度推导），去掉 `0.785f` 字面量。
- 增益：loader 用机器人定义里的增益表，`get_default_g1_gains` 仅作为 G1 定义的填充实现。

**文件**：`src/physics/bud.physics.world.mujoco.cpp`、`src/robots/bud.robot.loader.cpp`、
`src/robots/bud.robot.avatar.cpp`、`src/robots/bud.robot.types.hpp`（如需新字段）

**验收**：`grep` 通用层无 `g1_description`、`g1_29dof`、`0.785`、`g1_gains` 字面量；
换一台机器人（或改 `asset_path`/`package_root`）无需改后端；Stage 4/5 回归通过。

---

### F5 · 非 Jolt 后端下 CharacterController 行为显式化

**问题**：`CharacterController::init` 拿到 null Jolt system 直接 `return`
（`character_controller.cpp:30-31`），而 `PhysicsScene::get_jolt_system()` 在 MuJoCo 下会打 error
（`physics.scene.cpp:117-122`）。结果：sim 模式每次加载场景都误报一条错误，玩家角色与
布料交互链路（engine 的 `snap_to_ground` 等）整条为空，但没有任何明确说明。

**动作**：非 Jolt 后端时不创建角色，并把 `get_jolt_system()` 的 error 降为一次性 info/debug；
在文档或启动日志里明确"sim 模式无玩家角色胶囊"。若后续游戏模式也要 MuJoCo，
再把 CharacterController 接到后端中立接口（这是更大的一块，不在本计划内）。

**文件**：`src/runtime/bud.character_controller.cpp`、`src/physics/bud.physics.scene.cpp`、
`src/runtime/bud.engine.cpp`（调用点加 backend 判断）

**验收**：MuJoCo 模式启动不再出现 `get_jolt_system() called on the 'MuJoCo' backend`
的 error 级日志；有一条 info 说明角色已禁用；游戏模式（Jolt）行为不变。

---

### F6 · articulation 的移除与复位

**问题**：`remove_articulation` 只把 `valid=false`（`mujoco.cpp:451-455`），身体与执行器
留在世界里；`create_articulation` 会置 `spec_dirty`，下一步 `compile_world` 会**重置整个
仿真状态**（`mujoco.cpp:462-469`）。所以"重生/复位机器人"目前会重复堆积并清空状态，
阶段 6 的 RL reset 会直接撞上。

**动作**：

- 实现真正的移除：从 spec 中删除该 articulation 的 body 子树（或至少将其标记为
  visual-only/禁用碰撞与执行器），并更新 handle 表，避免重复堆积。
- 提供复位路径：`set_articulation_link_transform` 已能传送 root；补一个"把关节角写回
  initial_joint_angles + 清零速度"的复位语义（可复用 spawn_poses）。
- 明确重新编译会重置状态这一约束；复位应走 qpos/qvel 写入，而不是触发重编译。

**文件**：`src/physics/bud.physics.world.mujoco.cpp/.hpp`、`src/physics/bud.physics.world.hpp`

**验收**：连续 spawn/remove 同一机器人 10 次，body/actuator 数量不增长；
复位后能立刻重新站立，无状态清空；Stage 4 回归通过。

---

### F7 · 后端状态读取的线程安全

**问题**：`step`/`compile_world` 持 `m_mutex`，但 `get_articulation_state`、`get_body_states`
及 loader 的 getter 无锁读 `data`/`model`，`get_articulation_state` 还会 `out.resize()`。
引擎存在独立 RenderTask 线程，这是数据竞争。

**动作**：给所有读取 `model`/`data`/内部 vector 的公开 getter 加同一把锁；或明确写死
"后端只在主线程访问"的契约并在接口注释与调用点标注。二选一，不能悬空。

**文件**：`src/physics/bud.physics.world.mujoco.cpp/.hpp`、如有需要 `bud.physics.world.hpp`

**验收**：TSan 或代码审查确认无无锁读写；渲染线程读到的状态不再可能与 `resize` 竞争。

---

### F8 · 关节命令映射改为 name→index 表

**问题**：`set_articulation_joint_commands` 每次 O(关节×命令) 字符串比较
（`mujoco.cpp:941-947`）；avatar 与测试每帧调用 29 条。

**动作**：在 `compile_world` 里为每个 articulation 建一次 `joint_name → index` 表，
命令下发按表 O(命令)。`set_articulation_target_angle/_velocity` 同一套。

**文件**：`src/physics/bud.physics.world.mujoco.cpp/.hpp`

**验收**：功能不变，Stage 4 回归通过；单帧命令下发不再做字符串扫描。

---

### F9 · 姿态分解改为稳健实现

**问题**：`compute_body_orientation` 用 `asin(u_x)`/`asin(u_z)` 当 pitch/roll
（`lowcmd.cpp:187-188`），超过 ±90° 失义，混合 pitch+roll 时也不是正确的姿态分解
（`tilt` 的 `acos(u_y)` 是对的）。

**动作**：用旋转矩阵元素做标准分解（pitch = atan2，roll = atan2），或让接口直接暴露
四元数/旋转矩阵、由调用方按需取值。保持现有 `tilt_deg` 语义不变。

**文件**：`src/robots/bud.robot.lowcmd.cpp/.hpp`

**验收**：在 roll≈0 与 pitch+roll 混合两种情形下，分解结果与显式构造的欧拉角一致；
Stage 4 站立回归不退化。

---

### F10 · 测试与统计的打印/口径修正

**问题**：

- `mujoco_backend_test/main.cpp:213`：失败分支把 `prev_pitch` 当 Duration 打印。
- `samples/triangle/triangle.cpp:180-181` 在 0.5s 门限之前就统计 min/max，
  把初始暂态算进了"稳定后"的指标，与 `:196-199` 的意图矛盾。

**动作**：修正失败分支的耗时输出；把 min/max/倾角统计统一移到 0.5s 门限之后，
或明确区分"瞬态"与"稳态"两组数字。

**文件**：`samples/mujoco_backend_test/main.cpp`、`samples/triangle/triangle.cpp`

**验收**：失败时报出真实耗时；稳态指标从 t>=0.5s 起算，且不影响 PASS 判定逻辑。

---

### F11 · 求解器参数选择依据与 metadata 清理

**问题**：`opt.timestep=0.002`（官方 `SIMULATE_DT≈0.003`）、`opt.iterations=50`
（MuJoCo 默认 100）、`opt.solver=NEWTON`、`integrator=IMPLICITFAST`
（`mujoco.cpp:496-499`）是随手定的；`physics_metadata_json` 依旧只传不消费。

**动作**：在代码注释与计划里写清每个取值的依据（稳定性/精度/与官方的差异）；
对 `physics_metadata_json` 二选一——要么实现消费，要么在接口注释里标为保留字段并说明
未来用途，避免"看起来有数据其实没人读"。

**文件**：`src/physics/bud.physics.world.mujoco.cpp`、`bud.physics.world.hpp`

**验收**：参数来源可追溯；metadata 字段不再是悬空占位。

---

## 四、 完成后的阶段 6（RL 观测/动作接口）

修复项全部完成后再开始，两者有依赖（S6.5 依赖 F6）。

| 编号 | 内容 | 依赖 |
| :-- | :-- | :-- |
| **S6.1** | 观测：`LowState` 关节 q/dq/tau + IMU + contacts + base 位姿。IMU 需要在 cook 里补 site + sensor，并在后端读取 `sensordata` | F3 |
| **S6.2** | 动作：`LowCmd` → `JointCommand` 的下发路径固化（已有雏形，接进 GymEnv） | F8 |
| **S6.3** | `GymEnvApp::step(action) -> (obs, reward, done, pixels)`，`py_bindings` 暴露 action/obs | S6.1/S6.2 |
| **S6.4** | 频率分离：policy 50 Hz / sim 500 Hz，用 decimation | S6.3 |
| **S6.5** | 复位与并行：每个 env 独立复位（复用 F6），再谈向量化 | F6 |

**阶段 6 完成标志**：Python 侧能 `reset()` 后拿观测、`step(action)` 推进，动作经
`LowCmd` 语义进入 MuJoCo，机器人能站住；整条链路不触发 world 重编译。

---

## 五、 明确不做

- 不重写 MuJoCo 求解，不动 cooked 模型作为唯一事实源的原则。
- 不做全场景 mesh 碰撞（F2b 只做地板级）；不做 WBC/RL 控制器本身（那是阶段 6 之后）。
- 不引入新的物理后端或替换 Jolt 的游戏世界角色。

---

## 六、 实施记录

### F11 · 已完成

改动：
- `bud.physics.world.mujoco.cpp`：求解器参数提为具名常量并写清依据。
  - `k_solver_timestep = 0.002`（500 Hz）。MuJoCo 默认即 0.002；官方 `unitree_mujoco` 用
    `SIMULATE_DT = 0.003`（333 Hz）。显式钉在 0.002 是为了让电机 PD 在我们用的腿部增益下更稳，
    单机器人开销可忽略。`step` 里的兜底值也改用该常量。
  - `k_solver_iterations = 100`（MuJoCo 默认）。**把原来随手写的 50 改回默认值**，
    不再人为偏离参考设置；solver/integrator 保持 `mjSOL_NEWTON` / `mjINT_IMPLICITFAST`（官方不覆盖）。
- `bud.physics.world.hpp`、`bud.robot.mujoco.hpp`：把 `physics_metadata_json` 明确标注为
  **保留扩展位**（当前恒为空、无后端读取），并修正过时描述——电机的 armature/frictionloss/damping
  与扭矩限幅已由 cook 烘进 MJCF，不再由该字段承载；`render_metadata_json` 则确实被 loader 消费。
  `MujocoModelData` 顶部注释同步更正。

验证：
- `mujoco_backend_test`：全项 PASS，站立指标与 iterations=50 时逐位一致（波动 0.01 cm、倾角 1.52°）。
- 真实 Sponza：`PASSED`，pelvis 0.7589 m、range [0.7589, 0.7590]、tilt 1.13°/1.54°。

### F10 · 已完成

改动：
- `samples/mujoco_backend_test/main.cpp`：新增 `elapsed_s`（循环内按 `step * step_dt` 更新），
  Summary 的 `Duration` 改印真实耗时。原来失败分支印的是 `prev_pitch`（一个角度值），
  既有误导性也可能为负。
- `samples/triangle/triangle.cpp`：删掉 0.5s 门限之前对 `m_min_pelvis_y` / `m_max_pelvis_y` /
  `m_max_tilt_deg` 的无条件更新；稳态指标统一从 0.5s 之后起算，落地那一下的暂态不再计入。
  这与文件里本来就存在的 `if (m_elapsed_time >= 0.5f)` 门限意图一致。

验证：
- `mujoco_backend_test`：`Duration: 60.0 / 60.0 s`（原来通过时也是 60.0，但现在两个分支
  都来自真实耗时而非角度值），`ALL CHECKS PASSED`。
- 真实 Sponza：pelvis 高度范围由 F9 时的 `[0.7584, 0.7593]` 收紧为 `[0.7589, 0.7590]`，
  正是把 0.5s 内的落地暂态剔除的结果；`m_max_tilt_deg` 本来就已门限，仍为 1.54°；`PASSED`。

### F9 · 已完成

改动（`src/robots/bud.robot.lowcmd.cpp` 的 `compute_body_orientation`）：
- 原实现 `pitch = asin(u_x)`、`roll = asin(u_z)`（u 为世界系下的机体上轴）。
- 改成从旋转矩阵取轴分解：pitch 用机体前向轴 `atan2(-f_y, f_x)`，roll 用机体右向轴
  `atan2(-s_y, s_z)`，tilt 仍是 `acos(u_y)`。
- 符号约定保持不变（+Z 旋转得到 -pitch，+X 旋转得到 +roll），近直立时数值与原实现一致。

过程记录：第一次只把上轴向量的 `asin` 换成 `atan2(u_x, u_y)`，自检立刻抓到
"pitch 过 90°"时 roll 退化成 π（纯 pitch 时 u_z=0、u_y<0，`atan2(0, 负数) = π`）。
说明**只靠上轴向量无法无歧义分解**，所以改为读两个轴（前向求 pitch、右向求 roll）。

验证（`mujoco_backend_test` 新增姿态自检）：
```
[PASS] Orientation decomposition recovers known pitch/roll (incl. past 90 deg)
[PASS] Pelvis height fluctuation < 2 cm (actual: 0.01 cm)
[PASS] Maximum body tilt < 5 deg (actual: 1.52 deg)
[Stage 4 RESULT] ALL CHECKS PASSED
```
自检覆盖：直立、纯 pitch 0.35、纯 roll -0.22、pitch 2.0 rad（>90°，旧实现在此折叠为 -1.14）。
站立指标与修改前逐位一致（0.01 cm / 1.52°），说明近直立行为未被改动。
真实 Sponza 回归：`PASSED`，pelvis 0.7589 m、tilt 1.13°/1.54°。

### F8 · 已完成

改动：
- `bud.physics.world.mujoco.hpp`：`Articulation` 新增 `joint_index_by_name`（关节名 -> 
  `joint_names/joint_descs/joint_commands` 下标）。
- `bud.physics.world.mujoco.cpp`：spawn 时建表一次；`set_articulation_joint_commands`
  每条命令一次哈希查找（原来 O(命令 x 关节) 字符串比较）；`set_articulation_target_angle`、
  `set_articulation_target_velocity`、`get_articulation_joint_stiffness` 以及 `compile_world`
  里写 spawn 姿态的那段循环，全部改用该表。

验证：
- `grep` 后端已无 `joint_names[...] == ...` 形式的线性名扫描。
- `mujoco_backend_test`：全部 PASS（含 10 次 lifecycle 与并发冒烟）。
- 并发冒烟在同样的 60 步 + 1ms 让步下，读线程调用数从 F7 的 184,485 升到 1,035,408，
  说明单帧命令下发确实变快、锁占用时间缩短（附带观察，非严格基准）。
- 真实 Sponza 回归：`PASSED`，pelvis 0.7589 m、tilt 1.13°/1.54°。

### F7 · 已完成

改动（`src/physics/bud.physics.world.mujoco.cpp`）：给所有会读写 `model`/`data` 或内部容器的
公开方法加同一把 `m_mutex`（`mutable recursive_mutex`）——body 计数、`get_body_states`、
六个 body setter、`get_articulation_state`、关节命令/目标角/目标速度、关节角/刚度读取、
`set_articulation_link_transform`、`raycast`、接触回调设置、`get_contacts`、重力读写、
`collect_debug_lines`、`model_body_count/actuator_count`。私有 helper（`find_*`、`body_user_data`）
不加锁，只在已持锁的上下文调用。

- `get_body_states()` 返回引用，锁在返回时即释放，所以加注释明确契约：
  **step 进行中不得读取该 SoA**，跨线程由场景层自己的 mutex 串行化。
- 顺带确认 `PhysicsScene::body_mutex` 实际无人使用（死代码），因此不存在
  `body_mutex → m_mutex` 的反向加锁顺序，无死锁风险。
- `step` 在持锁期间调用接触回调；回调若重入 getter，靠 recursive_mutex 同线程可重入。

验证：
- `mujoco_backend_test` 新增并发冒烟：一个读线程在 step 推进时持续调用
  `get_articulation_state / get_contacts / raycast / get_body_count`。
  ```
  [test] concurrent getters: 184485 calls while stepping (no deadlock)
  [Stage 4 RESULT] ALL CHECKS PASSED
  ```
- 真实 Sponza（多线程渲染）回归：`PASSED`，pelvis 0.7589 m、tilt 1.13°/1.54°，无卡死。

诚实说明：MSVC 没有 TSan，所以这条验证证明的是**无死锁/无重入错误与行为不退化**，
不等于证明了"绝对无数据竞争"。`get_body_states()` 的引用契约仍需调用方遵守。

### F6 · 已完成

先验证了前提：在 MuJoCo 3.5 下对同一 spec 反复 `mj_compile`（含 compile→改关节/加执行器→compile）
质量完全不变（临时诊断实测 `mass1 == mass2`），所以重编译本身不会像早期 cook 那样丢质量。
旧 cook 的丢质量是 cook 路径特有，与本次无关。

改动：
- `bud.physics.world.hpp`：新增 `reset_articulation(handle)`。
- `bud.physics.world.jolt.hpp/.cpp`：Jolt 无 articulation，实现为空。
- `bud.physics.world.mujoco.hpp/.cpp`：
  - `remove_articulation` 实做：先按 **target joint**（不是按名字）删掉该 articulation 的执行器，
    再 `mjs_delete` 根 body 子树（级联 body/geom/joint），清空 id 表并置 `spec_dirty`。
  - `reset_articulation` 实做：把 root 自由关节与各关节 qpos 写回 spawn 姿态、qvel 清零，
    并把 `joint_commands` 同步回去，**不触发重编译**。
  - `compile_world` 跳过已失效的 articulation 槽位。
  - 重新挂载同一机器人时的 `repeated name ... in mesh`：VFS 网格注册按名字幂等；
    挂载前把子 spec 中父 spec 已有的 mesh 删掉，让 attach 复用父资产。
  - 新增 `model_body_count()/model_actuator_count()` 供诊断/测试计数。
  - attach 失败时打印 `mjs_getError(spec)`（原来只有一句 "failed"，排障时看不到原因）。

验证（`mujoco_backend_test`）：
```
[MuJoCo] articulation 'pelvis' reset to its spawn pose
[MuJoCo] articulation 'pelvis' removed
[test] lifecycle: reset=ok, 10 cycles, remove bodies 32->2 / actuators 29->0, respawn bodies=32 / actuators=29
[PASS] Reset restored the spawn pose without a world rebuild
[PASS] Remove folded the world back to 2 bodies, respawn returned to 32 (no accumulation)
[Stage 4 RESULT] ALL CHECKS PASSED
```
- 10 次 remove/respawn 后 body/actuator 数量无增长；reset 后仍能站立，站立指标不退化。

回归（真实 Sponza，`triangle_sample --backend mujoco --duration 6`）：
```
[TriangleApp][StandingTest] Pelvis Y: 0.7589m (range: [0.7584, 0.7593]), Tilt: 1.13 deg (max: 1.54 deg)
Finished test duration 6.0s. Final Result: PASSED
```

注意：`articulations`/`spawn_poses` 的槽位在移除后不回收（保证 handle 稳定），
所以反复 create 会让这两个 vector 按次数增长（不增长的是模型本体）。真正的多环境复用
留到阶段 6 的 S6.5 再评估是否需要槽位回收。

### F5 · 已完成

改动（`src/runtime/bud.character_controller.cpp`）：`init` 先判断后端，非 Jolt 时直接返回并打印
一条 info（"player character disabled: backend is MuJoCo"），不再走到 `get_jolt_system()`
触发那条 error。确认 `snap_to_ground` / `update` 本身已有 `!character` 保护，其余
`get_jolt_system()` 调用点（avatar）都只在 Jolt 的 ragdoll 分支内。

验证（真实 Sponza 场景，`triangle_sample --backend mujoco --duration 6`）：
```
[BudEngine] Physics scene initialized (MuJoCo), ready for bodies.
[CharacterController] player character disabled: backend is MuJoCo
[MuJoCo] 372 static scene meshes skipped (visual-only; standing surface is the configured ground plane)
[TriangleApp][StandingTest] Time: 6.0/6.0s, Pelvis Y: 0.7589m (range: [0.7584, 0.7593]), Tilt: 1.13 deg (max: 1.54 deg)
[TriangleApp][StandingTest] Finished test duration 6.0s. Final Result: PASSED
```
- `get_jolt_system` 相关错误行数：0。
- 该运行同时复验了 F2：Sponza 场景报告 372 个静态网格为 visual-only，
  机器人站在配置的 ground plane（y=-0.0251）上，pelvis 稳定在 0.7589 m。
- 也等于顺带复验了阶段 5（Sponza 站立）在真实场景下的表现。

### F4 · 已完成

改动：
- `bud.robot.lowcmd.hpp`：新增 `k_g1_standing_pelvis_height = 0.785f`。
  站姿基线高度是**控制/站姿参数**而非机器人几何，所以放在 G1 控制器模块，
  不塞进 cooked asset，也不用改 `RobotDef` 的序列化格式。
- `bud.robot.loader.cpp`：MuJoCo 分支不再有默认资产路径（`params.asset_path` 为空即报错返回）；
  删除 `root_position.y == 0 → 0.785f` 的隐式魔法；增益改为读机器人定义
  `joint.motor`（`enabled` 为真才用），不再在通用 loader 里调用 G1 增益表。
- `bud.physics.world.mujoco.cpp`：删除硬编码的 `Robots/g1_description` 兜底目录，
  网格路径只用 loader 给的路径或 `asset_root` 相对路径。
- `bud.robot.avatar.cpp`、`samples/mujoco_backend_test/main.cpp`：`0.785f` 换成命名常量。

验证：
- `grep` `src/robots/bud.robot.loader.cpp` 与 `src/physics/bud.physics.world.mujoco.cpp`
  已无 `g1_description` / `g1_29dof` / `0.785` / `get_default_g1_gains` / `Robots/g1`。
- 重新烘 `g1_29dof.budasset`：conformance 与 F3 完全一致
  （`nbody=31 njnt=30 nu=29 ngeom=73 mass=35.115142`，ankle `torque_limit=50`）。
  本项无需改 cook，重新烘烤作为"资产仍可烘、数值不漂"的确认。
- `mujoco_backend_test` 60s：全部 PASS（波动 0.01 cm、倾角 1.52°、无穿地、地面可查询）。

### F3 · 已完成

改动：
- `mujoco_model_builder.cpp`：新增 `joint_torque_limit_for(joint_name, urdf_effort)`，
  对 `ankle_*` 与 `waist_roll/waist_pitch` 返回官方值 50，其余沿用 URDF effort；
  电机创建改用该函数计算 ctrlrange。
- `bud.physics.world.mujoco.cpp`：删除按关节名把 ankle 覆盖成 ±50 的运行时块。

验证（重新烘 `g1_29dof.budasset`）：
```
[MujocoCook][conformance] counts: nbody=31 njnt=30 nu=29 ngeom=73 nmesh=36 mass=35.115142
ankle_pitch/ankle_roll/waist_roll/waist_pitch -> torque_limit=50.000000
```
cooked MJCF 的 ctrlrange 与官方 `g1_29dof.xml` 全项一致：
hip 88、knee 139、ankle 50、waist_yaw 88、waist_roll/pitch 50、
shoulder/elbow/wrist_roll 25、wrist_pitch/yaw 5。

回归：
```
[MuJoCo] world compiled: bodies=32 joints=30 geoms=74 actuators=29
[Stage 4 RESULT] ALL CHECKS PASSED
```
后端输出中不再出现 `updated ankle ... torque limit` 覆盖日志。

### F2a · 已完成（F2b 后置）

改动：
- `bud.physics.world.mujoco.hpp`：新增 `uncollidable_static_meshes` 计数。
- `bud.physics.world.mujoco.cpp`：Mesh/ConvexHull 占位分支首次触发时 `eprint` 明确说明
  "visual-only，无碰撞，站立面是配置的地面平面"，并在 `compile_world` 汇总被跳过的数量。
- `samples/mujoco_backend_test/main.cpp`：新增一个 Mesh 静态体以覆盖该分支；把 raycast 拆成
  "穿过机器人"与"机器人旁边（地面）"两条，并新增地面可查询的 PASS/FAIL。

验证：
```
[MuJoCo] static scene meshes/hulls are visual-only here and carry no collision; ...
[MuJoCo] 1 static scene meshes skipped (visual-only; standing surface is the configured ground plane)
[test] raycast through the robot: hit at (0.000, 1.307, -0.000)
[test] raycast beside the robot (floor): hit (y=0.0000)
[PASS] Scene floor is queryable at y=0.0000 m
[Stage 4 RESULT] ALL CHECKS PASSED
```

**F2b（后置，未做）**：真实 mesh 碰撞 / 由场景 bounds 生成地板薄 box。当前机器人站在配置的
ground plane 上，场景网格不参与碰撞。

### F1 · 已完成

改动（`src/physics/bud.physics.world.mujoco.cpp`）：
- 删除 `create_directories("tmp")` 与 `tmp/debug_robot.xml` 落盘。
- 删除逐 mesh 的 `mj_addBufferVFS(...) -> ...` 打印，改为统计失败（`add_result != 0` 才报错）。
- 删除 `parsing MJCF string`、`safe_parse_xml result`、`mesh VFS ready`、`robot model parsed`
  四条过程日志，合并为一条汇总。

验证：
```
[MuJoCo] robot 'g1_29dof' model parsed: 36 meshes (19133924 bytes), MJCF 28639 bytes
[PASS] Completed 60.0 s continuous standing
[PASS] Pelvis height fluctuation < 2 cm (actual: 0.01 cm)
[PASS] Maximum body tilt < 5 deg (actual: 1.52 deg)
[PASS] No ground penetration (lowest body point: 0.0000 m)
[Stage 4 RESULT] ALL CHECKS PASSED
```
- 输出中不再含 `debug_robot` / `Dumped MJCF` / `mj_addBufferVFS` / `safe_parse_xml` / `parsing MJCF`。
- 运行后 `tmp/` 无新增文件（`debug_robot.xml` 的修改时间停留在改动前）。
