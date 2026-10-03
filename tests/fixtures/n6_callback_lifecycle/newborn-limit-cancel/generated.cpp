#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>
#if __has_include(<pineforge/checked_settings.hpp>)
#include <pineforge/checked_settings.hpp>
#endif
#include <pineforge/math.hpp>
#include <pineforge/series.hpp>
#include <pineforge/na.hpp>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <numeric>
#include <string>
#include <vector>
#include <charconv>
#include <system_error>
#include <stdexcept>
#include <tuple>
#include <optional>
#include <type_traits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_map>
#include <pineforge/color.hpp>
#include <pineforge/log.hpp>
#include <pineforge/str_utils.hpp>
#include <pineforge/session_time.hpp>
#ifndef PINEFORGE_HAS_NATIVE_LOWERING_V1
#error "generated code requires pineforge-engine native lowering v1 (PINEFORGE_HAS_NATIVE_LOWERING_V1)"
#endif

using namespace pineforge;


struct _PFTvFormatValue {
    enum class Kind { Number, Text } kind;
    double number = 0.0;
    std::string text;

    template <class T, std::enable_if_t<std::is_arithmetic_v<T> &&
                                        !std::is_same_v<T, bool>, int> = 0>
    _PFTvFormatValue(T value)
        : kind(Kind::Number),
          number(is_na(value) ? na<double>() : static_cast<double>(value)) {}
    _PFTvFormatValue(bool value)
        : kind(Kind::Text), text(value ? "true" : "false") {}
    _PFTvFormatValue(const std::string& value)
        : kind(Kind::Text), text(value) {}
    _PFTvFormatValue(const char* value)
        : kind(Kind::Text), text(value) {}
};

static std::string _pf_tv_trim(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t");
    if (first == std::string::npos) return "";
    const size_t last = value.find_last_not_of(" \t");
    return value.substr(first, last - first + 1);
}

static void _pf_tv_increment_digits(std::string& digits) {
    for (size_t i = digits.size(); i > 0; --i) {
        if (digits[i - 1] != '9') {
            ++digits[i - 1];
            return;
        }
        digits[i - 1] = '0';
    }
    digits.insert(digits.begin(), '1');
}

static std::string _pf_tv_decimal(double value, int min_fraction,
                                  int max_fraction, bool grouping,
                                  int decimal_shift = 0) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
    max_fraction = std::max(0, std::min(max_fraction, 15));
    min_fraction = std::max(0, std::min(min_fraction, max_fraction));

    // C++17's no-precision to_chars gives the shortest round-trip decimal
    // spelling of this binary64 value. All subsequent scaling and rounding
    // operate on its digits, never on a floating-point intermediate.
    char buffer[128];
    const auto converted = std::to_chars(buffer, buffer + sizeof buffer, value);
    if (converted.ec != std::errc{})
        throw std::runtime_error("shortest-decimal conversion failed");
    std::string spelling(buffer, converted.ptr);
    const bool negative = !spelling.empty() && spelling[0] == '-';
    if (negative) spelling.erase(0, 1);

    const size_t exponent_pos = spelling.find_first_of("eE");
    const std::string mantissa = spelling.substr(0, exponent_pos);
    int exponent = 0;
    if (exponent_pos != std::string::npos) {
        size_t i = exponent_pos + 1;
        bool exponent_negative = false;
        if (i < spelling.size() && (spelling[i] == '+' || spelling[i] == '-')) {
            exponent_negative = spelling[i] == '-';
            ++i;
        }
        for (; i < spelling.size(); ++i)
            exponent = exponent * 10 + (spelling[i] - '0');
        if (exponent_negative) exponent = -exponent;
    }

    std::string digits;
    int decimal_point = 0;
    bool after_point = false;
    for (char ch : mantissa) {
        if (ch == '.') {
            after_point = true;
        } else {
            digits += ch;
            if (!after_point) ++decimal_point;
        }
    }
    decimal_point += exponent + decimal_shift;
    const size_t leading_zeroes = digits.find_first_not_of('0');
    if (leading_zeroes == std::string::npos) {
        digits = "0";
        decimal_point = 1;
    } else {
        digits.erase(0, leading_zeroes);
        decimal_point -= static_cast<int>(leading_zeroes);
    }

    // The retained digits form an integer in units of 10^-max_fraction.
    // The first discarded decimal digit decides a half-up tie exactly.
    std::string units;
    if (digits == "0") {
        units = "0";
    } else {
        const int keep = decimal_point + max_fraction;
        if (keep < 0) {
            units = "0";
        } else if (keep == 0) {
            units = digits[0] >= '5' ? "1" : "0";
        } else if (keep >= static_cast<int>(digits.size())) {
            units = digits;
            units.append(static_cast<size_t>(keep) - digits.size(), '0');
        } else {
            units = digits.substr(0, static_cast<size_t>(keep));
            if (digits[static_cast<size_t>(keep)] >= '5')
                _pf_tv_increment_digits(units);
        }
    }
    const bool nonzero = units.find_first_not_of('0') != std::string::npos;
    if (!nonzero) units = "0";
    if (units.size() <= static_cast<size_t>(max_fraction))
        units.insert(0, static_cast<size_t>(max_fraction) + 1 - units.size(), '0');
    const size_t integer_size = units.size() - static_cast<size_t>(max_fraction);
    std::string integer = units.substr(0, integer_size);
    std::string fraction = units.substr(integer_size);
    while (fraction.size() > static_cast<size_t>(min_fraction)
           && fraction.back() == '0') fraction.pop_back();
    if (grouping) {
        std::string grouped;
        for (size_t i = 0; i < integer_size; ++i) {
            if (i && (integer_size - i) % 3 == 0) grouped += ',';
            grouped += integer[i];
        }
        integer = grouped;
    }
    return (negative && nonzero ? "-" : "") + integer
        + (fraction.empty() ? "" : "." + fraction);
}

static std::string _pf_tv_pattern(double value, const std::string& pattern) {
    if (!std::isfinite(value)) return _pf_tv_decimal(value, 0, 0, false);
    const std::string fmt = _pf_tv_trim(pattern);
    const size_t dot = fmt.find('.');
    const std::string integer = fmt.substr(0, dot);
    const std::string fraction = dot == std::string::npos ? "" : fmt.substr(dot + 1);
    const int max_fraction = static_cast<int>(std::count_if(
        fraction.begin(), fraction.end(), [](char c) { return c == '0' || c == '#'; }));
    const int min_fraction = static_cast<int>(std::count(fraction.begin(), fraction.end(), '0'));
    const bool grouping = integer.find(',') != std::string::npos;
    const bool percent = fmt.find('%') != std::string::npos;
    return _pf_tv_decimal(value, min_fraction, max_fraction, grouping,
                          percent ? 2 : 0)
        + (percent ? "%" : "");
}

static std::string pine_str_tostring_tv(double value,
                                        const std::string& format_mode = "",
                                        double mintick = 0.0) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
    if (format_mode == "mintick")
        return pine_str_tostring(value, format_mode, mintick);
    if (format_mode == "percent")
        return _pf_tv_pattern(value, "#.##") + "%";
    if (format_mode == "volume") {
        const double magnitude = std::fabs(value);
        int decimal_shift = 0;
        const char* unit = "";
        if (magnitude >= 1.0e12) { decimal_shift = -12; unit = "T"; }
        else if (magnitude >= 1.0e9) { decimal_shift = -9; unit = "B"; }
        else if (magnitude >= 1.0e6) { decimal_shift = -6; unit = "M"; }
        else if (magnitude >= 1.0e3) { decimal_shift = -3; unit = "K"; }
        return _pf_tv_decimal(value, 0, decimal_shift ? 2 : 0, false,
                              decimal_shift) + unit;
    }
    return _pf_tv_pattern(value, format_mode.empty() ? "#.##########" : format_mode);
}

template <class T, std::enable_if_t<std::is_integral_v<T> &&
                                    !std::is_same_v<T, bool>, int> = 0>
static std::string pine_str_tostring_tv(T value,
                                        const std::string& format_mode = "",
                                        double mintick = 0.0) {
    return pine_str_tostring_tv(
        is_na(value) ? na<double>() : static_cast<double>(value),
        format_mode, mintick);
}

static std::string _pf_tv_number_style(double value, const std::string& style) {
    if (!std::isfinite(value)) return _pf_tv_decimal(value, 0, 0, false);
    const std::string fmt = _pf_tv_trim(style);
    if (fmt.empty()) return _pf_tv_pattern(value, "#,###.###");
    if (fmt == "integer") return _pf_tv_pattern(value, "#,###");
    if (fmt == "percent") return _pf_tv_decimal(value, 0, 0, true, 2) + "%";
    if (fmt == "currency") {
        const std::string digits = _pf_tv_decimal(value, 2, 2, true);
        return digits[0] == '-' ? "-$" + digits.substr(1) : "$" + digits;
    }
    return _pf_tv_pattern(value, fmt);
}

static std::string pine_str_format_tv(
    const std::string& format_string,
    const std::vector<_PFTvFormatValue>& args) {
    std::string result;
    bool quoted = false;
    for (size_t i = 0; i < format_string.size();) {
        const char c = format_string[i];
        if (c == '\'') {
            if (i + 1 < format_string.size() && format_string[i + 1] == '\'') {
                result += '\'';
                i += 2;
            } else {
                quoted = !quoted;
                ++i;
            }
            continue;
        }
        if (c != '{' || quoted) {
            result += c;
            ++i;
            continue;
        }
        const size_t end = format_string.find('}', i + 1);
        if (end == std::string::npos) {
            result += format_string.substr(i);
            break;
        }
        const std::string inside = format_string.substr(i + 1, end - i - 1);
        const size_t first_comma = inside.find(',');
        const std::string index_text = _pf_tv_trim(inside.substr(0, first_comma));
        if (index_text.empty() || !std::all_of(index_text.begin(), index_text.end(),
                                               [](char ch) { return ch >= '0' && ch <= '9'; })) {
            result += format_string.substr(i, end - i + 1);
            i = end + 1;
            continue;
        }
        const size_t index = static_cast<size_t>(std::stoul(index_text));
        if (index >= args.size()) {
            result += format_string.substr(i, end - i + 1);
            i = end + 1;
            continue;
        }
        const auto& arg = args[index];
        if (arg.kind == _PFTvFormatValue::Kind::Text) {
            result += arg.text;
        } else if (first_comma == std::string::npos) {
            result += _pf_tv_number_style(arg.number, "");
        } else {
            const size_t second_comma = inside.find(',', first_comma + 1);
            const std::string type = _pf_tv_trim(inside.substr(
                first_comma + 1, second_comma - first_comma - 1));
            if (type == "number") {
                const std::string style = second_comma == std::string::npos
                    ? "" : inside.substr(second_comma + 1);
                result += _pf_tv_number_style(arg.number, style);
            } else {
                result += format_string.substr(i, end - i + 1);
            }
        }
        i = end + 1;
    }
    return result;
}


// --- syminfo derivation helpers (PineForge G2) ---
static inline std::string _pf_derive_prefix(const std::string& tickerid) {
    std::size_t colon = tickerid.find(':');
    return (colon == std::string::npos) ? tickerid : tickerid.substr(0, colon);
}

static inline std::string _pf_derive_main_tickerid(const std::string& tickerid) {
    // Strip trailing digits (optionally followed by '!') from the symbol part.
    // e.g. "CME_MINI:ES1!" -> "CME_MINI:ES", "NYMEX:CL2!" -> "NYMEX:CL"
    std::string result = tickerid;
    std::size_t colon = result.find(':');
    std::size_t start = (colon == std::string::npos) ? 0 : colon + 1;
    // Find end of base symbol (strip trailing digits + optional '!')
    std::size_t end = result.size();
    if (end > start && result[end - 1] == '!') {
        --end;
    }
    while (end > start && std::isdigit((unsigned char)result[end - 1])) {
        --end;
    }
    return result.substr(0, end);
}

static inline std::string _pf_derive_country(const std::string& tickerid) {
    // Lookup country by exchange prefix (text before ':').
    std::size_t colon = tickerid.find(':');
    std::string prefix = (colon == std::string::npos)
        ? tickerid : tickerid.substr(0, colon);
    static const std::unordered_map<std::string, std::string> _tbl = {
        {"AMEX", "US"},
        {"AQUIS", "GB"},
        {"ARCA", "US"},
        {"ASX", "AU"},
        {"B3", "BR"},
        {"BMF", "BR"},
        {"BMFBOVESPA", "BR"},
        {"BSE", "IN"},
        {"CBOE", "US"},
        {"CBOT", "US"},
        {"CME", "US"},
        {"CME_MINI", "US"},
        {"COINBASE", "US"},
        {"COMEX", "US"},
        {"HKEX", "HK"},
        {"JSE", "ZA"},
        {"KOSPI", "KR"},
        {"KRX", "KR"},
        {"LSE", "GB"},
        {"MOEX", "RU"},
        {"NASDAQ", "US"},
        {"NSE", "IN"},
        {"NYMEX", "US"},
        {"NYSE", "US"},
        {"OSE", "JP"},
        {"OTC", "US"},
        {"SGX", "SG"},
        {"SIX", "CH"},
        {"SSE", "CN"},
        {"SZSE", "CN"},
        {"TSE", "JP"},
        {"TSX", "CA"},
        {"UPBIT", "KR"},
        {"VENTURE", "CA"},
        {"XETRA", "DE"}
    };
    auto it = _tbl.find(prefix);
    return (it != _tbl.end()) ? it->second : na<std::string>();
}
// --- end syminfo derivation helpers ---

class GeneratedStrategy : public pineforge::source::PineStrategyHost {
public:
    bool _use_precalc = false;
    double remembered;
    int lastSubmitted;
    int lastClosure;
    int seenClosed;
    int evaluation;
    bool buy = false;
    double trigger = 0.0;
    std::string sensor = std::string("");
    bool _var_initialized = false;
    bool _inputs_initialized_ = false;

    struct _PFScriptState {
        decltype(GeneratedStrategy::remembered) _pf_value_0;
        decltype(GeneratedStrategy::lastSubmitted) _pf_value_1;
        decltype(GeneratedStrategy::lastClosure) _pf_value_2;
        decltype(GeneratedStrategy::seenClosed) _pf_value_3;
        decltype(GeneratedStrategy::evaluation) _pf_value_4;
        decltype(GeneratedStrategy::buy) _pf_value_5;
        decltype(GeneratedStrategy::trigger) _pf_value_6;
        decltype(GeneratedStrategy::sensor) _pf_value_7;
        decltype(GeneratedStrategy::_var_initialized) _pf_value_8;
        decltype(GeneratedStrategy::_inputs_initialized_) _pf_value_9;
    };
    static_assert(std::is_copy_constructible_v<_PFScriptState>, "generated Pine state must be deep-copy constructible");
    static_assert(std::is_copy_assignable_v<_PFScriptState>, "generated Pine state must be deep-copy assignable");
    std::optional<_PFScriptState> _pf_script_state_checkpoint_;

    void snapshot_script_state() override {
        _pf_script_state_checkpoint_.emplace(_PFScriptState{
            remembered,
            lastSubmitted,
            lastClosure,
            seenClosed,
            evaluation,
            buy,
            trigger,
            sensor,
            _var_initialized,
            _inputs_initialized_,
        });
    }

    void restore_script_state() override {
        if (!_pf_script_state_checkpoint_) return;
        this->remembered = _pf_script_state_checkpoint_->_pf_value_0;
        this->lastSubmitted = _pf_script_state_checkpoint_->_pf_value_1;
        this->lastClosure = _pf_script_state_checkpoint_->_pf_value_2;
        this->seenClosed = _pf_script_state_checkpoint_->_pf_value_3;
        this->evaluation = _pf_script_state_checkpoint_->_pf_value_4;
        this->buy = _pf_script_state_checkpoint_->_pf_value_5;
        this->trigger = _pf_script_state_checkpoint_->_pf_value_6;
        this->sensor = _pf_script_state_checkpoint_->_pf_value_7;
        this->_var_initialized = _pf_script_state_checkpoint_->_pf_value_8;
        this->_inputs_initialized_ = _pf_script_state_checkpoint_->_pf_value_9;
    }

    void commit_script_state() override {
        snapshot_script_state();
    }

    explicit GeneratedStrategy() : remembered(na<double>()), lastSubmitted(-1), lastClosure(-1), seenClosed(0), evaluation(0) {
#if defined(PINEFORGE_HAS_EXPLICIT_PINE_EXECUTION_ADAPTER_V1)
        pineforge::source::PineStrategyHost::attach_pine_execution_adapter();
#elif defined(PINEFORGE_HAS_EXPLICIT_PINE_CAP_V1)
        pineforge::source::PineStrategyHost::enable_pine_intraday_cap();
#endif
        pineforge::source::PineStrategyConfig cfg{};
        cfg.calc_on_order_fills = true;
        cfg.initial_capital = 10000000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        cfg.default_qty_value = 100.0;
        cfg.pyramiding = 0;
        cfg.commission_value = 0.0;
        cfg.slippage = 0;
        cfg.margin_long = 100.0;
        cfg.margin_short = 100.0;
        configure_pine_strategy(cfg);
    }

    void set_strategy_override(const std::string& key, const std::string& value) {
        pineforge::source::StrategyOverrides overrides{};
        if (key == "initial_capital") {
            overrides.initial_capital = std::stod(value);
        } else if (key == "commission_value") {
            overrides.commission_value = std::stod(value);
        } else if (key == "default_qty_value") {
            overrides.default_qty_value = std::stod(value);
        } else if (key == "pyramiding") {
            overrides.pyramiding = std::stoi(value);
        } else if (key == "slippage") {
            overrides.slippage = std::stoi(value);
        } else if (key == "process_orders_on_close") {
            overrides.process_orders_on_close = (value == "true" || value == "1");
        } else if (key == "calc_on_order_fills") {
            overrides.calc_on_order_fills = (value == "true" || value == "1");
        } else if (key == "close_entries_rule") {
            overrides.close_entries_rule = (value == "ANY" || value == "any" || value == "1");
        } else if (key == "default_qty_type") {
            if (value == "fixed" || value == "strategy.fixed" || value == "0") overrides.default_qty_type = static_cast<int>(QtyType::FIXED);
            else if (value == "percent_of_equity" || value == "strategy.percent_of_equity" || value == "1") overrides.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
            else if (value == "cash" || value == "strategy.cash" || value == "2") overrides.default_qty_type = static_cast<int>(QtyType::CASH);
            else return;
        } else if (key == "commission_type") {
            if (value == "percent" || value == "strategy.commission.percent" || value == "0") overrides.commission_type = static_cast<int>(CommissionType::PERCENT);
            else if (value == "cash_per_order" || value == "strategy.commission.cash_per_order" || value == "1") overrides.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
            else if (value == "cash_per_contract" || value == "strategy.commission.cash_per_contract" || value == "2") overrides.commission_type = static_cast<int>(CommissionType::CASH_PER_CONTRACT);
            else return;
        } else {
            return;
        }
        pineforge::source::PineStrategyHost::set_strategy_override(overrides);
    }
    bool _pf_setting_failed_ = false;
    std::string _pf_setting_failure_;
    void _pf_record_failure(const char* entrypoint, const char* message) noexcept {
        try { last_error_ = entrypoint; last_error_ += ": "; last_error_ += message; } catch (...) {}
    }
    void _pf_record_setting_failure(const char* entrypoint, const char* message) noexcept {
        if (!_pf_setting_failed_) {
            _pf_setting_failed_ = true;
            try { _pf_setting_failure_ = entrypoint; _pf_setting_failure_ += ": "; _pf_setting_failure_ += message; } catch (...) {}
        }
        _pf_refuse_failed_setting(nullptr);
    }
    void _pf_require_settings_ok() const {
#ifdef PF_SETTINGS_API_VERSION
        if (_pf_setting_failed_) throw ::pineforge::checked_settings::LatchedSettingsFailure(_pf_setting_failure_.empty() ? "legacy strategy setter failed" : _pf_setting_failure_);
#else
        if (_pf_setting_failed_) throw std::runtime_error(_pf_setting_failure_.empty() ? "legacy strategy setter failed" : _pf_setting_failure_);
#endif
    }
    bool _pf_refuse_failed_setting(ReportC* out) noexcept {
        if (!_pf_setting_failed_) return false;
        try { last_error_ = _pf_setting_failure_.empty() ? "legacy strategy setter failed" : _pf_setting_failure_; } catch (...) {}
        if (out) *out = ReportC{};
        return true;
    }
#ifdef PF_SETTINGS_API_VERSION
    static pineforge::source::PineStrategyConfig _pf_settings_declared_config() {
        pineforge::source::PineStrategyConfig cfg{};
        cfg.calc_on_order_fills = true;
        cfg.initial_capital = 10000000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        cfg.default_qty_value = 100.0;
        cfg.pyramiding = 0;
        cfg.commission_value = 0.0;
        cfg.slippage = 0;
        cfg.margin_long = 100.0;
        cfg.margin_short = 100.0;
        return cfg;
    }
    std::vector<::pineforge::checked_settings::Setting> _pf_settings_inputs() const {
        return {
        };
    }
    std::vector<::pineforge::checked_settings::Setting> _pf_settings_overrides() const {
        const double _pf_nan = std::numeric_limits<double>::quiet_NaN();
        const auto _pf_defaults = _pf_settings_declared_config();
        return {
            {"initial_capital", "float", ::pineforge::checked_settings::number(_pf_defaults.initial_capital), {}, 0.0},
            {"commission_value", "float", ::pineforge::checked_settings::number(_pf_defaults.commission_value), {}, 0.0},
            {"default_qty_value", "float", ::pineforge::checked_settings::number(_pf_defaults.default_qty_value), {}, 0.0},
            {"pyramiding", "int", ::pineforge::checked_settings::number(_pf_defaults.pyramiding), {}, 0.0},
            {"slippage", "int", ::pineforge::checked_settings::number(_pf_defaults.slippage), {}, 0.0},
            {"process_orders_on_close", "bool", ::pineforge::checked_settings::number(_pf_defaults.process_orders_on_close), {}, _pf_nan},
            {"calc_on_order_fills", "bool", ::pineforge::checked_settings::number(_pf_defaults.calc_on_order_fills), {}, _pf_nan},
            {"close_entries_rule", "string", _pf_close_entries_rule_word(_pf_defaults.close_entries_rule_any), {"FIFO", "ANY"}, _pf_nan},
            {"default_qty_type", "string", _pf_default_qty_type_word(_pf_defaults.default_qty_type), {"fixed", "percent_of_equity", "cash"}, _pf_nan},
            {"commission_type", "string", _pf_commission_type_word(_pf_defaults.commission_type), {"percent", "cash_per_order", "cash_per_contract"}, _pf_nan},
        };
    }
    static std::string _pf_close_entries_rule_word(int _pf_value) {
        if (_pf_value == 0) return "FIFO";
        if (_pf_value == 1) return "ANY";
        return "invalid";
    }
    static std::string _pf_default_qty_type_word(int _pf_value) {
        if (_pf_value == 0) return "fixed";
        if (_pf_value == 1) return "percent_of_equity";
        if (_pf_value == 2) return "cash";
        return "invalid";
    }
    static std::string _pf_commission_type_word(int _pf_value) {
        if (_pf_value == 0) return "percent";
        if (_pf_value == 1) return "cash_per_order";
        if (_pf_value == 2) return "cash_per_contract";
        return "invalid";
    }
    void _pf_set_input_checked(const std::string& _pf_key, const std::string& _pf_value) {
        if (_pf_refuse_failed_setting(nullptr)) throw ::pineforge::checked_settings::Error{PF_SETTINGS_RUN_FAILED, last_error_.c_str()};
        ::pineforge::checked_settings::require(script_bars_processed() == 0 && stream_phase_ == StreamPhase::IDLE, "settings are frozen after execution begins", PF_SETTINGS_UNSUPPORTED);
        const auto _pf_inputs = _pf_settings_inputs();
        const ::pineforge::checked_settings::Setting* _pf_match = nullptr;
        for (const auto& _pf_input : _pf_inputs) {
            if (_pf_input.name != _pf_key) continue;
            ::pineforge::checked_settings::require(_pf_match == nullptr, "ambiguous input key", PF_SETTINGS_UNSUPPORTED);
            _pf_match = &_pf_input;
        }
        ::pineforge::checked_settings::require(_pf_match != nullptr, "unknown input key");
        const auto _pf_canonical = ::pineforge::checked_settings::validate(*_pf_match, _pf_value);
        set_input(_pf_key, _pf_canonical);
        ::pineforge::checked_settings::require(inputs_.count(_pf_key) && inputs_.at(_pf_key) == _pf_canonical, "input was not installed", PF_SETTINGS_UNSUPPORTED);
    }
    void _pf_set_override_checked(const std::string& _pf_key, const std::string& _pf_value) {
        if (_pf_refuse_failed_setting(nullptr)) throw ::pineforge::checked_settings::Error{PF_SETTINGS_RUN_FAILED, last_error_.c_str()};
        ::pineforge::checked_settings::require(script_bars_processed() == 0 && stream_phase_ == StreamPhase::IDLE, "settings are frozen after execution begins", PF_SETTINGS_UNSUPPORTED);
        for (const auto& _pf_override : _pf_settings_overrides()) {
            if (_pf_override.name != _pf_key) continue;
            auto _pf_alias = _pf_value;
            if (_pf_key == "close_entries_rule") {
                if (_pf_value == "0" || _pf_value == "fifo") _pf_alias = "FIFO";
                if (_pf_value == "1" || _pf_value == "any") _pf_alias = "ANY";
            }
            if (_pf_key == "default_qty_type") {
                if (_pf_value == "0" || _pf_value == "strategy.fixed") _pf_alias = "fixed";
                if (_pf_value == "1" || _pf_value == "strategy.percent_of_equity") _pf_alias = "percent_of_equity";
                if (_pf_value == "2" || _pf_value == "strategy.cash") _pf_alias = "cash";
            }
            if (_pf_key == "commission_type") {
                if (_pf_value == "0" || _pf_value == "strategy.commission.percent") _pf_alias = "percent";
                if (_pf_value == "1" || _pf_value == "strategy.commission.cash_per_order") _pf_alias = "cash_per_order";
                if (_pf_value == "2" || _pf_value == "strategy.commission.cash_per_contract") _pf_alias = "cash_per_contract";
            }
            const auto _pf_canonical = ::pineforge::checked_settings::validate(_pf_override, _pf_alias);
            set_strategy_override(_pf_key, _pf_canonical);
            return;
        }
        throw ::pineforge::checked_settings::Error{PF_SETTINGS_INVALID_ARGUMENT, "unknown override key"};
    }
    std::string _pf_settings_receipt() const {
        ::pineforge::checked_settings::require(!_pf_setting_failed_, _pf_setting_failure_.empty() ? "legacy strategy setter failed" : _pf_setting_failure_.c_str(), PF_SETTINGS_RUN_FAILED);
        std::string _pf_document = "{\"version\":1,\"inputs\":[";
        const auto _pf_inputs = _pf_settings_inputs();
        _pf_document += "],\"overrides\":[";
        const auto _pf_overrides = _pf_settings_overrides();
        _pf_document += ::pineforge::checked_settings::describe(_pf_overrides[0], ::pineforge::checked_settings::number((std::isnan(override_.initial_capital) ? config_.initial_capital : override_.initial_capital)));
        _pf_document += ',';
        _pf_document += ::pineforge::checked_settings::describe(_pf_overrides[1], ::pineforge::checked_settings::number((std::isnan(override_.commission_value) ? config_.commission_value : override_.commission_value)));
        _pf_document += ',';
        _pf_document += ::pineforge::checked_settings::describe(_pf_overrides[2], ::pineforge::checked_settings::number((std::isnan(override_.default_qty_value) ? config_.default_qty_value : override_.default_qty_value)));
        _pf_document += ',';
        _pf_document += ::pineforge::checked_settings::describe(_pf_overrides[3], ::pineforge::checked_settings::number((override_.pyramiding < 0 ? config_.pyramiding : override_.pyramiding)));
        _pf_document += ',';
        _pf_document += ::pineforge::checked_settings::describe(_pf_overrides[4], ::pineforge::checked_settings::number((override_.slippage < 0 ? config_.slippage : override_.slippage)));
        _pf_document += ',';
        _pf_document += ::pineforge::checked_settings::describe(_pf_overrides[5], ::pineforge::checked_settings::number(static_cast<bool>((override_.process_orders_on_close < 0 ? config_.process_orders_on_close : override_.process_orders_on_close))));
        _pf_document += ',';
        _pf_document += ::pineforge::checked_settings::describe(_pf_overrides[6], ::pineforge::checked_settings::number(static_cast<bool>((override_.calc_on_order_fills < 0 ? config_.calc_on_order_fills : override_.calc_on_order_fills))));
        _pf_document += ',';
        _pf_document += ::pineforge::checked_settings::describe(_pf_overrides[7], _pf_close_entries_rule_word((override_.close_entries_rule < 0 ? config_.close_entries_rule_any : override_.close_entries_rule)));
        _pf_document += ',';
        _pf_document += ::pineforge::checked_settings::describe(_pf_overrides[8], _pf_default_qty_type_word((override_.default_qty_type < 0 ? config_.default_qty_type : override_.default_qty_type)));
        _pf_document += ',';
        _pf_document += ::pineforge::checked_settings::describe(_pf_overrides[9], _pf_commission_type_word((override_.commission_type < 0 ? config_.commission_type : override_.commission_type)));
        return _pf_document + "]}";
    }
#endif

#ifndef PINEFORGE_HAS_SCRIPT_RUN_PREPARE_V1
#error "Generated lifecycle reset requires a matching PineForge engine; rebuild with script-run preparation support"
#endif
    void prepare_script_run(const Bar* bars, int n, bool allow_precalculation) override {
        _pf_require_settings_ok();
        _pf_script_state_checkpoint_.reset();
        this->_use_precalc = false;
        this->remembered = decltype(this->remembered)(na<double>());
        this->lastSubmitted = decltype(this->lastSubmitted)(-1);
        this->lastClosure = decltype(this->lastClosure)(-1);
        this->seenClosed = decltype(this->seenClosed)(0);
        this->evaluation = decltype(this->evaluation)(0);
        this->buy = false;
        this->trigger = 0.0;
        this->sensor = std::string("");
        this->_var_initialized = false;
        this->_inputs_initialized_ = false;
        (void)bars; (void)n; (void)allow_precalculation;
    }

    void on_source_bar(const Bar& bar) override {
        if (!_var_initialized) {
            _var_initialized = true;
        } else {
        }
        evaluation += 1;
        if (([&]{ auto _pna_l = (((int)trades_.size())); auto _pna_r = (seenClosed); double _pfc_l = static_cast<double>(_pna_l); double _pfc_r = static_cast<double>(_pna_r); bool _pfc_eq = (_pfc_l == _pfc_r) || (std::isfinite(_pfc_l) && std::isfinite(_pfc_r) && std::fabs(_pfc_l - _pfc_r) <= 1e-10); return !is_na(_pna_l) && !is_na(_pna_r) && ((_pfc_l > _pfc_r) && !_pfc_eq); }())) {
            seenClosed = ((int)trades_.size());
            lastClosure = pine_bar_index();
        }
        if (([&]{ auto _pna_l = (signed_position_size()); auto _pna_r = (0); double _pfc_l = static_cast<double>(_pna_l); double _pfc_r = static_cast<double>(_pna_r); bool _pfc_eq = (_pfc_l == _pfc_r) || (std::isfinite(_pfc_l) && std::isfinite(_pfc_r) && std::fabs(_pfc_l - _pfc_r) <= 1e-10); return !is_na(_pna_l) && !is_na(_pna_r) && (_pfc_eq); }())) {
            remembered = na<double>();
            if (((([&]{ auto _pna_l = (current_bar_.timestamp); auto _pna_r = ([&]() -> int64_t { std::string _tz = pineforge::normalize_timezone_for_posix((std::string("UTC"))); int _yr = (2025); int _mo = (4); int _dy = (3); int _hr = (16); int _min = (45); int _sc = (0); static thread_local std::string _last_tz; static thread_local int _last_yr = -1, _last_mo = -1, _last_dy = -1, _last_hr = -1, _last_min = -1, _last_sc = -1; static thread_local int64_t _last_res = -1; if (_last_res != -1 && _last_tz == _tz && _last_yr == _yr && _last_mo == _mo && _last_dy == _dy && _last_hr == _hr && _last_min == _min && _last_sc == _sc) { return _last_res; } struct tm t = {}; t.tm_year = _yr - 1900; t.tm_mon = _mo - 1; t.tm_mday = _dy; t.tm_hour = _hr; t.tm_min = _min; t.tm_sec = _sc; t.tm_isdst = -1; int64_t _res; if (_tz.empty() || _tz == "UTC" || _tz == "Etc/UTC") { _res = (int64_t)timegm(&t) * 1000; } else { static std::mutex _pf_ts_mu; std::lock_guard<std::mutex> _pf_ts_mu_lock(_pf_ts_mu); const char* _old = std::getenv("TZ"); std::string _old_tz = _old ? _old : ""; bool _had_old = (_old != nullptr); ::setenv("TZ", _tz.c_str(), 1); ::tzset(); _res = (int64_t)mktime(&t) * 1000; if (_had_old) { ::setenv("TZ", _old_tz.c_str(), 1); } else { ::unsetenv("TZ"); } ::tzset(); } _last_tz = _tz; _last_yr = _yr; _last_mo = _mo; _last_dy = _dy; _last_hr = _hr; _last_min = _min; _last_sc = _sc; _last_res = _res; return _res; }()); return !is_na(_pna_l) && !is_na(_pna_r) && (_pna_l == _pna_r); }()) && ([&]{ auto _pna_l = (lastSubmitted); auto _pna_r = (pine_bar_index()); double _pfc_l = static_cast<double>(_pna_l); double _pfc_r = static_cast<double>(_pna_r); bool _pfc_eq = (_pfc_l == _pfc_r) || (std::isfinite(_pfc_l) && std::isfinite(_pfc_r) && std::fabs(_pfc_l - _pfc_r) <= 1e-10); return !is_na(_pna_l) && !is_na(_pna_r) && (!_pfc_eq); }())) && ([&]{ auto _pna_l = (lastClosure); auto _pna_r = (pine_bar_index()); double _pfc_l = static_cast<double>(_pna_l); double _pfc_r = static_cast<double>(_pna_r); bool _pfc_eq = (_pfc_l == _pfc_r) || (std::isfinite(_pfc_l) && std::isfinite(_pfc_r) && std::fabs(_pfc_l - _pfc_r) <= 1e-10); return !is_na(_pna_l) && !is_na(_pna_r) && (!_pfc_eq); }()))) {
                lastSubmitted = pine_bar_index();
                strategy_cancel_all();
                buy = ([&]{ auto _pna_l = (true); auto _pna_r = (true); double _pfc_l = static_cast<double>(_pna_l); double _pfc_r = static_cast<double>(_pna_r); bool _pfc_eq = (_pfc_l == _pfc_r) || (std::isfinite(_pfc_l) && std::isfinite(_pfc_r) && std::fabs(_pfc_l - _pfc_r) <= 1e-10); return !is_na(_pna_l) && !is_na(_pna_r) && (_pfc_eq); }());
                trigger = round_to_mintick((current_bar_.close * ((buy) ? (1.0005) : (0.9995))));
                strategy_entry(std::string("ticket"), true, na<double>(), trigger, 1, std::string("scheduled"), "", 0, -1);
            }
        }
        if (([&]{ auto _pna_l = (signed_position_size()); auto _pna_r = (0); double _pfc_l = static_cast<double>(_pna_l); double _pfc_r = static_cast<double>(_pna_r); bool _pfc_eq = (_pfc_l == _pfc_r) || (std::isfinite(_pfc_l) && std::isfinite(_pfc_r) && std::fabs(_pfc_l - _pfc_r) <= 1e-10); return !is_na(_pna_l) && !is_na(_pna_r) && (!_pfc_eq); }())) {
            if (is_na(remembered)) {
                remembered = (round_to_mintick(((([&]{ auto _pna_l = (signed_position_size()); auto _pna_r = (0); double _pfc_l = static_cast<double>(_pna_l); double _pfc_r = static_cast<double>(_pna_r); bool _pfc_eq = (_pfc_l == _pfc_r) || (std::isfinite(_pfc_l) && std::isfinite(_pfc_r) && std::fabs(_pfc_l - _pfc_r) <= 1e-10); return !is_na(_pna_l) && !is_na(_pna_r) && ((_pfc_l > _pfc_r) && !_pfc_eq); }())) ? (current_bar_.high) : (current_bar_.low))) + 0.0);
            }
            sensor = (((((((((std::string("eval=") + pine_str_tostring_tv(evaluation, std::string(), syminfo_mintick_)) + std::string(" goal=")) + pine_str_tostring_tv(remembered, std::string("0.00000000"), syminfo_mintick_)) + std::string(" O=")) + pine_str_tostring_tv(current_bar_.open, std::string("0.00000000"), syminfo_mintick_)) + std::string(" H=")) + pine_str_tostring_tv(current_bar_.high, std::string("0.00000000"), syminfo_mintick_)) + std::string(" L=")) + pine_str_tostring_tv(current_bar_.low, std::string("0.00000000"), syminfo_mintick_));
            strategy_cancel(std::string("take"));
            strategy_exit(std::string("take"), std::string("ticket"), remembered, na<double>(), na<double>(), na<double>(), na<double>(), 100.0, sensor, na<double>(), "", na<double>(), na<double>());
            if (([&]{ auto _pna_l = ((pine_bar_index() - open_trade_entry_bar_index(0))); auto _pna_r = (2); double _pfc_l = static_cast<double>(_pna_l); double _pfc_r = static_cast<double>(_pna_r); bool _pfc_eq = (_pfc_l == _pfc_r) || (std::isfinite(_pfc_l) && std::isfinite(_pfc_r) && std::fabs(_pfc_l - _pfc_r) <= 1e-10); return !is_na(_pna_l) && !is_na(_pna_r) && ((_pfc_l > _pfc_r) || _pfc_eq); }())) {
                strategy_close(std::string("ticket"), std::string("age"), na<double>(), na<double>(), false, 115964117015ULL);
            }
        }
    }


};

extern "C" {
    void* strategy_create(const char* params_json) {
        try { return new GeneratedStrategy(); } catch (...) { return nullptr; }
    }
    void run_backtest(void* s, Bar* bars, int n, ReportC* out) {
        try {
        auto* strat = static_cast<GeneratedStrategy*>(s);
#ifndef PF_SETTINGS_API_VERSION
        if (strat->_pf_refuse_failed_setting(out)) return;
#endif
        strat->run(bars, n);
        if (!strat->_pf_refuse_failed_setting(out)) strat->fill_report(out);
        } catch (const std::exception& _pf_error) {
            if (out) *out = ReportC{};
            if (s) static_cast<GeneratedStrategy*>(s)->_pf_record_failure("run_backtest", _pf_error.what());
        } catch (...) {
            if (out) *out = ReportC{};
            if (s) static_cast<GeneratedStrategy*>(s)->_pf_record_failure("run_backtest", "unknown C++ exception");
        }
    }
    static void _pf_run_backtest_full_impl(void* s, Bar* bars, int n,
                           const char* input_tf, const char* script_tf,
                           int bar_magnifier, int magnifier_samples,
                           int magnifier_dist,
                           ReportC* out) {
        auto* strat = static_cast<GeneratedStrategy*>(s);
#ifndef PF_SETTINGS_API_VERSION
        if (strat->_pf_refuse_failed_setting(out)) return;
#endif
        std::string itf = input_tf ? input_tf : "";
        std::string stf = script_tf ? script_tf : "";
        bool needs_full_run = (bar_magnifier != 0)
            || !itf.empty() || !stf.empty();
        if (!needs_full_run) {
            strat->run(bars, n);
        } else {
            strat->run(bars, n, itf, stf, bar_magnifier != 0, magnifier_samples,
                       static_cast<MagnifierDistribution>(magnifier_dist));
        }
        if (!strat->_pf_refuse_failed_setting(out)) strat->fill_report(out);
    }
    void run_backtest_full(void* s, Bar* bars, int n, const char* input_tf, const char* script_tf,
                           int bar_magnifier, int magnifier_samples, int magnifier_dist, ReportC* out) {
        try { _pf_run_backtest_full_impl(s, bars, n, input_tf, script_tf, bar_magnifier, magnifier_samples, magnifier_dist, out); }
        catch (const std::exception& _pf_error) { if (out) *out = ReportC{}; if (s) static_cast<GeneratedStrategy*>(s)->_pf_record_failure("run_backtest_full", _pf_error.what()); }
        catch (...) { if (out) *out = ReportC{}; if (s) static_cast<GeneratedStrategy*>(s)->_pf_record_failure("run_backtest_full", "unknown C++ exception"); }
    }
    void strategy_free(void* s) {
        try { delete static_cast<GeneratedStrategy*>(s); } catch (...) {}
    }
    void report_free(ReportC* report) {
        try { BacktestEngine::free_report(report); } catch (...) {}
    }
    void strategy_set_input(void* s, const char* key, const char* value) {
        if (!s || !key || !value) return;
        try { static_cast<GeneratedStrategy*>(s)->set_input(key, value); }
        catch (const std::exception& _pf_error) { static_cast<GeneratedStrategy*>(s)->_pf_record_setting_failure("strategy_set_input", _pf_error.what()); }
        catch (...) { static_cast<GeneratedStrategy*>(s)->_pf_record_setting_failure("strategy_set_input", "unknown C++ exception"); }
    }
    void strategy_set_override(void* s, const char* key, const char* value) {
        if (!s || !key || !value) return;
        try { static_cast<GeneratedStrategy*>(s)->set_strategy_override(key, value); }
        catch (const std::exception& _pf_error) { static_cast<GeneratedStrategy*>(s)->_pf_record_setting_failure("strategy_set_override", _pf_error.what()); }
        catch (...) { static_cast<GeneratedStrategy*>(s)->_pf_record_setting_failure("strategy_set_override", "unknown C++ exception"); }
    }
    void strategy_set_magnifier_volume_weighted(void* s, int on) {
        if (!s) return;
        try { static_cast<GeneratedStrategy*>(s)->set_magnifier_volume_weighted(on != 0); }
        catch (const std::exception& _pf_error) { static_cast<GeneratedStrategy*>(s)->_pf_record_setting_failure("strategy_set_magnifier_volume_weighted", _pf_error.what()); }
        catch (...) { static_cast<GeneratedStrategy*>(s)->_pf_record_setting_failure("strategy_set_magnifier_volume_weighted", "unknown C++ exception"); }
    }
#ifdef PF_SETTINGS_API_VERSION
    uint32_t strategy_settings_api_version(void) { return PF_SETTINGS_API_VERSION; }
    int strategy_create_checked(const char* params_json, void** out, char* error, size_t error_capacity) {
        if (out) *out = nullptr;
        return ::pineforge::checked_settings::boundary(error, error_capacity, [&] {
            ::pineforge::checked_settings::require(out != nullptr, "strategy output pointer is null");
            ::pineforge::checked_settings::require(!params_json || !*params_json, "params_json is reserved; use checked setters", PF_SETTINGS_UNSUPPORTED);
            *out = new GeneratedStrategy();
        });
    }
    int strategy_set_input_checked(void* s, const char* key, const char* value, char* error, size_t error_capacity) {
        return ::pineforge::checked_settings::boundary(error, error_capacity, [&] {
            ::pineforge::checked_settings::require(s && key && value, "null strategy, key or value");
            static_cast<GeneratedStrategy*>(s)->_pf_set_input_checked(key, value);
        });
    }
    int strategy_set_override_checked(void* s, const char* key, const char* value, char* error, size_t error_capacity) {
        return ::pineforge::checked_settings::boundary(error, error_capacity, [&] {
            ::pineforge::checked_settings::require(s && key && value, "null strategy, key or value");
            static_cast<GeneratedStrategy*>(s)->_pf_set_override_checked(key, value);
        });
    }
    int strategy_get_effective_settings(void* s, char* json, size_t capacity, size_t* required, char* error, size_t error_capacity) {
        if (required) *required = 0;
        return ::pineforge::checked_settings::boundary(error, error_capacity, [&] {
            ::pineforge::checked_settings::require(s != nullptr, "null strategy");
            ::pineforge::checked_settings::receipt(static_cast<GeneratedStrategy*>(s)->_pf_settings_receipt(), json, capacity, required);
        });
    }
    int run_backtest_full_checked(void* s, Bar* bars, int n, const char* input_tf, const char* script_tf, int bar_magnifier, int magnifier_samples, int magnifier_dist, ReportC* out, char* error, size_t error_capacity) {
        return ::pineforge::checked_settings::boundary(error, error_capacity, [&] {
            ::pineforge::checked_settings::require(s && out && n >= 0 && (n == 0 || bars), "invalid batch arguments");
            _pf_run_backtest_full_impl(s, bars, n, input_tf, script_tf, bar_magnifier, magnifier_samples, magnifier_dist, out);
            const auto& _pf_error = static_cast<GeneratedStrategy*>(s)->last_error();
            ::pineforge::checked_settings::require(_pf_error.empty(), _pf_error.c_str(), PF_SETTINGS_RUN_FAILED);
        });
    }
#endif
}
