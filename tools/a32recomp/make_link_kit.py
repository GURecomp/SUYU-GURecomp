"""make_link_kit.py <build dir> <kit dir>

Collects what the game export needs to link the single-file launcher (suyu-cmd-static)
without a suyu build tree: suyu-cmd's objects and every library it links, plus link.rsp
listing them relative to the kit. game_export.cpp (LinkWithKit) uses it when suyu.exe has
no CMakeCache.txt above it, i.e. in a prebuilt package.

The build dir must be configured with SUYU_CMD_RECOMP_DIR pointing at a stub module (any
module dir with a CMakeLists.txt + recomp_export.c) so the suyu-cmd-static target exists,
and its objects must be built (ninja <the objects>). The stub's library and registration
object are left out; the export supplies the game's own.
"""
import os
import re
import shlex
import shutil
import sys

build, kit = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
ninja = open(os.path.join(build, "build.ninja"), encoding="utf-8", errors="replace").read()
m = re.search(r"^build bin[\\/]suyu-cmd-static\.exe: (.*?)\n((?:  .*\n)+)", ninja, re.M)
if not m:
    sys.exit("suyu-cmd-static is not configured in this build dir (set SUYU_CMD_RECOMP_DIR)")
header, body = m.group(1), m.group(2)
vars_ = dict(re.findall(r"^  (\w+) = (.*)$", body, re.M))


def split(s):
    return shlex.split(s.replace("\\", "/"), posix=True)


def ninja_paths(s):
    # ninja escapes spaces and colons in paths as "$ " and "$:"
    s = s.replace("$ ", "\0").replace("$:", ":")
    return [t.replace("\0", " ").replace("\\", "/") for t in s.split()]


inputs = [t for t in ninja_paths(header.split("|")[0])[1:]
          if t.lower().endswith((".obj", ".res"))]
libs = split(vars_["LINK_LIBRARIES"])
flags = [f for f in vars_["LINK_FLAGS"].split() if not f.upper().startswith("/DEBUG")]

if os.path.isdir(kit):
    shutil.rmtree(kit)
os.makedirs(os.path.join(kit, "obj"))
os.makedirs(os.path.join(kit, "lib"))

rsp = flags + ["/DEBUG:NONE", "/MANIFEST:NO"]
for obj in inputs:
    if "recomp_registration" in obj:
        continue
    src = obj if os.path.isabs(obj) else os.path.join(build, obj)
    name = os.path.basename(obj)
    if os.path.exists(os.path.join(kit, "obj", name)):
        sys.exit("duplicate object name " + name)
    shutil.copy2(src, os.path.join(kit, "obj", name))
    rsp.append("obj\\" + name)

seen = {}
for lib in libs:
    base = os.path.basename(lib)
    if base.startswith("recomp_static_"):
        continue  # the stub module; the export adds the game's modules
    if "/" not in lib:  # a system library (kernel32.lib ...): found through LIB
        rsp.append(lib)
        continue
    src = lib if os.path.isabs(lib) else os.path.join(build, lib)
    if base in seen and seen[base] != os.path.normcase(src):
        sys.exit("two different libraries named " + base)
    if base not in seen:
        seen[base] = os.path.normcase(src)
        shutil.copy2(src, os.path.join(kit, "lib", base))
    rsp.append("lib\\" + base)

with open(os.path.join(kit, "link.rsp"), "w", encoding="utf-8") as f:
    f.write("\n".join('"%s"' % a if " " in a else a for a in rsp) + "\n")
size = sum(os.path.getsize(os.path.join(r, n)) for r, _, ns in os.walk(kit) for n in ns)
print("kit: %d objects, %d libraries, %.0f MB" % (
    len(os.listdir(os.path.join(kit, "obj"))), len(seen), size / 1e6))
