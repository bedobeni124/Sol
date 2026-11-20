"""Inspect the focused Linux field and deliver dictations across Wayland and X11. All content stays local."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import threading
import time

TERMINAL_RE = re.compile(
    r"term|kitty|konsole|ghostty|alacritty|tilix|foot|ptyxis|contour|wezterm|warp|"
    r"urxvt|rxvt|sakura|terminator|guake|tilda|yakuake|rio|blackbox|kgx|zutty|tabby|hyper",
    re.I,
)
URL_BAR_RE = re.compile(
    r"url[\s_-]*bar|address|omnibox|location|enter\s+url|web\s+address|search\s+or\s+enter|"
    r"search\s+with|search\s+bar|search\s+box|find\s+in|what\s+do\s+you\s+want\s+to\s+play|"
    r"c[íi]m|keres[ée]s|adresse|suche|buscar|rechercher",
    re.I,
)
URL_TEXT_RE = re.compile(
    r"^\s*(?:https?://|www\.|about:|chrome://|brave://|edge://|vivaldi://|opera://|file://|localhost(?::\d+)?(?:/|$)|[a-z0-9-]+\.(?:com|org|net|io|ai|dev|co|edu|gov|app|me|uk|de|fr|hu|eu)(?:/|$))",
    re.I,
)
CODE_EXTENSIONS = {
    ".cpp", ".cc", ".cxx", ".hpp", ".h", ".c", ".py", ".qml", ".js", ".jsx", ".ts", ".tsx",
    ".rs", ".go", ".java", ".kt", ".swift", ".rb", ".php", ".cs", ".sh", ".json", ".yaml",
    ".yml", ".toml", ".xml", ".html", ".css", ".scss", ".sql", ".md", ".pro", ".cmake", ".lua", ".nix",
}
SKIP_DIRS = {
    ".git", ".hg", ".svn", "node_modules", "build", "dist", "target", "__pycache__",
    ".venv", "venv", ".cache", ".next", ".nuxt", "vendor", "out",
}

_ATSPI = None
_ATSPI_TRIED = False


def get_atspi():
    global _ATSPI, _ATSPI_TRIED
    if not _ATSPI_TRIED:
        _ATSPI_TRIED = True
        try:
            import gi
            gi.require_version("Atspi", "2.0")
            from gi.repository import Atspi as A
            A.set_timeout(90, 150)
            _ATSPI = A
        except (ImportError, ValueError, RuntimeError):
            _ATSPI = None
    return _ATSPI


def run(args, timeout=0.8):
    return subprocess.check_output(args, stderr=subprocess.DEVNULL, timeout=timeout).decode(errors="replace").strip()


def sway_focus(node):
    if node.get("focused"):
        return node
    for child in node.get("nodes", []) + node.get("floating_nodes", []):
        found = sway_focus(child)
        if found:
            return found
    return {}


def window():
    if os.getenv("HYPRLAND_INSTANCE_SIGNATURE") and shutil.which("hyprctl"):
        try:
            node = json.loads(run(["hyprctl", "-j", "activewindow"]))
            return dict(
                platform="hyprland",
                window=node.get("address", ""),
                app=node.get("class", ""),
                title=node.get("title", ""),
                pid=node.get("pid", 0),
                xwayland=bool(node.get("xwayland", False)),
            )
        except (OSError, subprocess.SubprocessError, ValueError):
            pass
    if os.getenv("SWAYSOCK") and shutil.which("swaymsg"):
        try:
            node = sway_focus(json.loads(run(["swaymsg", "-t", "get_tree", "-r"])))
            return dict(
                platform="sway",
                window=str(node.get("id", "")),
                app=node.get("app_id") or (node.get("window_properties") or {}).get("class", ""),
                title=node.get("name", ""),
                pid=node.get("pid", 0),
                xwayland=bool(node.get("window_properties") and not node.get("app_id")),
            )
        except (OSError, subprocess.SubprocessError, ValueError):
            pass
    if os.getenv("NIRI_SOCKET") and shutil.which("niri"):
        try:
            node = json.loads(run(["niri", "msg", "-j", "focused-window"]))
            return dict(
                platform="niri",
                window=str(node.get("id", "")),
                app=node.get("app_id", ""),
                title=node.get("title", ""),
                pid=node.get("pid", 0),
                xwayland=False,
            )
        except (OSError, subprocess.SubprocessError, ValueError):
            pass
    if not os.getenv("WAYLAND_DISPLAY") and os.getenv("DISPLAY") and shutil.which("xdotool"):
        try:
            wid = run(["xdotool", "getactivewindow"])
            info = dict(platform="x11", window=wid)
            for key, cmd in (("app", "getwindowclassname"), ("title", "getwindowname"), ("pid", "getwindowpid")):
                try:
                    info[key] = run(["xdotool", cmd, wid])
                except (OSError, subprocess.SubprocessError):
                    pass
            return info
        except (OSError, subprocess.SubprocessError):
            pass
    return dict(platform="accessibility")


def workspace_files(pid):
    """Best-effort fast scan of relevant source filenames in the focused process working directory."""
    try:
        pid_int = int(pid)
    except (TypeError, ValueError):
        return []
    if pid_int <= 0:
        return []
    candidates = []
    try:
        cwd = Path(os.readlink(f"/proc/{pid_int}/cwd"))
        children_path = Path(f"/proc/{pid_int}/task/{pid_int}/children")
        if children_path.exists():
            for child in children_path.read_text(errors="replace").split()[:4]:
                try:
                    child_cwd = Path(os.readlink(f"/proc/{child}/cwd"))
                    if child_cwd.is_dir() and child_cwd != Path.home():
                        cwd = child_cwd
                        break
                except OSError:
                    pass
        if not cwd.is_dir() or cwd in (Path("/"), Path.home()):
            return []
        for entry in sorted(cwd.iterdir(), key=lambda p: p.name)[:80]:
            name = entry.name
            if name.startswith(".") or name in SKIP_DIRS:
                continue
            if entry.is_file() and entry.suffix.lower() in CODE_EXTENSIONS:
                candidates.append(name)
            elif entry.is_dir():
                try:
                    for sub in sorted(entry.iterdir(), key=lambda p: p.name)[:60]:
                        if sub.is_file() and sub.suffix.lower() in CODE_EXTENSIONS:
                            candidates.append(sub.name)
                except OSError:
                    pass
            if len(candidates) >= 100:
                break
    except OSError:
        return []
    seen = set()
    unique = []
    for item in candidates:
        if item not in seen:
            seen.add(item)
            unique.append(item)
    return unique


def is_url_bar_field(role_name="", name="", description="", attrs=None, parent_name="", sample_text=""):
    """Detect browser address/URL bars and search inputs from AT-SPI metadata or URL-like field text."""
    role = (role_name or "").lower()
    if role in ("autocomplete", "searchbox", "search box"):
        return True
    if attrs:
        input_type = str(attrs.get("text-input-type") or attrs.get("type") or "").lower()
        if input_type in ("url", "search"):
            return True
        for k in ("id", "class", "xml-roles", "placeholder-text"):
            val = str(attrs.get(k) or "")
            if val and URL_BAR_RE.search(val):
                return True
    for text in (name, description, parent_name):
        if text and URL_BAR_RE.search(text):
            return True
    if sample_text and URL_TEXT_RE.match(sample_text):
        return True
    return False


def accessible(info, read_text):
    A = get_atspi()
    if A is None:
        return None
    root = A.get_desktop(0)
    if root is None:
        return None
    all_match = A.CollectionMatchType.ALL
    rule = A.MatchRule.new(
        A.StateSet.new([A.StateType.FOCUSED]),
        all_match, {}, all_match, [], all_match, [], all_match, False,
    )
    for i in range(root.get_child_count()):
        try:
            app = root.get_child_at_index(i)
            pid = app.get_process_id()
            if info.get("pid") and int(info["pid"]) != pid:
                continue
            wins = [app.get_child_at_index(j) for j in range(min(app.get_child_count(), 64))]
            active = [w for w in wins if w and w.get_state_set().contains(A.StateType.ACTIVE)]
            if not info.get("window") and not active:
                continue
            if not info.get("window"):
                info.update(window=f"{pid}:{active[0].path}", pid=pid, app=app.get_name() or "")
                if read_text:
                    info["title"] = active[0].get_name() or ""
            found = []
            try:
                found = app.get_matches(rule, A.CollectionSortOrder.CANONICAL, 3, True)
            except Exception:
                pass
            if not found:
                stack = (active or wins)[:]
                for _ in range(192):
                    if not stack:
                        break
                    node = stack.pop()
                    if not node:
                        continue
                    if node.get_state_set().contains(A.StateType.FOCUSED):
                        found = [node]
                        break
                    stack.extend(node.get_child_at_index(j) for j in range(min(node.get_child_count(), 40)))
            for field in found:
                info["field"] = f"{pid}:{field.path}"
                if field.get_role() == A.Role.PASSWORD_TEXT:
                    info["password"] = True
                    return None
                if "Text" not in field.get_interfaces():
                    continue
                txt = field.get_text_iface()
                start = end = txt.get_caret_offset()
                if txt.get_n_selections():
                    sel = txt.get_selection(0)
                    start, end = sorted((sel.start_offset, sel.end_offset))
                if start < 0:
                    return None
                info.update(start=start, end=end)
                sample_text = ""
                try:
                    sample_text = txt.get_text(0, min(txt.get_character_count(), 160)) or ""
                except Exception:
                    sample_text = ""
                try:
                    role_name = field.get_role_name() or ""
                    fname = field.get_name() or ""
                    fdesc = field.get_description() or ""
                    fattrs = field.get_attributes() or {}
                    parent = field.get_parent()
                    pname = (parent.get_name() or "") if parent else ""
                    if is_url_bar_field(role_name, fname, fdesc, fattrs, pname, sample_text):
                        info["urlBar"] = True
                except Exception:
                    if sample_text and URL_TEXT_RE.match(sample_text):
                        info["urlBar"] = True
                if read_text:
                    info["before"] = txt.get_text(max(0, start - 240), start) or ""
                    info["after"] = txt.get_text(end, min(txt.get_character_count(), end + 120)) or ""
                return field
        except Exception:
            continue
    return None


def inspect(read_text, code_context=False):
    info = window()
    try:
        field = accessible(info, read_text)
    except (ImportError, ValueError, RuntimeError):
        field = None
    if code_context and info.get("pid"):
        files = workspace_files(info.get("pid"))
        if files:
            info["files"] = files
    if not read_text:
        info.pop("title", None)
    return info, field


def read_clipboard(wayland):
    try:
        if wayland and shutil.which("wl-paste"):
            out = subprocess.check_output(
                ["wl-paste", "--no-newline"],
                stderr=subprocess.DEVNULL,
                timeout=0.4,
            )
            return out if len(out) <= 512 * 1024 else None
        if not wayland:
            if shutil.which("xclip"):
                out = subprocess.check_output(
                    ["xclip", "-o", "-selection", "clipboard"],
                    stderr=subprocess.DEVNULL,
                    timeout=0.4,
                )
                return out if len(out) <= 512 * 1024 else None
            if shutil.which("xsel"):
                out = subprocess.check_output(
                    ["xsel", "-ob"],
                    stderr=subprocess.DEVNULL,
                    timeout=0.4,
                )
                return out if len(out) <= 512 * 1024 else None
    except (OSError, subprocess.SubprocessError):
        return None
    return None


def write_clipboard_bytes(wayland, data):
    # Omit --type on wl-copy so it advertises text/plain, UTF8_STRING, STRING, and
    # text/plain;charset=utf-8 — required for XWayland apps like Spotify.
    if wayland and shutil.which("wl-copy"):
        subprocess.run(
            ["wl-copy"],
            input=data,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=True,
            timeout=2,
        )
        return True
    if not wayland:
        if shutil.which("xclip"):
            subprocess.run(
                ["xclip", "-selection", "clipboard", "-in"],
                input=data,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=True,
                timeout=2,
            )
            return True
        if shutil.which("xsel"):
            subprocess.run(
                ["xsel", "-ib"],
                input=data,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=True,
                timeout=2,
            )
            return True
    return False


def schedule_clipboard_restore(wayland, previous, pasted_bytes):
    if previous is None or previous == pasted_bytes:
        return

    def restore():
        time.sleep(0.80)
        try:
            current = read_clipboard(wayland)
            if current == pasted_bytes:
                write_clipboard_bytes(wayland, previous)
        except Exception:
            pass

    threading.Thread(target=restore, daemon=True).start()


def send_hyprland_paste(terminal, window_addr=""):
    """Send Ctrl+V (or Ctrl+Shift+V) via Hyprland down/up send_key_state so modifiers never stick."""
    mods = "CTRL SHIFT" if terminal else "CTRL"

    # 1. Hyprland 0.50+ Lua dispatcher (omitting window target reaches Wayland, XWayland & overlays).
    try:
        down = run([
            "hyprctl", "dispatch",
            f'hl.dsp.send_key_state({{ mods = "{mods}", key = "V", state = "down" }})',
        ])
        if down.lower() == "ok":
            time.sleep(0.045)
            run([
                "hyprctl", "dispatch",
                f'hl.dsp.send_key_state({{ mods = "{mods}", key = "V", state = "up" }})',
            ])
            return True
    except (OSError, subprocess.SubprocessError):
        pass

    # 2. Legacy Hyprland sendkeystate down/up.
    try:
        down = run(["hyprctl", "dispatch", "sendkeystate", f"{mods}, V, down"])
        if down.lower() == "ok":
            time.sleep(0.045)
            run(["hyprctl", "dispatch", "sendkeystate", f"{mods}, V, up"])
            return True
    except (OSError, subprocess.SubprocessError):
        pass

    # 3. Older Hyprland fallback: sendshortcut.
    if window_addr:
        try:
            reply = run(["hyprctl", "dispatch", "sendshortcut", f"{mods},V,address:{window_addr}"])
            if reply.lower() == "ok":
                return True
        except (OSError, subprocess.SubprocessError):
            pass
    return False


def send_wtype_paste(terminal):
    if not shutil.which("wtype"):
        return False
    # Always release modifiers (-m) explicitly so CTRL/SHIFT never remain stuck.
    keys = (
        ["-M", "ctrl", "-M", "shift", "-k", "v", "-m", "shift", "-m", "ctrl"]
        if terminal
        else ["-M", "ctrl", "-k", "v", "-m", "ctrl"]
    )
    subprocess.run(["wtype", *keys], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True, timeout=1.5)
    return True


def send_ydotool_paste(terminal):
    if shutil.which("ydotool"):
        # Linux evdev keycodes: 29 = LEFTCTRL, 42 = LEFTSHIFT, 47 = V
        seq = ["29:1", "42:1", "47:1", "47:0", "42:0", "29:0"] if terminal else ["29:1", "47:1", "47:0", "29:0"]
        subprocess.run(["ydotool", "key", *seq], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True, timeout=1.5)
        return True
    if shutil.which("dotool"):
        chord = "key ctrl+shift+v\n" if terminal else "key ctrl+v\n"
        subprocess.run(["dotool"], input=chord.encode(), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True, timeout=1.5)
        return True
    return False


def deliver(original, text, restore_clipboard=True, relaxed=False):
    current, field = inspect(False)
    if current.get("password"):
        return "Blocked: focused field is a password input."
    payload = text.encode("utf-8")
    wayland_env = bool(os.getenv("WAYLAND_DISPLAY"))

    if not relaxed and original.get("window"):
        if not current.get("window") or original["window"] != current.get("window"):
            copied = write_clipboard_bytes(wayland_env, payload) if (wayland_env or os.getenv("DISPLAY")) else False
            return "Focus changed -- copied to clipboard (Ctrl+V)." if copied else "Focus changed. Run 'sol history'."

    platform = current["platform"]
    terminal = bool(TERMINAL_RE.search(current.get("app", "")))
    xwayland = bool(current.get("xwayland", False))

    # Fast native path on X11, Hyprland, Sway, Niri.
    if platform in ("hyprland", "sway", "niri", "x11"):
        wayland = platform != "x11"
        previous = read_clipboard(wayland) if restore_clipboard else None
        if platform == "x11":
            if write_clipboard_bytes(False, payload):
                if not relaxed and original.get("window") and window().get("window") != current["window"]:
                    return "Focus changed -- copied to clipboard (Ctrl+V)."
                run(["xdotool", "key", "--clearmodifiers", "ctrl+shift+v" if terminal else "ctrl+v"])
                schedule_clipboard_restore(False, previous, payload)
                return ""
            if shutil.which("xdotool"):
                run(["xdotool", "type", "--clearmodifiers", "--delay", "1", "--", text], timeout=4)
                return ""
        else:
            if write_clipboard_bytes(True, payload):
                # Give Wayland / XWayland clipboard bridge time to sync (critical for Spotify & Electron).
                time.sleep(0.08 if xwayland else 0.025)
                if not relaxed and original.get("window") and window().get("window") != current["window"]:
                    return "Focus changed -- copied to clipboard (Ctrl+V)."
                if platform == "hyprland" and send_hyprland_paste(terminal, current.get("window", "")):
                    schedule_clipboard_restore(True, previous, payload)
                    return ""
                try:
                    if send_wtype_paste(terminal):
                        schedule_clipboard_restore(True, previous, payload)
                        return ""
                except (OSError, subprocess.SubprocessError):
                    pass
                try:
                    if send_ydotool_paste(terminal):
                        schedule_clipboard_restore(True, previous, payload)
                        return ""
                except (OSError, subprocess.SubprocessError):
                    pass
                return "Copied to clipboard -- press Ctrl+V to paste."

    # Universal AT-SPI2 direct insertion (GNOME, KDE, GTK, Qt, Firefox, LibreOffice).
    if field and "EditableText" in field.get_interfaces():
        edit = field.get_editable_text_iface()
        start, end = current.get("start", 0), current.get("end", 0)
        if text == "":
            if end > start and edit.delete_text(start, end):
                field.get_text_iface().set_caret_offset(start)
                return ""
        elif edit.insert_text(end, text, len(payload)):
            if end > start and not edit.delete_text(start, end):
                return "The new text was inserted, but the previous selection is still present."
            field.get_text_iface().set_caret_offset(start + len(text))
            return ""

    # Fallback via ydotool/dotool on generic Wayland when available.
    if wayland_env and (shutil.which("ydotool") or shutil.which("dotool")):
        previous = read_clipboard(True) if restore_clipboard else None
        if write_clipboard_bytes(True, payload):
            time.sleep(0.05)
            try:
                if send_ydotool_paste(terminal):
                    schedule_clipboard_restore(True, previous, payload)
                    return ""
            except (OSError, subprocess.SubprocessError):
                pass

    # Final fallback: copy to clipboard so dictation is never lost.
    if write_clipboard_bytes(wayland_env, payload):
        return {"unsupported": True, "notice": "Copied to clipboard -- press Ctrl+V (or enable portal input)."}
    return {
        "unsupported": True,
        "notice": "Could not paste automatically. Run 'sol history' to view the text.",
    }


def handle(request):
    op = request.get("op")
    if op == "focus":
        answer, _ = inspect(
            bool(request.get("context", False)),
            code_context=bool(request.get("codeContext", False)),
        )
        return answer
    if op == "deliver":
        reply = deliver(
            request.get("target") or {},
            request.get("text", ""),
            restore_clipboard=bool(request.get("restoreClipboard", True)),
            relaxed=bool(request.get("relaxed", False)),
        )
        return reply if isinstance(reply, dict) else {"notice": reply}
    if op == "paste_text":
        reply = deliver(
            {},
            request.get("text", ""),
            restore_clipboard=bool(request.get("restoreClipboard", True)),
            relaxed=True,
        )
        return reply if isinstance(reply, dict) else {"notice": reply}
    return {"notice": f"Unknown desktop operation: {op}"}


def main():
    if "--worker" in sys.argv[1:]:
        get_atspi()
        print(json.dumps({"ready": True}), flush=True)
        for line in sys.stdin:
            line = line.strip()
            if not line:
                continue
            req_id = None
            try:
                request = json.loads(line)
                req_id = request.get("id")
                answer = handle(request)
            except Exception as error:
                answer = {"notice": f"Desktop integration unavailable ({error}). Run 'sol history'."}
            if req_id is not None:
                answer["id"] = req_id
            print(json.dumps(answer, ensure_ascii=False), flush=True)
        return

    request = json.load(sys.stdin)
    try:
        answer = handle(request)
    except Exception as error:
        answer = {"notice": f"Desktop integration unavailable ({error}). Run 'sol history'."}
    print(json.dumps(answer, ensure_ascii=False))


if __name__ == "__main__":
    main()
