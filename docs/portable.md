# Sol (Portable Linux Release)

Extract and install:

```sh
./install.sh
sol setup
sol
```

Or run directly from the extracted folder:

```sh
./sol setup
./sol
```

`sol setup` downloads the offline Whisper model into `~/.local/share/sol/speech`, asks which language(s) you speak, and sets up your dictation shortcut. Everything runs locally. For CPU-only machines or smaller models, run `sol setup cpu small` (or `sol setup cpu base`).

Manage Sol from the CLI:

```sh
sol settings          # Configure languages, shortcut, mic, style, and HUD
sol shortcuts         # View or change global shortcut
sol doctor            # Check GPU, audio, clipboard, HUD, and shortcut
sol uninstall         # Remove Sol (--purge deletes models and data too)
```

## System requirements

The portable archive bundles Qt 6 Core/Gui/DBus and Wayland/X11 platform plugins (no QML or OpenGL runtime required). It targets `x86_64` or `aarch64` Linux systems with glibc 2.36 or newer.

Host tools used for desktop integration:
- **Audio**: PulseAudio (`parec`/`paplay`), PipeWire (`pw-record`/`pw-play`), or ALSA (`arecord`/`aplay`)
- **Python 3**: AT-SPI2 focus context and XDG desktop portals (`python-gobject`)
- **Wayland**: `wl-clipboard`, `hyprctl` (Hyprland), `wtype` (Sway/Niri/wlroots), or XDG Desktop Portals (GNOME/KDE/COSMIC)
- **X11**: `xdotool`, `xclip` (or `xsel`), and `python-xlib`
- **GPU (optional)**: NVIDIA CUDA driver for `large-v3-turbo`; CPU mode works anywhere

## Build the portable archive

On Debian 12 (or via Docker with `bin/release --portable`):

```sh
apt-get update
apt-get install -y build-essential qt6-base-dev qt6-wayland python3 patchelf xz-utils
bin/package-portable
```
