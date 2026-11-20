#!/usr/bin/env bash
# Universal Linux installer for Sol (installs to ~/.local without sudo).
set -euo pipefail

root=$(cd -- "$(dirname -- "$0")" && pwd)
prefix="${SOL_PREFIX:-$HOME/.local}"
data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
config_home="${XDG_CONFIG_HOME:-$HOME/.config}"
enable_autostart=true
install_model=false
model_mode="auto"
model_tier="auto"
do_uninstall=false
uninstall_args=()

usage() {
    cat <<'EOF'
Sol Installer

Usage:
  ./install.sh [OPTIONS]

Options:
  --prefix DIR           Install prefix (default: ~/.local)
  --no-autostart         Do not enable background daemon at login
  --with-model           Also run 'sol setup' right after install
  --cpu                  Use CPU-only speech model (with --with-model)
  --model TIER           Model tier: auto|tiny|base|small|medium|large-v3-turbo
  --uninstall            Uninstall Sol instead of installing
  --purge                With --uninstall, also delete downloaded models and data
  --yes, -y              Non-interactive confirmation
  -h, --help             Show this help
EOF
}

while (( $# > 0 )); do
    case "$1" in
        --prefix) prefix=${2:?Missing prefix}; shift 2 ;;
        --no-autostart) enable_autostart=false; shift ;;
        --with-model) install_model=true; shift ;;
        --cpu) model_mode="cpu"; shift ;;
        --model) model_tier=${2:?Missing model}; shift 2 ;;
        --uninstall) do_uninstall=true; shift ;;
        --purge) uninstall_args+=(--purge); shift ;;
        --yes|-y) uninstall_args+=(--yes); shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if $do_uninstall; then
    if [[ -x "$prefix/bin/sol" ]]; then
        exec env SOL_PREFIX="$prefix" "$prefix/bin/sol" uninstall "${uninstall_args[@]}"
    elif command -v sol >/dev/null 2>&1; then
        exec env SOL_PREFIX="$prefix" sol uninstall "${uninstall_args[@]}"
    else
        exec env SOL_PREFIX="$prefix" "$root/bin/uninstall" "${uninstall_args[@]}"
    fi
fi

binary=""
portable_bundle=false
if [[ -x "$root/bin/sol" && -f "$root/bin/qt.conf" ]]; then
    binary="$root/bin/sol"
    portable_bundle=true
elif [[ -f "$root/sol.pro" ]]; then
    missing=()
    for cmd in qmake6 make g++; do
        command -v "$cmd" >/dev/null 2>&1 || missing+=("$cmd")
    done
    if (( ${#missing[@]} > 0 )); then
        echo "Missing build tools: ${missing[*]}" >&2
        echo "" >&2
        echo "Install Qt 6 base dependencies for your distro:" >&2
        echo "  Arch / Omarchy:  sudo pacman -S base-devel qt6-base qt6-wayland layer-shell-qt" >&2
        echo "  Ubuntu / Debian: sudo apt install build-essential qt6-base-dev qt6-wayland" >&2
        echo "  Fedora:          sudo dnf install gcc-c++ make qt6-qtbase-devel qt6-qtwayland layer-shell-qt-devel" >&2
        echo "  openSUSE:        sudo zypper install gcc-c++ make qt6-base-devel qt6-wayland layer-shell-qt6-devel" >&2
        exit 1
    fi
    echo "Building Sol..."
    "$root/bin/build"
    binary="$root/build/sol"
else
    echo "Could not find Sol sources or prebuilt binary in $root." >&2
    exit 1
fi

if command -v sol >/dev/null 2>&1; then
    sol quit >/dev/null 2>&1 || true
fi

install -d "$prefix/bin" "$prefix/libexec/sol" \
    "$data_home/applications" "$data_home/icons/hicolor/scalable/apps"

if $portable_bundle; then
    rm -rf "$prefix/lib/sol"
    install -d "$prefix/lib/sol"
    cp -a "$root"/. "$prefix/lib/sol/"
    ln -sf "../lib/sol/bin/sol" "$prefix/bin/sol"
    ln -sf "../lib/sol/libexec/sol/setup-speech" "$prefix/bin/sol-setup-speech"
    icon_src="$root/share/sol/sol.svg"
else
    install -m755 "$binary" "$prefix/bin/sol"
    install -m755 "$root/bin/setup-speech" "$prefix/libexec/sol/setup-speech"
    ln -sf "../libexec/sol/setup-speech" "$prefix/bin/sol-setup-speech"
    icon_src="$root/pkgbuild/sol.svg"
fi

if [[ -f "$icon_src" ]]; then
    install -m644 "$icon_src" "$data_home/icons/hicolor/scalable/apps/org.sol.Dictation.svg"
fi

desktop_src="$root/pkgbuild/org.sol.Dictation.desktop"
if [[ -f "$desktop_src" ]]; then
    sed "s|^Exec=sol |Exec=$prefix/bin/sol |g" "$desktop_src" > "$data_home/applications/org.sol.Dictation.desktop"
    chmod 644 "$data_home/applications/org.sol.Dictation.desktop"
fi

if $enable_autostart; then
    install -d "$config_home/autostart"
    cat > "$config_home/autostart/org.sol.Dictation.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Sol
Comment=Local speech-to-text dictation
Exec=$prefix/bin/sol daemon
Icon=org.sol.Dictation
Terminal=false
Categories=Utility;Accessibility;
X-GNOME-Autostart-enabled=true
EOF
fi

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$data_home/applications" >/dev/null 2>&1 || true
fi

echo ""
echo "  + Sol installed to $prefix/bin/sol"
case ":$PATH:" in
    *":$prefix/bin:"*) ;;
    *) echo "  ! Add $prefix/bin to PATH:  export PATH=\"$prefix/bin:\$PATH\"" ;;
esac

if $install_model; then
    echo ""
    "$prefix/bin/sol" setup "$model_mode" "$model_tier"
fi

echo ""
echo "  Next steps:"
echo "    sol setup         Install offline Whisper model, languages, and shortcut"
echo "    sol               Start background daemon and show status"
echo "    sol settings      Configure languages, shortcut, mic, style, and HUD"
echo "    sol doctor        Check GPU, audio, clipboard, HUD, and shortcut"
echo "    sol uninstall     Remove Sol (--purge deletes models and data too)"
