#!/usr/bin/env python3
"""fingerprint_self_test.py — pins the backtest-fingerprint logic.

The fingerprint helpers are duplicated (byte-identical) in
docker/run_json.py and scripts/run_strategy.py because scripts/ is
.dockerignore'd, so a shared runtime module cannot be COPY'd into the
image. This test loads BOTH copies, runs one fixture generated.cpp
through each, and asserts (a) the parser/provenance/fingerprint behave
as designed and (b) the two copies produce identical output — so they
cannot silently drift.

Exit 0 iff every check passes.
"""
from __future__ import annotations

import ast
import base64
import hashlib
import importlib.util
import json
import struct
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RUN_JSON = REPO / "docker" / "run_json.py"
RUN_STRATEGY = REPO / "scripts" / "run_strategy.py"

# Fixture: a ctor that sets a SUBSET of strategy() fields (process_orders_on_close
# and close_entries_rule are intentionally absent — they default in the engine
# base class), a set_strategy_override with a decoy `initial_capital_ =` line
# (must NOT be parsed), and a mix of input types incl. a duplicate title.
FIXTURE_CPP = '''
struct GeneratedStrategy : Strategy {
    explicit GeneratedStrategy() : _ta_ema_1(5), _ta_ema_2(13) {
        initial_capital_ = 50000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 3.0;
        pyramiding_ = 2;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.04;
        slippage_ = 1;
    }
    void set_strategy_override(const std::string& key, const std::string& value) {
        if (key == "initial_capital") { initial_capital_ = std::stod(value); return; }
    }
    void on_bar(const Bar& bar) override {
        if (!_inputs_initialized_) {
            i_fast = get_input_int("Fast EMA", 5);
            i_slow = get_input_int("Slow EMA", 13);
            thr = get_input_double("ADX trend threshold", 25);
            mode = get_input_string("Mode", std::string("fast"));
            src = get_input_source("Source", close);
            _inputs_initialized_ = true;
        }
        i_fast = get_input_int("Fast EMA", 5);
    }
};
'''


def _load(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main() -> int:
    passed = failed = 0

    def check(name: str, cond: bool) -> None:
        nonlocal passed, failed
        if cond:
            passed += 1
            print(f"  OK   {name}")
        else:
            failed += 1
            print(f"  FAIL {name}")

    rj = _load(RUN_JSON, "pf_run_json")
    rs = _load(RUN_STRATEGY, "pf_run_strategy")

    for label, m in (("run_json", rj), ("run_strategy", rs)):
        # --- strategy() defaults from the ctor only -----------------------
        strat = m.parse_strategy_params(FIXTURE_CPP)
        check(f"{label}: initial_capital parsed", strat.get("initial_capital") == 50000.0)
        check(f"{label}: enum default_qty_type mapped",
              strat.get("default_qty_type") == "percent_of_equity")
        check(f"{label}: commission_value parsed", strat.get("commission_value") == 0.04)
        check(f"{label}: pyramiding parsed", strat.get("pyramiding") == 2)
        check(f"{label}: slippage parsed", strat.get("slippage") == 1)
        # decoy line in set_strategy_override must NOT pollute the parse
        check(f"{label}: override-fn line not parsed (still 50000.0)",
              strat.get("initial_capital") == 50000.0)
        # ctor does NOT set these two; parse_strategy_params must omit them
        check(f"{label}: ctor omits process_orders_on_close",
              "process_orders_on_close" not in strat)

        # --- effective strategy: seed fills the base-class-defaulted fields
        eff_no_ovr = m.effective_strategy(FIXTURE_CPP, {})
        check(f"{label}: seed fills process_orders_on_close=False (no override)",
              eff_no_ovr.get("process_orders_on_close") is False)
        check(f"{label}: seed fills close_entries_rule=FIFO (no override)",
              eff_no_ovr.get("close_entries_rule") == "FIFO")
        check(f"{label}: effective keeps ctor initial_capital",
              eff_no_ovr.get("initial_capital") == 50000.0)

        eff_ovr = m.effective_strategy(FIXTURE_CPP,
                                       {"pyramiding": "5", "process_orders_on_close": "true"})
        check(f"{label}: override wins (pyramiding=5)", eff_ovr.get("pyramiding") == "5")
        check(f"{label}: override wins (process_orders_on_close=true)",
              eff_ovr.get("process_orders_on_close") == "true")

        # --- inputs ------------------------------------------------------
        inp = m.parse_inputs(FIXTURE_CPP)
        check(f"{label}: 5 distinct inputs (dedup Fast EMA)", len(inp) == 5)
        check(f"{label}: int default typed", inp["Fast EMA"] == {"type": "int", "default": 5})
        check(f"{label}: double default typed",
              inp["ADX trend threshold"]["default"] == 25 and inp["ADX trend threshold"]["type"] == "double")
        check(f"{label}: string default unquoted", inp["Mode"]["default"] == "fast")
        check(f"{label}: source default kept", inp["Source"]["default"] == "close")

        eff_in = m.effective_inputs(FIXTURE_CPP, {"Fast EMA": "8"})
        check(f"{label}: input override -> value", eff_in["Fast EMA"]["value"] == "8")
        check(f"{label}: non-overridden input -> default value", eff_in["Slow EMA"]["value"] == 13)
        check(f"{label}: effective_inputs keeps default+type",
              eff_in["Fast EMA"] == {"type": "int", "default": 5, "value": "8"})

        # --- fingerprint round-trip + determinism ------------------------
        prov = {
            "engine": {"version_string": "0.10.2", "major": 0, "minor": 10, "patch": 2,
                       "commit_sha": "f3fc3a3"},
            "codegen": {"version": "0.6.4", "generated_cpp_sha256": "deadbeef",
                        "transpiled_from_pine": True},
            "strategy": eff_no_ovr,
            "inputs": eff_in,
            "applied": {"inputs": {"Fast EMA": "8"}, "overrides": {}},
            "runtime": {"input_tf": "", "bar_magnifier": False},
        }
        fp = m.build_fingerprint(prov)
        decoded = json.loads(base64.b64decode(fp["token"]))
        check(f"{label}: token decodes to provenance", decoded == prov)
        check(f"{label}: digest prefixed sha256:", fp["digest"].startswith("sha256:"))
        canonical = json.dumps(prov, sort_keys=True, separators=(",", ":"))
        check(f"{label}: digest matches canonical sha256",
              fp["digest"] == "sha256:" + hashlib.sha256(canonical.encode()).hexdigest())
        fp2 = m.build_fingerprint(prov)
        check(f"{label}: deterministic token", fp["token"] == fp2["token"])

    # --- the effective execution gate must participate in the fingerprint
    runtime_kwargs = {
        "input_tf": "15",
        "script_tf": "",
        "bar_magnifier": False,
        "magnifier_samples": 4,
        "magnifier_distribution": "ENDPOINTS",
        "chart_timezone": "UTC",
    }
    gate_ms = 1712345678901
    open_runtime = rs.build_runtime_provenance(runtime_kwargs, None)
    gated_runtime = rs.build_runtime_provenance(runtime_kwargs, gate_ms)
    same_gate_runtime = rs.build_runtime_provenance(runtime_kwargs, str(gate_ms))
    check("run_strategy: open execution records trade_start_ms=null",
          "trade_start_ms" in open_runtime and open_runtime["trade_start_ms"] is None)
    check("run_strategy: gated execution records trade_start_ms as int",
          gated_runtime["trade_start_ms"] == gate_ms
          and isinstance(gated_runtime["trade_start_ms"], int))
    check("run_strategy: equivalent gate values normalize identically",
          same_gate_runtime == gated_runtime)
    check("run_strategy: runtime helper is pure",
          "trade_start_ms" not in runtime_kwargs)

    open_fp = rs.build_fingerprint({"runtime": open_runtime})
    gated_fp = rs.build_fingerprint({"runtime": gated_runtime})
    same_gate_fp = rs.build_fingerprint({"runtime": same_gate_runtime})
    check("run_strategy: gated/open digests differ",
          gated_fp["digest"] != open_fp["digest"])
    check("run_strategy: same effective gate has deterministic digest",
          gated_fp["digest"] == same_gate_fp["digest"])

    # Timestamped FX provenance is pinned to the exact effective C-ABI values,
    # not only to a provider-file hash, point count, or endpoints.  Changing an
    # interior rate must therefore change both the value digest and the final
    # run fingerprint even when every summary field is otherwise identical.
    fx_timestamps = [1_000, 2_000, 3_000]
    fx_kwargs_a = dict(runtime_kwargs,
                       account_currency_fx_series=(fx_timestamps,
                                                    [1.0, 1.001, 1.002]),
                       account_currency_fx_source_sha256="a" * 64)
    fx_kwargs_b = dict(runtime_kwargs,
                       account_currency_fx_series=(fx_timestamps,
                                                    [1.0, 1.0015, 1.002]),
                       account_currency_fx_source_sha256="a" * 64)
    fx_runtime_a = rs.build_runtime_provenance(fx_kwargs_a, gate_ms)
    fx_runtime_b = rs.build_runtime_provenance(fx_kwargs_b, gate_ms)
    fx_meta_a = fx_runtime_a["account_currency_fx_series"]
    expected_fx_hasher = hashlib.sha256()
    expected_fx_hasher.update(
        b"pineforge:account-currency-fx:i64-f64-le:v1\0")
    fx_record = struct.Struct("<qd")
    for point in zip(fx_timestamps, [1.0, 1.001, 1.002]):
        expected_fx_hasher.update(fx_record.pack(*point))
    check("run_strategy: FX provenance names canonical packed-value contract",
          fx_meta_a["canonicalization"]
          == "pf-account-currency-fx-i64-f64-le-v1")
    check("run_strategy: FX effective-value digest is independently reproducible",
          fx_meta_a["effective_values_sha256"]
          == expected_fx_hasher.hexdigest())
    check("run_strategy: FX raw provider-file hash remains provenance",
          fx_meta_a["source_file_sha256"] == "a" * 64)
    check("run_strategy: FX series records its scalar pre-first fallback",
          fx_meta_a["fallback_account_per_quote"] == 1.0
          and fx_runtime_a["account_currency_fx_scalar"] == 1.0)
    check("run_strategy: changed interior FX rate changes effective digest",
          fx_meta_a["effective_values_sha256"]
          != fx_runtime_b["account_currency_fx_series"]["effective_values_sha256"])
    check("run_strategy: changed interior FX rate changes run fingerprint",
          rs.build_fingerprint({"runtime": fx_runtime_a})["digest"]
          != rs.build_fingerprint({"runtime": fx_runtime_b})["digest"])

    fx_kwargs_fallback = dict(
        fx_kwargs_a, syminfo_metadata={"account_currency_fx": 1.25})
    fx_runtime_fallback = rs.build_runtime_provenance(
        fx_kwargs_fallback, gate_ms)
    check("run_strategy: changed pre-first FX fallback changes provenance",
          fx_runtime_fallback["account_currency_fx_scalar"] == 1.25
          and fx_runtime_fallback["account_currency_fx_series"]
                  ["fallback_account_per_quote"] == 1.25
          and rs.build_fingerprint({"runtime": fx_runtime_fallback})["digest"]
              != rs.build_fingerprint({"runtime": fx_runtime_a})["digest"])

    # Provider adapter contract: a UTC daily close becomes effective at the
    # next day's 00:00Z boundary, key order is normalized, and duplicate dates
    # fail closed before json.loads can silently overwrite either value.
    with tempfile.TemporaryDirectory() as td:
        td_path = Path(td)
        valid_fx = td_path / "fx.json"
        valid_fx.write_text(
            '{"2026-01-02": 1.002, "2026-01-01": 1.001}',
            encoding="utf-8")
        (loaded_timestamps, loaded_rates), loaded_source_sha = \
            rs._load_account_currency_fx_daily_closes(valid_fx)
        check("run_strategy: daily FX closes sort and shift to D+1 00:00Z",
              loaded_timestamps == [1767312000000, 1767398400000]
              and loaded_rates == [1.001, 1.002])
        check("run_strategy: daily FX loader records raw file SHA-256",
              loaded_source_sha
              == hashlib.sha256(valid_fx.read_bytes()).hexdigest())

        duplicate_fx = td_path / "fx-duplicate.json"
        duplicate_fx.write_text(
            '{"2026-01-01": 1.0, "2026-01-01": 1.1}',
            encoding="utf-8")
        duplicate_rejected = False
        try:
            rs._load_account_currency_fx_daily_closes(duplicate_fx)
        except ValueError:
            duplicate_rejected = True
        check("run_strategy: duplicate daily FX date fails closed",
              duplicate_rejected)

    source_tree = ast.parse(RUN_STRATEGY.read_text(encoding="utf-8"))
    main_fn = next((node for node in source_tree.body
                    if isinstance(node, ast.FunctionDef) and node.name == "main"), None)
    main_uses_helper = main_fn is not None and any(
        isinstance(node, ast.Call)
        and isinstance(node.func, ast.Name)
        and node.func.id == "build_runtime_provenance"
        for node in ast.walk(main_fn)
    )
    check("run_strategy: main fingerprint path uses runtime helper",
          main_uses_helper)

    # --- canonical source-feed identity ---------------------------------
    # Seven test classes pin the feed contract independently of any strategy
    # binary. Rows use the exact BarC field order: O,H,L,C,V,timestamp.
    rows = [
        (100.0, 101.0, 99.0, 100.5, 10.0, 1_000),
        (100.5, 103.0, 100.0, 102.0, 20.0, 2_000),
        (102.0, 104.0, 101.0, 103.5, 30.0, 3_000),
        (103.5, 105.0, 102.5, 104.0, 40.0, 4_000),
    ]

    def expected_feed_sha(feed_rows) -> str:
        h = hashlib.sha256()
        h.update(b"pineforge:ohlcv:barc-le:v1\0")
        record = struct.Struct("<5dq")
        for row in feed_rows:
            h.update(record.pack(*row))
        return h.hexdigest()

    def loader_fails(loader, path: Path) -> bool:
        try:
            loader(path)
        except (OSError, KeyError, ValueError, IndexError):
            return True
        return False

    def provenance_rejects(module, digest) -> bool:
        try:
            module.build_provenance({}, None, False, {}, {}, {},
                                    source_feed_sha256=digest)
        except ValueError:
            return True
        return False

    with tempfile.TemporaryDirectory(prefix="pf-fingerprint-feed-") as td:
        tmp = Path(td)
        feed_a = tmp / "a.csv"
        feed_b = tmp / "b.csv"
        feed_changed = tmp / "changed.csv"
        feed_reordered = tmp / "reordered.csv"
        malformed = tmp / "malformed.csv"

        # A: canonical bytes are domain-prefixed <5dq> records.
        feed_a.write_text(
            "timestamp,open,high,low,close,volume\n" + "".join(
                f"{ts},{o:g},{h:g},{lo:g},{c:g},{v:g}\n"
                for o, h, lo, c, v, ts in rows),
            encoding="utf-8")
        expected = expected_feed_sha(rows)
        rs_bars, rs_n, rs_sha = rs._load_bars(feed_a)
        check("feed A: local hash pins domain-prefixed <5dq> contract",
              rs_n == len(rows) and rs_sha == expected
              and len(rs_bars) == len(rows))

        # B: Docker and local loaders must agree on one source tape.
        rj_bars, rj_n, rj_sha = rj.load_bars(feed_a)
        check("feed B: local/docker loaders agree",
              rj_n == rs_n and len(rj_bars) == len(rows)
              and rj_sha == rs_sha)

        # C: path, header order, newline style and numeric spelling are not
        # semantic; the parsed values stay identical.
        feed_b.write_bytes((
            "open,high,low,close,volume,timestamp\r\n" + "".join(
                f"{o:.1f},{h:.1f},{lo:.1f},{c:.1f},{v:.1f},{ts}\r\n"
                for o, h, lo, c, v, ts in rows)
        ).encode("utf-8"))
        _, _, rs_sha_b = rs._load_bars(feed_b)
        _, _, rj_sha_b = rj.load_bars(feed_b)
        check("feed C: equivalent parsed values ignore path/text formatting",
              rs_sha_b == expected and rj_sha_b == expected)

        # D: content and original row order are both identity-bearing, even
        # when count plus first/last timestamps are unchanged.
        changed_rows = list(rows)
        changed_rows[1] = (*changed_rows[1][:3], 102.25,
                           *changed_rows[1][4:])
        feed_changed.write_text(
            "timestamp,open,high,low,close,volume\n" + "".join(
                f"{ts},{o},{h},{lo},{c},{v}\n"
                for o, h, lo, c, v, ts in changed_rows),
            encoding="utf-8")
        reordered_rows = [rows[0], rows[2], rows[1], rows[3]]
        feed_reordered.write_text(
            "timestamp,open,high,low,close,volume\n" + "".join(
                f"{ts},{o},{h},{lo},{c},{v}\n"
                for o, h, lo, c, v, ts in reordered_rows),
            encoding="utf-8")
        _, _, changed_sha = rs._load_bars(feed_changed)
        _, _, reordered_sha = rs._load_bars(feed_reordered)
        check("feed D: middle value and row-order changes alter identity",
              changed_sha != expected and reordered_sha != expected
              and changed_sha != reordered_sha)

        # E: source identity is computed before local start/end slicing.
        _, filtered_n, filtered_sha = rs._load_bars(
            feed_a, ohlcv_start_ms=3_000, ohlcv_end_ms=4_000)
        check("feed E: validation slicing preserves source identity",
              filtered_n == 2 and filtered_sha == expected)

        # F: feed identity is mandatory provenance and changes the digest.
        prov_rs = rs.build_provenance(
            {}, None, False, {}, {}, {}, source_feed_sha256=expected)
        prov_rj = rj.build_provenance(
            {}, None, False, {}, {}, {}, source_feed_sha256=expected)
        changed_prov = rs.build_provenance(
            {}, None, False, {}, {}, {}, source_feed_sha256=changed_sha)
        check("feed F: provenance records feed and digest discriminates tapes",
              prov_rs == prov_rj
              and prov_rs["feed"] == {
                  "canonicalization": "pf-ohlcv-barc-le-v1",
                  "source_values_sha256": expected,
              }
              and rs.build_fingerprint(prov_rs)["digest"]
              != rs.build_fingerprint(changed_prov)["digest"])
        check("feed F: null/invalid identities fail closed",
              provenance_rejects(rs, None)
              and provenance_rejects(rj, None)
              and provenance_rejects(rs, expected.upper()))

        # G: absent/malformed sources fail in both loaders before provenance.
        malformed.write_text(
            "timestamp,open,high,low,close\n1000,1,2,0,1.5\n",
            encoding="utf-8")
        missing = tmp / "missing.csv"
        check("feed G: missing/malformed feeds produce no identity",
              loader_fails(rs._load_bars, missing)
              and loader_fails(rj.load_bars, missing)
              and loader_fails(rs._load_bars, malformed)
              and loader_fails(rj.load_bars, malformed))

    # Pin production call-site plumbing so a future refactor cannot compute a
    # digest and then silently omit it from one harness's provenance.
    docker_tree = ast.parse(RUN_JSON.read_text(encoding="utf-8"))
    callsite_has_feed = lambda tree: any(
        isinstance(node, ast.Call)
        and isinstance(node.func, ast.Name)
        and node.func.id == "build_provenance"
        and any(kw.arg == "source_feed_sha256" for kw in node.keywords)
        for node in ast.walk(tree))
    check("feed call sites: local and docker require source identity",
          callsite_has_feed(source_tree) and callsite_has_feed(docker_tree))

    # --- the two copies must agree byte-for-byte on the fixture ----------
    check("copies agree: parse_strategy_params",
          rj.parse_strategy_params(FIXTURE_CPP) == rs.parse_strategy_params(FIXTURE_CPP))
    check("copies agree: parse_inputs",
          rj.parse_inputs(FIXTURE_CPP) == rs.parse_inputs(FIXTURE_CPP))
    check("copies agree: effective_strategy",
          rj.effective_strategy(FIXTURE_CPP, {"pyramiding": "5"})
          == rs.effective_strategy(FIXTURE_CPP, {"pyramiding": "5"}))
    check("copies agree: effective_inputs",
          rj.effective_inputs(FIXTURE_CPP, {"Fast EMA": "8"})
          == rs.effective_inputs(FIXTURE_CPP, {"Fast EMA": "8"}))

    print(f"\n{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
