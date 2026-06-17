import json, types
import run_json  # docker/ is on sys.path in the engine test env


def test_apply_syminfo_calls_setters(tmp_path):
    calls = {}
    lib = types.SimpleNamespace(
        strategy_set_syminfo_mintick=lambda s, v: calls.setdefault("mintick", v),
        strategy_set_syminfo_pointvalue=lambda s, v: calls.setdefault("pointvalue", v),
        strategy_set_syminfo_timezone=lambda s, v: calls.setdefault("tz", v),
        strategy_set_syminfo_session=lambda s, v: calls.setdefault("session", v),
    )
    p = tmp_path / "syminfo.json"
    p.write_text(json.dumps({"syminfo": {"mintick": 0.5, "pointvalue": 2.0,
                                          "timezone": "UTC", "session": "24x7"}}))
    run_json.apply_syminfo(lib, object(), p)
    assert calls == {"mintick": 0.5, "pointvalue": 2.0, "tz": b"UTC", "session": b"24x7"}
