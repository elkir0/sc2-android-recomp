#!/usr/bin/env python3
"""rapport-bug.py — Markdown bug report for a GitHub issue, scrubbed of personal data.

    python3 scripts/rapport-bug.py [--journal FILE] ... [--lignes N] [--sortie FILE]

Builds a ready-to-paste Markdown report on stdout (or in --sortie FILE):

  ## Environment                  date, OS, architecture, Python, and the versions of
                                  cmake / cc / clang / git when found on the PATH
  ## Repository                   revision, clean or modified tree, repository root
  ## SC2_* environment variables  every SC2_* variable that is set (value scrubbed)
  ## Journal — FILE               the last N lines of each --journal FILE
                                  (N = --lignes, 200 by default)

Every line of the output is scrubbed: home directories (/Users/<name>/,
/home/<name>/ and the current home) become ~/ ; the login name becomes <user> ;
IPv4 addresses become <ip> ; e-mail addresses become <email> ; adb mDNS device
serials become <serial> ; token-like strings (sk-…, ghp_…, xox?-…) and hex strings
of 32 characters or more become <redacted>.

Refusals (exit code 2, one message on stderr, nothing written anywhere): a
--journal file with a forbidden extension (game binaries and assets — the very
list of .github/workflows/legal-scan.yml), larger than 20 MB, containing a NUL
byte (binary), or not a readable file. The script never uploads anything and
reads nothing but the --journal files it is given.

Options : --journal FILE (répétable), --lignes N (défaut 200), --sortie FILE.
Codes de sortie : 0 = rapport écrit ; 2 = option inconnue ou --journal refusé.
Bibliothèque standard uniquement ; aucun accès réseau.
"""

import argparse
import getpass
import os
import platform
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

TOOLS = ("cmake", "cc", "clang", "git")
DEFAULT_LINES = 200
MAX_BYTES = 20 * 1024 * 1024

# Refused --journal extensions: game binaries, assets and derived data — the exact
# list enforced on the repository by .github/workflows/legal-scan.yml (step 1).
FORBIDDEN_SUFFIXES = frozenset(
    "exe dll so apk mpq mpqe dds m3 s2map sc2map sc2bank bank s2v iso w3x "
    "bin pak ogv swf fxa ogg wav mp4 dylib s2ma sc2save sc2replay".split()
)


class Refused(Exception):
    """A --journal file must not enter the report; the message goes to stderr."""


def _login_name():
    try:
        return getpass.getuser()
    except Exception:
        return os.environ.get("USER") or os.environ.get("USERNAME") or ""


HOME_DIR = os.path.expanduser("~")
LOGIN = _login_name()

RE_TOKEN = re.compile(r"\b(?:sk-[A-Za-z0-9_-]{16,}|ghp_[A-Za-z0-9]{20,}|xox[a-z]?-[A-Za-z0-9-]{8,})")
RE_EMAIL = re.compile(r"[A-Za-z0-9._%+-]+@(?:[A-Za-z0-9-]+\.)+[A-Za-z]{2,}")
RE_ADB_SERIAL = re.compile(r"\badb-[A-Za-z0-9]{6,}-[A-Za-z0-9]+\._adb[A-Za-z0-9.-]*")
RE_HOME_PREFIX = re.compile(r"(?<![\w.$-])/(?:Users|home)/[^/\s'\"]+/")
RE_IPV4 = re.compile(r"\b(?:\d{1,3}\.){3}\d{1,3}\b")
RE_HEX32 = re.compile(r"\b[0-9a-fA-F]{32,}\b")


def _replace_ipv4(match):
    octets = match.group(0).split(".")
    return "<ip>" if all(int(octet) <= 255 for octet in octets) else match.group(0)


def scrub_line(line, home=None, login=None):
    """One line with every personal or secret pattern replaced by its placeholder."""
    home = HOME_DIR if home is None else home
    login = LOGIN if login is None else login
    out = RE_TOKEN.sub("<redacted>", line)
    out = RE_EMAIL.sub("<email>", out)
    out = RE_ADB_SERIAL.sub("<serial>", out)
    if home and len(home) > 1:
        home = home.rstrip("/")
        # the home directory, never the prefix of a longer name
        # (/Users/<login>dev/ is somebody else's home; the regex below takes it)
        garde = r"(?<![\w.$-])" + re.escape(home)
        out = re.sub(garde + r"/(?![\w-])", "~/", out)
        out = re.sub(garde + r"(?![\w-])", "~", out)
    out = RE_HOME_PREFIX.sub("~/", out)
    out = out.replace("~//", "~/")     # a home path ending on a separator: one ~/
    out = RE_IPV4.sub(_replace_ipv4, out)
    out = RE_HEX32.sub("<redacted>", out)
    if login and re.fullmatch(r"[A-Za-z][A-Za-z0-9._-]{2,31}", login):
        # a whole word only (anthonyedev is another user), and never inside a
        # placeholder (<email>, <redacted>, …) inserted by the rules above
        out = re.sub(r"(?<![<\w])" + re.escape(login) + r"(?![>\w])", "<user>", out)
    return out


def _cell(value):
    """Table cell text: a raw pipe would cut the column."""
    return str(value).replace("|", "\\|")


def tool_version(name):
    """First line of `<name> --version` and where it was found — never raises."""
    path = shutil.which(name)
    if not path:
        return "not found on PATH"
    try:
        done = subprocess.run([path, "--version"], capture_output=True, text=True, timeout=15)
        text = (done.stdout or done.stderr).strip()
        first = text.splitlines()[0] if text else "exit code %d, no output" % done.returncode
    except (OSError, subprocess.SubprocessError) as error:
        first = "could not run it (%s)" % type(error).__name__
    return "%s — %s" % (first, path)


def _git(args):
    """Output of one git command in the repository, empty when it fails."""
    try:
        done = subprocess.run(["git", "-C", str(REPO), *args],
                              capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return ""
    return done.stdout.strip() if done.returncode == 0 else ""


def read_journal(path):
    """The text of one --journal file, or raises Refused (bad extension, over
    20 MB, NUL byte, unreadable)."""
    file = Path(path)
    suffix = file.suffix.lower().lstrip(".")
    if suffix in FORBIDDEN_SUFFIXES:
        raise Refused("--journal '%s': forbidden extension .%s (game binaries and assets, "
                      "the list of .github/workflows/legal-scan.yml)" % (path, suffix))
    if not file.is_file():
        raise Refused("--journal '%s': not a readable file" % path)
    size = file.stat().st_size
    if size > MAX_BYTES:
        raise Refused("--journal '%s': %d bytes, above the 20 MB limit" % (path, size))
    data = file.read_bytes()          # at most 20 MB, checked just above
    if b"\x00" in data:
        raise Refused("--journal '%s': contains a NUL byte (binary file)" % path)
    return data.decode("utf-8", errors="replace")


def build_report(journals, tail):
    """The Markdown lines of the report, before scrubbing."""
    lines = []
    add = lines.append
    add("# Bug report — sc2-android-recomp")
    add("")
    add("Built locally by `scripts/rapport-bug.py` — nothing was uploaded. Every line below")
    add("is scrubbed: home paths → `~/`, login → `<user>`, IPv4 → `<ip>`, e-mail → `<email>`,")
    add("adb device serial → `<serial>`, tokens and 32+-character hex → `<redacted>`.")
    add("")
    add("## Environment")
    add("")
    add("| item | value |")
    add("|---|---|")
    add("| date (UTC) | %s |" % datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S"))
    add("| OS | %s %s |" % (platform.system(), platform.release()))
    mac = platform.mac_ver()[0]
    if mac:
        add("| macOS version | %s |" % mac)
    add("| architecture | %s |" % platform.machine())
    add("| Python | %s (%s) |" % (platform.python_version(), sys.executable))
    for name in TOOLS:
        add("| %s | %s |" % (name, _cell(tool_version(name))))
    add("")
    add("## Repository")
    add("")
    if shutil.which("git"):
        revision = _git(["rev-parse", "--short", "HEAD"]) or "unknown (not a git repository)"
        dirty = "modified (uncommitted changes)" if _git(["status", "--porcelain"]) else "clean"
    else:
        revision, dirty = "unknown (git not found on PATH)", "unknown"
    add("| item | value |")
    add("|---|---|")
    add("| root | %s |" % REPO)
    add("| revision | %s |" % revision)
    add("| tree | %s |" % dirty)
    add("")
    add("## SC2_* environment variables")
    add("")
    sc2 = sorted((name, value) for name, value in os.environ.items() if name.startswith("SC2_"))
    if not sc2:
        add("None set.")
    else:
        add("| name | value |")
        add("|---|---|")
        for name, value in sc2:
            if len(value) > 240:
                value = value[:240] + " … (truncated)"
            add("| `%s` | `%s` |" % (name, _cell(value)))
    for path, text in journals:
        file_lines = text.splitlines()
        kept = file_lines[-tail:] if tail > 0 else []
        add("")
        add("## Journal — %s (last %d of %d lines)" % (path, len(kept), len(file_lines)))
        add("")
        add("````text")
        lines.extend(kept)
        add("````")
    add("")
    return lines


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="rapport-bug.py",
        description="Build a scrubbed Markdown bug report (options and exit codes in the "
                    "header comment of scripts/rapport-bug.py).")
    parser.add_argument("--journal", action="append", default=[], metavar="FILE",
                        help="add the last lines of FILE to the report (repeatable)")
    parser.add_argument("--lignes", type=int, default=DEFAULT_LINES, metavar="N",
                        help="lines kept per --journal FILE (default: %d)" % DEFAULT_LINES)
    parser.add_argument("--sortie", metavar="FILE",
                        help="write the report to FILE instead of stdout")
    args = parser.parse_args(argv)
    if args.lignes < 0:
        parser.error("--lignes expects a positive number of lines")

    journals = []
    try:
        for path in args.journal:
            journals.append((path, read_journal(path)))
    except Refused as refused:
        print("rapport-bug: %s" % refused, file=sys.stderr)
        return 2

    # every journal accepted: the report can be written now, and only now
    report = "\n".join(scrub_line(line) for line in build_report(journals, args.lignes)) + "\n"
    if args.sortie:
        try:
            Path(args.sortie).write_text(report, encoding="utf-8")
        except OSError as error:
            print("rapport-bug: --sortie '%s': %s" % (args.sortie, error), file=sys.stderr)
            return 2
    else:
        try:
            sys.stdout.write(report)
        except BrokenPipeError:
            return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
