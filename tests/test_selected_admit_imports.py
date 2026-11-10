#!/usr/bin/env python3
"""Check the imports of the built pineforge-selected-admit helper.

The helper must import none of the listed process, thread, session, socket,
descriptor duplication, signalling or library loading functions except dlopen
and recvmsg. These known names come from engine objects linked in whole.
The allowed imports are pinned to those two names. A glibc fortified
(__name_chk), internal (__name) or bare name_chk spelling counts as the name.
A listing without the sentinel imports is rejected as empty, truncated or
unrelated; having them proves neither completeness nor which file nm read.
Every nonblank symbol line must parse or the check fails.
Only symbol tables are read; neither the helper nor a compiler is executed.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


FORBIDDEN = (
    "accept",
    "accept4",
    "bind",
    "clone",
    "clone3",
    "connect",
    "dlopen",
    "dup",
    "dup2",
    "dup3",
    "execl",
    "execle",
    "execlp",
    "execv",
    "execve",
    "execvp",
    "execvpe",
    "fexecve",
    "fork",
    "kill",
    "killpg",
    "popen",
    "posix_spawn",
    "posix_spawnp",
    "pthread_create",
    "recvfrom",
    "recvmmsg",
    "recvmsg",
    "sendmmsg",
    "sendmsg",
    "sendto",
    "setpgid",
    "setpgrp",
    "setsid",
    "socket",
    "socketpair",
    "syscall",
    "system",
    "tgkill",
    "thrd_create",
    "vfork",
)

KNOWN_PRESENT = (
    "dlopen",
    "recvmsg",
)

# The helper uses C++ exceptions, waits with poll and reads with pread, so its
# listing has these imports (a fortified __poll_chk or __pread_chk counts). A
# listing without them is empty, truncated or unrelated; they do not show which
# file nm read.
SENTINEL = ("__cxa_begin_catch", "poll", "pread")


def parse_imports(text, strip_underscore=False):
    """Return undefined symbol base names, raising ValueError on nonblank unparsable lines."""
    names = set()
    for line in text.splitlines():
        if not line.strip():
            continue
        match = re.fullmatch(r"\s*(?:[0-9A-Fa-f]+\s+)?([A-Za-z])\s+(\S+)\s*", line)
        if not match:
            raise ValueError(f"unparsable nm line: {line!r}")
        if match.group(1) not in ("U", "u", "w", "v"):
            continue
        name = match.group(2).split("@", 1)[0]
        if strip_underscore and name.startswith("_"):
            name = name[1:]
        if name:
            names.add(name)
    return names


def spellings(name):
    """Return a name and its base names: without a leading __ and without a trailing _chk."""
    base = name[2:] if name.startswith("__") else name
    forms = {name, base}
    if base.endswith("_chk"):
        forms.add(base[:-4])
    return forms


def violations(names):
    """Return forbidden imports other than the two known names, also under __x or __x_chk."""
    found = set()
    for name in names:
        found.update(spellings(name).intersection(FORBIDDEN))
    return sorted(found.difference(KNOWN_PRESENT))


def missing_sentinel(names):
    """Return expected helper imports that are absent from the listing."""
    present = set()
    for name in names:
        present.update(spellings(name))
    return sorted(set(SENTINEL).difference(present))


def list_imports(path):
    """Read the helper's imports with nm, failing if no names can be read."""
    nm = shutil.which("nm") or "nm"
    failures = []
    for options, strip_underscore in (
        (["-D", "--undefined-only"], False),
        (["-u"], True),
    ):
        command = [nm, *options, path]
        try:
            result = subprocess.run(command, capture_output=True, text=True, check=False)
        except OSError as error:
            failures.append(
                f"{command!r}: return code=None (not started), stderr={str(error)!r}"
            )
            continue
        detail = (
            f"{command!r}: return code={result.returncode}, "
            f"stderr={result.stderr!r}"
        )
        if result.returncode != 0:
            failures.append(detail)
            continue
        if not result.stdout.strip():
            failures.append(detail + "; empty symbol listing")
            continue
        try:
            names = parse_imports(result.stdout, strip_underscore=strip_underscore)
        except ValueError as error:
            raise AssertionError(detail + f"; {error}") from error
        if not names:
            raise AssertionError(detail + "; no undefined symbol names found")
        return names
    raise AssertionError(
        f"Cannot read the helper's symbol table with nm on {sys.platform}:\n"
        + "\n".join(failures)
    )


class HelperImports(unittest.TestCase):
    def test_built_helper_imports(self):
        """The registered helper imports no forbidden names beyond the known names."""
        path = os.environ.get("PINEFORGE_SELECTED_ADMIT_HELPER")
        self.assertTrue(
            path,
            "the test registration must supply PINEFORGE_SELECTED_ADMIT_HELPER",
        )
        self.assertTrue(
            os.path.isfile(path),
            "the test registration must supply an existing file through "
            f"PINEFORGE_SELECTED_ADMIT_HELPER: {path!r}",
        )
        names = list_imports(path)
        self.assertTrue(names, "the helper symbol listing must yield at least one import")
        unexpected = violations(names)
        self.assertEqual(unexpected, [], "forbidden helper imports: " + ", ".join(unexpected))
        missing = missing_sentinel(names)
        self.assertEqual(
            missing,
            [],
            "symbol listing does not look like the helper's imports; missing sentinel names: "
            + ", ".join(missing),
        )

    def test_forbidden_import_is_reported(self):
        """A versioned fork import is reported as a violation."""
        names = parse_imports("                 U fork@GLIBC_2.17\n")
        self.assertEqual(names, {"fork"})
        self.assertEqual(violations(names), ["fork"])

    def test_fortified_and_internal_spellings_are_reported(self):
        """A glibc fortified (__x_chk) or internal (__x) spelling counts as the forbidden name."""
        names = parse_imports("U __recvfrom_chk@GLIBC_2.4\nU __fork\nU __dup2@GLIBC_2.2.5\n")
        self.assertEqual(violations(names), ["dup2", "fork", "recvfrom"])
        # Unrelated fortified or internal names, names that only resemble a forbidden
        # one, and the two allowed names under any spelling are not reported.
        names = parse_imports(
            "U __pread_chk@GLIBC_2.15\nU __libc_start_main\nU fork_chk_helper\n"
            "U __recvmsg\nU __dlopen_chk\n"
        )
        self.assertEqual(violations(names), [])

    def test_sentinel_accepts_fortified_spellings(self):
        """A fortified __poll_chk or __pread_chk stands for poll or pread."""
        self.assertEqual(
            missing_sentinel({"__cxa_begin_catch", "__poll_chk", "__pread_chk"}), []
        )
        self.assertEqual(missing_sentinel({"__cxa_begin_catch", "__pread_chk"}), ["poll"])

    def test_unparsable_first_listing_is_an_error_not_a_fallback(self):
        """An unparsable first listing is an error; the second option's listing is not used instead."""
        with tempfile.TemporaryDirectory() as directory:
            fake = os.path.join(directory, "nm")
            with open(fake, "w", encoding="ascii") as handle:
                handle.write(
                    "#!/bin/sh\n"
                    'if [ "$1" = "-D" ]; then echo "this is not an nm line"; exit 0; fi\n'
                    "echo 'U __cxa_begin_catch'; echo 'U poll'; echo 'U pread'\n"
                )
            os.chmod(fake, 0o755)
            with mock.patch.object(shutil, "which", return_value=fake):
                with self.assertRaises(AssertionError) as caught:
                    list_imports(fake)
        self.assertIn("unparsable nm line", str(caught.exception))

    def test_known_present_imports(self):
        """The two known imports are accepted by name."""
        names = parse_imports("U dlopen@GLIBC_2.34\nU recvmsg@GLIBC_2.17\n")
        self.assertEqual(names, {"dlopen", "recvmsg"})
        self.assertEqual(violations(names), [])

    def test_symbol_versions(self):
        """Unversioned and singly or doubly versioned names have the same base."""
        for symbol in ("fork@@GLIBC_2.17", "fork@GLIBC_2.17", "fork"):
            with self.subTest(symbol=symbol):
                self.assertEqual(parse_imports(f"U {symbol}\n"), {"fork"})

    def test_weak_imports(self):
        """Weak undefined symbols count as imports."""
        names = parse_imports("w posix_spawn@GLIBC_2.17\n")
        self.assertEqual(violations(names), ["posix_spawn"])

    def test_definitions_are_ignored(self):
        """Defined functions and data do not count as imports."""
        names = parse_imports("T fork\nt system\nD socket\n")
        self.assertEqual(names, set())
        self.assertEqual(violations(names), [])

    def test_symbol_names_are_exact(self):
        """Symbols containing forbidden names are accepted as distinct names."""
        names = parse_imports("U ecosystem_x@GLIBC_2.17\nU fork_helper\n")
        self.assertEqual(names, {"ecosystem_x", "fork_helper"})
        self.assertEqual(violations(names), [])

    def test_macho_symbol_prefix(self):
        """The fallback removes one leading Mach-O symbol prefix."""
        self.assertEqual(parse_imports("U _fork\n", strip_underscore=True), {"fork"})

    def test_known_present_is_pinned(self):
        """The only allowed forbidden imports are dlopen and recvmsg."""
        self.assertEqual(KNOWN_PRESENT, ("dlopen", "recvmsg"))

    def test_known_present_is_bounded(self):
        """Known names belong to the forbidden list of 41 distinct names."""
        self.assertLessEqual(set(KNOWN_PRESENT), set(FORBIDDEN))
        self.assertEqual(len(FORBIDDEN), 41)
        self.assertEqual(len(set(FORBIDDEN)), 41)

    def test_new_forbidden_names(self):
        """The additional execution, messaging and syscall imports are forbidden."""
        names = parse_imports(
            "U execvpe\nU fexecve\nU sendmmsg\nU recvmmsg\nU syscall@GLIBC_2.17\n"
        )
        self.assertEqual(names, {"execvpe", "fexecve", "sendmmsg", "recvmmsg", "syscall"})
        self.assertEqual(
            violations(names), ["execvpe", "fexecve", "recvmmsg", "sendmmsg", "syscall"]
        )

    def test_unparsable_line_is_an_error(self):
        """Unexpected text and object headers cannot hide imports."""
        for text in ("U fork\nthis is not an nm line\n", "listing.o:\n"):
            with self.subTest(text=text):
                with self.assertRaises(ValueError) as caught:
                    parse_imports(text)
                self.assertTrue(str(caught.exception).startswith("unparsable nm line:"))

    def test_blank_lines_are_ignored(self):
        """Blank symbol lines do not contribute imports."""
        self.assertEqual(parse_imports("\n   \nU fork\n\n"), {"fork"})

    def test_sentinel_is_reported(self):
        """Missing helper imports are reported in sorted order."""
        self.assertEqual(missing_sentinel({"write"}), ["__cxa_begin_catch", "poll", "pread"])
        self.assertEqual(missing_sentinel({"poll", "pread", "write"}), ["__cxa_begin_catch"])
        self.assertEqual(missing_sentinel({"__cxa_begin_catch", "poll", "pread", "write"}), [])


if __name__ == "__main__":
    unittest.main()
