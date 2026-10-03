"""publish_check.py [forbidden word ...]

Run from the source root before every push or release. Checks every tracked file (git ls-files)
and the commit identities for things that must never be published:

  - the forbidden words given (your real name, e-mail, user name, ...), case-insensitive
  - local paths under C:\\Users\\ (other than Public/Default)
  - console keys, game files or dumps (prod.keys, title.keys, .nsp/.xci/.nca/.nso/.nro/...)
  - recompiled output, logs and run reports (recomp_report*, suyu_log*, aot_cache/, exefs/)
  - executables and object files (.exe/.dll/.obj/.lib/.pdb), and any file over 10 MB

Exits non-zero with a list of findings. A clean result is no legal review: also read
docs/a32recomp/RELEASE.md.
"""
import os
import re
import subprocess
import sys

forbidden = [w.lower().encode() for w in sys.argv[1:]]
BAD_EXT = {".keys", ".nsp", ".xci", ".nca", ".nso", ".nro", ".npdm", ".nsz", ".xcz", ".exe",
           ".dll", ".obj", ".lib", ".pdb", ".dmp", ".log"}
BAD_PATH = re.compile(r"(^|/)(aot_cache|exefs|romfs)(/|$)|recomp_report|suyu_log|prod\.keys|"
                      r"title\.keys", re.I)
USERS = re.compile(rb"[A-Za-z]:[\\/]+Users[\\/]+([^\\/\x00\"'\s]{1,40})", re.I)
OK_USERS = {b"public", b"default", b"all users", b"yuzu"}  # yuzu: upstream doc example


def real_user(u):
    """Placeholders such as <user>, {USERNAME}, %s or a bare dot are not names."""
    return u.lower() not in OK_USERS and u[:1].isalnum()

MAX_SIZE = 10 * 1024 * 1024
# Upstream files that are fine despite matching a rule above (path -> reason).
ALLOW = {
    "dist/sounds/midnight_tokyo_lofi.mp3": "upstream asset",
}

files = subprocess.run(["git", "ls-files", "-z"], capture_output=True, check=True).stdout
files = [f.decode() for f in files.split(b"\0") if f]
bad = []
for f in files:
    if f in ALLOW or not os.path.isfile(f):
        continue
    ext = os.path.splitext(f)[1].lower()
    if ext in BAD_EXT:
        bad.append("%s: file type %s" % (f, ext))
    if BAD_PATH.search(f):
        bad.append("%s: game output, key or log path" % f)
    if os.path.getsize(f) > MAX_SIZE:
        bad.append("%s: larger than 10 MB" % f)
    data = open(f, "rb").read()
    low = data.lower()
    for w in forbidden:
        if w in low:
            bad.append("%s: contains a forbidden word" % f)
    for u in set(USERS.findall(data)):
        if real_user(u):
            bad.append("%s: path under C:\\Users\\%s" % (f, u.decode(errors="replace")))

# A new orphan branch has no commits yet: nothing to check there.
ids = subprocess.run(["git", "log", "--format=%an <%ae>%n%cn <%ce>%n%B"], capture_output=True,
                     check=False).stdout.lower()
for w in forbidden:
    if w in ids:
        bad.append("git history (author, committer or message): contains a forbidden word")
for u in set(USERS.findall(ids)):
    if real_user(u):
        bad.append("git history: path under C:\\Users\\%s" % u.decode(errors="replace"))

print("checked %d tracked files" % len(files))
if bad:
    print("\n".join(sorted(set(bad))))
    sys.exit("NOT CLEAN: %d findings" % len(set(bad)))
print("clean")
