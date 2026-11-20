"""Opt-in keyboard and clipboard session for accessibility-only Wayland desktops."""
import json
import os
from pathlib import Path
import re
import select
import sys
import tempfile
import threading
import uuid


PORTAL = "org.freedesktop.portal.Desktop"
PATH = "/org/freedesktop/portal/desktop"
REMOTE = "org.freedesktop.portal.RemoteDesktop"
CLIP = "org.freedesktop.portal.Clipboard"
SESSION = "org.freedesktop.portal.Session"
REQUEST = "org.freedesktop.portal.Request"
MIMES = ["text/plain;charset=utf-8", "text/plain"]
DISABLED = "Enable keyboard and clipboard access in Sol Settings, or copy the dictation from Sol."
TERMINAL_RE = re.compile(
    r"term|kitty|konsole|ghostty|alacritty|tilix|foot|ptyxis|contour|wezterm|warp|"
    r"urxvt|rxvt|sakura|terminator|guake|tilda|yakuake|rio|blackbox|kgx|zutty|tabby|hyper",
    re.I,
)


def report(**message):
    print(json.dumps(message), flush=True)


def verify_target(original, current, relaxed=False):
    if original.get("password") or current.get("password"):
        raise ValueError("Sol kept the dictation out of the password field.")
    if relaxed:
        return
    if not original.get("window") or not current.get("window"):
        raise ValueError("Sol could not identify the active window. Open Sol to copy the dictation.")
    if any(original.get(k) != current.get(k) for k in ("window", "pid", "field", "start", "end")):
        raise ValueError("Focus or the cursor changed. Open Sol to copy the dictation.")


class PortalInput:
    def __init__(self, Gio, GLib, focus_reader):
        self.Gio, self.GLib, self.V = Gio, GLib, GLib.Variant
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        self.focus_reader = focus_reader
        self.session = self.request_path = ""
        self.callback = None
        self.ready = False
        self.payload = b""
        state = Path(os.environ.get("XDG_STATE_HOME") or Path.home() / ".local/state")
        self.token_path = state / "sol" / "portal-input-token"
        for interface in (CLIP, SESSION, REQUEST):
            self.bus.signal_subscribe(PORTAL, interface, None, None, None, Gio.DBusSignalFlags.NONE, self.signal)
        try:
            self.call("org.freedesktop.host.portal.Registry", "Register", "(sa{sv})", ("org.sol.Dictation", {}))
        except GLib.Error:
            pass

    def call(self, interface, member, signature=None, args=(), path=PATH):
        params = self.V(signature, args) if signature else None
        return self.bus.call_sync(PORTAL, path, interface, member, params, None,
                                  self.Gio.DBusCallFlags.NONE, 2000, None).unpack()

    def request(self, member, signature, args, options, callback):
        options["handle_token"] = self.V("s", "sol" + uuid.uuid4().hex)
        self.request_path, = self.call(REMOTE, member, signature, (*args, options))
        self.callback = callback

    def configure(self):
        if self.ready:
            report(ready=True, info="Desktop keyboard and clipboard access is active.")
            return
        if self.request_path:
            report(ready=False, info="Complete the pending system permission dialog.")
            return
        version, = self.call("org.freedesktop.DBus.Properties", "Get", "(ss)", (REMOTE, "version"))
        if version < 2:
            raise ValueError("This desktop portal does not support clipboard sharing. Open Sol to copy the dictation.")
        self.request("CreateSession", "(a{sv})", (),
                     {"session_handle_token": self.V("s", "sol" + uuid.uuid4().hex)}, self.created)

    def created(self, results):
        self.session = results["session_handle"]
        options = {"types": self.V("u", 1), "persist_mode": self.V("u", 2)}  # Keyboard only.
        try:
            token = self.token_path.read_text().strip()
        except FileNotFoundError:
            token = ""
        if token:
            options["restore_token"] = self.V("s", token)
        self.request("SelectDevices", "(oa{sv})", (self.session,), options, self.selected)

    def selected(self, results):
        self.call(CLIP, "RequestClipboard", "(oa{sv})", (self.session, {}))
        self.request("Start", "(osa{sv})", (self.session, ""), {}, self.started)

    def started(self, results):
        if results.get("devices") != 1 or not results.get("clipboard_enabled"):
            raise ValueError("Sol needs keyboard and clipboard access for this field. Reconnect in Settings or copy the dictation.")
        token = results.get("restore_token")
        if token:
            self.token_path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(mode="w", dir=self.token_path.parent, delete=False) as f:
                f.write(token)
            os.replace(f.name, self.token_path)
        else:
            self.token_path.unlink(missing_ok=True)
        self.ready = True
        report(ready=True, info="Desktop keyboard and clipboard access is active.")

    def close(self, forget=False):
        old_request, self.request_path = self.request_path, ""
        old_session, self.session = self.session, ""
        self.callback, self.ready, self.payload = None, False, b""
        for obj, iface in ((old_request, REQUEST), (old_session, SESSION)):
            if obj:
                try:
                    self.call(iface, "Close", path=obj)
                except self.GLib.Error:
                    pass
        if forget:
            self.token_path.unlink(missing_ok=True)

    def signal(self, bus, sender, path, interface, name, params, *args):
        values = params.unpack()
        try:
            if name == "Closed" and path == self.session:
                self.session = ""
                self.close()
                report(ready=False, info="Desktop keyboard access ended. Reconnect in Sol Settings.")
            elif name == "Response" and path == self.request_path:
                callback, self.callback = self.callback, None
                self.request_path = ""
                if values[0] != 0:
                    raise ValueError("Desktop keyboard access was declined or cancelled. " + DISABLED)
                callback(values[1])
            elif name == "SelectionTransfer" and values[0] == self.session:
                self.transfer(*values)
            elif name == "SelectionOwnerChanged" and values[0] == self.session:
                if values[1].get("session_is_owner") is False:
                    self.payload = b""
        except (self.GLib.Error, KeyError, OSError, ValueError) as error:
            self.close()
            report(ready=False, info=f"Desktop input unavailable. {error}")

    def transfer(self, session, mime, serial):
        data = self.payload
        def done(success):
            if self.session == session:
                try:
                    self.call(CLIP, "SelectionWriteDone", "(oub)", (session, serial, success))
                except self.GLib.Error:
                    pass
            return False
        if not data or mime not in MIMES:
            done(False)
            return
        try:
            result, fds = self.bus.call_with_unix_fd_list_sync(
                PORTAL, PATH, CLIP, "SelectionWrite", self.V("(ou)", (session, serial)),
                None, self.Gio.DBusCallFlags.NONE, 2000, None, None)
            fd = fds.get(result.unpack()[0])
        except (self.GLib.Error, OSError, IndexError):
            done(False)
            return

        # A clipboard client may stop reading. Keep the event loop responsive to release/close.
        def write():
            ok = False
            try:
                os.set_blocking(fd, False)
                rest = memoryview(data)
                while rest and select.select([], [fd], [], 2)[1]:
                    try:
                        rest = rest[os.write(fd, rest):]
                    except BlockingIOError:
                        continue
                ok = not rest
            except OSError:
                pass
            finally:
                os.close(fd)
                self.GLib.idle_add(done, ok)
        threading.Thread(target=write, daemon=True).start()

    def key(self, keysym, down):
        self.call(REMOTE, "NotifyKeyboardKeysym", "(oa{sv}iu)", (self.session, {}, keysym, int(down)))

    def paste(self, target, text, relaxed=False):
        if not self.ready:
            raise ValueError(DISABLED)
        if not text:
            return
        current = self.focus_reader()
        verify_target(target, current, relaxed=relaxed)
        self.payload = text.encode("utf-8")
        self.call(CLIP, "SetSelection", "(oa{sv})", (self.session, {"mime_types": self.V("as", MIMES)}))
        terminal = TERMINAL_RE.search(current.get("app", ""))
        # Check again immediately before the paste. Release every pressed modifier on errors.
        pressed = []
        try:
            for sym in ([0xffe3, 0xffe1] if terminal else [0xffe3]):  # Control_L and Shift_L.
                self.key(sym, True)
                pressed.append(sym)
            verify_target(target, self.focus_reader(), relaxed=relaxed)
            sym = ord("V" if terminal else "v")
            self.key(sym, True)
            pressed.append(sym)
        finally:
            release_error = None
            for sym in reversed(pressed):
                try:
                    self.key(sym, False)
                except self.GLib.Error as error:
                    release_error = error
            if release_error:
                self.close()
                report(ready=False, info="Desktop input disconnected. Reconnect in Settings.")
                raise ValueError("The desktop disconnected during paste. Check the field before copying again.")

    def control(self, line):
        msg = json.loads(line)
        if msg["op"] == "paste":
            try:
                self.paste(msg.get("target") or {}, msg["text"], relaxed=bool(msg.get("relaxed", False)))
                report(id=msg["id"], notice="")
            except (ValueError, OSError, KeyError, self.GLib.Error) as error:
                report(id=msg["id"], notice=str(error) + (" Open Sol to copy the dictation." if isinstance(error, self.GLib.Error) else ""))
        elif msg["op"] == "close":
            self.close(forget=True)
            report(ready=False, info="Desktop keyboard and clipboard access disconnected.")
        elif msg["op"] == "configure":
            try:
                self.configure()
            except (ValueError, self.GLib.Error, OSError) as error:
                self.close()
                report(ready=False, info=f"Desktop input unavailable ({error}). " + DISABLED)


def run(focus_reader=None):
    from gi.repository import Gio, GLib
    if focus_reader is None:
        from desktop import inspect
        def focus_reader():
            return inspect(False)[0]
    client = PortalInput(Gio, GLib, focus_reader)
    loop = GLib.MainLoop()
    pending = bytearray()
    def read_input(channel, condition):
        data = os.read(sys.stdin.fileno(), 65536)
        if not data:
            loop.quit()
            return False
        pending.extend(data)
        while b"\n" in pending:
            line, _, rest = pending.partition(b"\n")
            pending[:] = rest
            try:
                client.control(line)
            except (TypeError, ValueError, KeyError) as error:
                report(ready=client.ready, info=f"Invalid desktop input request ({error}).")
        return True
    GLib.io_add_watch(sys.stdin, GLib.IO_IN | GLib.IO_HUP, read_input)
    report(ready=False, info="Connect keyboard and clipboard access for terminals and custom fields.")
    try:
        loop.run()
    finally:
        client.close()


if __name__ == "__main__":
    try:
        run()
    except (ImportError, ValueError, RuntimeError, OSError) as error:
        report(ready=False, info=f"Desktop input unavailable ({error}). Open Sol to copy the dictation.")
