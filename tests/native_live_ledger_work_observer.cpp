// SPDX-License-Identifier: Apache-2.0
// Link observer for the real CLI, independent of runner work accounting.
// GNU --wrap observes references from both static support libraries and main;
// SQLite itself can be static or shared. No query/result/metric is substituted.
#include <pineforge/pineforge.h>
#include <sqlite3.h>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace {
struct Observer {
    std::mutex mutex;
    std::map<sqlite3_stmt*, unsigned long long> rows;
    FILE* output = nullptr;
    Observer() {
        if (const auto* path = std::getenv("PINEFORGE_LEDGER_WORK_LOG"))
            output = std::fopen(path, "a");
        if (output) {
            std::fprintf(output, "{\"type\":\"linkage\",\"sqlite_version\":\"%s\"}\n", sqlite3_libversion());
            std::fflush(output);
        }
    }
    ~Observer() { if (output) std::fclose(output); }
};
Observer& observer() { static Observer value; return value; }
}

extern "C" int __real_sqlite3_step(sqlite3_stmt*);
extern "C" int __real_sqlite3_finalize(sqlite3_stmt*);

extern "C" int __wrap_sqlite3_step(sqlite3_stmt* statement) {
    const auto result = __real_sqlite3_step(statement);
    auto& value = observer();
    if (value.output && result == SQLITE_ROW) {
        std::lock_guard<std::mutex> lock(value.mutex);
        ++value.rows[statement];
    }
    return result;
}

extern "C" int __wrap_sqlite3_finalize(sqlite3_stmt* statement) {
    auto& value = observer();
    if (statement && value.output) {
        std::lock_guard<std::mutex> lock(value.mutex);
        const auto* sql = sqlite3_sql(statement);
        // Both old ordinal selection and the indexed replacement return this
        // seventh column; batch unsent selection and replay do not.
        const bool dispatch = sql && std::strstr(sql, "s.unsent") &&
            std::strstr(sql, "LIMIT 1") && sqlite3_column_count(statement) == 7;
        std::fprintf(value.output,
            "{\"type\":\"%s\",\"rows\":%llu,\"vm_steps\":%d,\"fullscan_steps\":%d}\n",
            dispatch ? "dispatch" : "sqlite", value.rows[statement],
            sqlite3_stmt_status(statement, SQLITE_STMTSTATUS_VM_STEP, 0),
            sqlite3_stmt_status(statement, SQLITE_STMTSTATUS_FULLSCAN_STEP, 0));
        value.rows.erase(statement);
        std::fflush(value.output);
    }
    return __real_sqlite3_finalize(statement);
}

// Linux x86-64, libstdc++'s C++11 string ABI; the lab builds this observer.
// The entry is the existing eight-argument array metric routine, not the
// producer's staging counter. n is the actual whole-curve pass population.
#define PF_RATIO_SYMBOL \
    "_ZN9pineforge7metrics20compute_equity_statsEPK17pf_equity_point_sldRKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEddld"
extern "C" pf_equity_stats_t real_ratios(const pf_equity_point_t*, std::int64_t,
    double, const std::string&, double, double, std::int64_t, double)
    asm("__real_" PF_RATIO_SYMBOL);
extern "C" pf_equity_stats_t observed_ratios(const pf_equity_point_t*, std::int64_t,
    double, const std::string&, double, double, std::int64_t, double)
    asm("__wrap_" PF_RATIO_SYMBOL);

extern "C" pf_equity_stats_t observed_ratios(const pf_equity_point_t* points,
    std::int64_t count, double capital, const std::string& timezone,
    double first, double last, std::int64_t market, double net) {
    auto& value = observer();
    if (value.output) {
        std::lock_guard<std::mutex> lock(value.mutex);
        std::fprintf(value.output, "{\"type\":\"ratio\",\"points\":%lld}\n",
                     static_cast<long long>(count));
        std::fflush(value.output);
    }
    return real_ratios(points, count, capital, timezone, first, last, market, net);
}
