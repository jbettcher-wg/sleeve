# sleeve

**The multi-backend emulator application manager and terminal UI.**

`sleeve` manages, scans, configures, and wraps binaries running under CPU emulators on Linux. It produces safe, regenerable shell launchers in `~/.local/bin/`, desktop entries in `~/.local/share/applications/`, and merges per-application emulator configurations into `AppConfig/<program>.json`.

Currently supporting:
- **POWERarm** (AArch64 on PPC64LE / POWER9)
- **fastppcx86** (x86_64 on PPC64LE / POWER9)

---

## Features

1. **App Shape Detection**: Identifies application types and applies tuned preset flags:
   - **Electron**: VS Code, Antigravity IDE (`--no-sandbox`, `ELECTRON_OZONE_PLATFORM_HINT=auto`, WMClass extraction).
   - **Gecko**: Firefox (`--profile ~/.mozilla/<app>`, `MOZ_ENABLE_WAYLAND=1`).
   - **Game**: Factorio, SDL2/OpenGL/Vulkan games (run from install directory, MangoHud overlay integration).
   - **Pacman Overlay**: Applications installed into rootfs overlay via guest pacman.
   - **Runtime / CLI**: Single-binary tools and CLI utilities (launcher only, no desktop entry).
2. **Safe Shell Wrappers**: Generates POSIX `sh` launchers in `~/.local/bin/<name>` avoiding raw ELF symlinks (preventing `O_TRUNC` -> `SIGBUS` issues).
3. **Desktop Integration**: Generates `~/.local/share/applications/<name>-<arch>.desktop` with absolute icons, `StartupWMClass`, `X-Sleeve-Managed` tags for Omarchy / Hyprland, and `StartupNotify=false` (no emulated app completes the startup handshake, so the launcher's spinner would never stop; re-enable per app with `sleeve set NAME startupnotify=on`).
4. **Emulator AppConfig Management**: Merges managed keys (`RootFS`, `EnableCodeCachingWIP`, `CodeCacheScope`, `DisableCmpBranchFusion`, `ProfileStats`) into `~/.config/<emulator>/AppConfig/<program>.json`, strictly preserving all existing user overrides, `ThunksDB`, and custom sections.
5. **Health Checks**: `sleeve check` runs the app (via `--version` or timed with window detection via `hyprctl clients -j`), analyzes logs, deduplicates unimplemented instruction probes, and diagnoses missing shared libraries or emulator faults.
6. **Omarchy Theming**: Automatically detects the active Omarchy theme and palette via `omarchy-theme-color`. Watches `~/.local/state/omarchy/current/` via inotify to re-tint running TUI sessions dynamically upon theme switches.
7. **CLI Twin**: Every action in the interactive FTXUI terminal interface has a direct CLI counterpart for scripting and AI agents.

---

## CLI Usage

```sh
# Launch interactive terminal UI
sleeve

# Scan for guest applications. With no arguments this searches the standard locations and
# the rootfs overlays; with DIR arguments it searches only those. Every row says where it
# came from, and a version directory is reported through its `current` symlink when one
# points at it, so launchers survive app updates.
sleeve scan [DIR...] [--json]

# Add an application from a directory or binary
sleeve add ~/Development/vscode-arm64 --name code

# Import a foreign/hand-written launcher into sleeve management
sleeve import ~/.local/bin/antigravity-ide

# List and inspect managed applications
sleeve list [--json]
sleeve show code [--json]

# Configure application knobs
sleeve set factorio stats=on fusion=on cache=on scope=home
sleeve set code startupnotify=off
sleeve set code rootfs=/home/jbettcher/.local/share/powerarm/RootFS/ArchLinuxARM-vk

# Generate / update launcher, desktop entry, and AppConfig (with preview diff)
sleeve wrap code [--dry-run] [--yes]

# Run health check
sleeve check code --version
sleeve check factorio --seconds 20

# RootFS management. Discovery is scoped to the active backend and the guest architecture
# is read from the tree itself, so an x86_64 rootfs is never offered to an AArch64 guest.
# A name that does not resolve is refused rather than silently falling back to host binaries.
sleeve rootfs list
sleeve rootfs use ArchLinuxARM-m2

# Theme inspection
sleeve theme
```

---

## Building and Testing

Requirements: C++20 compiler (`g++` or `clang++`), `cmake` >= 3.20, and `ninja`.

```sh
git clone https://github.com/jbettcher/sleeve.git
cd sleeve
git submodule update --init --recursive

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build

# Run unit test suite (Catch2)
ctest --test-dir build --output-on-failure
```
