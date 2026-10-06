"""make_link_kit.py <build dir> <kit dir>

Collects what the game export needs to link the single-file launcher (suyu-cmd-static)
without a suyu build tree: suyu-cmd's objects and every library it links, plus link.rsp
listing them relative to the kit. game_export.cpp (LinkWithKit) uses it when suyu has
no CMakeCache.txt above it, i.e. in a prebuilt package.

The build dir must be configured with SUYU_CMD_RECOMP_DIR pointing at a stub module (any
module dir with a CMakeLists.txt + recomp_export.c) so the suyu-cmd-static target exists,
and its objects must be built (ninja <the objects>). The stub's library and registration
object are left out; the export supplies the game's own.

Windows (MSVC): link.rsp holds link.exe arguments. Linux (gcc/clang): link.rsp holds
compiler-driver arguments. There, libraries that come from the system are linked by their
runtime name (-l:libz.so.1) so the player needs no -dev packages, and every other shared
library is copied into lib/: the export links against it and ships it beside the game.
"""
import os
import re
import shlex
import shutil
import subprocess
import sys

build, kit = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
ninja = open(os.path.join(build, "build.ninja"), encoding="utf-8", errors="replace").read()
m = re.search(r"^build bin[\\/]suyu-cmd-static(\.exe)?: (.*?)\n((?:  .*\n)+)", ninja, re.M)
if not m:
    sys.exit("suyu-cmd-static is not configured in this build dir (set SUYU_CMD_RECOMP_DIR)")
windows = m.group(1) is not None
header, body = m.group(2), m.group(3)
vars_ = dict(re.findall(r"^  (\w+) = (.*)$", body, re.M))


def split(s):
    return shlex.split(s.replace("\\", "/"), posix=True)


def ninja_paths(s):
    # ninja escapes spaces and colons in paths as "$ " and "$:"
    s = s.replace("$ ", "\0").replace("$:", ":")
    return [t.replace("\0", " ").replace("\\", "/") for t in s.split()]


def soname(path):
    """The DT_SONAME of a shared library (its file name when it has none)."""
    try:
        out = subprocess.run(["readelf", "-d", path], capture_output=True, text=True).stdout
        found = re.search(r"\(SONAME\)\s+Library soname: \[([^\]]+)\]", out)
        if found:
            return found.group(1)
    except OSError:
        pass
    return os.path.basename(path)


obj_ext = (".obj", ".res") if windows else (".o",)
inputs = [t for t in ninja_paths(header.split("|")[0])[1:] if t.lower().endswith(obj_ext)]
libs = split(vars_["LINK_LIBRARIES"])
if windows:
    flags = [f for f in vars_["LINK_FLAGS"].split() if not f.upper().startswith("/DEBUG")]
else:
    # The compiler's own flags matter for the driver (-pthread, -fuse-ld=..., -static-libgcc ...);
    # rpaths into this build tree don't.
    flags = [f for f in split(vars_.get("FLAGS", "") + " " + vars_.get("LINK_FLAGS", ""))
             if not f.startswith(("-Wl,-rpath", "-I", "-D", "-g", "-W", "-O", "-std="))]

if os.path.isdir(kit):
    shutil.rmtree(kit)
os.makedirs(os.path.join(kit, "obj"))
os.makedirs(os.path.join(kit, "lib"))

rsp = flags + (["/DEBUG:NONE", "/MANIFEST:NO"] if windows else [])
sep = "\\" if windows else "/"
if not windows and "-static-libstdc++" in flags:
    # The kit's objects were compiled against this compiler's C++ library headers: link the
    # matching static libstdc++ from the kit (lib/ is searched first), not the player's.
    cxx = os.environ.get("CXX", "g++")
    for name in ("libstdc++.a", "libstdc++fs.a"):
        path = subprocess.run([cxx, "-print-file-name=" + name], capture_output=True,
                              text=True).stdout.strip()
        if os.path.isabs(path) and os.path.exists(path):
            shutil.copy2(path, os.path.join(kit, "lib", name))
    rsp.insert(0, "-Llib")
for obj in inputs:
    if "recomp_registration" in obj:
        continue
    src = obj if os.path.isabs(obj) else os.path.join(build, obj)
    name = os.path.basename(obj)
    if os.path.exists(os.path.join(kit, "obj", name)):
        # Linux object names keep their source extension (foo.cpp.o); two sources of the same
        # name in different folders are still possible: number them.
        stem, n = name, 2
        while os.path.exists(os.path.join(kit, "obj", name)):
            name = "%d_%s" % (n, stem)
            n += 1
        if windows:
            sys.exit("duplicate object name " + stem)
    shutil.copy2(src, os.path.join(kit, "obj", name))
    rsp.append("obj" + sep + name)

SYSTEM_DIRS = ("/usr/", "/lib/", "/lib64/", "/opt/")


def copy_library(src, dest):
    """Copies a library into the kit. suyu builds GNU thin archives on Linux (ar qcTP): they only
    name their members' .o files in the build tree, so they are rebuilt as normal archives."""
    with open(src, "rb") as f:
        thin = f.read(8) == b"!<thin>\n"
    if not thin:
        shutil.copy2(src, dest)
        return
    folder = os.path.dirname(src)
    members = subprocess.run(["ar", "t", src], cwd=folder, capture_output=True, text=True,
                             check=True).stdout.split()
    listing = dest + ".members"
    with open(listing, "w") as f:
        f.write("\n".join(os.path.abspath(os.path.join(folder, m)) for m in members) + "\n")
    if os.path.exists(dest):
        os.remove(dest)
    # q appends without replacing same-named members (different folders, same file name).
    subprocess.run(["ar", "qc", dest, "@" + listing], check=True)
    subprocess.run(["ranlib", dest], check=True)
    os.remove(listing)


seen = {}
for lib in libs:
    base = os.path.basename(lib)
    if base.startswith(("recomp_static_", "librecomp_static_")):
        continue  # the stub module; the export adds the game's modules
    if windows:
        if "/" not in lib:  # a system library (kernel32.lib ...): found through LIB
            rsp.append(lib)
            continue
    else:
        if lib.startswith("-Wl,-rpath"):
            continue
        if lib.startswith("-") or "/" not in lib:  # -lpthread, -ldl, -Wl,--as-needed ...
            rsp.append(lib)
            continue
        src = lib if os.path.isabs(lib) else os.path.join(build, lib)
        if ".so" in base and os.path.abspath(src).startswith(SYSTEM_DIRS):
            # A desktop library (libz, libX11 ...): link it by its runtime name.
            rsp.append("-l:" + soname(src))
            continue
    src = lib if os.path.isabs(lib) else os.path.join(build, lib)
    if not windows and ".so" in base:
        # A shared library built or downloaded with suyu: shipped as its runtime name, which
        # is what the linked game records and what it looks for in lib/ beside it.
        base = soname(src)
    if base in seen and seen[base] != os.path.normcase(os.path.realpath(src)):
        sys.exit("two different libraries named " + base)
    if base not in seen:
        seen[base] = os.path.normcase(os.path.realpath(src))
        copy_library(os.path.realpath(src), os.path.join(kit, "lib", base))
    rsp.append("lib" + sep + base)

with open(os.path.join(kit, "link.rsp"), "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join('"%s"' % a if " " in a else a for a in rsp) + "\n")
size = sum(os.path.getsize(os.path.join(r, n)) for r, _, ns in os.walk(kit) for n in ns)
print("kit: %d objects, %d libraries, %.0f MB" % (
    len(os.listdir(os.path.join(kit, "obj"))), len(seen), size / 1e6))
