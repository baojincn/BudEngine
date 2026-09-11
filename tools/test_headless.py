import os
import sys

print("[Python] Starting test_headless.py...", flush=True)

# Ensure module search path includes project root and build bin output
project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
bin_debug = os.path.join(project_root, "build", "bin", "debug")
sys.path.insert(0, project_root)
sys.path.insert(0, bin_debug)

if hasattr(os, "add_dll_directory"):
    for d in [bin_debug,
              os.path.join(project_root, "build", "vcpkg_installed", "x64-windows", "bin"),
              os.path.join(project_root, "build", "vcpkg_installed", "x64-windows", "debug", "bin")]:
        if os.path.exists(d):
            os.add_dll_directory(d)

try:
    import bud_rl
    print("[Python] Successfully imported bud_rl module.")
except ImportError as e:
    print(f"[Python] Failed to import bud_rl: {e}")
    sys.exit(1)

def main():
    cfg = bud_rl.AppConfig()
    cfg.scene_file = "Content/Scenes/sponza_page_scene.json"
    cfg.is_headless = True
    cfg.is_puppet_mode = True
    cfg.width = 256
    cfg.height = 256

    print(f"[Python] Creating GymEnv (Headless={cfg.is_headless}, Puppet={cfg.is_puppet_mode}, Res={cfg.width}x{cfg.height})...")
    app = bud_rl.GymEnv.create(cfg)

    print("[Python] Stepping environment for 5 frames...")
    for step in range(5):
        obs = app.step(0.016)
        if obs is not None and len(obs) > 0:
            print(f"[Python] Step {step + 1}: Observation received ({len(obs)} bytes, expected {cfg.width * cfg.height * 4})")
        else:
            print(f"[Python] Step {step + 1}: Empty observation received!")

    print("[Python] Headless Python RL test completed successfully!")

if __name__ == "__main__":
    main()
