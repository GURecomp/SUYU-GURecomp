"""make_package.py <build bin dir> <kit dir> <out dir> [forbidden word ...]

Copies a prebuilt suyu for testers: suyu.exe and the DLLs/Qt plugins it loads, the pipeline
manifests and the link kit (make_link_kit.py), plus README.txt. Leaves out debug symbols,
test tools and the dev frontends. Then scans every byte of the package for the forbidden
words (and C:\\Users\\ paths) and fails if any is found: build the package from a source
tree at a neutral path (e.g. C:\\suyu-src) so the compiler embeds no personal paths.
"""
import os
import re
import shutil
import sys

bin_dir, kit, out = (os.path.abspath(a) for a in sys.argv[1:4])
forbidden = [w.lower().encode() for w in sys.argv[4:]]

KEEP_FILES = ["suyu.exe", "dxcompiler.dll", "dxil.dll", "libcrypto.dll", "libssl.dll"]
KEEP_DIRS = ["plugins", "pipelines", "coverage"]  # Qt plugins; pipeline manifests and
# coverage seeds (offsets only, no game data)

if os.path.isdir(out):
    shutil.rmtree(out)
os.makedirs(out)
for name in KEEP_FILES + [n for n in os.listdir(bin_dir) if re.fullmatch(r"Qt6\w+\.dll", n)]:
    shutil.copy2(os.path.join(bin_dir, name), os.path.join(out, name))
for d in KEEP_DIRS:
    shutil.copytree(os.path.join(bin_dir, d), os.path.join(out, d))
shutil.copytree(kit, os.path.join(out, "link_kit"))
here = os.path.dirname(os.path.abspath(__file__))
repo = os.path.dirname(os.path.dirname(here))
shutil.copy2(os.path.join(here, "package_readme.txt"), os.path.join(out, "README.txt"))
# Licenses: suyu's own (GPL-3.0-or-later), every SPDX text the sources use, and Qt's.
shutil.copy2(os.path.join(here, "package_notices.txt"), os.path.join(out, "LICENSES.txt"))
shutil.copy2(os.path.join(repo, "LICENSE.txt"), os.path.join(out, "LICENSE.txt"))
shutil.copytree(os.path.join(repo, "LICENSES"), os.path.join(out, "licenses"))
qt_licenses = os.environ.get("QT_LICENSES", r"C:\Qt\Licenses")
if not os.path.isdir(qt_licenses):
    sys.exit("Qt license texts not found: set QT_LICENSES to the Qt install's Licenses folder")
shutil.copytree(qt_licenses, os.path.join(out, "licenses", "qt"))

users = re.compile(rb"[A-Za-z]:[\\/]+Users[\\/]+([^\\/\x00\"]{1,40})", re.I)
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
size = sum(os.path.getsize(os.path.join(r, n)) for r, _, ns in os.walk(out) for n in ns)
print("package: %.0f MB in %s" % (size / 1e6, out))
if bad:
    print("\n".join(sorted(set(bad))[:50]))
    sys.exit("NOT CLEAN: %d findings" % len(set(bad)))
print("clean: no forbidden words, no C:\\Users paths")
