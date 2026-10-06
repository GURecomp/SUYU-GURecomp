"""make_package.py <build bin dir> <kit dir> <out dir> [forbidden word ...]

Copies a prebuilt suyu for testers: suyu (suyu.exe on Windows) and the libraries/Qt plugins
it loads, the pipeline manifests and the link kit (make_link_kit.py), plus README.txt.
Leaves out debug symbols, test tools and the dev frontends. Then scans every byte of the
package for the forbidden words (and C:\\Users\\ / /home/ paths) and fails if any is found:
build the package from a source tree at a neutral path (e.g. C:\\suyu-src, /opt/suyu-src) so
the compiler embeds no personal paths.

Linux: suyu's rpath is $ORIGIN/lib (set by build_release_package.sh). Every library it or its
Qt plugins load that isn't part of a normal desktop goes to lib/, the Qt plugins to plugins/.
"""
import os
import re
import shutil
import subprocess
import sys

bin_dir, kit, out = (os.path.abspath(a) for a in sys.argv[1:4])
forbidden = [w.lower().encode() for w in sys.argv[4:]]
linux = sys.platform.startswith("linux")

KEEP_DIRS = ["pipelines", "coverage"]  # pipeline manifests and coverage seeds (offsets only,
# no game data)

# Libraries every desktop has, or that must come from the system (GPU drivers, glibc): the
# usual AppImage exclude list, shortened to what suyu and Qt link.
LINUX_SYSTEM_LIBS = re.compile(
    r"^(linux-vdso|ld-linux|libc|libm|libdl|libpthread|librt|libresolv|libutil|libgcc_s|"
    r"libstdc\+\+|libGL|libGLX|libGLdispatch|libEGL|libOpenGL|libvulkan|libdrm|libgbm|"
    r"libX\w*|libxcb\w*|libxkbcommon\w*|libwayland-\w+|libasound|libpulse\w*|libjack|"
    r"libpipewire\S*|libfontconfig|libfreetype|libharfbuzz|libpng16|libz|libbz2|libbrotli\w*|"
    r"libexpat|libuuid|libudev|libsystemd|libdbus-1|libglib-2\.0|libgobject-2\.0|libgio-2\.0|"
    r"libgthread-2\.0|libgmodule-2\.0|libpcre2-\S+|libffi|libmount|libblkid|libselinux|"
    r"libcap|libgcrypt|libgpg-error|liblzma|libzstd|liblz4|libICE|libSM|libgssapi_krb5|"
    r"libkrb5\S*|libk5crypto|libcom_err|libkeyutils)\.so")


def ldd(path, env=None, optional=False):
    """{soname: resolved path} of the shared libraries path needs (recursively, as ldd does).
    optional: return None instead of failing when one is missing (an optional Qt plugin)."""
    res = subprocess.run(["ldd", path], capture_output=True, text=True, env=env)
    found = {}
    for line in res.stdout.splitlines():
        m = re.match(r"\s*(\S+) => (\S+)", line)
        if m and m.group(2) != "not":
            found[m.group(1)] = m.group(2)
        elif "not found" in line:
            if optional:
                return None
            sys.exit("%s: a library is missing: %s" % (path, line.strip()))
    return found


if os.path.isdir(out):
    shutil.rmtree(out)
os.makedirs(out)

if linux:
    shutil.copy2(os.path.join(bin_dir, "suyu"), os.path.join(out, "suyu"))
    # The Qt that suyu was built against (the build tree's CMakeCache knows where it is).
    cache = open(os.path.join(os.path.dirname(bin_dir), "CMakeCache.txt"), encoding="utf-8",
                 errors="replace").read()
    qt_dir = re.search(r"^Qt6_DIR:\w+=(.*)$", cache, re.M)
    qt_root = os.path.abspath(os.path.join(qt_dir.group(1), "..", "..", "..")) if qt_dir else ""
    plugins_src = os.path.join(qt_root, "plugins")
    if not os.path.isdir(plugins_src):
        sys.exit("Qt plugins not found next to Qt6_DIR (%s)" % qt_root)
    plugins = []
    for d in ("platforms", "platformthemes", "platforminputcontexts", "xcbglintegrations",
              "wayland-shell-integration", "wayland-decoration-client",
              "wayland-graphics-integration-client", "imageformats", "iconengines", "styles",
              "tls"):
        if os.path.isdir(os.path.join(plugins_src, d)):
            shutil.copytree(os.path.join(plugins_src, d), os.path.join(out, "plugins", d))
            plugins += [os.path.join(out, "plugins", d, n)
                        for n in os.listdir(os.path.join(out, "plugins", d))]
    with open(os.path.join(out, "qt.conf"), "w", newline="\n") as f:
        f.write("[Paths]\nPrefix=.\nPlugins=plugins\n")
    # Resolve with Qt's own lib folder first so its plugins' dependencies are found.
    env = dict(os.environ, LD_LIBRARY_PATH=os.path.join(qt_root, "lib") + ":" +
               os.environ.get("LD_LIBRARY_PATH", ""))
    os.makedirs(os.path.join(out, "lib"))
    for binary in [os.path.join(out, "suyu")] + plugins:
        needed = ldd(binary, env, optional=binary in plugins)
        if needed is None:
            # e.g. the GTK3 theme needs GTK: on a desktop without it Qt skips the plugin anyway,
            # and bundling a toolkit isn't wanted. Leave the plugin out.
            print("left out %s (needs a library the build system lacks)" %
                  os.path.relpath(binary, out))
            os.remove(binary)
            continue
        for name, path in needed.items():
            if not LINUX_SYSTEM_LIBS.match(name) and \
                    not os.path.exists(os.path.join(out, "lib", name)):
                shutil.copy2(os.path.realpath(path), os.path.join(out, "lib", name))
    for d in KEEP_DIRS:
        shutil.copytree(os.path.join(bin_dir, d), os.path.join(out, d))
else:
    KEEP_FILES = ["suyu.exe", "dxcompiler.dll", "dxil.dll", "libcrypto.dll", "libssl.dll"]
    for name in KEEP_FILES + [n for n in os.listdir(bin_dir) if re.fullmatch(r"Qt6\w+\.dll", n)]:
        shutil.copy2(os.path.join(bin_dir, name), os.path.join(out, name))
    for d in KEEP_DIRS + ["plugins"]:  # Qt plugins
        shutil.copytree(os.path.join(bin_dir, d), os.path.join(out, d))

shutil.copytree(kit, os.path.join(out, "link_kit"))
here = os.path.dirname(os.path.abspath(__file__))
repo = os.path.dirname(os.path.dirname(here))
shutil.copy2(os.path.join(here, "package_readme_linux.txt" if linux else "package_readme.txt"),
             os.path.join(out, "README.txt"))
# Licenses: suyu's own (GPL-3.0-or-later), every SPDX text the sources use, and Qt's.
shutil.copy2(os.path.join(here, "package_notices.txt"), os.path.join(out, "LICENSES.txt"))
shutil.copy2(os.path.join(repo, "LICENSE.txt"), os.path.join(out, "LICENSE.txt"))
shutil.copytree(os.path.join(repo, "LICENSES"), os.path.join(out, "licenses"))
qt_licenses = os.environ.get("QT_LICENSES", r"C:\Qt\Licenses")
if linux and not os.path.isdir(qt_licenses):
    qt_licenses = os.path.join(qt_root, "Licenses")
if not os.path.isdir(qt_licenses):
    sys.exit("Qt license texts not found: set QT_LICENSES to the Qt install's Licenses folder")
shutil.copytree(qt_licenses, os.path.join(out, "licenses", "qt"))

users = re.compile(rb"[A-Za-z]:[\\/]+Users[\\/]+([^\\/\x00\"]{1,40})", re.I)
homes = re.compile(rb"/home/([A-Za-z0-9_.-]{1,40})/")
bad = []
for root, _, names in os.walk(out):
    for n in names:
        p = os.path.join(root, n)
        data = open(p, "rb").read()
        low = data.lower()
        for w in forbidden:
            if w in low:
                bad.append("%s: contains a forbidden word" % os.path.relpath(p, out))
        for u in set(users.findall(data)):
            # "qt": the Qt Company's own build paths inside the official Qt DLLs.
            if u.lower() not in (b"public", b"default", b"all users", b"qt"):
                bad.append("%s: path under C:\\Users\\%s" % (
                    os.path.relpath(p, out), u.decode(errors="replace")))
        for u in set(homes.findall(data)):
            # "qt": Qt's CI build paths; "runner"/"user": CI and placeholder homes.
            if u.lower() not in (b"qt", b"runner", b"user", b"username"):
                bad.append("%s: path under /home/%s" % (
                    os.path.relpath(p, out), u.decode(errors="replace")))
size = sum(os.path.getsize(os.path.join(r, n)) for r, _, ns in os.walk(out) for n in ns)
print("package: %.0f MB in %s" % (size / 1e6, out))
if bad:
    print("\n".join(sorted(set(bad))[:50]))
    sys.exit("NOT CLEAN: %d findings" % len(set(bad)))
print("clean: no forbidden words, no C:\\Users or /home paths")
