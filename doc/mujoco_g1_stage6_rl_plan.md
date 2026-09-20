# 阶段 6：RL 策略接入方案

承接 `doc/mujoco_g1_stand_plan.md` 与 `doc/mujoco_g1_fix_plan.md`。修复阶段（F1-F11）已全部完成，
MuJoCo G1 能稳定站立（Stage 4 全项 PASS）且支持 10 次无累积的 remove/respawn 与不重编译的 reset，
S6.5 的前置已满足。

本阶段目标：**让 BudEngine 能跑强化学习策略，用手柄/键盘给速度指令，让 G1 在 Sponza 里站立与行走。**
本阶段**不实现 WBC**（见第八节）。

---

## 〇、优先目标调整：外部 ONNX 策略部署优先

原计划的顺序是"接口 → 单环境训练 → 部署"。实际优先级调整如下：

**用户要的是：能加载官方/外部的 ONNX 策略来驱动 G1。**
这意味着两件事：

1. **部署链路是第一优先级**，训练链路（S6.6）退到其后。
2. **不能只冻结一套 obs/action 规格。** 外部策略的观测/动作布局各不相同
   （维度、顺序、缩放、归一化是否烘进图、关节顺序、控制频率、甚至关节数 23/29/12 都可能不同），
   所以引擎必须提供**配置驱动的策略接口（PolicySpec）**：用一份描述文件把任意 ONNX 策略的
   输入/输出映射到我们机器人的传感器与关节。这比"训练自己的策略"更通用，也更有产品价值——
   客户带着自己的策略来，不需要改我们的代码。

**修订后的关键路径（部署优先）：**

| 序 | 内容 | 说明 |
| :-- | :-- | :-- |
| D1 | IMU 与观测源（S6.1） | 外部策略的 obs 几乎都要角速度/投影重力 |
| D2 | PolicySpec 描述文件 + 动作映射（新 S6.2b） | 配置驱动，适配任意策略布局 |
| D3 | ONNX Runtime + PolicyRunner（S6.7 提前） | 加载/查询 I/O/推理 |
| D4 | 运行时接管 + 手柄/键盘命令（S6.7） | 用策略替代脚本步态 |
| D5 | 部署验证（合成 ONNX → 真策略） | 先用零动作策略验证链路，再接真策略 |
| — | GymEnv / 训练（S6.3-S6.6） | 退到部署之后，接口复用同一份 PolicySpec |

**验收（部署优先版）**：`triangle_sample --policy <外部.onnx> --policy-spec <spec.json>` 能在 Sponza 里
驱动 G1 站立/行走；换一个布局不同的策略只改 spec，不改代码。

---

## 一、目标与验收

**主目标**
G1 在 Sponza 场景中接受速度指令 `(vx, vy, yaw_rate)`（来自手柄摇杆/键盘），完成站立、前进、转向、停止。

**中间目标**
BudEngine 能作为 RL 环境被 Python 驱动：`reset() -> obs`、`step(action) -> (obs, reward, terminated, truncated, info)`。

| # | 验收项 | 判定 |
| :-- | :-- | :-- |
| 1 | Python 环境可用 | `reset` 返回定长 obs；`step` 推进物理；episode 可复位 |
| 2 | 规格一致 | obs/action 的维度、顺序、缩放与训练侧逐项一致（同一份规格文件生成） |
| 3 | 指令跟随 | 手柄给 vx=0.5 m/s 时实际前进速度误差 < 0.15 m/s；yaw 同理 |
| 4 | 稳定性 | 站立 + 行走连续 60 s 不摔；被推一下能恢复 |
| 5 | 复位 | 随机复位 10 次无状态累积、无崩溃（复用 F6） |
| 6 | 不崩 | 长时间运行无内存增长（body/actuator 计数恒定） |

---

## 二、关键决策与取舍

### 2.1 训练在哪里（最重要的一条）

| 方案 | 做法 | 优点 | 缺点 |
| :-- | :-- | :-- | :-- |
| **A 引擎内训练** | pybind 暴露 env，外部 RL 库驱动 BudEngine 训练 | 单一事实源；obs/渲染/物理完全一致 | 单世界 MuJoCo + Vulkan，**不适合上万并行环境**，大规模训练慢 |
| **B 外部训练 + 引擎部署** | 在 `mjlab` / Isaac Lab 训练（相同 obs/action 规格），导出 ONNX 到引擎推理 | 训练快、生态成熟 | 需要维护两份环境实现，规格必须严格对齐 |

**决策：接口先行，A 打通验证，B 承担规模化。**

- S6.0-S6.5 把接口、obs/action、复位做出来，用**方案 A** 跑小规模 PPO，验证规格与奖励正确。
- 真正要一个能走稳的策略时，走**方案 B**：用 `mjlab`（MuJoCo Warp）或 Isaac Lab 大规模训练，
  导出 ONNX 交给引擎 S6.7 的推理路径。
- 两种方案共用同一份**规格文件**（见第三节），这样策略可以互操作。

**为什么不直接在引擎里训到能走：** 双足 locomotion 的 PPO 通常需要 2k-16k 并行环境 × 上亿步。
单世界 MuJoCo 就算跑 500 Hz，也要跑很久。把训练放在能做 GPU 并行的 trainer 里是工程上唯一现实的选择。

### 2.2 策略层次（不引入 WBC）

```
手柄/键盘 → command (vx, vy, yaw_rate)
                ↓
        RL policy (50 Hz)  → 29 维 action
                ↓ q_des = default_pose + action * scale
        LowCmd PD (500 Hz) → tau
                ↓
             MuJoCo
```
复用已有的 `set_articulation_joint_commands` 与电机模型；`kp/kd` 沿用站姿表（腿高臂低），
力矩上限用资产的 `ctrlrange`（cook 已与官方对齐）。

### 2.3 频率
- policy：50 Hz（`policy_dt = 0.02 s`）
- sim：500 Hz（`opt.timestep = 0.002`，已有常量）
- `decimation = 10`：每个 policy 周期内跑 10 个 sim 子步，动作在周期内保持不变。

### 2.4 推理引擎
**ONNX Runtime**（vcpkg）。理由：C++ API 稳定、体积可控、跨平台；归一化与裁剪烘进图，
引擎侧只做张量搬运。备选 LibTorch（少一步导出，但依赖重）。

### 2.5 算法
**PPO**（legged locomotion 事实标准）。实现可选 `rsl_rl` 风格 / `skrl` / 自研最小 PPO。
第一版只要 PPO + GAE + 观测归一化（running mean/std），先跑通再谈调优。

---

## 三、接口规格（S6.0 冻结 v1）

**规格必须写进一个共享文件**（如 `doc/rl_spec_g1_v1.md` 或 `src/rl/bud.rl.spec.hpp` + 对应 Python 常量），
训练侧与引擎侧都从它生成，禁止手动两处维护。

### 3.1 观测 obs（float32，顺序固定）

| 段 | 维度 | 来源 | 缩放 |
| :-- | :-- | :-- | :-- |
| base_ang_vel | 3 | IMU 陀螺 | ×0.25 |
| projected_gravity | 3 | IMU 四元数 → 世界系下的机体上轴 | 无 |
| velocity_commands | 3 | `(vx, vy, yaw_rate)` | ×(2.0, 2.0, 0.25) |
| joint_pos − default | 29 | 关节角 | 无 |
| joint_vel | 29 | 关节角速度 | ×0.05 |
| last_action | 29 | 上一步动作 | 无 |

合计 **96**。可选特权观测（仅训练用，部署时不可用）：`base_lin_vel`(3)、`height_scan`、`contact_state`。

> 关节顺序固定为**资产里的关节顺序**（`ArticulationDesc.joints` 顺序），且必须与训练侧一致。
> 这是最容易出错的地方，S6.0 要落成断言。

### 3.2 动作 action（float32，29 维，范围 [-1, 1]）

```
q_des[i] = default_joint_pos[i] + action[i] * action_scale
```
- `default_joint_pos` = 官方站姿（`get_g1_standing_joint_angles()`）。
- `action_scale` 起始值 0.25 rad（可按关节组细分：腿/臂不同）。
- 结果裁剪到关节限位。
- `kp/kd`：命令里携带（G1 增益表），不随动作变化。

### 3.3 命令 command
`(vx ∈ [-0.6, 1.0], vy ∈ [-0.4, 0.4], yaw_rate ∈ [-1.0, 1.0])`。
训练时随机采样 + 站立指令（全 0）若干比例；部署时来自手柄，键盘作为降级方案。

### 3.4 复位 reset
- `reset_articulation()`（F6）回到 spawn 姿态并清速。
- 随机化（训练时）：初始关节角 ±0.1 rad、初始高度 ±0.02 m、初始速度小随机。
- 复位**不触发 world 重编译**（F6 已保证）。

### 3.5 奖励（起始参考，需实测调整）

| 项 | 含义 | 参考权重 |
| :-- | :-- | :-- |
| track_lin_vel_xy | 跟踪 vx/vy 指令 | +1.0 |
| track_ang_vel_yaw | 跟踪 yaw 指令 | +0.5 |
| alive | 每步存活 | +0.5 |
| orientation | 投影重力 xy 偏离惩罚 | −1.0 |
| base_height | 骨盆高度偏离目标 | −1.0 |
| lin_vel_z | 上下窜动惩罚 | −2.0 |
| action_rate | 动作变化率惩罚 | −0.01 |
| torques | 力矩平方惩罚 | −1e-5 |
| dof_acc | 关节加速度惩罚 | −2.5e-7 |
| feet_air_time | 抬脚时间匹配 | +0.25 |
| feet_slip / collision | 打滑与非法接触 | −0.1 / −1.0 |

### 3.6 PolicySpec：配置驱动的外部策略接口（部署优先的核心）

3.1/3.2 的布局只是**我们自己的默认规格**。要跑外部策略，必须由一份描述文件定义映射关系：

```jsonc
{
  "name": "unitree_g1_29dof_velocity",
  "policy_dt": 0.02,              // 策略周期 50 Hz；decimation 由引擎按 sim dt 算
  "obs": {
    "terms": [                    // 顺序 = 策略输入向量的拼接顺序
      { "signal": "base_ang_vel",        "scale": 0.25 },
      { "signal": "projected_gravity",   "scale": 1.0 },
      { "signal": "command",             "scale": [2.0, 2.0, 0.25] },
      { "signal": "joint_pos_rel",       "scale": 1.0 },
      { "signal": "joint_vel",           "scale": 0.05 },
      { "signal": "last_action",         "scale": 1.0 }
    ],
    "normalization": null          // 或 { "mean": [...], "std": [...] }；若已烘进图则为 null
  },
  "action": {
    "joint_order": ["left_hip_pitch_joint", "..."],  // 策略输出的关节顺序，必须显式列出
    "default_pose": "g1_standing",                   // 命名姿态，或直接给数组
    "scale": 0.25,
    "clip": [-1.0, 1.0]
  },
  "inputs": { "joystick": true }   // command 是否来自手柄
}
```

要点：
- **关节顺序必须显式声明**，加载时与资产关节做双向断言，缺一个就报错退出（这是最容易出错的点）。
- `normalization` 为空表示归一化已烘进 ONNX 图；非空则引擎在推理前做 `(x-mean)/std`。
- `signal` 是一组受支持的观测项（`base_ang_vel` / `projected_gravity` / `base_lin_vel` / `command` /
  `joint_pos_rel` / `joint_pos` / `joint_vel` / `joint_torque` / `last_action` / `phase`），引擎按名字组装。
- 不支持的历史堆叠/相位等，先用 spec 里的 `history: N` 或 `phase_period` 扩展；实在不支持就在 spec 里明确报错，
  而不是静默给错数据。

---

## 四、实施步骤

| 阶段 | 内容 | 依赖 |
| :-- | :-- | :-- |
| **S6.0** | 冻结接口规格与常量（obs/action/命令/频率/关节顺序），落地为共享头 + Python 常量 | 无 |
| **S6.1** | IMU 与关节传感器：cook 补 site + sensor，后端读 `sensordata` 并暴露 | F3 |
| **S6.2** | 动作应用：action → q_des → 现有 LowCmd PD | S6.0 |
| **S6.3** | GymEnv：`reset()` / `step(action)` + pybind 暴露（复用现有 `bud_rl` 模块与 puppet 模式） | S6.1/S6.2/F6 |
| **S6.4** | 频率与时间：policy 50 Hz / sim 500 Hz，decimation，固定 dt | S6.3 |
| **S6.5** | 复位与随机化：复用 `reset_articulation` + 观测/命令随机化 | S6.3 |
| **S6.6** | 训练：Python env wrapper（gymnasium 接口）+ PPO + 奖励 + 日志 + checkpoint | S6.3-S6.5 |
| **S6.7** | 部署：导出 ONNX（含归一化）+ C++ `PolicyRunner` + 手柄/键盘命令 → 行走 | S6.2/S6.6 |
| **S6.8** | 规模化（可选）：引擎内多环境或走 mjlab/Isaac 训练 | S6.6 |

### S6.0 冻结接口
- 新增 `src/rl/bud.rl.spec.hpp`：维度常量、关节顺序来源、缩放系数、默认姿态、频率。
- 同时导出一份 Python 可读的常量（生成 `python/bud_rl/spec.py` 或 JSON），训练侧 import 它。
- 加一条静态断言：`obs_dim == 3+3+3+29+29+29`，关节数 == 资产关节数。

### S6.1 IMU 与传感器
- **cook**（`mujoco_model_builder.cpp`）：在根 body（pelvis）加 `site name="imu"`；
  加传感器 `framequat` / `gyro` / `accelerometer`（绑 imu site）；每关节加 `jointpos` / `jointvel` / `jointactuatorfrc`。
- **后端**（`world.mujoco.cpp/.hpp`）：编译后按名字解析一次 sensor id 并缓存；
  读 `data->sensordata`，把 IMU 四元数/角速度/线加速度暴露出来。
  建议扩展 `ArticulationStateSoA` 或新增 `ArticulationImu` + `get_articulation_imu()`。
- **坐标系**：IMU 在 MuJoCo 是 Z-up，需要转引擎 Y-up（后端已有 `from_mujoco` 系列换算，复用）。

### S6.2 动作应用
- `action[i] → q_des[i] = default[i] + action[i] * scale`，裁剪到限位，填充 29 条 `JointCommand`（含 kp/kd），
  调用现有 `set_articulation_joint_commands`。
- 关节顺序映射在 S6.0 的规格里定义，代码里一次建表。

### S6.3 GymEnv
- `GymEnvApp`：`reset() -> obs vector`；`step(action) -> (obs, reward, terminated, truncated, info)`；
  复用 `GameFramework::init_puppet/step_puppet`（puppet 模式就是为离散 RL 步进准备的）。
- `py_bindings.cpp`：暴露 `reset/step` 与规格常量；注意 GIL 与线程（物理在主线程，Python 在调用线程）。
- 观测在 C++ 侧组装（S6.1 的数据 + 上一步动作 + 命令），避免 Python 每步拼接。

### S6.4 频率与时间
- `step(action)` 内部按 `decimation` 跑 sim 子步；动作缓存不变。
- `dt` 固定，禁止用真实墙钟时间推进物理（保证训练可复现）。

### S6.5 复位与随机化
- `reset()` 调 `reset_articulation`，再按规格随机化（训练开关、部署关闭）。
- 终止条件：骨盆高度低于阈值或倾角超阈值 → `terminated = true`。
- 必须验证：连续 10 次 reset 后 `model_body_count` / `model_actuator_count` 不变（F6 的验收已在测试里）。

### S6.6 训练
- Python：`gymnasium.Env` 包一层（支持向量化时为 VecEnv）；PPO + GAE + obs 归一化。
- 先跑"站立"（命令恒 0）能收敛，再开速度指令。
- 日志：reward 分量、episode length、tracking error；checkpoint 定期存。
- **验收（方案 A）**：站立任务能到达稳定；速度指令任务能看到 tracking error 下降。
  （不要求在这一步就得到能走稳的策略，那是 S6.8/方案 B 的目标。）

### S6.7 部署（手柄/键盘行走）
- 导出 ONNX：把 obs 归一化（running mean/std）与动作裁剪烘进图，输入 `obs[1,96]`，输出 `action[1,29]`。
- C++ 新增 `src/rl/bud.rl.policy.{hpp,cpp}`：ONNX Runtime session，`act(obs) -> action`。
- 命令来源：手柄（摇杆 → vx/vy，另一个轴 → yaw_rate）或键盘（WASD + QE）。
  接到 `RobotAvatarController` 的 MuJoCo 分支，替代现有的脚本步态。
- 推理频率 50 Hz，与训练一致；PD 仍在 500 Hz。

### S6.8 规模化（可选，方案 B）
- 引擎内多环境：N 个 `mjData` 共享一个 `mjModel`，批量 step；或 N 个 `PhysicsWorld`（更简单但更重）。
- 或直接在 `mjlab` / Isaac Lab 训练（用同一份规格），ONNX 导出到 S6.7。
- 域随机化：质量、摩擦、电机增益、延迟、外部推力。

---

## 五、文件与改动点

| 区域 | 文件 | 改动 |
| :-- | :-- | :-- |
| cook | `src/tools/asset_pipeline/builders/mujoco_model_builder.cpp` | imu site + 传感器元素 |
| 后端 | `src/physics/bud.physics.world.mujoco.{hpp,cpp}` | sensor id 缓存、读 sensordata、IMU 暴露 |
| 接口 | `src/physics/bud.physics.world.hpp`、`bud.physics.types.hpp` | IMU 状态 / 新 getter |
| 规格 | `src/rl/bud.rl.spec.hpp`（新） | 维度、缩放、顺序、频率 |
| RL 逻辑 | `src/rl/bud.rl.observation.{hpp,cpp}`（新） | obs 组装 |
| RL 逻辑 | `src/rl/bud.rl.action.{hpp,cpp}`（新） | action → q_des |
| 策略 | `src/rl/bud.rl.policy.{hpp,cpp}`（新） | ONNX Runtime 推理 |
| Gym | `samples/gym_env/gym_env.{hpp,cpp}`、`py_bindings.cpp` | reset/step/规格 |
| 机器人 | `src/robots/bud.robot.avatar.cpp`、`bud.robot.lowcmd.{hpp,cpp}` | 命令→策略接管、默认姿态/缩放常量 |
| 输入 | `src/input/bud.input.manager.*` | 手柄映射到 command |
| 构建 | `CMakeLists.txt` | onnxruntime 依赖、新源文件、bud_rl 模块 |

---

## 六、训练 → 部署 流水线

```
[规格 spec] ──┬─> 训练侧 (Python: env wrapper + PPO)  ──> checkpoint (.pt)
              │                                            │
              │                                   导出 (归一化烘入)
              │                                            ↓
              └─> 引擎侧 (C++: obs 组装 + ONNX 推理) <── policy.onnx
                              ↓
                    action → LowCmd PD → MuJoCo
```

命令：
- 训练：`python train.py --task g1_velocity_flat --spec doc/rl_spec_g1_v1`
- 导出：`python export_onnx.py --ckpt runs/xxx.pt --out g1_locomotion.onnx`
- 部署：`triangle_sample --backend mujoco --policy g1_locomotion.onnx`

---

## 七、风险与明确不做

| 风险 | 应对 |
| :-- | :-- |
| 单环境训练太慢 | 方案 A 只做验证，规模化走 B（mjlab/Isaac） |
| obs/action 规格两侧漂移 | S6.0 单一规格源 + 静态断言 + 部署前对拍 |
| 关节顺序不一致 | 规格里写死顺序，加载时断言 |
| sim2real gap | 域随机化 + 延迟建模（后续） |
| 现成 checkpoint 用不了 | 已发布策略的 obs/action 布局多半与本规格不同，不能直接吃；要么按其布局适配，要么自己训 |
| 手柄命令与训练分布不一致 | 命令范围/缩放写进规格，部署时同样裁剪 |

**明确不做**
- 不做 WBC（见与用户的结论：先 RL；WBC 是有全身操作/硬约束需求时再上）。
- 不做全身操作、抓取、复杂地形。
- 不做真机部署与实机标定。
- 不改渲染管线 / 布料求解器 / Jolt 游戏世界（游戏模式不受影响）。
- 不在本阶段追求"训练出一个 SOTA 行走策略"；本阶段交付的是**接口 + 推理 + 一条能跑通的训练流水线**。

---

## 八、里程碑切分建议

**部署优先（当前执行顺序）：**

| 里程碑 | 交付 | 可演示 |
| :-- | :-- | :-- |
| **M1** | D1+D2：IMU/观测源 + PolicySpec + 动作映射 | 合成 ONNX（零动作）能驱动 G1 保持站姿 |
| **M2** | D3+D4：ONNX Runtime + PolicyRunner + 手柄命令接管 | `--policy <外部.onnx> --policy-spec <spec.json>` 在 Sponza 里站立/行走 |
| **M3** | D5：换一个布局不同的策略只改 spec | 证明接口通用性 |

**训练链路（部署之后再回来）：**

| 里程碑 | 交付 | 可演示 |
| :-- | :-- | :-- |
| M4 | S6.3-S6.5（GymEnv + 复位） | Python 侧能 reset/step，obs/action 正确，随机策略不崩 |
| M5 | S6.6（PPO 训练） | 站立任务收敛，日志与 checkpoint |
| M6 | S6.8（可选，规模化） | 更大规模训练，行走更稳 |

---

## 九、实施记录（部署链路 D1-D4 已完成）

### D1 · IMU 与观测源
- cook（`mujoco_model_builder.cpp`）：根 link 上加 `site "imu"` + `imu_quat`(framequat) /
  `imu_gyro` / `imu_accel`；conformance 增加 `nsensor`。重烘后 `nsensor=3`，质量/nu 不变。
- 接口（`bud.physics.world.hpp`）：新增 `ArticulationImu{orientation, angular_velocity,
  linear_acceleration}` 与 `get_articulation_imu()`（Jolt 侧返回 false）。
- 后端（`bud.physics.world.mujoco.cpp`）：编译后按名字解析 sensor id；`sensordata` 读取后
  用既有的 `from_mujoco` / `direction_from_mujoco` 转成引擎帧（世界系方向 + 机体系向量）。
- `remove_articulation` 现在同时删除属于该 articulation 的传感器（按 site/body 归属匹配），
  否则 respawn 会重名堆积。

### D2 · PolicySpec 与观测/动作映射
- `src/rl/bud.rl.policy_spec.{hpp,cpp}`：JSON 规格 + 校验（信号名、缩放、归一化、关节顺序
  唯一性、默认姿态长度、policy_dt 范围、history 暂只支持 1）。
- `src/rl/bud.rl.observation.{hpp,cpp}`：按规格组装观测（base_ang_vel / projected_gravity /
  base_lin_vel / command / joint_pos(_rel) / joint_vel / joint_torque / last_action），
  支持逐项缩放与可选归一化。
- `src/rl/bud.rl.g1_policy_controller.{hpp,cpp}`：把 policy 关节顺序映射到资产关节（缺一个就报错），
  解析默认姿态（数组优先，否则命名姿态），增益取 `joint.motor` 或 G1 增益表，
  动作 `q_des = default + action * scale` 经现有 `set_articulation_joint_commands` 下发。
- 参考规格：`Content/rl/unitree_g1_29dof_dense_spec.json`（29 关节；此前那份 96 维示例规格
  已随 12-DOF 方案删除）。

### D3 · ONNX Runtime 推理
- `src/rl/bud.rl.policy_runner.{hpp,cpp}`：pimpl 包装 Ort::Session（单线程、固定形状 [1,N]），
  加载时校验输入维度 == 规格观测维度、输出维度 == 关节数。onnxruntime 已在 vcpkg/cmake 里，无新增依赖。

### D4 · 运行时接管与命令
- `RobotAvatarController::load_policy()`；加载成功后**由策略接管关节**，脚本步态/踝平衡被旁路
  （避免两个控制器抢同一批目标）。
- 命令：WASD → (vx, vy)，Q/E 或手柄 → yaw_rate；`policy_command_from_input()`。
- 采样程序：`triangle_sample --policy <onnx> --policy-spec <json>`。

### 验证证据
```
[RobotAvatarController] policy 'g1_velocity_flat' loaded: obs=96 action=29
[RL] first policy step: obs=96 action=29 cmd=(0.00,0.00,0.00) q[0]=-0.150 kp[0]=150.0 kd[0]=3.00 gravity_y=-1.000
```
（空白策略：q[0] 正好是站姿 hip_pitch -0.150，hip 增益 150/3，机体上轴与重力反向 → 帧约定正确。）

用一个"常量输出"探测 ONNX（输出 hip action=0.4）验证图输出真的进到关节：
```
[RobotAvatarController] policy 'g1_velocity_flat' loaded: obs=96 action=29
[RL] first policy step: ... q[0]=-0.050 ...      # = -0.150 + 0.4 * 0.25，完全吻合
```
（注意第二条日志没有 "null policy" 后缀，说明走的是 ONNX 图。）

`mujoco_backend_test`（回归）：
```
[test] lifecycle: reset=ok, 10 cycles, remove bodies 32->2 / actuators 29->0 / sensors 3->0,
                  respawn bodies=32 / actuators=29 / sensors=3
[test] imu: ok
[PASS] IMU reports an upright unit orientation with near-zero angular velocity
[Stage 4 RESULT] ALL CHECKS PASSED
```

### 已知限制（如实记录）
- **空白策略（无 ONNX）只保持站姿，没有平衡控制器，会倒**。观测/动作/PD 链路已验证正确，
  但"站着/走路"必须由真实的训练策略提供平衡修正。这不是 bug：站姿 PD 本身不是平衡控制器。
- 训练链路（M4-M6：GymEnv / PPO / 规模化）未做，按计划在部署之后。

### 官方 Unitree G1 策略接入：已适配，卡在 ONNX Runtime

> **历史记录 / 已废弃**：本节是 12-DOF（`unitree_rl_gym`）方案的排查过程。
> 它已被下面的 29-DOF（`unitree_rl_lab`）方案取代——**本节提到的所有 12-DOF 配置与权重文件均已删除**。
> 保留此节只为说明 ONNX Runtime 的阻塞是如何被发现并绕开的。

已下载并转换官方策略（`unitree_rl_gym/deploy/pre_train/g1/motion.pt`），其规格与部署脚本已逐行核对：

| 项 | 官方值 | 我们的支持 |
| :-- | :-- | :-- |
| 动作 | 12 个（仅腿） | spec 子集 joint_order 支持 |
| 观测 | 47 = 角速度3 + 重力3 + 命令3 + 关节12 + 速度12 + 上一步动作12 + 相位2 | 全部支持（新增 `phase`、`frame`） |
| 轴系 | 机器人（URDF/MuJoCo）轴系 | 新增 `frame: "robot"`，IMU 输出机器人轴系 |
| 增益 | kp/kd 与官方不同 | spec 新增 `action.kp/kd` 逐关节覆盖 |
| 策略 | LSTM（h/c 为 buffer） | runner 支持多输入/输出 + 持久状态；导出脚本把 h/c 显式化 |

产物：
- `Content/rl/unitree_g1_12dof_spec.json` —— 与官方策略逐项对齐的规格。
- `Content/rl/export_unitree_g1_onnx.py` —— 从官方 `.pt` 重建前向、h/c 显式导出、并与 TorchScript 对拍。

**阻塞点：vcpkg 的 onnxruntime 1.23.2 在解析"含初始化器（权重）"的 ONNX 图时，于
`Ort::Session` 构造中抛出未捕获的 C++ 异常（exit 0xE06D7363），进程直接终止。**
已排除的因素：
- 不是我们的代码：无权重（只有 Constant 节点）的模型在**同一构建**下加载、推理、驱动关节全部正常。
- 不是 opset：手工构造的 opset 13 与 opset 17 带权重模型**都崩**。
- 不是 debug 构建：Debug 与 Release 都崩。
- 不是模型非法：同一文件在 Python onnxruntime 1.30 下能加载并推理，且与 TorchScript 逐位一致（1e-6）。
- 不是 torch 新导出器：改用手工构造的最小带权重模型同样崩。

**最强假设：protobuf 符号冲突。** 引擎自身依赖 `libprotobufd.dll` / `libprotobuf-lited.dll`
（exe 旁即有），而 ONNX Runtime 内部也使用 protobuf 解析图与初始化器；若两者符号互相绑定，
只有走到"解析 initializers"这条路径才会崩。已验证该 DLL 是引擎的硬依赖（改名后进程直接起不来），
所以不能简单移除。

**下一步解法（按优先级）**
1. 让 ORT 使用私有 protobuf：换一份静态链接 protobuf 的 onnxruntime（官方预编译包通常是静态的），
   或调整 vcpkg 的 ORT feature，使其不导出 protobuf 符号。
2. 或减少引擎侧 protobuf 的使用面（找出谁引入的：assimp / 资产管线），改为静态链接或改名隔离。
3. 或把策略推理放进独立进程/模块，与引擎的 protobuf 隔离。
4. 兜底：先用 Python 侧的 onnxruntime 驱动（把 obs/action 通过 IPC 传给引擎）验证策略行为，
   把 C++ 推理作为后续集成项。

**注意**：观测/动作链路本身已经用真实 ONNX 图验证过（探测模型证明图输出进入了关节目标），
所以剩下的纯粹是"ORT 在本进程内解析含权重图"的集成问题，不是接口设计问题。

### 突破：官方策略已在本引擎中站立与行走（走的是"密集权重"路径）

为绕开 ORT，新增了一条**不依赖 ONNX Runtime 的策略运行时**：把策略权重导出成扁平 float32，
由 `src/rl/bud.rl.dense_policy.*` 直接计算「1 层 LSTM + ELU MLP」。这条路不仅绕过了 ORT/protobuf，
对国产化硬件部署也更友好（没有 protobuf、没有 ORT 依赖）。

规格 `network` 段：
```json
"network": { "type": "lstm_mlp", "weights": "Content/rl/unitree_g1_weights.bin",
             "input_size": 47, "hidden_size": 64, "actor_hidden": 32,
             "output_size": 12, "layers": 1 }
```

**抓到的关键 bug：关节顺序。** 官方 YAML 的 `default_angles` 顺序必须匹配其 MuJoCo 模型的
`qpos` 顺序，即 URDF 声明顺序 **[hip_pitch, hip_roll, hip_yaw, knee, ankle_pitch, ankle_roll]**；
我最初误用了 `g1_config` 字典的 [yaw, roll, pitch] 顺序，动作映射到错误关节，机器人立刻倒地。
改成 pitch-first 后立即站住。**这条对所有外部策略都成立：关节顺序必须按模型的 qpos/动作顺序，
不能按配置字典的书写顺序。**

验证（`triangle_sample --backend mujoco --policy-spec Content/rl/unitree_g1_12dof_dense_spec.json`）：
```
[RL] dense policy ready: obs=47 action=12 state=128 weights='Content/rl/unitree_g1_weights.bin'
[RL] first policy step: obs=47 action=12 state=128 q[0]=-0.175 gravity_robot=(0,0,-1)
[StandingTest] Time: 12.0/12.0s, Pelvis Y: 0.7479m (range: [0.7463, 0.7542]), Tilt: 2.14 deg (max: 6.06 deg)
Final Result: PASSED
```

按速度指令行走（`--cmd 0.5 0 0`，遥测每 2s 打印一次）：
```
t=2s  base=(0.690,0.736,0.075)   cmd=(0.50,0.00,0.00)
t=6s  base=(2.375,0.734,0.316)
t=10s base=(4.009,0.734,0.633)
t=14s base=(5.587,0.735,1.153)
Final Result: PASSED
```
x 方向约 0.41 m/s（指令 0.5 m/s），高度稳定在 0.74 m，全程不倒。**官方 G1 策略在本引擎中
完成了站立与按指令行走。**

顺带修掉一个真实的解析 bug：`std::stof(argv[++i])` 三个写在同一函数调用里，参数求值顺序未定义
（MSVC 从右往左），导致 `(0.5,0,0)` 被读成 `(0,0,0.5)`。已改为先读具名局部变量。

**产物**
- **（已删除）** `unitree_g1_12dof_dense_spec.json` / `unitree_g1_12dof_spec.json` /
  `unitree_g1_weights.bin` / `export_unitree_g1_weights.py` / `export_unitree_g1_onnx.py`
  —— 12-DOF 方案的规格、权重与导出脚本，已随 29-DOF 方案上线移除，避免被误用。
- `triangle_sample --policy-spec <spec> [--policy <onnx>] [--cmd vx vy yaw]` —— 部署入口；
  不给 `--cmd` 时由 WASD/QE 或手柄实时控制。

**ONNX 路径的状态（未解决但不阻塞）**：ORT 在本进程内解析含初始化器的图仍会终止进程，
现象与证据见上一小节。由于密集权重路径已经跑通并可用，ORT 问题现在属于"以后要修的集成项"，
不再是阻塞项。

### 最终结果：官方 29-DOF 策略在本引擎中站定、直线行走、按指令转向

`unitree_rl_gym` 只有 12-DOF 策略（腿 + 固定躯干），跑在我们的 29-DOF 机器人上会绕圈漂移。
真正匹配的是 **`unitree_rl_lab`** 里的 29-DOF 版本：

```
deploy/robots/g1_29dof/config/policy/velocity/v0/exported/policy.onnx   (1.6 MB)
deploy/robots/g1_29dof/config/policy/velocity/v0/params/deploy.yaml
```
- `obs [1,480]` = (3+3+3+29+29+29) × **5 帧历史**；`actions [1,29]`；节点只有 **Gemm+Elu**（纯 MLP，无 LSTM）。
- 逐行核对官方部署源码后确定的布局：
  - `Articulation::update()`：`joint_pos[i] = motor_state[joint_ids_map[i]].q()` → **观测关节也是策略顺序**；
    `root_ang_vel_b = IMU gyro`、`projected_gravity_b = quat.conjugate()·(0,0,-1)` —— 与我们的实现一致。
  - `ObservationTermCfg::get()`：按项分组，**每项内部 旧→新**，项间按 YAML 顺序。
  - `State_RLBase::run()`：`motor_cmd[joint_ids_map[i]].q = action[i]`。
- `joint_ids_map` 与标准 G1 SDK 关节顺序逐项验证通过（含左右对称的 shoulder_roll ±0.25 / elbow 0.97 / wrist_roll ±0.15）。

为此新增：密集运行时的 **MLP** 支持（`DensePolicy::Kind::Mlp`）、观测的**逐项历史堆叠**（`ObservationBuilder`）、
以及一个**航向保持外层环**（该策略只跟踪偏航角速度、训练时 `heading: null`，没有绝对朝向反馈，
不加外环会画圈）。

产物：
- `Content/rl/unitree_g1_29dof_dense_spec.json` —— 29 关节策略顺序、默认姿态、kp/kd、obs 缩放与历史。
- `Content/rl/unitree_g1_29dof_weights.bin` —— 从 ONNX initializer 导出的扁平权重（414,237 floats）。
- 密集权重由引擎自己的资源管线从 ONNX 生成（`Content/rl/unitree_g1_29dof_weights.bin`），不保留外部 Python 导出脚本。
- `triangle_sample --policy-spec <spec> [--cmd vx vy yaw]`。

**实测（`triangle_sample --backend mujoco --policy-spec Content/rl/unitree_g1_29dof_dense_spec.json`）**

| 工况 | 结果 |
| :-- | :-- |
| 零指令 12 s | 位移 6 mm（0.6 mm/s），`foot_swing=0.000 m`（双脚站定），PASSED |
| `--cmd 0.5 0 0` 15 s | x 前进 6.5 m（**0.47 m/s**，指令 0.5，跟踪 94%），横向漂移 0.35 m（加航向外环前是 2.4 m） |
| `--cmd 0.3 0 0.2` 12 s | 前进同时按指令转弯，PASSED |

也就是说：**观测/动作/历史/时序整条链路与官方策略完全对齐，策略在本引擎里能站定、直线行走、按指令转向。**

尚未做：GymEnv 训练链路（M4-M6）；ORT 含权重图的集成问题；这份策略只跟踪速度不跟踪绝对朝向，
"绝对朝向"由我们的外层环提供（若要策略原生支持需换带 heading 观测的版本）。
