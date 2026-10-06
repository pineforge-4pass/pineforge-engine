#!/usr/bin/env python3
"""Real-git tests of scripts/gen_run_failure_catalog_diff.py, and the
catalog's tables in docs/pages/run-failure-codes.md.

The generator's in-file --self-test drives every rule against in-memory
fixtures, and every other mode runs it first. These tests drive the command
line against real repositories instead: the release cycle the release
workflow runs (--write, --check, --stamp, --reset before the tag exists, then
--check on the tagged commit), a removal, a clone without tags (a diff whose
from tag published no catalog is recomputed offline; one whose from tag
published a catalog fails closed), the flags a sibling catalog uses, and
this tree's own checked-in diff.

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

SCRIPT = Path(__file__).resolve().parent / "gen_run_failure_catalog_diff.py"
ROOT = SCRIPT.parents[1]
CATALOG = "docker/run_failure_codes.json"
DIFF = "docker/run_failure_codes_diff.json"
SCHEMA = "pineforge-run-failure-catalog/v1"
# The fixtures' git: no user configuration (signing, hooks, templates) leaks in.
GIT_ENV = {**os.environ, "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": os.devnull,
           "GIT_TERMINAL_PROMPT": "0"}
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

    def run(self, *argv: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, str(SCRIPT), "--root", str(self.path), *argv],
                              env=GIT_ENV, capture_output=True, text=True, cwd=self.path)

    def diff(self) -> dict:
        return json.loads((self.path / DIFF).read_text(encoding="utf-8"))


class CatalogDiff(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="pf-catalog-diff-test-")
        self.addCleanup(temporary.cleanup)
        self.tmp = Path(temporary.name)
        self.repo = Repo(self.tmp / "repo")
        self.repo.path.mkdir()
        self.repo.git("init", "-q")
        self.repo.write("README.md", b"fixture\n")
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
                                 "--output", str(asset)), 1, "previous release tag v0.9.0")
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
        # A diff the release did not reset fails on main.
        self.repo.tag("v1.2.0")
        self.fails(self.repo.run("--check"), 1, "newest release tag reachable from HEAD is v1.2.0")

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
        self.start({"alpha": code("1.1.0")})
        unpublished = self.repo.git("rev-parse", "HEAD")
        self.release("v1.1.0")
        self.repo.write(CATALOG, catalog_bytes({"alpha": code("1.1.0"), "beta": code("1.2.0")}))
        self.ok(self.repo.run("--write"))
        self.repo.commit("beta")
        clone = Repo(self.tmp / "clone")
        subprocess.run([*GIT, "clone", "-q", "--no-tags", str(self.repo.path), str(clone.path)],
                       env=GIT_ENV, check=True, capture_output=True)
        self.assertEqual(clone.git("tag", "--list"), "")
        # The diff starts at v1.1.0, which published a catalog: fail closed.
        self.fails(clone.run("--check"), 2, "Fetch the tags")
        # Before that release the diff started at v1.0.0, which published none.
        clone.git("checkout", "-q", unpublished)
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
