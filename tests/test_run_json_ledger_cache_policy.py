"""Real C descriptor/consumer projection and exact ctypes layout; author UNRUN."""
import ctypes
import importlib.util
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "docker"))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    # Dataclasses used by both wrappers resolve their defining module here.
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


@pytest.fixture(params=["docker/run_json.py", "scripts/run_strategy.py"])
def wrapper(request):
    return load("ledger_policy_" + Path(request.param).stem, ROOT / request.param)


def test_exact_existing_c_layout(wrapper):
    # A baseline behavioral failure (24 != 32 on LP64), not a missing new API.
    assert ctypes.sizeof(wrapper.CachePolicyEntryC) == 16
    assert ctypes.sizeof(wrapper.CachePolicyC) == 32
    assert wrapper.CachePolicyEntryC.kind.offset == 0
    assert wrapper.CachePolicyEntryC.reserved.offset == 4
    assert wrapper.CachePolicyEntryC.capacity_bytes.offset == 8
    assert wrapper.CachePolicyC.entries.offset == 16
    assert wrapper.CachePolicyC.reserved.offset == 24


class MissingSetterAPINegativeFixture:
    """API-negative object only; not a runtime or memory measurement."""

    def __init__(self):
        self.lookups = []

    def __getattr__(self, name):
        self.lookups.append(name)
        if name == "strategy_set_cache_policy_v1":
            raise AttributeError(name)
        raise AssertionError(f"unexpected runtime API access: {name}")


def reject_policy_work(*args, **kwargs):
    raise AssertionError("unexpected policy binding or construction")


def test_missing_setter_api_negative_controls_detect_access():
    lib = MissingSetterAPINegativeFixture()
    with pytest.raises(AssertionError, match="unexpected runtime API access"):
        getattr(lib, "strategy_get_last_error_code")
    assert lib.lookups == ["strategy_get_last_error_code"]
    with pytest.raises(AssertionError, match="unexpected policy binding or construction"):
        reject_policy_work()


@pytest.mark.parametrize("capacity", [None, 4096, 0], ids=["omitted-auto", "finite", "zero"])
def test_wrapper_requires_setter_for_every_policy_request(wrapper, monkeypatch, capacity):
    # Reject every explicit replacement request before policy/native work.
    # These sentinels detect accesses; they supply no policy or runtime result.
    lib = MissingSetterAPINegativeFixture()
    monkeypatch.setattr(wrapper, "CachePolicyEntryC", reject_policy_work)
    monkeypatch.setattr(wrapper, "CachePolicyC", reject_policy_work)
    if hasattr(wrapper, "_bind_cache_policy"):
        monkeypatch.setattr(wrapper, "_bind_cache_policy", reject_policy_work)
    error_type = getattr(wrapper, "StrategyLibraryError", RuntimeError)
    with pytest.raises(
        error_type, match="ledger cache policy requires strategy_set_cache_policy_v1"
    ) as refused:
        wrapper.apply_ledger_cache_policy(lib, object(), capacity)
    assert type(refused.value) is error_type
    if hasattr(wrapper, "StrategyLibraryError"):
        assert refused.value.code == "strategy_library_incompatible"
        assert refused.value.code_args == {"reason": "setter_missing"}
    assert lib.lookups == ["strategy_set_cache_policy_v1"]


@pytest.fixture
def runtime():
    path = os.environ.get("PF_LEDGER_CACHE_POLICY_FIXTURE")
    if not path:
        pytest.fail("the real ledger fixture is required; no mocked green control")
    lib = ctypes.CDLL(path)
    lib.ledger_fixture_create.restype = ctypes.c_void_p
    lib.ledger_fixture_free.argtypes = [ctypes.c_void_p]
    lib.ledger_fixture_policy.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.ledger_fixture_policy.restype = ctypes.c_int
    lib.ledger_fixture_run.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
    lib.ledger_fixture_run.restype = ctypes.c_int
    lib.ledger_fixture_initial_rows.restype = ctypes.c_uint64
    lib.ledger_fixture_capacity.argtypes = [ctypes.c_uint64]
    lib.ledger_fixture_capacity.restype = ctypes.c_uint64
    lib.ledger_fixture_rows.argtypes = [ctypes.c_void_p]
    lib.ledger_fixture_rows.restype = ctypes.c_uint64
    lib.ledger_fixture_read.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
    lib.ledger_fixture_read.restype = ctypes.c_uint64
    lib.strategy_get_last_error_code.argtypes = [ctypes.c_void_p]
    lib.strategy_get_last_error_code.restype = ctypes.c_char_p
    lib.strategy_get_last_error_args.argtypes = [ctypes.c_void_p]
    lib.strategy_get_last_error_args.restype = ctypes.c_char_p
    return lib


def test_real_descriptor_presence_and_repeated_run_setup(wrapper, runtime):
    state = runtime.ledger_fixture_create()
    initial = runtime.ledger_fixture_initial_rows()
    finite = runtime.ledger_fixture_capacity(initial)
    try:
        for capacity in (None, finite, 0, None, finite):
            # Use the actual wrapper types, including both reserved fields.
            entries = ((wrapper.CachePolicyEntryC * 1)(wrapper.CachePolicyEntryC(
                kind=1, reserved=0, capacity_bytes=capacity)) if capacity is not None else None)
            policy = wrapper.CachePolicyC(
                struct_size=ctypes.sizeof(wrapper.CachePolicyC), version=1,
                entry_count=0 if entries is None else 1, entries=entries)
            assert runtime.ledger_fixture_policy(state, ctypes.byref(policy)) == 0
            assert runtime.ledger_fixture_run(state, initial + 3) == 0
            assert runtime.ledger_fixture_rows(state) == (
                2 * initial if capacity is None else 0 if capacity == 0 else initial)
            reads = runtime.ledger_fixture_read(state, 0)
            if capacity is None:
                assert reads == 0
            else:
                assert 0 < reads < (1 << 64) - 1
    finally:
        runtime.ledger_fixture_free(state)


def test_wrapper_helper_projects_intent_without_default_literal(wrapper, runtime):
    state = runtime.ledger_fixture_create()
    try:
        # Both wrapper helpers call the genuine runtime setter. Omission
        # selects AUTO, never a guessed copy of today's default byte value.
        initial = runtime.ledger_fixture_initial_rows()
        for capacity in (None, runtime.ledger_fixture_capacity(initial), 0):
            wrapper.apply_ledger_cache_policy(runtime, state, capacity)
            assert runtime.ledger_fixture_run(state, initial + 3) == 0
            assert runtime.ledger_fixture_rows(state) == (
                2 * initial if capacity is None else initial if capacity else 0)
        with pytest.raises(Exception) as refused:
            wrapper.apply_ledger_cache_policy(runtime, state, 1)
        assert "value_below_minimum" in str(refused.value)
        assert runtime.strategy_get_last_error_code(state) == b"setting_rejected"
        assert b"value_below_minimum" in runtime.strategy_get_last_error_args(state)
        # Rejection is atomic: the preceding explicit zero remains zero.
        assert runtime.ledger_fixture_run(state, initial + 3) == 0
        assert runtime.ledger_fixture_rows(state) == 0
    finally:
        runtime.ledger_fixture_free(state)


@pytest.mark.parametrize("value", [-1, 1 << 64, True, 1.5, "auto"])
def test_wrapper_does_not_wrap_invalid_bytes(wrapper, runtime, value):
    state = runtime.ledger_fixture_create()
    try:
        with pytest.raises((TypeError, ValueError)):
            wrapper.apply_ledger_cache_policy(runtime, state, value)
    finally:
        runtime.ledger_fixture_free(state)
