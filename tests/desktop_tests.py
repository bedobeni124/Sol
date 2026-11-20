#!/usr/bin/env python3
"""Tests for desktop worker, paste drivers, audio preprocessing, X11 chord parsing, and CLI install/uninstall."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
sys.path.insert(0, str(SRC))

import desktop
import shortcuts
import worker


class DesktopAndCliTests(unittest.TestCase):
    def test_worker_audio_preprocessing(self):
        try:
            import numpy as np
        except ImportError:
            self.skipTest("numpy not installed in system python")
        t = np.linspace(0, 0.25, 4000, dtype=np.float32)
        whisper_clip = (0.008 * np.sin(2 * np.pi * 440 * t) + 0.003).astype(np.float32)
        boosted = worker.preprocess(whisper_clip, whisper_mode=True)
        self.assertLess(abs(float(np.mean(boosted))), 0.01)
        self.assertGreater(float(np.sqrt(np.mean(boosted * boosted))), 0.06)
        self.assertLessEqual(float(np.max(np.abs(boosted))), 1.0)

        normal_clip = (0.5 * np.sin(2 * np.pi * 440 * t)).astype(np.float32)
        unchanged = worker.preprocess(normal_clip, whisper_mode=False)
        self.assertAlmostEqual(float(np.max(np.abs(unchanged))), 0.5, places=2)

    def test_x11_chord_parser(self):
        class FakeX:
            Mod4Mask = 1 << 6
            Mod1Mask = 1 << 3
            ControlMask = 1 << 2
            ShiftMask = 1 << 0

        class FakeXK:
            @staticmethod
            def string_to_keysym(name):
                table = {"v": 0x76, "space": 0x20, "F9": 0xffc6}
                return table.get(name, 0)

        class FakeDisplay:
            def keysym_to_keycode(self, sym):
                return sym & 0xff

        code, mods = shortcuts.parse_x11_chord("Super+Alt+V", FakeDisplay(), FakeX, FakeXK)
        self.assertEqual(code, 0x76)
        self.assertEqual(mods, FakeX.Mod4Mask | FakeX.Mod1Mask)

        code2, mods2 = shortcuts.parse_x11_chord("Ctrl+Space", FakeDisplay(), FakeX, FakeXK)
        self.assertEqual(code2, 0x20)
        self.assertEqual(mods2, FakeX.ControlMask)

        code3, mods3 = shortcuts.parse_x11_chord("F9", FakeDisplay(), FakeX, FakeXK)
        self.assertEqual(code3, 0xc6)
        self.assertEqual(mods3, 0)

    def test_hyprland_paste_uses_down_up_key_state_and_full_mime_wl_copy(self):
        calls = []

        def fake_run(args, timeout=0.8):
            calls.append(args)
            return "ok"

        with mock.patch.object(desktop, "run", side_effect=fake_run), mock.patch.object(desktop.time, "sleep"):
            self.assertTrue(desktop.send_hyprland_paste(terminal=False))
            self.assertEqual(len(calls), 2)
            self.assertIn('state = "down"', calls[0][2])
            self.assertIn('mods = "CTRL"', calls[0][2])
            self.assertIn('state = "up"', calls[1][2])

        # Verify wl-copy omits --type so XWayland apps (e.g. Spotify) receive UTF8_STRING/text/plain.
        with mock.patch.object(desktop.shutil, "which", return_value="/usr/bin/wl-copy"), \
             mock.patch.object(desktop.subprocess, "run") as mock_sub:
            self.assertTrue(desktop.write_clipboard_bytes(True, b"hello spotify"))
            self.assertEqual(mock_sub.call_args[0][0], ["wl-copy"])

        # Verify wtype explicitly releases modifiers with -m ctrl so CTRL never sticks.
        with mock.patch.object(desktop.shutil, "which", return_value="/usr/bin/wtype"), \
             mock.patch.object(desktop.subprocess, "run") as mock_sub:
            self.assertTrue(desktop.send_wtype_paste(terminal=False))
            cmd = mock_sub.call_args[0][0]
            self.assertEqual(cmd, ["wtype", "-M", "ctrl", "-k", "v", "-m", "ctrl"])

    def test_desktop_persistent_worker_guards(self):
        env = dict(os.environ)
        env.pop("HYPRLAND_INSTANCE_SIGNATURE", None)
        env.pop("SWAYSOCK", None)
        env.pop("NIRI_SOCKET", None)
        env.pop("WAYLAND_DISPLAY", None)
        env.pop("DISPLAY", None)
        proc = subprocess.Popen(
            [sys.executable, "-u", str(SRC / "desktop.py"), "--worker"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            env=env,
            text=True,
        )
        try:
            ready = json.loads(proc.stdout.readline())
            self.assertTrue(ready.get("ready"))

            proc.stdin.write(json.dumps({
                "id": 1,
                "op": "deliver",
                "target": {"window": "win-1"},
                "text": "hello",
            }) + "\n")
            proc.stdin.flush()
            resp = json.loads(proc.stdout.readline())
            self.assertEqual(resp["id"], 1)
            self.assertIn("Focus changed", resp["notice"])

            proc.stdin.write(json.dumps({"id": 2, "op": "bogus"}) + "\n")
            proc.stdin.flush()
            resp2 = json.loads(proc.stdout.readline())
            self.assertEqual(resp2["id"], 2)
            self.assertIn("Unknown", resp2["notice"])
        finally:
            proc.stdin.close()
            proc.stdout.close()
            proc.wait(timeout=3)

    def test_url_bar_detection(self):
        self.assertTrue(desktop.is_url_bar_field(name="Address and search bar"))
        self.assertTrue(desktop.is_url_bar_field(name="Search with Google or enter address"))
        self.assertTrue(desktop.is_url_bar_field(role_name="autocomplete"))
        self.assertTrue(desktop.is_url_bar_field(attrs={"id": "urlbar-input"}))
        self.assertTrue(desktop.is_url_bar_field(attrs={"text-input-type": "url"}))
        self.assertTrue(desktop.is_url_bar_field(sample_text="https://github.com/spn/sol"))
        self.assertTrue(desktop.is_url_bar_field(name="What do you want to play?"))
        self.assertFalse(desktop.is_url_bar_field(role_name="entry", name="Message body", sample_text="Hello there"))

    def test_workspace_files_scan(self):
        files = desktop.workspace_files(os.getpid())
        if Path.cwd() == ROOT:
            self.assertTrue(any(f.endswith(".cpp") for f in files))

    def test_cli_install_config_and_uninstall_sandbox(self):
        sol_bin = ROOT / "build" / "sol"
        if not sol_bin.is_file():
            self.skipTest("build/sol not compiled yet")
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            prefix = base / "local"
            data_home = base / "share"
            config_home = base / "config"
            state_home = base / "state"
            env = dict(
                os.environ,
                SOL_PREFIX=str(prefix),
                XDG_DATA_HOME=str(data_home),
                XDG_CONFIG_HOME=str(config_home),
                XDG_STATE_HOME=str(state_home),
                NO_COLOR="1",
            )
            env.pop("DBUS_SESSION_BUS_ADDRESS", None)

            res = subprocess.run(
                [str(ROOT / "install.sh"), "--prefix", str(prefix)],
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )
            self.assertIn("Sol installed", res.stdout)
            installed_sol = prefix / "bin" / "sol"
            self.assertTrue(installed_sol.is_file())
            self.assertTrue((data_home / "applications" / "org.sol.Dictation.desktop").is_file())
            self.assertTrue((data_home / "icons" / "hicolor" / "scalable" / "apps" / "org.sol.Dictation.svg").is_file())
            self.assertTrue((config_home / "autostart" / "org.sol.Dictation.desktop").is_file())

            # CLI settings: whisper, languages, hud, shortcut
            subprocess.run([str(installed_sol), "settings", "whisper", "on"], env=env, check=True, capture_output=True)
            self.assertEqual(subprocess.check_output([str(installed_sol), "settings", "whisper"], env=env, text=True).strip(), "true")

            subprocess.run([str(installed_sol), "settings", "languages", "en,hu"], env=env, check=True, capture_output=True)
            self.assertEqual(subprocess.check_output([str(installed_sol), "settings", "languages"], env=env, text=True).strip(), "auto (en, hu)")

            subprocess.run([str(installed_sol), "settings", "hud", "mono"], env=env, check=True, capture_output=True)
            self.assertEqual(subprocess.check_output([str(installed_sol), "settings", "theme"], env=env, text=True).strip(), "mono")

            subprocess.run([str(installed_sol), "settings", "hud", "glass"], env=env, check=True, capture_output=True)
            self.assertEqual(subprocess.check_output([str(installed_sol), "settings", "theme"], env=env, text=True).strip(), "glass")

            subprocess.run([str(installed_sol), "settings", "hud", "off"], env=env, check=True, capture_output=True)
            self.assertEqual(subprocess.check_output([str(installed_sol), "settings", "hud"], env=env, text=True).strip(), "false")

            subprocess.run([str(installed_sol), "style", "casual"], env=env, check=True, capture_output=True)
            self.assertIn("casual", subprocess.check_output([str(installed_sol), "style"], env=env, text=True))

            shortcuts_out = subprocess.check_output([str(installed_sol), "shortcuts"], env=env, text=True)
            self.assertIn("F9", shortcuts_out)

            subprocess.run([str(installed_sol), "dict", "add", "kube", "Kubernetes"], env=env, check=True, capture_output=True)
            dict_out = subprocess.check_output([str(installed_sol), "dict", "list"], env=env, text=True)
            self.assertIn("Kubernetes", dict_out)

            subprocess.run([str(installed_sol), "snippet", "add", "sig", "Cheers, Sol"], env=env, check=True, capture_output=True)
            snip_out = subprocess.check_output([str(installed_sol), "snippet", "list"], env=env, text=True)
            self.assertIn("Cheers, Sol", snip_out)

            stats_out = subprocess.check_output([str(installed_sol), "stats", "--json"], env=env, text=True)
            self.assertIn('"totalWords":0', stats_out)

            un = subprocess.run(
                [str(installed_sol), "uninstall", "--purge", "--yes"],
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )
            self.assertIn("Sol uninstalled", un.stdout)
            self.assertFalse(installed_sol.exists())
            self.assertFalse((config_home / "autostart" / "org.sol.Dictation.desktop").exists())
            self.assertFalse((data_home / "applications" / "org.sol.Dictation.desktop").exists())
            self.assertFalse((data_home / "sol").exists())


if __name__ == "__main__":
    unittest.main()
