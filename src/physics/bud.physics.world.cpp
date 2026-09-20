#include "src/physics/bud.physics.world.hpp"
#include "src/physics/bud.physics.world.jolt.hpp"
#include "src/physics/bud.physics.world.mujoco.hpp"

namespace bud::physics {

    // Backend selection. Jolt is the world backend (game scenes, mesh levels, many bodies);
    // MuJoCo is the robot backend and is added next. A future GPU XPBD world becomes a third case
    // here without any call site changing.
    std::unique_ptr<PhysicsWorldBase> create_physics_world(PhysicsBackend backend) {
        switch (backend) {
        case PhysicsBackend::Jolt:
            return std::make_unique<JoltPhysicsWorld>();
        case PhysicsBackend::Mujoco:
            return std::make_unique<MujocoPhysicsWorld>();
        case PhysicsBackend::GpuXpbd:
            // TODO(physics abstraction): unified rigid + soft XPBD world.
            return nullptr;
        }
        return nullptr;
    }

} // namespace bud::physics
