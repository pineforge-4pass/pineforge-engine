#pragma once

#include <pineforge/run_failure_codes.hpp>

#include <cstdint>

namespace pineforge::detail::selected_capacity {

enum class Field {
    SourceRows,
    PrimaryRows,
    ScriptBars,
    AuxiliaryRows,
    Bytes,
};

enum class Role {
    Source,
    Primary,
    Auxiliary,
};

struct Bound {
    bool present = false;
    std::uint64_t value = 0;
};

struct Limits {
    Bound source_rows;
    Bound primary_rows;
    Bound script_bars;
    Bound auxiliary_rows;
    Bound bytes;
};

struct State {
    std::uint64_t source_rows = 0;
    std::uint64_t primary_rows = 0;
    std::uint64_t script_bars = 0;
    std::uint64_t auxiliary_rows = 0;
    std::uint64_t bytes = 0;
    bool has_last_primary_bucket = false;
    std::int64_t last_primary_bucket = 0;
};

struct RecordEvent {
    Role role = Role::Source;
    std::int64_t timestamp_ms = 0;
    bool has_script_bucket = false;
    std::int64_t script_bucket_key = 0;
};

void reserve(std::uint64_t& used, const Bound& bound, std::uint64_t requested,
             RunFailureCode code, const char* english);

void observe_source_rows(State&, const Limits&, const RecordEvent&);
void observe_primary_rows(State&, const Limits&, const RecordEvent&);
void observe_script_bars(State&, const Limits&, const RecordEvent&);
void observe_auxiliary_rows(State&, const Limits&, const RecordEvent&);
void observe_bytes(State&, const Limits&, std::uint64_t actual_csv_bytes);

}  // namespace pineforge::detail::selected_capacity
