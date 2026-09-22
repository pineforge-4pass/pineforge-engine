// test_native_c_api_frozen_header.cpp — the frozen-header pairing for the
// L13 C API.
//
// The include below is the whole point: tests/fixtures/native_c_api/v1 holds
// a byte-for-byte copy of <pineforge/native_c_api.h> at PF_NATIVE_API_VERSION
// 1. It defines the header guard before pulling in <pineforge/pineforge.h>,
// so the live header is skipped and every declaration, POD layout and size
// constant in this translation unit is the FROZEN one. It is then linked
// against the current runtime.
//
// The evidence is behavioural, not a size literal: a frozen caller sends the
// v1 `struct_size` it was compiled with, and the current runtime must still
// accept it. The live header has appended deliberately additive tails since
// (the request, the spec extension, the callback table and the working-row
// readout), each publishing the earlier length as a `*_SIZE` constant the
// runtime keeps accepting; these rows are that promise, executed. A change
// that is not such a tail — or a runtime that stops accepting a published
// length — answers PF_NATIVE_E_STRUCT and fails them: the refusal the header
// documents.
//
// Everything here is written to the C subset the frozen header declares; the
// file is C++ only so ctest can link one executable without a second C target.

#include "fixtures/native_c_api/v1/native_c_api.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

// The size prefix is the versioning mechanism, so its position is part of the
// contract: every versioned POD starts with struct_size then version.
static_assert(offsetof(pf_native_request_v1, struct_size) == 0, "request size prefix moved");
static_assert(offsetof(pf_native_request_v1, version) == 4, "request version moved");
static_assert(offsetof(pf_native_callbacks_v1, struct_size) == 0, "table size prefix moved");
static_assert(offsetof(pf_native_callbacks_v1, version) == 4, "table version moved");
static_assert(offsetof(pf_native_state_v1, struct_size) == 0, "state size prefix moved");
static_assert(offsetof(pf_native_working_v1, struct_size) == 0, "working size prefix moved");
static_assert(offsetof(pf_native_event_v1, struct_size) == 0, "event size prefix moved");
static_assert(offsetof(pf_native_decision_v1, struct_size) == 0, "decision size prefix moved");
static_assert(offsetof(pf_native_applied_v1, struct_size) == 0, "applied size prefix moved");
static_assert(offsetof(pf_native_run_spec_ext_v1, struct_size) == 0, "ext size prefix moved");
static_assert(PF_NATIVE_API_VERSION == 1, "the frozen fixture is v1");

struct FrozenState {
    pf_strategy_t host = nullptr;
    int calculations = 0;
    int refused_struct = 0;
    int submitted = 0;
    uint64_t entry = 0;
    // The resting request the frozen reader reads back.
    uint64_t resting = 0;
    int working_rows = -1;
    int working_found = 0;
    int working_refused = 0;
    int working_overrun = 0;
    int working_fields = 0;
    int rest_cancelled = 0;
};

// A frozen working row with a guard band behind it. pf_native_working_v1 grew
// an additive tail (trail_has_arm_price, reserved1) after this header froze, so
// the frozen sizeof is the runtime's PF_NATIVE_WORKING_V1_BASE_SIZE: the
// runtime must serve it and write no byte past it.
struct GuardedWorkingRow {
    pf_native_working_v1 row;
    unsigned char guard[32];
};
static_assert(offsetof(GuardedWorkingRow, guard) == sizeof(pf_native_working_v1),
              "the guard band must start where the frozen row ends");
constexpr unsigned char kGuardByte = 0xA5;

void read_frozen_working_rows(FrozenState* state) {
    state->working_rows = strategy_native_working_len_v1(state->host);
    for (int index = 0; index < state->working_rows; ++index) {
        GuardedWorkingRow guarded;
        std::memset(&guarded, 0, sizeof(guarded.row));
        std::memset(guarded.guard, kGuardByte, sizeof(guarded.guard));
        guarded.row.struct_size = static_cast<uint32_t>(sizeof(pf_native_working_v1));
        const int rc = strategy_native_working_get_v1(state->host, index, &guarded.row);
        if (rc == PF_NATIVE_E_STRUCT) state->working_refused = 1;
        for (unsigned char byte : guarded.guard) {
            if (byte != kGuardByte) state->working_overrun = 1;
        }
        if (rc != PF_NATIVE_OK || guarded.row.incarnation != state->resting) continue;
        state->working_found = 1;
        state->working_fields =
            guarded.row.struct_size == sizeof(pf_native_working_v1)
            && guarded.row.version == PF_NATIVE_API_VERSION
            && guarded.row.intent == PF_NATIVE_INTENT_TRANSACT
            && guarded.row.intent_value == 1.0
            && guarded.row.trigger == PF_NATIVE_TRIGGER_LIMIT
            && guarded.row.trigger_state == PF_NATIVE_TRIGGER_STATE_LIMIT_READY
            && guarded.row.p1 == 50.0
            && guarded.row.owner == PF_NATIVE_OWNER_INDEPENDENT
            && guarded.row.label != nullptr
            && std::strcmp(guarded.row.label, "frozen-rest") == 0;
    }
}

const pf_bar_t* frozen_bars(int* n) {
    static pf_bar_t bars[12];
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < 12; ++i) {
            const double open = 100.0 + static_cast<double>(i);
            bars[i].open = open;
            bars[i].high = open + 2.0;
            bars[i].low = open - 1.0;
            bars[i].close = open + 1.0;
            bars[i].volume = 5.0;
            bars[i].timestamp = static_cast<int64_t>(i) * 300000;
        }
        ready = true;
    }
    if (n) *n = 12;
    return bars;
}

int frozen_on_bar(void* user, const pf_bar_t*, const pf_native_decision_v1* at) {
    FrozenState* state = static_cast<FrozenState*>(user);
    if (at->struct_size != sizeof(pf_native_decision_v1)) {
        // The runtime filled a POD the frozen caller cannot read.
        state->refused_struct = 1;
        return 0;
    }
    ++state->calculations;
    if (state->calculations == 2) {
        pf_native_request_v1 request;
        std::memset(&request, 0, sizeof(request));
        request.struct_size = static_cast<uint32_t>(sizeof(request));
        request.version = PF_NATIVE_API_VERSION;
        request.intent = PF_NATIVE_INTENT_TRANSACT;
        request.intent_value = 1.0;
        request.label = "frozen-long";
        const int rc = strategy_native_submit_v1(state->host, &request, &state->entry, nullptr);
        if (rc == PF_NATIVE_E_STRUCT) state->refused_struct = 1;
        if (rc == PF_NATIVE_OK) state->submitted = 1;
    } else if (state->calculations == 3) {
        // A buy limit far below every bar (lows 99..110): it rests, so the
        // next calculation has a working row to read back.
        pf_native_request_v1 request;
        std::memset(&request, 0, sizeof(request));
        request.struct_size = static_cast<uint32_t>(sizeof(request));
        request.version = PF_NATIVE_API_VERSION;
        request.intent = PF_NATIVE_INTENT_TRANSACT;
        request.intent_value = 1.0;
        request.trigger = PF_NATIVE_TRIGGER_LIMIT;
        request.p1 = 50.0;
        request.label = "frozen-rest";
        const int rc = strategy_native_submit_v1(state->host, &request, &state->resting, nullptr);
        if (rc == PF_NATIVE_E_STRUCT) state->refused_struct = 1;
    } else if (state->calculations == 4) {
        read_frozen_working_rows(state);
    } else if (state->calculations == 5) {
        if (state->resting != 0
            && strategy_native_cancel_v1(state->host, state->resting) == PF_NATIVE_OK) {
            state->rest_cancelled = 1;
        }
    } else if (state->calculations == 6) {
        pf_native_request_v1 request;
        std::memset(&request, 0, sizeof(request));
        request.struct_size = static_cast<uint32_t>(sizeof(request));
        request.version = PF_NATIVE_API_VERSION;
        request.intent = PF_NATIVE_INTENT_FLATTEN;
        request.label = "frozen-flat";
        const int rc = strategy_native_submit_v1(state->host, &request, nullptr, nullptr);
        if (rc == PF_NATIVE_E_STRUCT) state->refused_struct = 1;
    }
    return 0;
}

pf_native_run_spec_v1 frozen_spec() {
    pf_native_run_spec_v1 spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.struct_size = static_cast<uint32_t>(sizeof(spec));
    spec.session_key = "native-c-api-frozen";
    spec.run_number = 1;
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.description = "";
    spec.volumetype = "";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.chart_timezone = "";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.allowed_open_directions = 3;
    return spec;
}

}  // namespace

int main() {
    check(strategy_native_api_version() == PF_NATIVE_API_VERSION,
          "the runtime speaks a different native C API version than the frozen header");

    FrozenState state;
    pf_native_callbacks_v1 table;
    std::memset(&table, 0, sizeof(table));
    table.struct_size = static_cast<uint32_t>(sizeof(table));
    table.version = PF_NATIVE_API_VERSION;
    table.user = &state;
    table.on_bar = &frozen_on_bar;

    state.host = strategy_native_host_create_v1(&table);
    check(state.host != nullptr, "the current runtime refused the frozen callback table");
    if (!state.host) {
        std::fprintf(stderr, "test_native_c_api_frozen_header: %d failure(s)\n", failures + 1);
        return 1;
    }

    pf_native_run_spec_v1 spec = frozen_spec();
    check(strategy_configure_native_v1(state.host, &spec) == 0, "frozen configure refused");

    int n = 0;
    const pf_bar_t* bars = frozen_bars(&n);
    pf_report_t report;
    std::memset(&report, 0, sizeof(report));
    check(strategy_native_run_v1(state.host, bars, n, &report) == PF_NATIVE_OK,
          "the frozen caller's run did not complete");
    check(state.refused_struct == 0,
          "the current runtime refused a frozen-sized struct (the v1 layout moved)");
    check(state.submitted == 1, "the frozen caller's request was not accepted");
    check(state.resting != 0, "the frozen caller's resting limit was not accepted");
    check(state.working_rows >= 1, "the frozen reader saw no working row");
    check(state.working_refused == 0,
          "the current runtime refused the frozen caller's working row "
          "(the readout's base length is no longer served)");
    check(state.working_overrun == 0,
          "the current runtime wrote past the frozen caller's working row");
    check(state.working_found == 1, "the frozen reader did not find its resting request");
    check(state.working_fields == 1,
          "the frozen reader's working row does not describe its resting limit");
    check(state.rest_cancelled == 1, "the frozen caller could not cancel its resting limit");
    check(report.total_trades == 1, "the frozen caller saw a different closed-trade count");
    strategy_native_report_free_v1(&report);

    // The read-back PODs are size-prefixed too: a frozen reader must still be
    // able to name its own layout.
    pf_native_state_v1 run_state;
    std::memset(&run_state, 0, sizeof(run_state));
    run_state.struct_size = static_cast<uint32_t>(sizeof(run_state));
    check(strategy_native_state_v1(state.host, &run_state) == PF_NATIVE_OK,
          "the current runtime refused the frozen state struct");
    check(run_state.lifecycle == PF_NATIVE_LIFECYCLE_COMPLETED,
          "the frozen reader saw a lifecycle other than Completed");

    // The frozen caller's own pf_native_run_spec_ext_v1 is the BASE layout:
    // the live header has appended L9's risk tail since. The runtime publishes
    // both lengths, so this struct_size must still be accepted — that is what
    // "additive" means here, and a second host proves it without disturbing
    // the completed run above.
    FrozenState ext_state;
    pf_native_callbacks_v1 ext_table;
    std::memset(&ext_table, 0, sizeof(ext_table));
    ext_table.struct_size = static_cast<uint32_t>(sizeof(ext_table));
    ext_table.version = PF_NATIVE_API_VERSION;
    ext_table.user = &ext_state;
    ext_state.host = strategy_native_host_create_v1(&ext_table);
    check(ext_state.host != nullptr, "the current runtime refused a second frozen table");
    if (ext_state.host) {
        pf_native_run_spec_v1 ext_spec = frozen_spec();
        pf_native_run_spec_ext_v1 ext;
        std::memset(&ext, 0, sizeof(ext));
        ext.struct_size = static_cast<uint32_t>(sizeof(ext));
        ext.version = PF_NATIVE_API_VERSION;
        ext.present_mask = PF_NATIVE_SPEC_EXT_REPORT;
        ext.report_policy = 1u;  // KernelRecorded
        check(strategy_configure_native_ext_v1(ext_state.host, &ext_spec, &ext) == PF_NATIVE_OK,
              "the current runtime refused the frozen spec extension (the tail is not additive)");
        check(strategy_native_run_v1(ext_state.host, bars, n, nullptr) == PF_NATIVE_OK,
              "the frozen extended run did not complete");
        strategy_native_host_free(ext_state.host);
    }

    pf_native_event_v1 events[16];
    std::memset(events, 0, sizeof(events));
    const int written = strategy_native_events_v1(state.host, 0, events, 16);
    check(written > 0, "the frozen reader polled no events");
    if (written > 0) {
        check(events[0].struct_size == sizeof(pf_native_event_v1),
              "the runtime filled an event POD the frozen reader cannot stride");
        check(events[0].version == PF_NATIVE_API_VERSION,
              "the runtime filled an event POD of another version");
        // Every tag this run produced is one the frozen header names. A tag
        // added after the freeze (PF_NATIVE_EVENT_RISK is the first) is only
        // ever produced by a spec this caller cannot write, and a reader that
        // met one would skip it by tag — which is why the addition did not
        // renumber PF_NATIVE_EVENT_DRIVER_POINT or _ACCOUNT.
        for (int i = 0; i < written; ++i) {
            check(events[i].kind >= PF_NATIVE_EVENT_ACCEPTED
                      && events[i].kind <= PF_NATIVE_EVENT_ACCOUNT,
                  "the frozen reader met a tag its header does not name");
        }
    }

    strategy_native_host_free(state.host);

    if (failures != 0) {
        std::fprintf(stderr, "test_native_c_api_frozen_header: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_native_c_api_frozen_header: ok\n");
    return 0;
}
