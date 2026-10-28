// Same executed rows as the unit binary, with an independent artifact writer
// and a strict full-population sink input. Missing streaming-ratio stays a nonzero result;
// unavailable candidate bits/witnesses are printed as unavailable, never NaN
// successes. No WILL_FAIL/skip/exception catcher wraps the feature assertion.
#define METRICS_STREAMING_COMPARE_TOOL 1
#include "test_metrics_streaming_ratios.cpp"

#include <iostream>

namespace streaming_compare {
using namespace streaming_suite;
using namespace pineforge;
using namespace pineforge::metrics;
namespace fs = std::filesystem;

std::string quote(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c == '\n') out << "\\n";
        else if (c == '\r') out << "\\r";
        else if (c == '\t') out << "\\t";
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                             << static_cast<unsigned>(c) << std::dec;
        else out << c;
    }
    out << '"';
    return out.str();
}
void print_extent(const MetricsExtentV1& value) {
    std::cout << "{\"empty\":" << value.empty << ",\"count\":" << value.count
              << ",\"first_time_ms\":" << value.first_time_ms
              << ",\"last_time_ms\":" << value.last_time_ms << "}";
}
void print_ratio(double value) {
    std::cout << "{\"bits\":" << quote(mt::hex_bits(value))
              << ",\"printed\":" << quote(mt::printed(value))
              << ",\"printed_2\":" << quote([&] {
                    std::ostringstream out; out << std::fixed << std::setprecision(2) << value;
                    return out.str();
                 }()) << "}";
}
void print_trace(const std::vector<mt::Trace>& trace) {
    std::cout << "[";
    bool first = true;
    for (const auto& t : trace) {
        if (!first) std::cout << ",";
        first = false;
        std::cout << "[" << quote(t.stage) << "," << t.index << "," << quote(t.operation)
                  << "," << quote(t.code) << "," << quote(t.field) << "," << quote(t.reason) << "]";
    }
    std::cout << "]";
}
struct Maximum {
    double value = 0.;
    std::string row;
    double old = mt::nan, actual = mt::nan, kappa = mt::nan;
    std::int64_t q = 0;
    std::size_t m = 0;
    void add(double difference, const Artifact& a, double before, double after) {
        if (row.empty() || difference > value) {
            value = difference; row = a.name; old = before; actual = after; kappa = a.kappa;
            q = a.actual_extent.count; m = a.old.m;
        }
    }
    void print(const char* name) const {
        if (row.empty()) {
            std::cout << "{\"maximum\":" << quote(name)
                      << ",\"value\":null,\"row\":null,\"status\":\"unavailable\"}\n";
            return;
        }
        std::cout << "{\"maximum\":" << quote(name) << ",\"value\":" << quote(mt::printed(value))
                  << ",\"row\":" << quote(row) << ",\"old\":" << quote(mt::printed(old))
                  << ",\"new\":" << quote(mt::printed(actual))
                  << ",\"attaining_kappa\":" << quote(mt::printed(kappa))
                  << ",\"q\":" << q << ",\"m\":" << m << "}\n";
    }
};
void report() {
    std::size_t streaming = 0, initial_curve = 0, stored_fallback = 0, rerun = 0, missing = 0;
    Maximum sharpe_abs, sharpe_rel, sortino_abs, sortino_rel, kappa_max;
    for (std::size_t i = 0; i < artifacts.size(); ++i) {
        const auto& a = artifacts[i];
        const int after = i + 1 < artifacts.size() ? artifacts[i + 1].failures_before : failures;
        std::cout << "{\"row\":" << quote(a.name)
                  << ",\"result\":" << quote(after == a.failures_before ? "pass" : "fail")
                  << ",\"source_identity\":" << quote(a.source_identity)
                  << ",\"legacy_control_passes_on_base\":" << (a.legacy_control ? "true" : "false")
                  << ",\"plain_subset\":" << quote(a.plain)
                  << ",\"predicate_inputs\":" << quote(a.predicate_inputs)
                  << ",\"extent_plan\":{\"present\":" << a.planned.present << ",\"extent\":";
        print_extent(a.planned.extent);
        std::cout << ",\"attempt\":" << a.planned.attempt
                  << ",\"generation\":" << a.planned.input_generation
                  << ",\"admitted_view\":" << a.planned.admitted_view
                  << ",\"spec_identity\":" << a.planned.spec_identity << "},\"actual_extent\":";
        print_extent(a.actual_extent);
        std::cout << ",\"witness\":" << quote(a.candidate
            ? (a.candidate->receipt.witness == MetricsExtentWitnessV1::Match ? "match"
               : a.candidate->receipt.witness == MetricsExtentWitnessV1::Mismatch ? "mismatch"
               : "not_evaluated") : "unavailable");
        const auto route = a.candidate ? a.candidate->receipt.route : MetricsRatioRouteV1::Unavailable;
        if (route == MetricsRatioRouteV1::Streaming) ++streaming;
        else if (route == MetricsRatioRouteV1::LegacyRerun) ++rerun;
        else if (route == MetricsRatioRouteV1::LegacyActualCurve || a.legacy_control) {
            if (a.candidate && a.candidate->receipt.route_reason == MetricsRouteReasonV1::ExtentMismatch)
                ++stored_fallback;
            else ++initial_curve;
        } else ++missing;
        std::cout << ",\"route\":" << quote(a.legacy_control ? "legacy_actual_curve" : mt::route_name(route))
                  << ",\"candidate_status\":" << quote(a.unavailable ? "streaming-ratio not implemented"
                       : a.candidate ? "available" : a.legacy_control ? "legacy_control" : "unavailable")
                  << ",\"m\":" << a.old.m << ",\"span_ms\":" << quote(mt::printed(a.old.span))
                  << ",\"rf_bits\":" << quote(mt::hex_bits(a.old.rf))
                  << ",\"rf\":" << quote(mt::printed(a.old.rf))
                  << ",\"ann_bits\":" << quote(mt::hex_bits(a.old.ann))
                  << ",\"ann\":" << quote(mt::printed(a.old.ann))
                  << ",\"kappa\":" << quote(mt::printed(a.kappa))
                  << ",\"old_sharpe\":";
        if (a.reference_available) print_ratio(a.old.sharpe); else std::cout << "null";
        std::cout << ",\"old_sortino\":";
        if (a.reference_available) print_ratio(a.old.sortino); else std::cout << "null";
        std::cout << ",\"new_sharpe\":";
        if (a.candidate) print_ratio(a.candidate->sharpe_bar);
        else if (a.curve_control) print_ratio(a.curve_control->sharpe_bar);
        else std::cout << "null";
        std::cout << ",\"new_sortino\":";
        if (a.candidate) print_ratio(a.candidate->sortino_bar);
        else if (a.curve_control) print_ratio(a.curve_control->sortino_bar);
        else std::cout << "null";
        std::cout << ",\"old_monthly\":";
        if (a.old_monthly) {
            std::cout << "{\"sharpe\":"; print_ratio(a.old_monthly->first);
            std::cout << ",\"sortino\":"; print_ratio(a.old_monthly->second);
            std::cout << "}";
        } else std::cout << "null";
        std::cout << ",\"new_monthly\":";
        if (a.new_monthly) {
            std::cout << "{\"sharpe\":"; print_ratio(a.new_monthly->first);
            std::cout << ",\"sortino\":"; print_ratio(a.new_monthly->second);
            std::cout << "}";
        } else std::cout << "null";
        std::cout << ",\"original_failure\":{\"code\":" << quote(a.original_code)
                  << ",\"args\":" << quote(a.original_args) << ",\"message\":" << quote(a.original_message)
                  << "},\"stop_boundary\":{\"kind\":" << static_cast<unsigned>(a.boundary.kind)
                  << ",\"completed_marks\":" << a.boundary.completed_marks
                  << ",\"input_index\":" << a.boundary.input_index
                  << "},\"executions\":" << a.executions << ",\"publications\":" << a.publications
                  << ",\"state_bytes\":" << a.state_bytes << ",\"provider_reads\":" << a.provider_reads
                  << ",\"exact_reference\":" << quote(a.exact)
                  << ",\"legacy_ratio_failure\":{\"code\":" << quote(a.old.failure_code)
                  << ",\"field\":" << quote(a.old.failure_field)
                  << ",\"reason\":" << quote(a.old.failure_reason) << "}"
                  << ",\"mutation\":" << quote(a.mutation) << ",\"legacy_operation_trace\":";
        print_trace(a.old.trace);
        std::cout << ",\"candidate_operation_trace\":"; print_trace(a.candidate_trace);
        std::cout << ",\"trials\":[";
        bool first_trial = true;
        for (const auto& entry : a.trials) {
            if (!first_trial) std::cout << ",";
            first_trial = false;
            const auto& trial = entry.second;
            std::cout << "{\"name\":" << quote(entry.first)
                      << ",\"status\":" << static_cast<std::int32_t>(trial.status)
                      << ",\"stage\":" << quote(trial.stage)
                      << ",\"actual_extent\":";
            print_extent(trial.planned.extent);
            std::cout << ",\"provider_reads\":" << trial.provider_reads
                      << ",\"legacy_sharpe\":";
            print_ratio(trial.old.sharpe);
            std::cout << ",\"legacy_sortino\":";
            print_ratio(trial.old.sortino);
            std::cout << ",\"route\":"
                      << quote(trial.result ? mt::route_name(trial.result->receipt.route) : "unavailable")
                      << ",\"sharpe\":";
            if (trial.result) print_ratio(trial.result->sharpe_bar); else std::cout << "null";
            std::cout << ",\"sortino\":";
            if (trial.result) print_ratio(trial.result->sortino_bar); else std::cout << "null";
            std::cout << ",\"operation_trace\":";
            print_trace(trial.trace);
            std::cout << "}";
        }
        std::cout << "]";
        std::cout << ",\"mutation_output\":";
        if (a.mutant_result) {
            std::cout << "{\"sharpe\":";
            print_ratio(a.mutant_result->sharpe);
            std::cout << ",\"sortino\":";
            print_ratio(a.mutant_result->sortino);
            std::cout << ",\"downside_sum\":" << quote(mt::printed(a.mutant_result->down)) << "}";
        } else std::cout << "null";
        std::cout << ",\"replay\":";
        if (a.replay_receipt && a.replay_report) {
            ++rerun;
            std::cout << "{\"route\":" << quote(mt::route_name(a.replay_receipt->route))
                      << ",\"attempts\":" << a.replay_receipt->replay_attempts
                      << ",\"sharpe\":";
            print_ratio(a.replay_report->metrics.equity.sharpe_bar);
            std::cout << ",\"sortino\":";
            print_ratio(a.replay_report->metrics.equity.sortino_bar);
            std::cout << "}";
        } else std::cout << "null";
        std::cout << "}\n";
        if (a.candidate && route != MetricsRatioRouteV1::LegacyRerun) {
            if (std::isfinite(a.old.sharpe) && std::isfinite(a.candidate->sharpe_bar)) {
                const double d = std::abs(a.candidate->sharpe_bar - a.old.sharpe);
                sharpe_abs.add(d, a, a.old.sharpe, a.candidate->sharpe_bar);
                sharpe_rel.add(a.old.sharpe == 0. ? (d == 0. ? 0. : INFINITY)
                                                : d / std::abs(a.old.sharpe),
                               a, a.old.sharpe, a.candidate->sharpe_bar);
            }
            if (std::isfinite(a.old.sortino) && std::isfinite(a.candidate->sortino_bar)) {
                const double d = std::abs(a.candidate->sortino_bar - a.old.sortino);
                sortino_abs.add(d, a, a.old.sortino, a.candidate->sortino_bar);
                sortino_rel.add(a.old.sortino == 0. ? (d == 0. ? 0. : INFINITY)
                                                  : d / std::abs(a.old.sortino),
                                a, a.old.sortino, a.candidate->sortino_bar);
            }
        }
        if (std::isfinite(a.kappa))
            kappa_max.add(a.kappa, a, a.old.sharpe,
                          a.candidate ? a.candidate->sharpe_bar : mt::nan);
    }
    sharpe_abs.print("sharpe_abs_diff"); sharpe_rel.print("sharpe_rel_diff");
    sortino_abs.print("sortino_abs_diff"); sortino_rel.print("sortino_rel_diff");
    kappa_max.print("sharpe_kappa_max");
    std::cout << "{\"sharpe_rel_diff_at_kappa_max\":";
    if (std::isfinite(kappa_max.actual) && std::isfinite(kappa_max.old)) {
        const double d = std::abs(kappa_max.actual - kappa_max.old);
        std::cout << quote(mt::printed(kappa_max.old == 0. ? (d == 0. ? 0. : INFINITY)
                                                          : d / std::abs(kappa_max.old)));
    } else std::cout << "null";
    std::cout << ",\"row\":" << quote(kappa_max.row) << "}\n";
    std::cout << "{\"totals\":{\"streaming\":" << streaming
              << ",\"initial_curve_route\":" << initial_curve
              << ",\"stored_curve_fallback\":" << stored_fallback
              << ",\"legacy_rerun\":" << rerun << ",\"unavailable\":" << missing
              << ",\"population\":" << artifacts.size() << ",\"failures\":" << failures << "}}\n";
}

std::string trim(std::string value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}
void allowed_curve_writers() {
    if (!row("allowed-curve-writers-source-gate")) return;
    const fs::path root = fs::path(mt::tests_directory()).parent_path();
    const std::string pattern =
        R"(equity_curve_\.(push_back|emplace_back|clear|resize|erase|insert|assign|swap)|equity_curve_\.(back|front)\(\)(\.(equity|open_profit|time_ms))?\s*([-+*/]?=|\+\+|--)|equity_curve_\s*\[[^\]\r\n]+\](\.(equity|open_profit|time_ms))?\s*([-+*/]?=|\+\+|--)|equity_curve_\s*=\s*|swap\([^;\n]*equity_curve_|last\.(equity|open_profit|time_ms)\s*([-+*/]?=|\+\+|--)|last_equity_point\(\)->(equity|open_profit|time_ms)\s*([-+*/]?=|\+\+|--))";
    // python3 is already required by this tool's sink reader, the exact
    // reference and the CTest row wrapper; rg is absent on the release host.
    // Keep the same file:line:source records and the full writer pattern.
    const std::string python =
        "import pathlib,re,sys\n"
        "root=pathlib.Path(sys.argv[1])\n"
        "pattern=re.compile(sys.argv[2])\n"
        "for tree in ('src','include'):\n"
        "    for path in sorted((root/tree).rglob('*')):\n"
        "        if not path.is_file():\n"
        "            continue\n"
        "        for number,line in enumerate(path.read_text(encoding='utf-8').splitlines(),1):\n"
        "            if pattern.search(line):\n"
        "                print(f'{path.relative_to(root).as_posix()}:{number}:{line}')\n";
    const std::string command = "python3 -c " + mt::shell_quote(python)
        + " " + mt::shell_quote(root.string()) + " " + mt::shell_quote(pattern);
    FILE* pipe = popen(command.c_str(), "r");
    STREAM_CHECK(pipe != nullptr);
    if (!pipe) return;
    std::map<std::string, int> observed;
    char buffer[4096];
    while (std::fgets(buffer, sizeof buffer, pipe)) {
        const std::string line = buffer;
        const auto a = line.find(':'), b = line.find(':', a + 1);
        STREAM_CHECK(a != std::string::npos && b != std::string::npos);
        if (b != std::string::npos)
            ++observed[line.substr(0, a) + ":" + trim(line.substr(b + 1))];
    }
    STREAM_CHECK(pclose(pipe) == 0);
    // Source-derived inventory: begin_report_sink (engine_report.cpp:1200,
    // :1240), reset_run_state (engine_run.cpp:119), restore/store_equity_point
    // (native_execution_consumer.cpp:13611-13623), record_equity_point
    // (engine.hpp:1630-1631), and Pine's presenter/range-end amendments
    // (pine_strategy_host.cpp:2216-2275). Line numbers are not key identities.
    const std::map<std::string, int> allowed{
        {"src/engine_report.cpp:equity_curve_.swap(held);", 1}, // M15
        {"src/engine_report.cpp:std::vector<pf_equity_point_t>().swap(equity_curve_);", 1},
        {"src/engine_run.cpp:equity_curve_.clear();           // retain capacity (handle-reuse sweep pattern)", 1},
        {"src/native_execution_consumer.cpp:engine.equity_curve_.swap(restored);", 1}, // M16
        {"src/native_execution_consumer.cpp:equity_curve_.back() = point;", 1},
        {"src/native_execution_consumer.cpp:equity_curve_.push_back(point);", 1},
        {"include/pineforge/engine.hpp:if (equity_curve_.empty()) equity_curve_.push_back(p);", 1},
        {"include/pineforge/engine.hpp:else equity_curve_.back() = p;", 1},
        {"src/source/pine_strategy_host.cpp:last.open_profit = 0.0;", 2},
        {"src/source/pine_strategy_host.cpp:last.equity = initial_capital_ + net_profit_sum_ + range_end_pnl;", 2},
        {"src/source/pine_strategy_host.cpp:last.time_ms = report_quote_time_ms_;", 1},
        {"src/source/pine_strategy_host.cpp:last.equity = initial_capital_ + out->net_profit;", 1},
    };
    STREAM_CHECK(observed == allowed);
    // Mutation of the inventory, without editing a working-tree source.
    auto added_writer = observed;
    ++added_writer["src/new_writer.cpp:equity_curve_.push_back(point);"];
    STREAM_CHECK(added_writer != allowed);
    active->legacy_control = true; // Guard of today's curve implementation.
}

std::vector<std::string> split(const std::string& line, char separator = '\t') {
    std::vector<std::string> out;
    std::istringstream in(line);
    std::string cell;
    while (std::getline(in, cell, separator)) out.push_back(cell);
    if (!line.empty() && line.back() == separator) out.emplace_back();
    return out;
}
std::vector<pf_equity_point_t> read_verified_sink(const fs::path& root, const fs::path& path) {
    // Use the project's strict reader solely to validate/deserialize inputs;
    // neither expected ratio nor reference arithmetic imports production code.
    const std::string python =
        "import sys\n"
        "sys.path.insert(0,sys.argv[1])\n"
        "from pf_sink import SinkReader\n"
        "with SinkReader(sys.argv[2]) as r:\n"
        " for x in r:\n"
        "  if x.tag=='E':\n"
        "   t,e,o=x.values\n"
        "   print(str(t)+'\\t'+float('nan' if e is None else e).hex()+'\\t'"
        "+float('nan' if o is None else o).hex())\n"
        " assert r.complete\n"
        " print('VERIFIED\\t'+r.sha256)\n";
    const std::string command = "python3 -c " + mt::shell_quote(python)
        + " " + mt::shell_quote((root / "docker").string()) + " " + mt::shell_quote(path.string());
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) throw std::runtime_error("cannot start strict sink reader");
    std::vector<pf_equity_point_t> points;
    bool verified = false;
    char buffer[4096];
    while (std::fgets(buffer, sizeof buffer, pipe)) {
        const auto cells = split(trim(buffer));
        if (cells.size() == 2 && cells[0] == "VERIFIED") { verified = true; continue; }
        if (cells.size() != 3) throw std::runtime_error("invalid strict-reader record");
        points.push_back({std::stoll(cells[0]), std::stod(cells[1]), std::stod(cells[2])});
    }
    if (pclose(pipe) != 0 || !verified)
        throw std::runtime_error("incomplete/invalid sink: " + path.string());
    return points;
}
void corpus(const fs::path& directory, std::size_t expected_count) {
    if (expected_count == 0) throw std::runtime_error("empty expected population");
    // population.tsv is captured BEFORE dispatch by the collection host, not
    // inferred from E records. One row per probe, including failures/empty
    // results. "sink_is_metric" must be 1; selected hidden metric suffixes
    // require their authoritative separately captured metric sink.
    std::ifstream manifest(directory / "population.tsv");
    if (!manifest) throw std::runtime_error("missing population.tsv");
    std::string line;
    if (!std::getline(manifest, line)) throw std::runtime_error("empty population");
    const auto names = split(line);
    std::map<std::string, std::size_t> columns;
    for (std::size_t i = 0; i < names.size(); ++i)
        if (!columns.emplace(names[i], i).second) throw std::runtime_error("duplicate manifest column");
    const std::vector<std::string> required{
        "probe", "sink", "sink_is_metric", "plan_present", "planned_count", "planned_first",
        "planned_last", "attempt", "generation", "view", "spec", "plain_subset",
        "predicate_inputs", "original_code", "original_args", "module_identity",
        "entry", "declaration_version", "raw_input_tf", "raw_script_tf", "resolved_input_tf",
        "resolved_script_tf", "chart_tf", "is_stream", "simple_run", "timeframe_undetected",
        "security_sites", "lower_tf_sites", "declared_magnifier", "begin_magnifier",
        "intrabar_kind", "pending_selection", "admitted_selection", "realtime_tail",
        "tail_horizon", "probe_tail", "report_policy", "label_policy", "calculation",
        "coof", "pooc", "tick", "history_tick", "normal_completion",
        "presenter_identity", "held_value_amended", "recovery_proved", "numeric_proved",
        "initial_capital", "chart_timezone", "first_open", "last_close", "bars_in_market",
        "net_profit", "original_status", "new_status", "original_domain", "new_domain",
        "new_code", "new_args", "chosen_route", "replay_attempts",
        "new_sharpe_bits", "new_sortino_bits", "module_contract_bound",
        "builtin_presenter", "builtin_marks", "builtin_scheduler", "final_view_bound"};
    for (const auto& name : required)
        if (!columns.count(name)) throw std::runtime_error("missing pre-run/witness column: " + name);
    std::set<std::string> population;
    const fs::path root = fs::path(mt::tests_directory()).parent_path();
    while (std::getline(manifest, line)) {
        const auto cells = split(line);
        if (cells.size() != names.size()) throw std::runtime_error("incomplete population row");
        const auto get = [&](const std::string& key) -> const std::string& {
            return cells.at(columns.at(key));
        };
        if (!population.insert(get("probe")).second) throw std::runtime_error("duplicate probe");
        if (!row("corpus/" + get("probe"))) continue;
        STREAM_CHECK(get("original_status") == get("new_status"));
        STREAM_CHECK(get("original_domain") == get("new_domain"));
        STREAM_CHECK(get("original_code") == get("new_code"));
        STREAM_CHECK(get("original_args") == get("new_args"));
        active->original_code = get("original_code"); active->original_args = get("original_args");
        active->plain = get("plain_subset");
        // Each failed predicate is emitted with its observed/required value
        // and known/missing status; do not replace it with a shape nickname.
        std::ostringstream audit;
        audit << "{\"observed\":{";
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i) audit << ",";
            audit << quote(names[i]) << ":" << quote(cells[i]);
        }
        audit << "},\"failed_predicate_inputs\":[";
        bool first_failed = true;
        const auto predicate = [&](const std::string& name, const std::string& observed,
                                   const std::string& required_value, bool matches) {
            if (matches) return;
            if (!first_failed) audit << ",";
            first_failed = false;
            const bool missing = observed == "unknown" || observed == "missing" || observed == "?";
            audit << "{\"input\":" << quote(name) << ",\"observed\":" << quote(observed)
                  << ",\"required\":" << quote(required_value) << ",\"status\":"
                  << quote(missing ? "missing" : "known") << "}";
        };
        for (const auto& name : {"declaration_version", "module_contract_bound",
                 "builtin_presenter", "builtin_marks", "builtin_scheduler", "final_view_bound"})
            predicate(name, get(name), "1", get(name) == "1");
        for (const auto& name : {"is_stream", "timeframe_undetected", "security_sites",
                 "lower_tf_sites", "declared_magnifier", "begin_magnifier", "pending_selection",
                 "admitted_selection", "realtime_tail", "probe_tail", "history_tick"})
            predicate(name, get(name), "0", get(name) == "0");
        predicate("module_identity", get("module_identity"), "known module identity",
                  !get("module_identity").empty() && get("module_identity") != "unknown"
                    && get("module_identity") != "missing" && get("module_identity") != "?");
        predicate("entry", get("entry"), "run_simple, run_tf or run_rich",
                  get("entry") == "run_simple" || get("entry") == "run_tf"
                    || get("entry") == "run_rich");
        predicate("resolved_timeframes", get("resolved_input_tf") + "/" + get("resolved_script_tf"),
                  "nonempty and equal", !get("resolved_input_tf").empty()
                    && get("resolved_input_tf") != "unknown"
                    && get("resolved_input_tf") != "missing" && get("resolved_input_tf") != "?"
                    && get("resolved_input_tf") == get("resolved_script_tf"));
        predicate("chart_tf", get("chart_tf"), get("resolved_script_tf"),
                  get("chart_tf") == get("resolved_script_tf"));
        predicate("intrabar_kind", get("intrabar_kind"), "none", get("intrabar_kind") == "none");
        predicate("report_policy", get("report_policy"), "KernelRecordedAtHostMarks",
                  get("report_policy") == "KernelRecordedAtHostMarks");
        predicate("label_policy", get("label_policy"), "FeedTolerant",
                  get("label_policy") == "FeedTolerant");
        predicate("calculation", get("calculation"), "BarClose or BarCloseAndFills",
                  get("calculation") == "BarClose" || get("calculation") == "BarCloseAndFills");
        audit << "]}";
        active->predicate_inputs = audit.str();
        STREAM_CHECK((active->plain == "yes") == first_failed);
        if (get("original_domain") == "no_report") {
            STREAM_CHECK(get("sink") == "-");
            active->source_identity = "no_report:existing_failure";
            // No ratio row is invented for a never-begun attempt. A hard
            // kill/timeout is absent from this report population entirely.
            mt::CompletedMarkHost host({});
            MetricsExtentReceiptV1 receipt;
            implemented(host.metrics_extent_receipt_v1(&receipt));
            continue;
        }
        if (get("sink_is_metric") != "1")
            throw std::runtime_error("emitted E is not the authoritative metric source");
        const auto vector = read_verified_sink(root, directory / get("sink"));
        const auto points = mt::source(vector);
        MetricsExtentPlanV1 p;
        p.present = std::stoul(get("plan_present"));
        p.extent.count = std::stoll(get("planned_count"));
        p.extent.empty = p.extent.count == 0;
        p.extent.first_time_ms = std::stoll(get("planned_first"));
        p.extent.last_time_ms = std::stoll(get("planned_last"));
        p.attempt = std::stoull(get("attempt")); p.input_generation = std::stoull(get("generation"));
        p.admitted_view = std::stoull(get("view")); p.spec_identity = std::stoull(get("spec"));
        auto cfg = mt::config();
        cfg.attempt = p.attempt; cfg.input_generation = p.input_generation;
        cfg.admitted_view = p.admitted_view; cfg.spec_identity = p.spec_identity;
        cfg.deterministic_replay = get("recovery_proved") == "1";
        active->source_identity = (directory / get("sink")).string();
        const bool streaming = active->plain == "yes" && get("numeric_proved") == "1"
            && cfg.deterministic_replay && get("normal_completion") == "1";
        candidate(points, p, cfg, streaming ? MetricsRatioRouteV1::Streaming
                                          : MetricsRatioRouteV1::LegacyActualCurve);
        if (active->candidate) {
            STREAM_CHECK(get("chosen_route") == mt::route_name(active->candidate->receipt.route));
            STREAM_CHECK(std::stoul(get("replay_attempts")) == active->candidate->receipt.replay_attempts);
            STREAM_CHECK(get("new_sharpe_bits") == mt::hex_bits(active->candidate->sharpe_bar));
            STREAM_CHECK(get("new_sortino_bits") == mt::hex_bits(active->candidate->sortino_bar));
        }
        // Exact monthly outputs and all other fields remain the frozen walk.
        // Nonfinite report failures stay in the population with their code.
        if (get("original_code").empty()) {
            const auto old = equity_stats_reference::compute_equity_stats(
                vector.data(), static_cast<std::int64_t>(vector.size()),
                std::stod(get("initial_capital")), get("chart_timezone"),
                std::stod(get("first_open")), std::stod(get("last_close")),
                std::stoll(get("bars_in_market")), std::stod(get("net_profit")));
            const auto actual = metrics::compute_equity_stats(vector.data(),
                static_cast<std::int64_t>(vector.size()), std::stod(get("initial_capital")),
                get("chart_timezone"), std::stod(get("first_open")), std::stod(get("last_close")),
                std::stoll(get("bars_in_market")), std::stod(get("net_profit")));
            STREAM_CHECK(mt::same(old.sharpe_monthly, actual.sharpe_monthly));
            STREAM_CHECK(mt::same(old.sortino_monthly, actual.sortino_monthly));
            active->old_monthly = std::make_pair(old.sharpe_monthly, old.sortino_monthly);
            active->new_monthly = std::make_pair(actual.sharpe_monthly, actual.sortino_monthly);
        }
        if (points.count >= 3) {
            const auto exact = mt::exact_reference(points);
            if (exact.count("kappa_rounded")) active->kappa = std::stod(exact.at("kappa_rounded"));
        }
    }
    if (population.size() != expected_count)
        throw std::runtime_error("population count differs: expected " + std::to_string(expected_count)
                                 + ", read " + std::to_string(population.size()));
}
void corpus_kappa_extensions() {
    double measured = -1.;
    for (const auto& a : artifacts)
        if (a.name.find("corpus/") == 0 && std::isfinite(a.kappa))
            measured = std::max(measured, a.kappa);
    if (measured < 0.) {
        std::cout << "{\"corpus_kappa_extensions\":\"no_finite_nonconstant_kappa\"}\n";
        return;
    }
    for (int orders : {3, 6}) {
        if (!row("corpus-kappa-plus-" + std::to_string(orders) + "-orders")) continue;
        const double target = std::max(1., measured) * std::pow(10., orders);
        std::vector<double> values{1.};
        for (int i = 0; i < 7; ++i) {
            const double perturbation = static_cast<double>(i % 3 - 1) / target;
            values.push_back(values.back() * (2. + perturbation));
        }
        const auto points = mt::equities(std::move(values));
        const auto exact = mt::exact_reference(points);
        if (exact.count("kappa_rounded")) active->kappa = std::stod(exact.at("kappa_rounded"));
        active->exact = "corpus_max=" + mt::printed(measured)
            + ",target=" + mt::printed(target) + ",exact_mean=" + exact.at("exact_mean");
        if (!std::isfinite(active->kappa) || active->kappa < target) {
            // Explicitly bound THIS realizable eight-point family, not all
            // binary64 curves. At the spacing limit its perturbations round
            // to the same factor; print the actual rational result as proof.
            active->exact += ",binary64_representability_limit_for_eight_point_family"
                ",centered_sum=" + exact.at("exact_centered_sum");
        }
        candidate(points, mt::plan(points), mt::config(),
            exact.at("mean_screen_pass") == "0" ? MetricsRatioRouteV1::LegacyActualCurve
                                               : MetricsRatioRouteV1::Streaming);
    }
}
} // namespace streaming_compare

int main(int argc, char** argv) {
    using namespace streaming_suite;
    std::string directory;
    std::size_t expected = 312;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--row" && i + 1 < argc) filter = argv[++i];
        else if (arg == "--mutant" && i + 1 < argc) mutant = argv[++i];
        else if (arg == "--curves" && i + 1 < argc) directory = argv[++i];
        else if (arg == "--expected-count" && i + 1 < argc) expected = std::stoull(argv[++i]);
        else if (arg == "--list") list_only = true;
        else { std::fprintf(stderr, "unknown comparison argument: %s\n", arg.c_str()); return 2; }
    }
    // No catch around test execution: a candidate exception is a hard failure.
    if (directory.empty()) run_suite();
    else {
        streaming_compare::corpus(directory, expected);
        streaming_compare::corpus_kappa_extensions();
    }
    streaming_compare::allowed_curve_writers();
    if (list_only) return 0;
    if (artifacts.empty()) { std::fprintf(stderr, "no comparison population\n"); return 2; }
    std::cout << "{\"population_scope\":" << streaming_compare::quote(filter.empty() ? "full" : "filtered")
              << ",\"row_filter\":" << streaming_compare::quote(filter) << "}\n";
    streaming_compare::report();
    return failures ? 1 : 0;
}
