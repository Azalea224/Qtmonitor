# Qtmonitor

A Qt6/KF6 resource and task monitor for Linux — the clear, at-a-glance layout
Windows Task Manager is known for, delivered as a Linux-native tool. Targets
Arch Linux (CachyOS included) and runs correctly on any desktop environment
or window manager (Plasma, GNOME, LXQt, Hyprland, Niri, sway, ...).

## Status

Early development. See the phase plan below; the current milestone is noted
under "Build".

## Dependencies

All packages are from the official Arch repositories. KF6 libraries are
packaged under their plain names (`kauth`, `kconfig`, `solid`) — there is no
`kf6-` prefix in Arch package names.

### Required (build time)

| Package | Purpose |
|---|---|
| `cmake` | Build system |
| `ninja` (or `make`) | Build tool |
| `gcc` | C++17 compiler |
| `extra/qt6-base` | Qt6 Widgets |
| `extra/kauth` | Privileged actions via polkit (KAuth) |
| `extra/kconfig` | Settings storage (KConfig) |
| `extra/solid` | Hardware/device info (Solid) |
| `extra/qt6-charts` | Performance tab graphs (Phase 2 onward) |

### Optional (runtime, auto-detected)

Qtmonitor probes for these at startup with `QStandardPaths::findExecutable`.
If a tool is missing, the corresponding UI section is hidden with an inline
hint — never a crash or blank panel.

| Package | Enables |
|---|---|
| `nvidia-utils` | NVIDIA GPU stats via `nvidia-smi` (AMD/Intel fall back to `/sys/class/drm`) |
| `lm_sensors` | Temperature/fan sensors via `sensors -j` |

## Build

```bash
cmake -B build -G Ninja
cmake --build build
./build/qtmonitor
```

Install system-wide (binary + desktop entry):

```bash
sudo cmake --install build
```

## Design notes

- Core metrics (CPU, memory, swap, disk I/O, network I/O, process list) are
  read directly from `/proc` and `/sys` — no heavyweight system monitor
  daemon required.
- Theming follows Qt's active platform theme and the system palette; no
  Breeze or Plasma assumption anywhere. A manual dark/light override will be
  available via `QPalette`.
- Icons resolve through `QIcon::fromTheme()` with bundled fallbacks, so the
  app looks correct under any icon theme.
- Privileged actions (ending another user's process, changing nice values)
  go through KAuth/polkit's standard dialog. Actions on your own processes
  need no elevation.

## License

GPL-3.0. See [LICENSE](LICENSE).
