#!/usr/bin/env python3
"""Real-git tests of scripts/gen_run_failure_catalog_diff.py, and the
catalog's tables in docs/pages/run-failure-codes.md.

The generator's in-file --self-test drives every rule against in-memory
fixtures, and every other mode runs it first. These tests drive the command
line against real repositories instead: the release cycle the release
workflow runs (--write, --check, --stamp, --reset before the tag exists, then
--check on the tagged commit), a final release after its candidate (the diff
from the previous final release and the chain step, then both rebuilt from
git alone with --to-tag), the minor-release rule, a removal, a clone without
tags (a diff whose from tag published no catalog is recomputed offline; one
whose from tag published a catalog is rebuilt by running the diff backwards;
a hidden removal, a forged null from and required tags fail), the flags a
sibling catalog uses, and this tree's own checked-in diff. What a diff says of
a code follows the diff's own start (from.version against the code's since),
so those tests hold before and after the reset a release brings.

The documentation page lists every class, code and argument kind of
docker/run_failure_codes.json between two markers; test_docs_tables holds
that block to the catalog. After a catalog edit, regenerate it with

    python3 scripts/test_gen_run_failure_catalog_diff.py --write-docs-tables
"""
from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parent / "gen_run_failure_catalog_diff.py"
ROOT = SCRIPT.parents[1]
CATALOG = "docker/run_failure_codes.json"
DIFF = "docker/run_failure_codes_diff.json"
SCHEMA = "pineforge-run-failure-catalog/v1"
REQUIRE_TAGS = "PINEFORGE_REQUIRE_RELEASE_TAGS"
# The fixtures' git: no user configuration (signing, hooks, templates) leaks in,
# and no caller's PINEFORGE_REQUIRE_RELEASE_TAGS: a test that wants it sets it.
GIT_ENV = {**{key: value for key, value in os.environ.items() if key != REQUIRE_TAGS},
           "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": os.devnull,
           "GIT_TERMINAL_PROMPT": "0"}

sys.dont_write_bytecode = True
sys.path.insert(0, str(SCRIPT.parent))
import gen_run_failure_catalog_diff as generator  # noqa: E402  (builds the forged diffs)
import check_run_failure_codes as registry_guard  # noqa: E402
GIT = ("git", "-c", "user.name=PineForge test", "-c", "user.email=test@example.invalid",
       "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false", "-c", "init.defaultBranch=main",
       "-c", "core.hooksPath=" + os.devnull)


DOCS_PAGE = ROOT / "docs/pages/run-failure-codes.md"
DOCS_BEGIN = "<!-- BEGIN generated from docker/run_failure_codes.json: " \
             "python3 scripts/test_gen_run_failure_catalog_diff.py --write-docs-tables -->"
DOCS_END = "<!-- END generated tables -->"


def _cell(text: str) -> str:
    return text.replace("|", "\\|").replace("\n", " ")


def docs_tables(catalog: dict) -> str:
    """The page's generated block: the classes, the codes, the argument kinds."""
    lines = ["### Classes", "", "| Class | What failed |", "| --- | --- |"]
    lines += [f"| `{name}` | {_cell(text)} |" for name, text in sorted(catalog["classes"].items())]
    lines += ["", "### Codes", "",
              "| Code | Class | Retryable | Arguments | What it means |",
              "| --- | --- | --- | --- | --- |"]
    for name, entry in sorted(catalog["codes"].items()):
        args = []
        for arg_name, arg in sorted(entry["args"].items()):
            detail = arg["kind"]
            if arg["kind"] == "vocab":
                detail += f", {len(arg['values'])} values"
            if arg.get("optional"):
                detail += ", optional"
            args.append(f"`{arg_name}` ({detail})")
        meaning = entry["description"]
        if entry.get("deprecated"):
            meaning += " Deprecated: use " + ", ".join(f"`{code}`" for code in
                                                         entry.get("replacedBy", [])) + "."
        lines.append(f"| `{name}` | `{entry['class']}` | {'yes' if entry['retryable'] else 'no'}"
                     f" | {', '.join(args) or 'none'} | {_cell(meaning)} |")
    lines += ["", "### Argument kinds", "", "| Kind | Its value |", "| --- | --- |"]
    lines += [f"| `{name}` | {_cell(text)} |" for name, text in sorted(catalog["kinds"].items())]
    return "\n".join(lines) + "\n"


def docs_page_with_tables(page: str, catalog: dict) -> str:
    """`page` with its generated block replaced by the catalog's tables."""
    head, begin, rest = page.partition(DOCS_BEGIN)
    _, end, tail = rest.partition(DOCS_END)
    if not begin or not end:
        raise ValueError(f"{DOCS_PAGE.name} has no generated block ({DOCS_BEGIN} ... {DOCS_END})")
    return head + DOCS_BEGIN + "\n\n" + docs_tables(catalog) + "\n" + DOCS_END + tail


def code(since: str, reason_values: list | None = None) -> dict:
    entry = {"class": "input", "retryable": False, "since": since, "description": "d",
             "english": ["x"], "args": {}}
    if reason_values is not None:
        entry["args"]["reason"] = {"kind": "vocab", "values": reason_values}
    return entry


def catalog_bytes(codes: dict) -> bytes:
    catalog = {"schema": SCHEMA, "classes": {"input": "i"}, "kinds": {"vocab": "v"},
               "codes": codes}
    return (json.dumps(catalog, indent=1, sort_keys=True) + "\n").encode()


def field_at(entry: dict, path: str) -> tuple:
    """Whether `path` (object keys joined by '.', as a diff change spells a
    field) names a member of `entry`, and that member (None when it does not)."""
    value = entry
    for key in path.split("."):
        if not isinstance(value, dict) or key not in value:
            return False, None
        value = value[key]
    return True, value


class Repo:
    def __init__(self, path: Path):
        self.path = path

    def git(self, *argv: str) -> str:
        result = subprocess.run([*GIT, "-C", str(self.path), *argv], env=GIT_ENV,
                                capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(f"git {' '.join(argv)} failed: {result.stderr}")
        return result.stdout.strip()

    def write(self, rel: str, data: bytes) -> None:
        target = self.path / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)

    def commit(self, message: str) -> str:
        self.git("add", "-A")
        self.git("commit", "-q", "-m", message)
        return self.git("rev-parse", "HEAD")

    def tag(self, tag: str) -> None:
        self.git("tag", "-a", tag, "-m", "Release " + tag)

    def run(self, *argv: str, env: dict | None = None) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, str(SCRIPT), "--root", str(self.path), *argv],
                              env={**GIT_ENV, **(env or {})}, capture_output=True, text=True,
                              cwd=self.path)

    def diff(self) -> dict:
        return json.loads((self.path / DIFF).read_text(encoding="utf-8"))

    def write_diff(self, diff: dict) -> None:
        self.write(DIFF, generator.render(diff).encode("utf-8"))

    def built_diff(self, old: bytes | None, tag: str) -> dict:
        """What --write would give from `old`, the catalog `tag` published, to
        this tree's catalog: what a forger runs."""
        return generator.build_diff(old, (self.path / CATALOG).read_bytes(), tag,
                                    generator.default_config(self.path))


class CatalogDiff(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="pf-catalog-diff-test-")
        self.addCleanup(temporary.cleanup)
        self.tmp = Path(temporary.name)
        self.repo = Repo(self.tmp / "repo")
        self.repo.path.mkdir()
        self.repo.git("init", "-q")
        self.repo.write("README.md", b"fixture\n")
        self.repo.write("VERSION", b"1.0.0\n")   # what release.yml writes with each tag
        self.repo.commit("initial")
        self.repo.tag("v1.0.0")          # a release that published no catalog

    def ok(self, result: subprocess.CompletedProcess) -> str:
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def fails(self, result: subprocess.CompletedProcess, status: int, needle: str) -> None:
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)
        self.assertIn(needle, result.stderr)

    def start(self, codes: dict) -> None:
        self.repo.write(CATALOG, catalog_bytes(codes))
        self.ok(self.repo.run("--write"))
        self.repo.commit("catalog")

    def release(self, tag: str) -> None:
        """What .github/workflows/release.yml does: reset, commit, tag, check."""
        self.ok(self.repo.run("--reset", "--tag", tag))
        self.repo.write("VERSION", (tag[1:] + "\n").encode())
        self.repo.commit("chore(release): " + tag)
        self.repo.tag(tag)
        self.ok(self.repo.run("--check"))

    def assert_diff_states_code(self, diff: dict, name: str, entry: dict) -> None:
        """What `diff` must say of the code `name`, whose catalog entry is `entry`.

        The diff's own start decides, so one expectation holds before and after
        the reset a release brings (--reset: from that release, nothing added).
        `since` is the release that first ships the code, from.version the
        release whose catalog the diff is against; the generator's version_key
        compares them as numbers, a candidate X.Y.Z-rc.N below its release X.Y.Z.
        """
        start, since = diff["from"]["version"], entry["since"]
        start_key, since_key = generator.version_key(start), generator.version_key(since)
        self.assertIsNotNone(start_key, f"from.version {start!r} is no release version")
        self.assertIsNotNone(since_key, f"{name}: since {since!r} is no release version")
        added = [item for item in diff["added"] if item["code"] == name]
        self.assertNotIn(name, [item["code"] for item in diff["removed"]],
                         f"{name} is removed: a published code is deprecated, never removed")
        # since is X.Y.Z, which a release shares with its candidates: from a candidate of
        # its own release (since X.Y.Z, the diff from X.Y.Z-rc.N) it cannot tell whether
        # the candidate shipped the code, so the diff says, and is held to what it says.
        after_start = bool(added) if start.startswith(since + "-rc.") else since_key > start_key
        if after_start:
            # It has not shipped in the diff's published base: corrections to its
            # class/args are part of the whole addition, not a breaking change.
            self.assertEqual(added, [{"code": name, "collection": "codes", "entry": entry}],
                             f"{name} first ships in {since}, after {start} where the diff "
                             "starts: the diff must add it, whole")
            self.assertNotIn(name, [item["code"] for item in diff["changed"]],
                             f"{name} first ships in {since}, after {start} where the diff "
                             "starts: it is an addition, not a change")
            return
        # The start's catalog carries it: the diff does not add it. It may list it as
        # changed -- a correction made since the release -- but for real changes only,
        # and as this tree's catalog holds them.
        self.assertEqual(added, [],
                         f"{name} first ships in {since}, not after {start} where the diff "
                         "starts: it is no addition")
        changed = [item for item in diff["changed"] if item["code"] == name]
        self.assertLessEqual(len(changed), 1, f"{name} is changed twice")
        for item in changed:
            self.assertEqual(item["collection"], "codes")
            self.assertTrue(item["fields"], f"{name} is changed in no field")
            for change in item["fields"]:
                sides = [(change[f"{side}Present"], json.dumps(change[side], sort_keys=True))
                         for side in ("before", "after")]
                where = f"{name} {change['path']}"
                self.assertNotEqual(sides[0], sides[1],
                                    f"{where} is listed as changed, and did not change")
                held, value = field_at(entry, change["path"])
                self.assertEqual(sides[1], (held, json.dumps(value, sort_keys=True)),
                                 f"{where}: the after side is not this tree's catalog")

    def test_self_test(self) -> None:
        result = subprocess.run([sys.executable, str(SCRIPT), "--self-test"],
                                capture_output=True, text=True)
        self.assertIn("self-test OK", self.ok(result))

    def test_release_cycle(self) -> None:
        self.start({"alpha": code("1.1.0")})
        diff = self.repo.diff()
        self.assertEqual(diff["from"], {"version": "1.0.0", "tag": "v1.0.0",
                                        "catalogSha256": None})
        self.assertEqual([item["code"] for item in diff["added"]], ["alpha"])
        self.ok(self.repo.run("--check"))
        # The catalog moves without the diff: --check names it, --write heals it.
        self.repo.write(CATALOG, catalog_bytes({"alpha": code("1.1.0"),
                                                "beta": code("1.1.0", ["one"])}))
        self.fails(self.repo.run("--check"), 1, "to.catalogSha256")
        self.ok(self.repo.run("--write"))
        self.repo.commit("beta")
        self.ok(self.repo.run("--check"))
        # Stamp the release's asset; a since that is not this release is refused.
        asset = self.tmp / "assets" / "run_failure_codes_diff-v1.1.0.json"
        self.ok(self.repo.run("--stamp", "--to-version", "1.1.0", "--previous-tag", "v1.0.0",
                              "--output", str(asset)))
        stamped = json.loads(asset.read_text(encoding="utf-8"))
        checked_in = self.repo.diff()
        self.assertEqual(stamped["to"], {"version": "1.1.0",
                                         "catalogSha256": checked_in["to"]["catalogSha256"]})
        self.assertEqual({k: v for k, v in stamped.items() if k != "to"},
                         {k: v for k, v in checked_in.items() if k != "to"})
        self.fails(self.repo.run("--stamp", "--to-version", "1.2.0", "--previous-tag", "v1.0.0",
                                 "--output", str(asset)), 1, "since is '1.1.0'")
        self.fails(self.repo.run("--stamp", "--to-version", "1.1.0", "--previous-tag", "v0.9.0",
                                 "--output", str(asset)), 1, "--previous-tag is v0.9.0")
        # The release: reset before the tag exists, commit with VERSION, tag, check.
        self.release("v1.1.0")
        catalog = (self.repo.path / CATALOG).read_bytes()
        self.assertEqual(self.repo.diff()["from"], {
            "version": "1.1.0", "tag": "v1.1.0",
            "catalogSha256": hashlib.sha256(catalog).hexdigest()})
        self.assertEqual(self.repo.diff()["added"], [])
        # The next change on main starts from the release's catalog.
        self.repo.write(CATALOG, catalog_bytes({"alpha": code("1.1.0"),
                                                "beta": code("1.1.0", ["one", "two"]),
                                                "gamma": code("1.2.0")}))
        self.fails(self.repo.run("--check"), 1, "to.catalogSha256")
        self.ok(self.repo.run("--write"))
        self.repo.commit("gamma")
        self.ok(self.repo.run("--check"))
        diff = self.repo.diff()
        self.assertEqual([item["code"] for item in diff["added"]], ["gamma"])
        self.assertEqual([(item["code"], [f["path"] for f in item["fields"]])
                          for item in diff["changed"]], [("beta", ["args.reason.values"])])
        # Tag arrival preserves the explicit base and the checked-in bytes.
        self.repo.tag("v1.2.0")
        self.ok(self.repo.run("--check"))
        self.assertEqual(self.repo.diff(), diff)

    def prepare_tag_arrival(self) -> bytes:
        """S adds beta after v1.7.0; tagging S must not require a reset."""
        self.start({"alpha": code("1.7.0")})
        self.release("v1.7.0")
        base = (self.repo.path / CATALOG).read_bytes()
        self.repo.write("VERSION", b"1.8.0\n")
        self.repo.write(CATALOG, catalog_bytes({"alpha": code("1.7.0"),
                                               "beta": code("1.8.0")}))
        self.ok(self.repo.run("--write"))
        self.repo.commit("S: prepare 1.8.0 by ordinary PR")
        self.ok(self.repo.run("--check", env={REQUIRE_TAGS: "1"}))
        self.repo.tag("v1.8.0")
        return base

    def test_tag_arrival_preserves_s_and_unchanged_descendant(self) -> None:
        self.prepare_tag_arrival()
        before = (self.repo.path / DIFF).read_bytes()
        with self.subTest(tree="S"):
            self.ok(self.repo.run("--check", env={REQUIRE_TAGS: "1"}))
        self.repo.write("README.md", b"D: unrelated change\n")
        self.repo.commit("D")
        with self.subTest(tree="D"):
            self.ok(self.repo.run("--check", env={REQUIRE_TAGS: "1"}))
        self.assertEqual((self.repo.path / DIFF).read_bytes(), before)
        self.assertEqual(self.repo.diff()["from"]["tag"], "v1.7.0")

    def test_tag_arrival_on_ancestor_preserves_preexisting_descendant(self) -> None:
        self.start({"alpha": code("1.7.0")})
        self.release("v1.7.0")
        self.repo.write("VERSION", b"1.8.0\n")
        source = self.repo.commit("S: prepared release")
        self.ok(self.repo.run("--check"))
        self.repo.write("README.md", b"D: already merged before tag arrival\n")
        self.repo.commit("D")
        self.ok(self.repo.run("--check"))
        self.repo.git("tag", "-a", "v1.8.0", source, "-m", "Release 1.8.0")
        self.ok(self.repo.run("--check", env={REQUIRE_TAGS: "1"}))

    def test_tag_arrival_allows_descendant_addition_from_old_base(self) -> None:
        base = self.prepare_tag_arrival()
        self.repo.write(CATALOG, catalog_bytes({"alpha": code("1.7.0"),
                                               "beta": code("1.8.0"),
                                               "gamma": code("1.9.0")}))
        self.repo.write_diff(self.repo.built_diff(base, "v1.7.0"))
        self.repo.commit("D: add gamma against the explicit base")
        self.ok(self.repo.run("--check", env={REQUIRE_TAGS: "1"}))
        self.assertEqual([item["code"] for item in self.repo.diff()["added"]],
                         ["beta", "gamma"])
        self.repo.tag("v1.9.0")
        self.ok(self.repo.run("--check", env={REQUIRE_TAGS: "1"}))
        # Housekeeping still advances the base to the newest reachable tag.
        self.ok(self.repo.run("--write"))
        self.assertEqual(self.repo.diff()["from"]["tag"], "v1.9.0")
        self.ok(self.repo.run("--check"))

    def test_tag_arrival_refuses_removal_first_published_after_base(self) -> None:
        base = self.prepare_tag_arrival()
        self.repo.write(CATALOG, catalog_bytes({"alpha": code("1.7.0")}))
        self.repo.write_diff(self.repo.built_diff(base, "v1.7.0"))
        self.repo.commit("D: hide removal of beta behind old base")
        self.assertEqual(self.repo.diff()["removed"], [])
        self.fails(self.repo.run("--check", env={REQUIRE_TAGS: "1"}), 1,
                   "v1.8.0: removed is not empty (codes.beta)")
        self.fails(self.repo.run("--check"), 1,
                   "a published code is deprecated, never removed")
        # A nearer tag without beta must not hide the earlier publication.
        self.repo.tag("v1.8.1")
        self.fails(self.repo.run("--check"), 1, "v1.8.0: removed is not empty (codes.beta)")
        self.repo.write(CATALOG, catalog_bytes({
            "alpha": code("1.7.0"),
            "beta": dict(code("1.8.0"), deprecated=True, replacedBy=["alpha"])}))
        self.repo.write_diff(self.repo.built_diff(base, "v1.7.0"))
        self.ok(self.repo.run("--check"))

    def test_explicit_base_must_be_an_ancestor(self) -> None:
        self.start({"alpha": code("1.7.0")})
        self.release("v1.7.0")
        base = (self.repo.path / CATALOG).read_bytes()
        self.repo.git("checkout", "-q", "-b", "unmerged")
        self.repo.write("README.md", b"unmerged release\n")
        self.repo.commit("unmerged")
        self.repo.tag("v1.7.1")
        self.repo.git("checkout", "-q", "main")
        self.repo.write_diff(self.repo.built_diff(base, "v1.7.1"))
        self.fails(self.repo.run("--check"), 1,
                   "from.tag v1.7.1 is not an ancestor of HEAD")

    def test_registry_guard_keeps_every_reachable_publication(self) -> None:
        base = self.prepare_tag_arrival()
        self.repo.write(CATALOG, catalog_bytes({"alpha": code("1.7.0")}))
        self.repo.write_diff(self.repo.built_diff(base, "v1.7.0"))
        self.repo.commit("D: beta removed")
        self.repo.tag("v1.8.1")
        # Isolate history from the unrelated generated C++ and throw scanner.
        # The compact fixture has the full validator's required metadata.
        current = json.loads((self.repo.path / CATALOG).read_bytes())
        current["classes"] = {name: "c" for name in registry_guard.CLASSES}
        current["kinds"] = {name: "k" for name in registry_guard.KINDS}
        self.repo.write(CATALOG, registry_guard.canonical(current).encode())
        with patch.object(registry_guard, "check_generated", return_value=[]), \
                patch.object(registry_guard, "uncoded_throws", return_value=[]):
            errors, _ = registry_guard.run_checks(self.repo.path, require_tags="test")
        self.assertTrue(any("against v1.8.0: code beta was removed" in error
                            for error in errors), errors)
        # Existing compatibility checks also see codes absent from the nearest tag.
        current["codes"]["beta"] = code("1.9.0")
        self.repo.write(CATALOG, registry_guard.canonical(current).encode())
        with patch.object(registry_guard, "check_generated", return_value=[]), \
                patch.object(registry_guard, "uncoded_throws", return_value=[]):
            errors, _ = registry_guard.run_checks(self.repo.path)
        self.assertTrue(any("against v1.8.0: code beta: since changed" in error
                            for error in errors), errors)

    def test_explicit_base_must_exist_when_other_tags_are_readable(self) -> None:
        self.start({"alpha": code("1.7.0")})
        self.repo.write_diff(self.repo.built_diff(None, "v1.6.0"))
        self.fails(self.repo.run("--check"), 2, "from.tag v1.6.0 does not exist")

    def test_explicit_base_hash_and_current_catalog_bytes_are_checked(self) -> None:
        self.prepare_tag_arrival()
        good = self.repo.diff()
        forged = json.loads(json.dumps(good))
        forged["from"]["catalogSha256"] = "0" * 64
        self.repo.write_diff(forged)
        self.fails(self.repo.run("--check"), 1, "from.catalogSha256")
        self.repo.write_diff(good)
        self.repo.write(CATALOG, (self.repo.path / CATALOG).read_bytes() + b"\n")
        self.fails(self.repo.run("--check"), 1, "to.catalogSha256")
        self.repo.write(CATALOG, (self.repo.path / CATALOG).read_bytes().rstrip() + b"\n")
        forged = json.loads(json.dumps(good))
        forged["added"][0]["entry"]["description"] = "forged"
        self.repo.write_diff(forged)
        self.fails(self.repo.run("--check"), 1, "is stale")

    def test_stamp_to_sha_uses_unpublished_source_after_head_advances(self) -> None:
        self.start({"alpha": code("1.7.0")})
        self.release("v1.7.0")
        self.repo.write("VERSION", b"1.8.0\n")
        self.repo.write(CATALOG, catalog_bytes({"alpha": code("1.7.0"),
                                               "beta": code("1.8.0")}))
        self.ok(self.repo.run("--write"))
        source = self.repo.commit("S: unpublished source")
        source_diff = (self.repo.path / DIFF).read_bytes()
        self.repo.write(CATALOG, catalog_bytes({"alpha": code("1.7.0"),
                                               "beta": code("1.8.0"),
                                               "gamma": code("1.9.0")}))
        self.ok(self.repo.run("--write"))
        self.repo.commit("D: next release")
        asset = self.tmp / "pretag.json"
        args = ("--stamp", "--to-sha", source, "--to-version", "1.8.0",
                "--previous-tag", "v1.7.0", "--output", str(asset))
        self.ok(self.repo.run(*args))
        pretag = asset.read_bytes()
        stamped = json.loads(pretag)
        self.assertEqual([item["code"] for item in stamped["added"]], ["beta"])
        self.assertEqual(stamped["to"], {
            "version": "1.8.0", "catalogSha256": json.loads(source_diff)["to"]["catalogSha256"]})
        self.repo.git("tag", "-a", "v1.8.0", source, "-m", "Release 1.8.0")
        self.ok(self.repo.run("--stamp", "--to-tag", "v1.8.0", "--previous-tag", "v1.7.0",
                              "--output", str(asset)))
        self.assertEqual(asset.read_bytes(), pretag)
        self.ok(self.repo.run(*args))
        self.assertEqual(asset.read_bytes(), pretag)
        self.fails(self.repo.run("--stamp", "--to-sha", source, "--to-version", "1.9.0",
                                 "--previous-tag", "v1.7.0", "--output", str(asset)), 1,
                   "VERSION at --to-sha is '1.8.0', expected '1.9.0'")

    def test_stamp_to_sha_preserves_rc_chain_without_source_reset(self) -> None:
        self.start({"alpha": code("1.7.0")})
        self.release("v1.7.0")
        self.repo.write("VERSION", b"1.8.0-rc.1\n")
        self.repo.write(CATALOG, catalog_bytes({"alpha": code("1.7.0"),
                                               "beta": code("1.8.0")}))
        self.ok(self.repo.run("--write"))
        candidate = self.repo.commit("candidate")
        stable, step = self.tmp / "stable.json", self.tmp / "step.json"
        args = ("--stamp", "--to-sha", candidate, "--to-version", "1.8.0-rc.1",
                "--previous-tag", "v1.7.0", "--output", str(stable),
                "--chain-output", str(step))
        self.ok(self.repo.run(*args))
        self.assertFalse(step.exists())
        self.repo.tag("v1.8.0-rc.1")
        base = self.repo.git("show", "v1.7.0:" + CATALOG).encode() + b"\n"
        self.repo.write("VERSION", b"1.8.0\n")
        self.repo.write(CATALOG, catalog_bytes({"alpha": code("1.7.0"),
                                               "beta": code("1.8.0"),
                                               "gamma": code("1.8.0")}))
        self.repo.write_diff(self.repo.built_diff(base, "v1.7.0"))
        source = self.repo.commit("final: explicit base remains v1.7.0")
        args = ("--stamp", "--to-sha", source, "--to-version", "1.8.0",
                "--previous-tag", "v1.7.0", "--output", str(stable))
        self.fails(self.repo.run(*args), 2, "name --chain-output")
        self.ok(self.repo.run(*args, "--chain-output", str(step)))
        self.assertEqual([item["code"] for item in json.loads(stable.read_bytes())["added"]],
                         ["beta", "gamma"])
        self.assertEqual(json.loads(step.read_bytes())["from"]["tag"], "v1.8.0-rc.1")
        self.assertEqual([item["code"] for item in json.loads(step.read_bytes())["added"]],
                         ["gamma"])
        before = stable.read_bytes(), step.read_bytes()
        self.repo.tag("v1.8.0")
        self.ok(self.repo.run(*args, "--chain-output", str(step)))
        self.assertEqual((stable.read_bytes(), step.read_bytes()), before)
        again = self.tmp / "again.json"
        for start, expected in (("v1.7.0", before[0]), ("v1.8.0-rc.1", before[1])):
            self.ok(self.repo.run("--stamp", "--to-tag", "v1.8.0", "--previous-tag", start,
                                  "--output", str(again)))
            self.assertEqual(again.read_bytes(), expected)

    def test_stamp_to_sha_checks_source_bytes_and_ancestry(self) -> None:
        self.start({"alpha": code("1.7.0")})
        self.release("v1.7.0")
        self.repo.write("VERSION", b"1.8.0\n")
        self.repo.write(CATALOG, catalog_bytes({"alpha": code("1.7.0"),
                                               "beta": code("1.8.0")}))
        self.ok(self.repo.run("--write"))
        source = self.repo.commit("S: valid source")
        self.repo.write(CATALOG, catalog_bytes({"alpha": code("1.7.0"),
                                               "beta": code("1.8.0"),
                                               "gamma": code("1.8.0")}))
        stale = self.repo.commit("stale source diff")
        self.ok(self.repo.run("--write"))
        self.repo.commit("D: repair diff")
        asset = self.tmp / "asset.json"

        def stamp(sha: str, previous: str = "v1.7.0", output: Path = asset):
            return self.repo.run("--stamp", "--to-sha", sha, "--to-version", "1.8.0",
                                 "--previous-tag", previous, "--output", str(output))

        self.fails(stamp(stale), 1, "to.catalogSha256")
        self.assertFalse(asset.exists())
        self.fails(stamp(source[:12]), 2, "full lowercase 40-character commit SHA")
        self.fails(stamp(source, output=self.repo.path / DIFF), 2, "checked-in diff")
        self.repo.git("checkout", "-q", "-b", "unmerged")
        self.repo.write("README.md", b"unmerged source\n")
        unmerged = self.repo.commit("unmerged")
        self.repo.tag("v1.7.1")
        self.repo.git("checkout", "-q", "main")
        self.fails(stamp(unmerged), 1, "--to-sha is not an ancestor of HEAD")
        self.fails(stamp(source, "v1.7.1"), 1,
                   "--to-sha does not reach --previous-tag v1.7.1")
        self.assertFalse(asset.exists())
        self.ok(stamp(source))

    def test_stamp_to_sha_keeps_minor_and_since_rules(self) -> None:
        self.start({"alpha": code("1.7.0")})
        self.release("v1.7.0")
        for version, since, finding in (
                ("1.7.1", "1.7.1", "bump minor (or override 1.8.0)"),
                ("1.8.0", "1.9.0", "which is after this release 1.8.0")):
            with self.subTest(version=version):
                self.repo.write("VERSION", (version + "\n").encode())
                self.repo.write(CATALOG, catalog_bytes({"alpha": code("1.7.0"),
                                                       "beta": code(since)}))
                self.ok(self.repo.run("--write"))
                source = self.repo.commit("prepared " + version)
                asset = self.tmp / (version + ".json")
                self.fails(self.repo.run("--stamp", "--to-sha", source,
                                         "--to-version", version, "--previous-tag", "v1.7.0",
                                         "--output", str(asset)), 1, finding)
                self.assertFalse(asset.exists())

    def test_final_release_after_a_candidate(self) -> None:
        self.start({"alpha": code("1.1.0")})
        self.release("v1.1.0")
        # 1.2.0 ships through a candidate: beta in the candidate, gamma after it.
        self.repo.write(CATALOG, catalog_bytes({"alpha": code("1.1.0"), "beta": code("1.2.0")}))
        self.ok(self.repo.run("--write"))
        self.repo.commit("beta")
        assets = self.tmp / "assets"
        candidate_step = assets / "run_failure_codes_diff-v1.2.0-rc.1-from-v1.1.0.json"
        self.ok(self.repo.run("--stamp", "--to-version", "1.2.0-rc.1", "--previous-tag", "v1.1.0",
                              "--output", str(assets / "run_failure_codes_diff-v1.2.0-rc.1.json"),
                              "--chain-output", str(candidate_step)))
        # A candidate's diff starts where its notes do: no chain step of its own.
        self.assertFalse(candidate_step.exists())
        self.release("v1.2.0-rc.1")
        self.repo.write(CATALOG, catalog_bytes({"alpha": code("1.1.0"), "beta": code("1.2.0"),
                                                "gamma": code("1.2.0")}))
        self.ok(self.repo.run("--write"))
        self.repo.commit("gamma")
        self.assertEqual(self.repo.diff()["from"]["tag"], "v1.2.0-rc.1")
        # The final release: its notes start at v1.1.0, its chain at the candidate.
        stable = assets / "run_failure_codes_diff-v1.2.0.json"
        step = assets / "run_failure_codes_diff-v1.2.0-from-v1.2.0-rc.1.json"
        stamp = ("--stamp", "--to-version", "1.2.0", "--previous-tag", "v1.1.0",
                 "--output", str(stable))
        self.fails(self.repo.run(*stamp), 2, "--chain-output")
        self.ok(self.repo.run(*stamp, "--chain-output", str(step)))
        catalog = (self.repo.path / CATALOG).read_bytes()
        published = catalog_bytes({"alpha": code("1.1.0")})   # v1.1.0's catalog
        stable_diff = json.loads(stable.read_text(encoding="utf-8"))
        step_diff = json.loads(step.read_text(encoding="utf-8"))
        self.assertEqual(stable_diff["from"], {
            "version": "1.1.0", "tag": "v1.1.0",
            "catalogSha256": hashlib.sha256(published).hexdigest()})
        self.assertEqual([item["code"] for item in stable_diff["added"]], ["beta", "gamma"])
        self.assertEqual(step_diff["from"]["tag"], "v1.2.0-rc.1")
        self.assertEqual([item["code"] for item in step_diff["added"]], ["gamma"])
        for diff in (stable_diff, step_diff):
            self.assertEqual(diff["to"], {"version": "1.2.0",
                                          "catalogSha256": hashlib.sha256(catalog).hexdigest()})
        # Once tagged, git alone rebuilds both assets byte for byte (the
        # recovery when the GitHub release was not created after the push).
        self.release("v1.2.0")
        again = self.tmp / "again.json"
        for start, asset in (("v1.1.0", stable), ("v1.2.0-rc.1", step)):
            self.ok(self.repo.run("--stamp", "--to-tag", "v1.2.0", "--previous-tag", start,
                                  "--output", str(again)))
            self.assertEqual(again.read_bytes(), asset.read_bytes(), start)
        # A previous tag HEAD does not reach (a hotfix tagged on another branch)
        # is refused: the diff and the notes would start at different releases.
        self.repo.git("checkout", "-q", "-b", "hotfix")
        self.repo.write("README.md", b"hotfix\n")
        self.repo.commit("hotfix")
        self.repo.tag("v1.2.1")
        self.repo.git("checkout", "-q", "main")
        self.fails(self.repo.run("--stamp", "--to-version", "1.3.0", "--previous-tag", "v1.2.1",
                                 "--output", str(again)), 1, "HEAD does not reach")

    def test_a_release_that_adds_or_changes_codes_is_a_minor_one(self) -> None:
        self.start({"alpha": code("1.1.0")})
        self.release("v1.1.0")
        self.repo.write(CATALOG, catalog_bytes({"alpha": code("1.1.0"), "beta": code("1.2.0")}))
        self.ok(self.repo.run("--write"))
        self.repo.commit("beta")
        asset = str(self.tmp / "asset.json")
        # The default patch bump: one line says what to do.
        result = self.repo.run("--stamp", "--to-version", "1.1.1", "--previous-tag", "v1.1.0",
                               "--output", asset)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(result.stderr.splitlines(), [
            "gen_run_failure_catalog_diff: this release adds or changes codes: bump minor "
            "(or override 1.2.0)"])
        self.ok(self.repo.run("--stamp", "--to-version", "1.2.0", "--previous-tag", "v1.1.0",
                              "--output", asset))
        # A changed field alone needs the minor release too.
        self.repo.write(CATALOG, catalog_bytes({"alpha": dict(code("1.1.0"), description="e")}))
        self.ok(self.repo.run("--write"))
        self.fails(self.repo.run("--stamp", "--to-version", "1.1.1", "--previous-tag", "v1.1.0",
                                 "--output", asset), 1, "bump minor (or override 1.2.0)")
        # An added code names a release after the diff's start: --check says so.
        self.repo.write(CATALOG, catalog_bytes({"alpha": code("1.1.0"), "beta": code("1.1.0")}))
        self.ok(self.repo.run("--write"))
        self.fails(self.repo.run("--check"), 1, "since is '1.1.0', but the diff starts at 1.1.0")

    def test_removed_code_is_refused(self) -> None:
        self.start({"alpha": code("1.1.0"), "beta": code("1.1.0")})
        self.release("v1.1.0")
        self.repo.write(CATALOG, catalog_bytes({"alpha": code("1.1.0")}))
        self.fails(self.repo.run("--write"), 1, "removed is not empty (codes.beta)")
        self.assertEqual([item["code"] for item in self.repo.diff()["removed"]], ["beta"])
        self.fails(self.repo.run("--check"), 1, "removed is not empty")
        self.fails(self.repo.run("--stamp", "--to-version", "1.2.0", "--previous-tag", "v1.1.0",
                                 "--output", str(self.tmp / "asset.json")), 1,
                   "removed is not empty")
        self.assertFalse((self.tmp / "asset.json").exists())

    def test_reset_refuses_a_tag_carrying_another_catalog(self) -> None:
        self.start({"alpha": code("1.1.0")})
        self.release("v1.1.0")
        self.repo.write(CATALOG, catalog_bytes({"alpha": code("1.1.0"), "beta": code("1.2.0")}))
        self.fails(self.repo.run("--reset", "--tag", "v1.1.0"), 1, "is not this tree's catalog")

    def test_clone_without_tags(self) -> None:
        # v1.0.0 published no catalog; v1.4.0, the first catalog release, does.
        self.start({"alpha": code("1.4.0", ["one"])})
        unpublished = self.repo.git("rev-parse", "HEAD")
        self.release("v1.4.0")
        published = (self.repo.path / CATALOG).read_bytes()
        self.repo.write(CATALOG, catalog_bytes({"alpha": code("1.4.0", ["one", "two"]),
                                                "beta": code("1.5.0")}))
        self.ok(self.repo.run("--write"))
        self.repo.commit("beta")
        clone = Repo(self.tmp / "clone")
        subprocess.run([*GIT, "clone", "-q", "--no-tags", str(self.repo.path), str(clone.path)],
                       env=GIT_ENV, check=True, capture_output=True)
        self.assertEqual(clone.git("tag", "--list"), "")
        # The diff starts at v1.4.0, which published a catalog: the diff run
        # backwards rebuilds it, its sha256 is from.catalogSha256, and the diff
        # is then recomputed in full.
        self.assertIn("rebuilt the catalog v1.4.0 published", self.ok(clone.run("--check")))
        self.assertEqual(generator.reconstruct_old_catalog(
            clone.diff(), (clone.path / CATALOG).read_bytes(),
            generator.default_config(clone.path)), published)
        # Where the tags are required -- every workflow that runs the guards -- it fails.
        self.fails(clone.run("--check", env={REQUIRE_TAGS: "1"}), 2, "Fetch the tags")
        # A removal the diff hides: alpha gone, the diff says nothing of it.
        clone.write(CATALOG, catalog_bytes({"beta": code("1.5.0")}))
        hidden = clone.built_diff(published, "v1.4.0")
        self.assertEqual([item["code"] for item in hidden["removed"]], ["alpha"])
        hidden["removed"] = []
        clone.write_diff(hidden)
        self.fails(clone.run("--check"), 1, "rebuilds a catalog hashing to")
        # A forged null from: "v1.4.0 published no catalog", everything added.
        clone.write_diff(clone.built_diff(None, "v1.4.0"))
        self.fails(clone.run("--check"), 1, "not older than 1.4.0")
        # A forged null from under an older tag -- or a branch cut before
        # v1.4.0 and rebased after it, keeping its old diff: VERSION names
        # v1.4.0, the release the diff must start at.
        for older in ("v1.3.0", "v1.3.99", "v0.0.0"):
            clone.write_diff(clone.built_diff(None, older))
            self.fails(clone.run("--check"), 1, f"from.tag is {older}, but VERSION is '1.4.0'")
        # Before that release the diff started at v1.0.0, which published none.
        clone.git("checkout", "-q", "--force", unpublished)
        result = clone.run("--check")
        self.assertIn("recomputed in full without git", self.ok(result))
        # Recomputed means compared: a tampered addition still fails offline.
        diff = clone.diff()
        diff["added"][0]["entry"]["since"] = "9.9.9"
        clone.write(DIFF, (json.dumps(diff, indent=2) + "\n").encode())
        self.fails(clone.run("--check"), 1, "stale")

    def test_generic_catalog_flags(self) -> None:
        catalog = {"schema": "pineforge-diagnostics-catalog/v2", "features": ["a"],
                   "codes": {"PF-E0001": {"message": "m", "args": {}}},
                   "fragments": {"PF-F0001": {"message": "f", "args": {}}}}
        self.repo.write("pkg/diagnostics_catalog.json",
                        (json.dumps(catalog, indent=2) + "\n").encode())
        flags = ("--catalog", "pkg/diagnostics_catalog.json",
                 "--diff", "pkg/diagnostics_catalog_diff.json",
                 "--schema", "pineforge-diagnostics-catalog/v2",
                 "--diff-schema", "pineforge-diagnostics-catalog-diff/v1",
                 "--collection", "codes", "--collection", "fragments")
        self.ok(self.repo.run("--write", *flags))
        diff = json.loads((self.repo.path / "pkg/diagnostics_catalog_diff.json").read_text())
        self.assertEqual(diff["schema"], "pineforge-diagnostics-catalog-diff/v1")
        self.assertEqual([(item["collection"], item["code"]) for item in diff["added"]],
                         [("codes", "PF-E0001"), ("fragments", "PF-F0001")])
        self.assertEqual([change["path"] for change in diff["metadataChanges"]], ["features"])
        self.ok(self.repo.run("--check", *flags))
        self.fails(self.repo.run("--check", *flags[:-2]), 1, "stale")

    def test_this_tree(self) -> None:
        # The source guard's own command, on the checked-in diff.
        result = subprocess.run([sys.executable, str(SCRIPT), "--check"], capture_output=True,
                                text=True, cwd=ROOT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_report_sink_failed_is_engine_fault(self) -> None:
        # APP derives billing from this shipped JSON. Pin the named code's
        # policy independently of the C++ registry's classification test.
        catalog = json.loads((ROOT / CATALOG).read_text(encoding="utf-8"))
        entry = catalog["codes"]["report_sink_failed"]
        self.assertEqual(entry["class"], "engine_fault")
        self.assertIs(entry["retryable"], False)
        self.assertEqual(entry["since"], "1.7.0")
        self.assertEqual(entry["args"], {
            "arena_bytes": {"kind": "integer", "optional": True},
            "cap_bytes": {"kind": "integer", "optional": True},
            "claimed_bytes": {"kind": "integer", "optional": True},
            "high_water_bytes": {"kind": "integer", "optional": True},
            "needed_bytes_lower_bound": {"kind": "integer", "optional": True},
            "reason": {
                "kind": "vocab",
                "optional": True,
                "values": ["arena_exhausted", "arena_io", "arena_backing", "scratch_io",
                           "sink_io", "sink_peer_closed", "sink_exhausted"],
            },
            "sink_bytes": {"kind": "integer", "optional": True},
            "store": {
                "kind": "vocab",
                "optional": True,
                "values": ["curve", "position_tail", "cohort", "exit_rank", "sequence_index",
                           "sequence_index_idx", "erased_minima", "erased_minima_idx",
                           "partial_cycles", "partial_cycles_idx", "successor_roots", "command",
                           "unknown"],
            },
        })
        self.assertEqual(entry["english"], [
            "report sink open failed",
            "report sink write failed",
            "report sink write made no progress",
        ])
        # What the diff says of the code follows the diff's own start: an addition
        # while its release is in progress (the diff starts at an older release),
        # part of the base once a release reset has moved the start to its release.
        diff = json.loads((ROOT / DIFF).read_text(encoding="utf-8"))
        self.assert_diff_states_code(diff, "report_sink_failed", entry)

    def test_a_codes_diff_holds_before_and_after_its_release_reset(self) -> None:
        # alpha ships in 1.1.0. While that release is in progress the diff, from
        # v1.0.0, adds it; after the reset the release brings it is in the diff's
        # base and the diff adds nothing. One expectation holds on both sides.
        alpha = code("1.1.0")
        self.start({"alpha": alpha})
        before = self.repo.diff()
        self.assertEqual([item["code"] for item in before["added"]], ["alpha"])
        self.assert_diff_states_code(before, "alpha", alpha)
        self.release("v1.1.0")
        after = self.repo.diff()
        self.assertEqual(after["added"], [])
        self.assert_diff_states_code(after, "alpha", alpha)

        def refused(diff: dict, needle: str) -> None:
            with self.assertRaises(AssertionError) as raised:
                self.assert_diff_states_code(diff, "alpha", alpha)
            self.assertIn(needle, str(raised.exception))

        def starting_at(diff: dict, version: str) -> dict:
            moved = json.loads(json.dumps(diff))
            moved["from"]["version"] = version
            return moved

        # The expectation can fail: each diff below says the other side's thing.
        refused(starting_at(before, "1.1.0"), "is no addition")
        refused(starting_at(after, "1.0.0"), "the diff must add it")
        # A candidate sorts below its release: 1.0.9-rc.2 starts before 1.1.0,
        # 1.2.0-rc.1 after it.
        for version, state in (("1.0.9-rc.2", before), ("1.2.0-rc.1", after)):
            self.assert_diff_states_code(starting_at(state, version), "alpha", alpha)
        refused(starting_at(after, "1.0.9-rc.2"), "the diff must add it")
        refused(starting_at(before, "1.2.0-rc.1"), "is no addition")
        # 1.1.0-rc.1 shares alpha's since: the diff says whether the candidate
        # shipped alpha, and either way is held to its rules.
        for state in (before, after):
            self.assert_diff_states_code(starting_at(state, "1.1.0-rc.1"), "alpha", alpha)
        # A correction made after the release is a change of a published code: the
        # diff may say so, truthfully, and nothing else.
        said = {"path": "description", "beforePresent": True, "afterPresent": True,
                "before": "older", "after": "d"}
        corrected = starting_at(after, "1.1.0")
        corrected["changed"] = [{"code": "alpha", "collection": "codes", "fields": [said]}]
        self.assert_diff_states_code(corrected, "alpha", alpha)
        for lie, needle in ((dict(said, before="d"), "did not change"),
                            (dict(said, after="e"), "not this tree's catalog")):
            lying = starting_at(corrected, "1.1.0")
            lying["changed"][0]["fields"] = [lie]
            refused(lying, needle)
        # Before its release the code is an addition, whole: no change beside it.
        unreleased = starting_at(before, "1.0.0")
        unreleased["changed"] = corrected["changed"]
        refused(unreleased, "not a change")
        # A published code is never removed.
        removed = starting_at(after, "1.1.0")
        removed["removed"] = [{"code": "alpha", "collection": "codes", "entry": alpha}]
        refused(removed, "is removed")

    def test_docs_tables(self) -> None:
        catalog = json.loads((ROOT / CATALOG).read_text(encoding="utf-8"))
        page = DOCS_PAGE.read_text(encoding="utf-8")
        self.assertEqual(page, docs_page_with_tables(page, catalog),
                         f"{DOCS_PAGE.relative_to(ROOT)} does not list the catalog's classes, "
                         "codes and kinds: run python3 "
                         "scripts/test_gen_run_failure_catalog_diff.py --write-docs-tables")
        # The block must be able to fail: a code the page lacks is caught.
        extra = json.loads(json.dumps(catalog))
        extra["codes"]["zz_unlisted_code"] = code("9.9.9")
        self.assertNotEqual(page, docs_page_with_tables(page, extra))


if __name__ == "__main__":
    if sys.argv[1:] == ["--write-docs-tables"]:
        catalog = json.loads((ROOT / CATALOG).read_text(encoding="utf-8"))
        page = DOCS_PAGE.read_text(encoding="utf-8")
        DOCS_PAGE.write_text(docs_page_with_tables(page, catalog), encoding="utf-8")
        print(f"wrote the generated tables of {DOCS_PAGE.relative_to(ROOT)}")
        raise SystemExit(0)
    unittest.main()
