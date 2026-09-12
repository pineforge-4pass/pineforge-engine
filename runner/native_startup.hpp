// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "json.hpp"

#include <pineforge/native_run_spec.hpp>
#include <pineforge/pineforge.h>

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pineforge::live {

struct NativeConfigValues {
    std::string session_key, input_tf, script_tf, ticker, tickerid, type, currency, basecurrency,
        description, volumetype, timezone, session, chart_timezone;
    std::uint64_t run_number = 1, max_open_lots = 0;
    double initial_capital = 0, point_value = 0, account_fx = 0, price_tick = 0, fee_value = 0,
        quantity_grid = 0, max_abs_units = 0, initial_margin_fraction = 0;
    std::uint32_t slippage_ticks = 0, fee_kind = 0, close_execution = 0, allowed_open_directions = 3,
        optional_mask = 0;
    bool present = false;
};

struct NativeClockBindings {
    std::string input_tf, script_tf, timezone, session, chart_timezone, symbol;
    std::set<std::string> explicit_flags;
};

struct LegacyIdentityFields {
    std::string mode, input_tf, script_tf, session, timezone, chart_timezone, symbol, name, webhook;
    std::vector<std::pair<std::string, std::string>> inputs, overrides, syminfo;
};

bool semantic_utf8(std::string_view bytes);
Json json_real(double value);
Json json_u64(std::uint64_t value);

NativeConfigValues parse_native_config(const std::string& text);
void apply_native_config(NativeClockBindings& clock, const NativeConfigValues& native);
void validate_native_config(NativeConfigValues& native);
pineforge::NativeRunSpec native_run_spec(const NativeConfigValues& native);

std::vector<pf_bar_t> history(const std::string& csv, bool native = false);
void require_native_warmup(const NativeConfigValues& spec, const std::vector<pf_bar_t>& bars);

Json timezone_rule_identity(std::string_view timezone, bool required);

std::string identity_document(const LegacyIdentityFields& fields, const std::string& warmup,
                              const std::string& library, const std::string& parser_bytes,
                              const std::string& parser_config);
std::string identity(const LegacyIdentityFields& fields, const std::string& warmup,
                     const std::string& library, const std::string& parser_bytes,
                     const std::string& parser_config);

std::string native_identity_document(const NativeConfigValues& native, const std::string& mode,
                                     const std::string& name, const std::string& webhook,
                                     const std::string& warmup, const std::string& library,
                                     const std::string& parser_bytes,
                                     const std::string& parser_config);
std::string native_identity(const NativeConfigValues& native, const std::string& mode,
                            const std::string& name, const std::string& webhook,
                            const std::string& warmup, const std::string& library,
                            const std::string& parser_bytes, const std::string& parser_config);

}  // namespace pineforge::live
