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

import base64
import importlib.util
import json
import sys
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
            mode = get_input_string("Mode", "fast");
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
        import hashlib
        check(f"{label}: digest matches canonical sha256",
              fp["digest"] == "sha256:" + hashlib.sha256(canonical.encode()).hexdigest())
        fp2 = m.build_fingerprint(prov)
        check(f"{label}: deterministic token", fp["token"] == fp2["token"])

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
