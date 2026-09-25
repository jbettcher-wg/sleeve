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
6. **Unmet Guest Libraries, all at once**: `sleeve libs` walks the application's ELF objects — the executable, everything it ships (Electron bundles ffmpeg, ANGLE, swiftshader and native `.node` addons) and the transitive closure of what those need — resolves every `DT_NEEDED` soname the way the guest loader would, and reports the ones no layer satisfies. Sonames a real run named are merged in for what `dlopen` hides. `--packages` traces each one to a guest package with `pacman -F`, and `--install` installs the set into the overlay, after showing it.
7. **Guest Provisioning**: a missing rootfs is the same kind of unmet precondition as a missing library, so it is answered in the same place. `sleeve rootfs check` says which of "no rootfs", "wrong architecture", "no overlay" and "an overlay a half-finished build left behind" it is; `sleeve rootfs build` drives `POWERarmRootFSFetcher` to fix the two worth fixing automatically. Nothing about mirrors, manifests or the pacman bootstrap is reimplemented here.
8. **Omarchy Theming**: Automatically detects the active Omarchy theme and palette via `omarchy-theme-color`. Watches `~/.local/state/omarchy/current/` via inotify to re-tint running TUI sessions dynamically upon theme switches.
9. **CLI Twin**: Every action in the interactive FTXUI terminal interface has a direct CLI counterpart for scripting and AI agents.

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

# Unmet guest libraries, in one pass rather than one failed launch at a time.
# Plain `libs` is read-only and starts nothing: it walks the app's ELF objects and
# resolves every soname against the overlay, then the base rootfs, then the host.
sleeve libs code
sleeve libs code --json

# Trace the unmet sonames to guest packages (runs `pacman -F` under the emulator,
# which needs the file database -- see --sync below).
sleeve libs code --packages

# Install them. The package set and the exact command are printed first, and nothing
# is installed without --yes, because the overlay is shared by every guest on the
# machine. --dry-run prints the set and stops.
sleeve libs code --install --dry-run
sleeve libs code --install --yes

# Static analysis cannot see a dlopen. By default the sonames from the newest health
# log are merged in; --run takes a fresh one, --no-log ignores them.
sleeve libs code --run

# Refresh the guest file database (pacman -Fy; downloads into the overlay)
sleeve libs code --sync --yes

# Is there a usable guest here at all? Non-zero when there is not.
sleeve rootfs check
sleeve rootfs check ArchLinuxARM-vk

# Build one. This drives POWERarmRootFSFetcher -- base, per-user overlay, guest pacman
# and the config write -- and downloads about 860 MB. --dry-run asks the fetcher to
# print its plan and stop.
sleeve rootfs build --dry-run
sleeve rootfs build --yes
sleeve libs code --build --yes      # the same thing, from where the problem showed up

# RootFS management. Discovery is scoped to the active backend and the guest architecture
# is read from the tree itself, so an x86_64 rootfs is never offered to an AArch64 guest.
# A name that does not resolve is refused rather than silently falling back to host binaries.
sleeve rootfs list
sleeve rootfs use ArchLinuxARM-m2

# Theme inspection
sleeve theme
```

---

## Interactive TUI

`sleeve` with no arguments opens the terminal interface.

| key | does |
|---|---|
| `Tab` / `Shift+Tab` | move between screens |
| `Up` / `Down` | move the selection |
| `Enter` | open the selected app's options |
| `s` | scan for applications |
| `w` | build the launcher / desktop / AppConfig preview, then write |
| `h` | health check the selected app — shows the command line first and launches nothing until you confirm |
| `l` | read what the selected app needs, and open the Libraries screen |
| `t` | reload the theme |
| `Esc` | back a screen, or cancel a running action |
| `q` | quit |

On the Libraries screen: `p` traces the unmet sonames to packages, `i` installs them, `y` refreshes
the guest file database, `B` builds a rootfs when there is none, `b` goes back. Every one of the
three that writes anything — the install, the database refresh, the build — puts up the same
confirmation panel the health check uses, naming the exact command, and does nothing until it is
accepted.

Anything that takes time — scanning, health checks, theme reloads, building a preview, writing files
— runs off the event loop. The keypress repaints immediately into a panel with a live spinner and an
elapsed counter, the rest of the interface keeps responding while the work runs, and `Esc` cancels.
One action runs at a time: pressing the same key again says it is still going rather than starting a
second. Writing files is the one exception that cannot be interrupted — a launcher pointing at an
AppConfig that was never written is worse than waiting for three short files.

---

## How a soname is decided to be unmet

The emulator's rootfs is two trees: a read-only base, and a per-user overlay that shadows it
under the guest-owned prefixes (`/usr`, `/etc`, `/opt`, `/var/lib/pacman`, `/var/cache/pacman`).
`sleeve libs` resolves each soname through them in the same order the emulator does, so that what
it reports is what the guest loader would find:

1. **`DT_RPATH` of the object that names it**, when that object has no `DT_RUNPATH` — plus the main
   executable's, which is how a bundled application finds what it ships from one of its own
   libraries. `$ORIGIN`, `$LIB` and `$PLATFORM` are expanded against the object's own directory.
2. **`LD_LIBRARY_PATH`**, taken from the record's environment, which is exactly what the generated
   launcher exports.
3. **`DT_RUNPATH` of the object that names it.**
4. **The directories `/etc/ld.so.conf` names**, read through the layers, includes and globs
   followed. The binary `ld.so.cache` is deliberately not parsed: it is generated from these
   directories, and a fresh overlay has not built one yet.
5. **`/lib`, `/usr/lib`, `/lib64`, `/usr/lib64`.**

Each candidate path is then resolved **overlay first, base second, host last**. A symlink whose
target is absolute is re-rooted at the layer stack rather than at the host's `/`, so Arch's
`/lib -> usr/lib` and the usual `libfoo.so -> libfoo.so.1` reach the guest's copy. Overlay
whiteouts (`.wh.<name>`, `.wh..wh..opq`) hide what the base has, and `/var/lib/pacman` never falls
through to the host at all.

**Found is not the same as satisfied.** The host filesystem is ppc64le, so a library that only the
host has is reported as unmet with that said explicitly — the guest loader opens it, rejects the
machine and carries on searching, which is why the error a person sees is still "cannot open shared
object file".

**`dlopen` is the gap, and it is filled from the other side.** No ELF header names a library loaded
by name at run time, and that is how most of Electron's optional pieces (PulseAudio, the speech
stack) load. `sleeve libs` merges in every `<soname>: cannot open shared object file` from the
newest health log, marks those rows `run` rather than `static`, and `--run` takes a fresh check
first. A soname a run could not open that now resolves is *not* put back on the list: that log
predates whatever fixed it.

---

## Settings

`~/.config/<emulator>/sleeve/settings.json`:

```json
{
  "apps_dir": "",
  "last_theme": "",
  "scan_dirs": [],
  "rootfs_mirrors": []
}
```

`rootfs_mirrors` is handed to `POWERarmRootFSFetcher` as repeated `--mirror`, in order, when
sleeve builds a rootfs. It is empty by default and sleeve supplies no mirror of its own: the
fetcher already knows POWERarm's pinned snapshot and upstream Arch Linux ARM, and a second opinion
here could only drift from the emulator's. Adding a mirror later is an edit to this file.

---

## Building and Testing

Requirements: C++20 compiler (`g++` or `clang++`), `cmake` >= 3.20, and `ninja`.

```sh
git clone https://github.com/jbettcher/sleeve.git
cd sleeve
git submodule update --init --recursive

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build

# Run unit test suite (Catch2) -- 69 tests
ctest --test-dir build --output-on-failure
```
