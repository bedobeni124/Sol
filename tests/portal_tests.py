#!/usr/bin/env python3
"""Exercise Sol against a private D-Bus portal, with no live desktop permissions."""
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import threading
import unittest

SRC = Path(__file__).resolve().parents[1] / "src"
BASE = "/org/freedesktop/portal/desktop"
DBUS = "org.freedesktop.portal."
REMOTE = DBUS + "RemoteDesktop"
CLIP = DBUS + "Clipboard"
KEYS = DBUS + "GlobalShortcuts"


def serve(work):
    import dbus
    import dbus.service as service
    from dbus.mainloop.glib import DBusGMainLoop
    from gi.repository import GLib
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    name = service.BusName(DBUS + "Desktop", bus, do_not_queue=True)
    session = BASE + "/session/fake/sol"
    bound = []
    fds = []
    def log(**row):
        with (work / "log").open("a") as file:
            print(json.dumps(row), file=file)

    class Response(service.Object):
        @service.signal(DBUS + "Request", signature="ua{sv}")
        def Response(self, status, result):
            pass

    def request(opt, result):
        path = BASE + "/request/fake/" + str(opt["handle_token"])
        resp = Response(name, path)
        def reply():
            resp.Response(0, result)
            resp.remove_from_connection()
            return False
        GLib.timeout_add(5, reply)
        return dbus.ObjectPath(path)

    class Session(service.Object):
        @service.method(DBUS + "Session", in_signature="", out_signature="")
        def Close(self):
            log(action="close")

    class Remote(service.Object):
        @service.method(dbus.PROPERTIES_IFACE, in_signature="ss", out_signature="v")
        def Get(self, iface, prop):
            return dbus.UInt32(2)

        @service.method(REMOTE, in_signature="a{sv}", out_signature="o")
        def CreateSession(self, options):
            return request(options, {"session_handle": session})

        @service.method(REMOTE, in_signature="oa{sv}", out_signature="o")
        def SelectDevices(self, handle, options):
            log(types=int(options["types"]), restore=str(options.get("restore_token", "")))
            return request(options, {})

        @service.method(CLIP, in_signature="oa{sv}", out_signature="")
        def RequestClipboard(self, handle, options):
            log(action="request-clipboard")

        @service.method(REMOTE, in_signature="osa{sv}", out_signature="o")
        def Start(self, handle, window, options):
            return request(options, {"devices":dbus.UInt32(1), "clipboard_enabled":dbus.Boolean(True),
                                     "restore_token":"next-token"})

        @service.method(CLIP, in_signature="oa{sv}", out_signature="")
        def SetSelection(self, handle, options):
            log(mimes=list(options["mime_types"]))
            if (work / "change-on-select").exists():
                (work / "focus").write_text(json.dumps({"window": "other"}))

        @service.signal(CLIP, signature="osu")
        def SelectionTransfer(self, handle, mime, serial):
            pass

        @service.method(REMOTE, in_signature="oa{sv}iu", out_signature="")
        def NotifyKeyboardKeysym(self, handle, options, key, state):
            log(key=int(key), state=int(state))
            if key in [86,118] and state == 1:
                self.SelectionTransfer(session, "text/plain;charset=utf-8", 3)

        @service.method(CLIP, in_signature="ou", out_signature="h")
        def SelectionWrite(self, handle, serial):
            read, write = os.pipe()
            def capture():
                with os.fdopen(read, "rb") as file:
                    (work / "pasted").write_bytes(file.read())
            threading.Thread(target=capture, daemon=True).start()
            result = dbus.types.UnixFd(write)
            os.close(write)
            return result

        @service.method(CLIP, in_signature="oub", out_signature="")
        def SelectionWriteDone(self, handle, serial, success):
            log(transfer=bool(success))

    class Shortcuts(service.Object):
        @service.method(KEYS, in_signature="a{sv}", out_signature="o")
        def CreateSession(self, options):
            return request(options, {"session_handle":session})

        @service.method(KEYS, in_signature="oa{sv}", out_signature="o")
        def ListShortcuts(self, handle, options):
            return request(options, {"shortcuts":dbus.Array(bound, signature="(sa{sv})")})

        @service.method(KEYS, in_signature="oa(sa{sv})sa{sv}", out_signature="o")
        def BindShortcuts(self, handle, shortcuts, window, options):
            for key, values in shortcuts:
                bound.append((key, {"trigger_description": str(values["preferred_trigger"])}))
            GLib.timeout_add(100, self.emit_keys)
            return request(options, {"shortcuts":dbus.Array(bound, signature="(sa{sv})")})

        @service.signal(KEYS, signature="osta{sv}")
        def Activated(self, handle, key, timestamp, options):
            pass

        @service.signal(KEYS, signature="osta{sv}")
        def Deactivated(self, handle, key, timestamp, options):
            pass

        def emit_keys(self):
            self.Activated(session, "hold", 1, {})
            self.Deactivated(session, "hold", 2, {})
            self.Activated(session, "toggle", 3, {})
            self.Deactivated(session, "toggle", 4, {})
            return False

    sess = Session(name, session)
    portal = (Shortcuts if (work / "keys").exists() else Remote)(name, BASE)
    print("ready", flush=True)
    GLib.MainLoop().run()


class PortalTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.env = dict(os.environ, XDG_STATE_HOME=self.tmp.name, WAYLAND_DISPLAY="test-wayland", PYTHONPATH=str(SRC))
        self.target = {"window":"11:/app/main", "pid":11, "app":"Konsole", "field":"11:/app/term"}
        (self.root / "focus").write_text(json.dumps(self.target))
        self.pending = b""

    def start(self, shortcuts=False):
        if shortcuts:
            (self.root / "keys").touch()
        self.service = subprocess.Popen([sys.executable, __file__, "serve", self.tmp.name], stdout=subprocess.PIPE)
        self.addCleanup(self.stop, self.service)
        self.assertTrue(select.select([self.service.stdout], [], [], 3)[0])
        self.assertEqual(self.service.stdout.readline(), b"ready\n")
        if shortcuts:
            command = [sys.executable, "-u", str(SRC / "shortcuts.py")]
        else:
            command = [sys.executable, "-u", "-c",
                       "from portalinput import run; import json,sys; from pathlib import Path; "
                       "run(lambda:json.loads(Path(sys.argv[1]).read_text()))", str(self.root / "focus")]
        self.client = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=self.env)
        self.addCleanup(self.stop, self.client)
        return self.read()

    def stop(self, process):
        if process.stdin:
            process.stdin.close()
        if not process.stdin or process.poll() is None:
            try:
                process.wait(timeout=0.2)
            except subprocess.TimeoutExpired:
                process.terminate()
        process.wait(timeout=3)
        if process.stdout:
            process.stdout.close()

    def read(self):
        while b"\n" not in self.pending:
            self.assertTrue(select.select([self.client.stdout], [], [], 4)[0], "No portal response")
            data = os.read(self.client.stdout.fileno(), 4096)
            self.assertTrue(data, "Unexpected portal helper exit")
            self.pending += data
        line, _, self.pending = self.pending.partition(b"\n")
        return json.loads(line)

    def send(self, **msg):
        self.client.stdin.write((json.dumps(msg) + "\n").encode())
        self.client.stdin.flush()
        return self.read()

    def test_keyboard_clipboard(self):
        import time
        self.assertFalse(self.start()["ready"])
        self.assertFalse((self.root / "log").exists(), "Startup must not request input")
        self.assertIn("Enable", self.send(op="paste", id=7, target=self.target, text="hi")["notice"])
        tokens = self.root / "sol" / "portal-input-token"
        tokens.parent.mkdir()
        tokens.write_text("old")
        self.assertTrue(self.send(op="configure")["ready"])
        self.assertEqual(tokens.read_text(), "next-token")
        self.assertEqual(tokens.stat().st_mode & 0o777, 0o600)
        txt = "árvíztűrő tükörfúrógép\n予定"
        self.assertEqual(self.send(op="paste", target=self.target, id=1, text=txt), {"id":1, "notice":""})
        for _ in range(50):
            if '"transfer": true' in (self.root / "log").read_text(): break
            time.sleep(.02)
        self.assertEqual((self.root / "pasted").read_text(), txt)
        rows = [json.loads(l) for l in (self.root / "log").read_text().splitlines()]
        self.assertIn({"types":1, "restore":"old"}, rows)
        self.assertIn({"transfer":True}, rows)
        self.assertEqual([(r["key"], r["state"]) for r in rows if "key" in r],
                         [(0xffe3,1),(0xffe1,1),(86,1),(86,0),(0xffe1,0),(0xffe3,0)])
        # Force a focus change in the interval between setting selection and sending V.
        (self.root / "change-on-select").touch()
        self.assertIn("changed", self.send(op="paste", target=self.target, id=2, text="other")["notice"])
        self.assertEqual((self.root / "log").read_text().count('"key": 86, "state": 1'), 1)
        self.assertFalse(self.send(op="close")["ready"])
        self.assertFalse(tokens.exists())

    def test_shortcut_activation(self):
        self.assertIn("Configure", self.start(shortcuts=True)["info"])
        self.client.stdin.write(b"configure\n")
        self.client.stdin.flush()
        self.assertIn("hold", self.read())
        self.assertEqual([self.read()["action"] for _ in range(3)], ["start","stop","toggle"])


if __name__ == "__main__":
    if sys.argv[1:2] == ["serve"]:
        serve(Path(sys.argv[2]))
    elif sys.argv[1:2] == ["private"]:
        del sys.argv[1]
        unittest.main()
    else:
        # Never register a mock portal service on the user's real session bus.
        sys.exit(subprocess.call(["dbus-run-session", "--", sys.executable, __file__, "private", *sys.argv[1:]]))
