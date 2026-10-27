"""cS U1/U2: fixed binary64 population and independently classified spellings.

CTest registers the four perturbation families separately. Every family visits
ALL 200,000 finite random patterns and the named edges; none samples native
acceptance or skips an input because either parser dislikes it. Run on the lab.
"""
from __future__ import annotations

import itertools
import math
import random
import struct

import pytest

import native_sink_review_support as review
from test_run_json_native_sink_json import native_library


SEED = 0x397C5E20261027
RANDOM_PATTERNS = 200_000
BATCH = 1024
EDGES = (
    ("tie-even-down", 562949953421312.25),
    ("tie-even-up", 562949953421312.75),
    ("2^53-1", float((1 << 53) - 1)),
    ("2^53+1-rounded", float((1 << 53) + 1)),
    ("2^53+2", float((1 << 53) + 2)),
    ("largest-subnormal", struct.unpack("!d", bytes.fromhex("000fffffffffffff"))[0]),
    ("smallest-subnormal", struct.unpack("!d", bytes.fromhex("0000000000000001"))[0]),
    ("literal-5e-324", 5e-324),
    ("max-finite", 1.7976931348623157e308),
    ("1e22", 1e22),
    ("1e23", 1e23),
    ("9007199254740993-as-float-text", float("9007199254740993")),
    ("0.1+0.2", 0.1 + 0.2),
    ("negative-zero", -0.0),
    ("positive-zero", 0.0),
    ("smallest-normal", 2.2250738585072014e-308),
    ("negative-max-finite", -1.7976931348623157e308),
    ("negative-smallest-subnormal", -5e-324),
)
FAMILIES = ("digit", "trailing-zero", "uppercase-E", "plus")


def samples(label):
    assert RANDOM_PATTERNS >= 200_000 and len(EDGES) >= 13
    print(f"{label}: seed={SEED:#x}, finite random quota={RANDOM_PATTERNS}, "
          f"named edges={len(EDGES)}", flush=True)
    for name, value in EDGES:
        assert math.isfinite(value), name
        yield name, value
    rng = random.Random(SEED)
    finite = draws = nonfinite = 0
    while finite < RANDOM_PATTERNS:
        bits = rng.getrandbits(64)
        draws += 1
        value = struct.unpack("!d", bits.to_bytes(8, "big"))[0]
        if not math.isfinite(value):
            nonfinite += 1  # Only the requested finite domain is selected.
            continue
        finite += 1
        yield f"random[{finite}]={bits:016x}", value
    assert finite == RANDOM_PATTERNS and draws == finite + nonfinite
    print(f"{label}: generated finite={finite}, draws={draws}, "
          f"nonfinite patterns excluded={nonfinite}", flush=True)


def trailing_zero(token):
    mantissa, exponent_marker, exponent = token.partition("e")
    if "." not in mantissa:
        mantissa += ".0"
    return mantissa + "0" + (exponent_marker + exponent if exponent_marker else "")


def perturb(token, family):
    if family == "trailing-zero":
        return trailing_zero(token)
    if family == "uppercase-E":
        return token.replace("e", "E") if "e" in token else token + "E+0"
    if family == "plus":
        # Leading '+' is not JSON, including '+-...' for negative values.
        return "+" + token
    assert family == "digit"
    mantissa_end = token.index("e") if "e" in token else len(token)
    position = next(index for index in range(mantissa_end - 1, -1, -1)
                    if token[index].isdigit())
    digit = token[position]
    replacement = str(int(digit) - 1) if digit != "0" else "1"
    return token[:position] + replacement + token[position + 1:]


def test_f64_fragment_population(native_library, tmp_path):
    """Every input value appears in BOTH E f64s and BOTH T price fields."""
    population = iter(samples("fragment bytes"))
    checked = batches = 0
    path = tmp_path / "f64.sink"
    while True:
        batch = list(itertools.islice(population, BATCH))
        if not batch:
            break
        rows = []
        for index, (name, value) in enumerate(batch):
            # Python's actual float, not a hand-maintained expected spelling.
            assert review.compact(value).decode("ascii") == repr(value), name
            rows += [review.row("E", index, value, value), review.trade(index + 1, value)]
        expected = review.host_fragments(native_library[2], path, review.pack(rows))
        assert expected.counts == {"E": len(batch), "T": len(batch)}
        checked += len(batch)
        batches += 1
        if batches % 25 == 0:
            print(f"fragment bytes: checked={checked}, batches={batches}", flush=True)
    assert checked == RANDOM_PATTERNS + len(EDGES)
    assert checked >= 200_000 + 13 and batches > 0
    print(f"fragment bytes: checked={checked}, E rows={checked}, T rows={checked}, "
          f"f64 field comparisons={4 * checked}", flush=True)


@pytest.mark.parametrize("family", FAMILIES)
def test_f64_wire_perturbations(native_library, tmp_path, family):
    """One refused spelling per value per family, with canonical digit twins."""
    checked = refused = canonical_twins = 0
    per_tag = {"E": 0, "T": 0}
    path = tmp_path / "nonrepr.sink"
    for index, (name, value) in enumerate(samples(family)):
        token = perturb(repr(value), family)
        tag = "E" if index % 2 == 0 else "T"
        context = (family, name, repr(value), token, tag)
        outcome = None
        if family == "digit":
            # A single changed digit can be a DIFFERENT canonical double.
            # Test that twin too; never falsely require its refusal or skip it.
            converted = float(token)
            canonical = math.isfinite(converted) and repr(converted) == token
            twin = review.compare_prepare(
                native_library[2], path, review.pack([review.token_row(tag, token.encode("ascii"))]),
                must_refuse=not canonical, context=context)
            assert isinstance(twin, review.Source) == canonical, context
            if canonical:
                canonical_twins += 1
                # Also refuse a non-repr spelling of the ORIGINAL input.
                token = trailing_zero(repr(value))
            else:
                outcome = twin  # Already compared and refused by BOTH routes.
        if outcome is None:
            if family != "plus":
                converted = float(token)
                assert math.isfinite(converted) and token != repr(converted), context
            outcome = review.compare_prepare(
                native_library[2], path, review.pack([review.token_row(tag, token.encode("ascii"))]),
                must_refuse=True, context=(family, name, token, tag))
        assert isinstance(outcome, review.Refusal)
        if family == "plus":
            assert "invalid UTF-8 or JSON:" in outcome.message, context
        else:
            assert outcome.message.endswith(
                "f64 column is not the Python repr of its double"), context
        refused += 1
        per_tag[tag] += 1
        checked += 1
        if checked % 25_000 == 0:
            print(f"{family}: checked={checked}, refused={refused}, "
                  f"canonical digit twins={canonical_twins}", flush=True)
    expected = RANDOM_PATTERNS + len(EDGES)
    assert checked == refused == expected
    assert checked >= 200_000 + 13 and min(per_tag.values()) >= 100_000
    print(f"{family}: seed={SEED:#x}, checked={checked}, refused={refused}, "
          f"by tag={per_tag}, canonical digit twins={canonical_twins}", flush=True)


@pytest.mark.parametrize("tag", ("E", "T"))
@pytest.mark.parametrize("token", (b"-0", b"1E+22", b"9007199254740993.0"),
                         ids=("integer-negative-zero", "uppercase-exponent", "rounded-text"))
def test_f64_spelling_controls(native_library, tmp_path, tag, token):
    refusal = review.source_refusal(native_library[2], tmp_path / "spelling.sink",
                                   review.pack([review.token_row(tag, token)]))
    assert refusal.message.endswith("f64 column is not the Python repr of its double")
