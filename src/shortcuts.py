"""Global keys for X11 and XDG portals. The parent talks JSON over stdio."""
import json
import os
import sys
import uuid


def report(**event):
    print(json.dumps(event), flush=True)


def parse_x11_chord(chord, d, X, XK):
    if not chord or not chord.strip():
        return None
    parts = [p.strip() for p in chord.split("+") if p.strip()]
    if not parts:
        return None
    mod_map = {
        "super": X.Mod4Mask, "meta": X.Mod4Mask, "logo": X.Mod4Mask, "win": X.Mod4Mask,
        "alt": X.Mod1Mask, "option": X.Mod1Mask,
        "ctrl": X.ControlMask, "control": X.ControlMask,
        "shift": X.ShiftMask,
    }
    key_aliases = {
        "space": "space", "enter": "Return", "return": "Return", "tab": "Tab",
        "esc": "Escape", "escape": "Escape", "backspace": "BackSpace",
        "rightalt": "Alt_R", "alt_r": "Alt_R", "rightctrl": "Control_R",
        "control_r": "Control_R", "rightsuper": "Super_R", "super_r": "Super_R",
    }
    mods = 0
    key_name = parts[-1]
    for p in parts[:-1]:
        low = p.lower()
        if low in mod_map:
            mods |= mod_map[low]
        else:
            return None
    sym_name = key_aliases.get(key_name.lower(), key_name.lower() if len(key_name) == 1 else key_name)
    keysym = XK.string_to_keysym(sym_name)
    if not keysym:
        keysym = XK.string_to_keysym(key_name)
    if not keysym:
        return None
    keycode = d.keysym_to_keycode(keysym)
    if not keycode:
        return None
    return (keycode, mods)


def x11():
    from Xlib import X, XK, display
    import select

    d = display.Display()
    root = d.screen().root

    # Discover Num Lock's real modifier slot; keyboard mappings differ.
    num = d.keysym_to_keycode(XK.string_to_keysym("Num_Lock"))
    locks = {0, X.LockMask}
    lock_mask = X.LockMask
    for index, codes in enumerate(d.get_modifier_mapping()):
        if num in codes:
            locks |= {1 << index, X.LockMask | (1 << index)}
            lock_mask |= (1 << index)

    hold_chord = os.getenv("SOL_HOLD_KEY", "F9")
    grabbed = []
    parsed = None

    def apply_grabs():
        nonlocal grabbed, parsed
        for code, mods in grabbed:
            root.ungrab_key(code, mods)
        d.sync()
        grabbed = []
        parsed = None
        conflicts = []

        def failed(e, request):
            conflicts.append(e)

        spec = parse_x11_chord(hold_chord, d, X, XK)
        if spec:
            code, mods = spec
            parsed = (code, mods)
            for lock in locks:
                root.grab_key(code, mods | lock, False, X.GrabModeAsync, X.GrabModeAsync, onerror=failed)
                grabbed.append((code, mods | lock))
        d.sync()
        if conflicts:
            for code, mods in grabbed:
                root.ungrab_key(code, mods)
            d.sync()
            grabbed = []
            raise ValueError("Shortcut is already in use. Change it with 'sol shortcuts setup'.")

    apply_grabs()
    info = "X11 global shortcut is active."
    report(info=info, hold=hold_chord)

    down_code = 0
    while True:
        inputs, _, _ = select.select([d.fileno(), sys.stdin], [], [], None if not d.pending_events() else 0)
        if sys.stdin in inputs:
            line = sys.stdin.readline()
            if not line:
                break
            line = line.strip()
            if line.startswith("{"):
                try:
                    msg = json.loads(line)
                    if msg.get("op") == "rebind" and "hold" in msg:
                        hold_chord = msg["hold"]
                        apply_grabs()
                        report(info=info, hold=hold_chord)
                except Exception as err:
                    report(info=str(err))
            else:
                report(info=info)

        while d.pending_events():
            event = d.next_event()
            if event.type not in (X.KeyPress, X.KeyRelease) or not parsed:
                continue
            code, mods = parsed
            clean_state = event.state & ~lock_mask & (X.ShiftMask | X.ControlMask | X.Mod1Mask | X.Mod4Mask)
            if event.type == X.KeyPress and not down_code:
                if event.detail == code and clean_state == mods:
                    down_code = event.detail
                    report(action="start")
            elif event.type == X.KeyRelease and event.detail == down_code:
                # Core X reports auto-repeat as a release/press pair with the same timestamp.
                if d.pending_events():
                    following = d.next_event()
                    if following.type == X.KeyPress and following.detail == down_code and following.time == event.time:
                        continue
                    d.put_back_event(following)
                report(action="stop")
                down_code = 0


def portal():
    from gi.repository import Gio, GLib

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    name = "org.freedesktop.portal.Desktop"
    path = "/org/freedesktop/portal/desktop"
    iface = "org.freedesktop.portal.GlobalShortcuts"
    V = GLib.Variant
    none = Gio.DBusCallFlags.NONE
    loop = GLib.MainLoop()
    session = ""
    pending = {}
    configuring = False
    has_bindings = [False]
    manual = "Bind your dictation key to press -> 'sol start' and release -> 'sol stop' (or 'sol toggle')."

    def method(interface, member, params):
        return bus.call_sync(name, path, interface, member, params, None, none, 2500, None)

    try:
        method("org.freedesktop.host.portal.Registry", "Register", V("(sa{sv})", ("org.sol.Dictation", {})))
    except GLib.Error:
        pass

    def request(member, params, cb, token):
        sender = bus.get_unique_name()[1:].replace(".", "_")
        predicted = f"/org/freedesktop/portal/desktop/request/{sender}/{token}"
        pending[predicted] = cb
        try:
            actual = method(iface, member, params).unpack()[0]
            if actual != predicted:
                pending[actual] = pending.pop(predicted)
        except GLib.Error:
            pending.pop(predicted, None)
            report(info=manual)

    def bound(results):
        shortcuts = dict(results.get("shortcuts", []))
        has_bindings[0] = bool(shortcuts)
        hold = shortcuts.get("hold", {}).get("trigger_description", "")
        toggle = shortcuts.get("toggle", {}).get("trigger_description", "")
        active = bool(hold or toggle)
        report(
            hold=hold or toggle,
            info=("Desktop shortcut is active." if active else manual),
        )

    def bind():
        t = "sol" + uuid.uuid4().hex
        pref = os.getenv("SOL_PORTAL_KEY", "F9")
        shortcuts = [
            ("hold", {"description": V("s", "Hold to dictate"), "preferred_trigger": V("s", pref)}),
            ("toggle", {"description": V("s", "Toggle dictation"), "preferred_trigger": V("s", "SHIFT+" + pref)}),
        ]
        request("BindShortcuts", V("(oa(sa{sv})sa{sv})", (session, shortcuts, "", {"handle_token": V("s", t)})), bound, t)

    def listed(results):
        if results.get("shortcuts"):
            bound(results)
        elif configuring:
            bind()
        else:
            report(info="Configure shortcuts with 'sol shortcuts bind'.")

    def created(results):
        nonlocal session
        session = results["session_handle"]
        t = "sol" + uuid.uuid4().hex
        request("ListShortcuts", V("(oa{sv})", (session, {"handle_token": V("s", t)})), listed, t)

    def new():
        t = "sol" + uuid.uuid4().hex
        request("CreateSession", V("(a{sv})", ({"handle_token": V("s", t), "session_handle_token": V("s", t)},)), created, t)

    def reset():
        nonlocal session
        old, session = session, ""
        try:
            bus.call_sync(name, old, "org.freedesktop.portal.Session", "Close", None, None, none, 500, None)
        except GLib.Error:
            pass
        new()

    def signal(conn, sender, obj, interface, member, params, *extra):
        nonlocal session
        args = params.unpack()
        if member == "Response" and obj in pending:
            callback = pending.pop(obj)
            if args[0] == 0:
                callback(args[1])
            else:
                report(info="Desktop shortcut setup was cancelled. " + manual)
        elif member == "Closed" and obj == session:
            session = ""
            report(info="Desktop shortcut session closed. Run 'sol shortcuts bind' to reconnect.")
        elif interface == iface and args[0] == session:
            if member == "ShortcutsChanged":
                bound({"shortcuts": args[1]})
            elif args[1] == "hold":
                report(action="start" if member == "Activated" else "stop")
            elif args[1] == "toggle" and member == "Activated":
                report(action="toggle")

    for interface in (iface, "org.freedesktop.portal.Request", "org.freedesktop.portal.Session"):
        bus.signal_subscribe(name, interface, None, None, None, Gio.DBusSignalFlags.NONE, signal)

    def control(fd, condition):
        nonlocal configuring, session
        line = sys.stdin.readline()
        if not line:
            loop.quit()
            return False
        if line.strip().startswith("{"):
            return True
        configuring = True
        if session:
            try:
                method(iface, "ConfigureShortcuts", V("(osa{sv})", (session, "", {})))
                if not has_bindings[0]:
                    reset()
            except GLib.Error:
                reset()
        else:
            new()
        return True

    GLib.io_add_watch(sys.stdin, GLib.IO_IN | GLib.IO_HUP, control)
    new()
    loop.run()


if __name__ == "__main__":
    try:
        if os.getenv("DISPLAY") and not os.getenv("WAYLAND_DISPLAY"):
            x11()
        else:
            portal()
    except Exception as e:
        report(info=f"Desktop shortcuts unavailable ({e}). Bind 'sol toggle' in desktop keyboard settings.")
