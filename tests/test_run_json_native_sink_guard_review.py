"""cS U6 and section 3: window, cap, terminal-record and refusal priority gaps."""
from __future__ import annotations

import hashlib

import pytest

import native_sink_review_support as review
from test_run_json_native_sink_json import native_library


def escaped(row):
    tag, line = row
    literal = f'["{tag}"'.encode("ascii")
    encoded = ('["\\u%04x"' % ord(tag)).encode("ascii")
    assert line.startswith(literal)
    return tag, encoded + line[len(literal):]


@pytest.mark.parametrize("offset", (65535, 65536, 65537))
def test_guard_escaped_tag_window(native_library, tmp_path, offset):
    """Offsets are zero-based stream positions of the tag's backslash."""
    for tag in ("E", "T"):
        header = review.existing.fixtures.header()
        anchor = review.row("E", 0, 1.0, -0.0)
        filler = review.trade()
        target = escaped(review.row("E", 1, 2.0, -0.0) if tag == "E"
                         else review.trade(2, 2.0))
        padding = offset - (len(review.compact(header)) + 1
                            + len(anchor[1]) + len(filler[1]) + 2)
        assert padding > 0
        filler = review.trade(comment="x" * padding)
        payload = review.pack([anchor, filler, target], header=header)
        escape = ("\\u%04x" % ord(tag)).encode("ascii")
        assert payload[offset:offset + 6] == escape
        assert len(filler[1]) < review.CAP
        with review.guarded_pair(native_library[2], tmp_path / f"window-{tag}.sink",
                                 payload) as pair:
            answer = pair.compare(tag)
            assert answer == b"[" + pair.source.fragments[tag] + b"]"
            assert pair.calls[-1] == (2, ord(tag), 0, 1)


def test_guard_skipped_line_over_one_mib(native_library, tmp_path):
    """Skip an oversize T body during E extraction, with both digest witnesses."""
    path = tmp_path / "oversize-skipped.sink"
    rows = [review.row("E", 0, 3.0, -0.0),
            review.trade(1, comment="a" * (review.CAP // 2 + 64)),
            review.trade(2, comment="b" * (review.CAP // 2 + 64))]
    baseline = review.pack(rows)
    with review.guarded_pair(native_library[2], path, baseline) as pair:
        assert pair.compare("E") == b"[" + pair.source.fragments["E"] + b"]"
        # Replace precisely the first T's LF in place. Identity/size stay put.
        joined = rows[1][1][:-1] + b"x" + rows[2][1]
        assert len(joined) > review.CAP
        prefix = pair.source.raw_header + rows[0][1] + joined
        trailer = baseline[baseline.rfind(b"\n{") + 1:]
        changed = prefix + trailer
        assert len(changed) == len(baseline)
        path.write_bytes(changed)
        refusal = pair.compare("E", must_refuse=True)
        assert refusal.message.endswith("digest differs")
        assert pair.calls[-1][2] == 5, "skipped line was parsed/capped"
        # Isolate guard semantics with a coherent Python-computed witness.
        # This is NOT a claim that prepare accepts the merged T record.
        pair.strict.sha256 = pair.native.sha256 = hashlib.sha256(prefix).hexdigest()
        answer = pair.compare("E")
        assert answer == b"[" + pair.source.fragments["E"] + b"]"
        assert pair.calls[-1] == (2, ord("E"), 0, 1)


@pytest.mark.parametrize("ending", ("lf", "quote"))
def test_guard_escaped_tag_cap_plus_one(native_library, tmp_path, ending):
    header = review.existing.fixtures.header()
    header["engine"] = "padding"
    anchor = review.row("E", 0, 1.0, -0.0)
    empty = review.trade()
    filler = review.trade(comment="x" * (review.CAP - len(empty[1])))
    assert len(filler[1]) == review.CAP
    baseline = review.pack([anchor, filler], header=header)
    path = tmp_path / "escaped-cap.sink"
    with review.guarded_pair(native_library[2], path, baseline) as pair:
        # The exact-cap twin must first pass prepare AND both guard scans.
        for tag in ("E", "T"):
            assert pair.compare(tag) == b"[" + pair.source.fragments[tag] + b"]"
        opening = b'["\\u0045'
        suffix = b"\n" if ending == "lf" else b'"\n'
        # The (cap+1)th scanned tag byte is LF or its closing quote.
        line = opening + b"x" * (review.CAP - len(opening)) + suffix
        assert len(line) == review.CAP + len(suffix)
        assert line[review.CAP:review.CAP + 1] == suffix[:1]
        header["engine"] = header["engine"][:-len(suffix)]
        prefix = review.compact(header) + b"\n" + anchor[1] + line
        trailer = baseline[baseline.rfind(b"\n{") + 1:]
        changed = prefix + trailer
        assert len(changed) == len(baseline)
        path.write_bytes(changed)
        refusal = pair.compare("E", must_refuse=True)
        assert refusal.message.endswith("line exceeds 1 MiB including LF")
        assert pair.calls[-1][2:] == (2, 0)


@pytest.mark.parametrize("tag", ("E", "T"))
def test_last_record_only_defect(native_library, tmp_path, tag):
    rows = [review.row("E", 0, 1.0, -0.0), review.trade()]
    last = review.row("E", 1, 2.0, 0.0) if tag == "E" else review.trade(2, 2.0)
    path = tmp_path / "last-record.sink"
    baseline = review.pack([*rows, last])
    # Only the last row's f64 spelling is wrong; counts and SHA are recomputed.
    fields = last[1].split(b",")
    fields[2 if tag == "E" else 5] = b"1e4"
    bad = tag, b",".join(fields)
    payload = review.pack([*rows, bad])
    assert len(payload) == len(baseline)
    with review.guarded_pair(native_library[2], path, baseline) as pair:
        for selected in ("T", "E"):
            assert pair.compare(selected) == b"[" + pair.source.fragments[selected] + b"]"
        path.write_bytes(payload)
        guard_refusal = pair.compare(tag, must_refuse=True)
        assert guard_refusal.message.endswith("f64 column is not the Python repr of its double")
    refusal = review.source_refusal(native_library[2], path, payload)
    assert "line 4:" in refusal.message
    assert refusal.message.endswith("f64 column is not the Python repr of its double")


def test_empty_sink_zero_counts(native_library, tmp_path):
    """Zero is valid on an empty sink; Python, rather than the brief, decides."""
    expected = review.host_fragments(native_library[2], tmp_path / "empty.sink", review.pack())
    assert expected.counts == {"E": 0, "T": 0}
    assert expected.fragments == {"E": b"", "T": b""}


@pytest.mark.parametrize("tag", ("E", "T"))
@pytest.mark.parametrize("count", (1, 0.0, False), ids=("one", "float-zero", "bool-zero"))
def test_empty_sink_wrong_count(native_library, tmp_path, tag, count):
    def mismatch(trailer):
        trailer["counts"][tag] = count
        return trailer
    refusal = review.source_refusal(
        native_library[2], tmp_path / "empty-count.sink", review.pack(trailer_edit=mismatch))
    assert refusal.message.endswith(f"line 2: trailer count mismatch for {tag}")


@pytest.mark.parametrize("pair", ("2", "5", "11", "13"))
def test_two_defect_source_priority(native_library, tmp_path, pair):
    rows = [review.row("E", 0, 1.0, 0.0)]
    edit = None
    if pair == "2":
        rows = [("E", b'["E",0,1e999,"\xff"]\n')]
    elif pair == "5":
        rows = [("E", b'["E",true,NaN,0.0]\n')]
    elif pair == "11":
        def edit(trailer):
            # Exactly key order + marker value; counts/digest remain correct.
            return {"trailer": "x", "counts": trailer["counts"],
                    "version": trailer["version"], "sha256": trailer["sha256"]}
    else:
        assert pair == "13"
        def edit(trailer):
            trailer["counts"]["E"] = 9
            trailer["sha256"] = "0" * 64
            return trailer
    refusal = review.source_refusal(native_library[2], tmp_path / "priority.sink",
                                    review.pack(rows, trailer_edit=edit))
    # These locate the two defects; the expected FINAL refusal above came
    # from SinkReader and the actual strict host, never the native witness.
    if pair == "2":
        assert "codec can't decode byte 0xff" in refusal.message
    elif pair == "5":
        assert refusal.message.endswith("non-JSON numeric constant NaN")
    elif pair == "11":
        assert refusal.message.endswith("trailer keys are missing, extra or out of order")
    else:
        assert refusal.message.endswith("trailer count mismatch for E")


def test_two_defect_guard_g2(native_library, tmp_path):
    path = tmp_path / "g2.sink"
    baseline = review.pack([review.row("E", 0, 1.0, 0.0)])
    with review.guarded_pair(native_library[2], path, baseline) as pair:
        changed = baseline.replace(b'"version":1,', b'"version":?,', 1)
        assert changed != baseline and len(changed) == len(baseline)
        prefix = changed[:changed.rfind(b"\n{") + 1]
        assert hashlib.sha256(prefix).hexdigest() != pair.source.digest
        path.write_bytes(changed)
        refusal = pair.compare("E", must_refuse=True)
        assert "invalid UTF-8 or JSON:" in refusal.message
        # Compare the FINAL host refusal, irrespective of the raw C witness.


def test_guard_identity_before_header_framing(native_library, tmp_path):
    path = tmp_path / "identity.sink"
    baseline = review.pack([review.row("E", 0, 1.0, 0.0)])
    with review.guarded_pair(native_library[2], path, baseline) as pair:
        path.write_bytes(b"no LF and a different size")
        refusal = pair.compare("E", must_refuse=True)
        assert refusal.message.endswith("file identity differs")
        assert pair.calls[-1] == (2, ord("E"), 4, 0)


@pytest.mark.parametrize("token", (b"9007199254740991", b"9007199254740992",
                                 b"-9007199254740992"), ids=("bound", "above", "below"))
def test_i53_token_control(native_library, tmp_path, token):
    line = b'["E",' + token + b",1.0,0.0]\n"
    path = tmp_path / "i53.sink"
    payload = review.pack([("E", line)])
    if abs(int(token)) <= review.pf_sink.I53_MAX:
        review.host_fragments(native_library[2], path, payload)
    else:
        refusal = review.source_refusal(native_library[2], path, payload)
        assert refusal.message.endswith("integer exceeds the exact JSON integer bound")
