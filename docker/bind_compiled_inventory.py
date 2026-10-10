#!/usr/bin/env python3
"""Bind a source-bound request-feed inventory to the strategy library just linked.

docker/entrypoint.sh calls this once, after g++ linked the strategy and before it
starts run_json.py, on the selected-window path only:

    bind_compiled_inventory.py --inventory <source-bound inventory>
                               --cpp <the C++ file g++ compiled>
                               --so <the library g++ linked>
                               --out <the bound inventory to write>

The inventory is the one the pinned transpiler returned beside the generated C++
(transpile_with_request_inventory), or the trusted caller's for a precompiled C++.
This helper does file and JSON work and reports its outcome by exit status, and
nothing else. The binding itself, the exact-shape check and the source check
included, is pineforge_codegen.request_feed_inventory.bind_request_feed_inventory
of the pinned codegen the image bundles: nothing is canonicalized, hashed or
compared here, so there is no second implementation to drift from the pinned one.
The library is read as bytes and never loaded, and no source or library is
evaluated to infer a dependency.

The bound inventory is written and synced as a temporary file beside --out, then
published with link(2), which creates --out only if nothing has that name. A file
(or a dangling symlink) that exists at that instant, one that appeared after the
early check included, makes the link fail with EEXIST and the run is refused: --out
is never replaced, the caller's inventory included, and a reader of --out sees all
of the synced bytes or no file. The temporary name stays beside --out, a second
name of the same inode, and after a refusal it stays as well. Nothing is deleted
here: the pipeline's per-run work dir, which the entrypoint removes on exit, takes
the temporary name with the rest.

Fails closed, one diagnostic line on stderr and no output file, with exit status:

     2  usage error (argparse)
    10  the inventory cannot be read, or is over 8 MiB
    11  the inventory is not strict JSON (bad UTF-8, syntax, a duplicate key or a
        non-finite number)
    12  the compiled C++ or the linked library cannot be read
    13  the bundled pineforge-codegen does not provide the pinned binder
    14  the binder refused the inventory (shape, already bound, or a source_sha256
        that is not that of the C++ bytes given)
    15  the output cannot be written, or exists already (a file that appears while
        the link is made included)
     0  bound; --out holds the bound inventory
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
from typing import Any, List, Optional, Tuple

PROG = "bind_compiled_inventory"

EXIT_INVENTORY_UNREADABLE = 10
EXIT_INVENTORY_NOT_JSON = 11
EXIT_ARTIFACT_UNREADABLE = 12
EXIT_BINDER_MISSING = 13
EXIT_BINDER_REFUSED = 14
EXIT_OUTPUT_REFUSED = 15

# The consumer (docker/request_feed_inventory.py) reads at most this much; a
# larger inventory is incomplete there, so it is refused here, before binding.
INVENTORY_MAX_BYTES = 8 * 1024 * 1024


class Refusal(Exception):
    """An outcome that ends the run with exit_code and one line on stderr."""

    def __init__(self, exit_code: int, message: str) -> None:
        super().__init__(message)
        self.exit_code = exit_code


def _no_duplicate_keys(pairs: List[Tuple[str, Any]]) -> dict:
    out: dict = {}
    for key, value in pairs:
        if key in out:
            raise ValueError(f"duplicate key {key!r:.80}")
        out[key] = value
    return out


def _no_constant(name: str) -> Any:
    raise ValueError(f"non-finite JSON number {name}")


def _read_inventory(path: str) -> Any:
    try:
        with open(path, "rb") as handle:
            data = handle.read(INVENTORY_MAX_BYTES + 1)
    except (OSError, ValueError) as error:
        raise Refusal(EXIT_INVENTORY_UNREADABLE,
                      f"the inventory cannot be read: {error}") from None
    if len(data) > INVENTORY_MAX_BYTES:
        raise Refusal(EXIT_INVENTORY_UNREADABLE,
                      f"the inventory is over {INVENTORY_MAX_BYTES} bytes")
    try:
        return json.loads(data.decode("utf-8"), object_pairs_hook=_no_duplicate_keys,
                          parse_constant=_no_constant)
    except (ValueError, RecursionError) as error:
        raise Refusal(EXIT_INVENTORY_NOT_JSON,
                      f"the inventory is not strict JSON: {error}") from None


def _read_bytes(path: str, what: str) -> bytes:
    try:
        with open(path, "rb") as handle:
            return handle.read()
    except (OSError, ValueError) as error:
        raise Refusal(EXIT_ARTIFACT_UNREADABLE, f"the {what} cannot be read: {error}") from None


def _check_output(out: str) -> str:
    """The directory --out goes into, once --out is known to be a new name there."""
    if os.path.lexists(out):
        raise Refusal(EXIT_OUTPUT_REFUSED,
                      f"{out} exists already; the bound inventory never replaces a file")
    directory = os.path.dirname(os.path.abspath(out))
    if not os.path.isdir(directory):
        raise Refusal(EXIT_OUTPUT_REFUSED, f"the directory of {out} does not exist")
    return directory


def _load_binder() -> Tuple[Any, Any]:
    try:
        from pineforge_codegen.request_feed_inventory import (
            RequestFeedInventoryError, bind_request_feed_inventory)
    except Exception as error:  # noqa: BLE001 -- whatever stops the pinned package loading
        raise Refusal(EXIT_BINDER_MISSING,
                      "the bundled pineforge-codegen does not provide "
                      "request_feed_inventory.bind_request_feed_inventory: "
                      f"{type(error).__name__}: {error}") from None
    return bind_request_feed_inventory, RequestFeedInventoryError


def _serialize(bound: Any) -> str:
    try:
        return json.dumps(bound, ensure_ascii=True, allow_nan=False,
                          separators=(",", ":")) + "\n"
    except (TypeError, ValueError) as error:
        raise Refusal(EXIT_BINDER_REFUSED,
                      f"the bound inventory is not serializable JSON: {error}") from None


def _publish(directory: str, out: str, text: str) -> None:
    """Write text beside out and make it out, never replacing a file named out.

    link(2) is the whole decision: it creates the name only if it is free, so a file
    (or dangling symlink) that appears between the early check and here is refused,
    where a rename would have replaced it. Once linked, the temporary name and out
    are two names of one inode, and both stay until the pipeline's work dir is
    removed; nothing is deleted here, a refusal included."""
    try:
        descriptor, temporary = tempfile.mkstemp(prefix=".bound-inventory-", suffix=".tmp",
                                                 dir=directory)
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
    except OSError as error:
        raise Refusal(EXIT_OUTPUT_REFUSED,
                      f"the bound inventory cannot be written: {error}") from None
    try:
        os.link(temporary, out)
    except FileExistsError:
        raise Refusal(EXIT_OUTPUT_REFUSED,
                      f"{out} exists already; the bound inventory never replaces a file"
                      ) from None
    except OSError as error:
        raise Refusal(EXIT_OUTPUT_REFUSED,
                      f"the bound inventory cannot be written: {error}") from None


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog=PROG, allow_abbrev=False,
        description="Bind a source-bound request-feed inventory to the linked strategy "
                    "library with the pinned pineforge-codegen binder.")
    parser.add_argument("--inventory", required=True,
                        help="source-bound inventory (artifact_sha256 null); only read")
    parser.add_argument("--cpp", required=True, help="the C++ file g++ compiled; only read")
    parser.add_argument("--so", required=True, help="the library g++ linked; read, not loaded")
    parser.add_argument("--out", required=True, help="the bound inventory to create")
    args = parser.parse_args(argv)
    try:
        inventory = _read_inventory(args.inventory)
        cpp_bytes = _read_bytes(args.cpp, "compiled C++ source")
        artifact_bytes = _read_bytes(args.so, "linked library")
        directory = _check_output(args.out)
        bind, binder_error = _load_binder()
        try:
            bound = bind(inventory, cpp_bytes, artifact_bytes)
        except binder_error as error:
            raise Refusal(EXIT_BINDER_REFUSED, f"the inventory is refused: {error}") from None
        except Exception as error:  # noqa: BLE001 -- any other binder failure is a refusal too
            raise Refusal(EXIT_BINDER_REFUSED,
                          f"the inventory cannot be bound: {type(error).__name__}: {error}"
                          ) from None
        _publish(directory, args.out, _serialize(bound))
    except Refusal as refusal:
        sys.stderr.write(f"{PROG}: {refusal}\n")
        return refusal.exit_code
    return 0


if __name__ == "__main__":
    sys.exit(main())
