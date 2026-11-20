"""Collect minimal Debian dynamic Qt runtime (QtCore + QtGui + QtDBus only) for Sol."""
from collections import defaultdict
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

bundle, source = map(Path, sys.argv[1:])
plugins = Path(subprocess.check_output(["qmake6", "-query", "QT_INSTALL_PLUGINS"], text=True).strip())
library = bundle / "lib"
library.mkdir()
originals = {}

def copy(origin, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(origin, destination)
    originals[destination] = origin

# Pure QRasterWindow HUD needs only Wayland/X11 platform & shell plugins (no QML, OpenGL, SVG, or TLS).
for group in ("platforms", "platforminputcontexts", "wayland-decoration-client", "wayland-shell-integration"):
    group_dir = plugins / group
    if not group_dir.is_dir():
        continue
    for path in group_dir.glob("*.so"):
        if group == "platforms" and not re.match(r"libq(?:xcb|offscreen|wayland[^.]*|minimal)\.so$", path.name):
            continue
        copy(path, bundle / "plugins" / group / path.name)

def elf(path):
    with path.open("rb") as stream:
        return stream.read(4) == b"\x7fELF"

# The platform supplies glibc and its dynamic loader as a compatible set.
glibc = {"libc.so.6", "libm.so.6", "libpthread.so.0", "librt.so.1", "libdl.so.2",
         "libresolv.so.2", "libutil.so.1", "libanl.so.1", "libmvec.so.1", "libBrokenLocale.so.1"}
roots = [bundle / "bin/sol"] + [dest for dest in originals if elf(dest)]
shared = {}
for path in roots:
    out = subprocess.check_output(["ldd", str(originals.get(path, path))], text=True)
    if "=> not found" in out:
        sys.exit(f"Unresolved shared library for {path}:\n{out}")
    for soname, original in re.findall(r"^\s*(\S+) => (/\S+)", out, re.M):
        if soname not in glibc:
            shared[soname] = Path(original)
for soname, origin in shared.items():
    copy(origin, library / soname)
for path in roots + list(library.iterdir()):
    relative = os.path.relpath(library, path.parent)
    runpath = "$ORIGIN" if relative == "." else "$ORIGIN/" + relative
    subprocess.run(["patchelf", "--set-rpath", runpath, str(path)], check=True)
(bundle / "bin/qt.conf").write_text("[Paths]\nPrefix=..\nPlugins=plugins\nLibraries=lib\n")

files_by_package = defaultdict(list)
docs = bundle / "share/licenses"
docs.mkdir(parents=True)
cached = {}
for listing in Path("/var/lib/dpkg/info").glob("*.list"):
    pkg = listing.name[:-5]
    for entry in listing.read_text(errors="replace").splitlines():
        cached[entry] = pkg
for dest, origin in originals.items():
    keys = [str(origin), str(origin.resolve())]
    keys.extend("/usr" + k for k in keys[:] if k.startswith("/lib/"))
    keys.extend(k[4:] for k in keys[:] if k.startswith("/usr/lib/"))
    pkg = next((cached[k] for k in keys if k in cached), None)
    if not pkg:
        sys.exit(f"Cannot identify the Debian package supplying {origin}")
    files_by_package[pkg].append(str(dest.relative_to(bundle)))
inventory = []
for package, entries in sorted(files_by_package.items()):
    copyright = Path("/usr/share/doc") / package.split(":")[0] / "copyright"
    if copyright.is_file():
        shutil.copy2(copyright, docs / (package.replace(":", "-") + ".copyright"))
    metadata = subprocess.check_output(["dpkg-query", "-W", "-f=${source:Package}\t${source:Version}", package], text=True)
    inventory.extend(f"{package}\t{metadata}\t{entry}" for entry in sorted(entries))
shutil.copytree("/usr/share/common-licenses", docs / "common-licenses", symlinks=True)
(docs / "debian-files.tsv").write_text("Binary package\tSource package\tSource version\tFile\n" + "\n".join(inventory) + "\n")
