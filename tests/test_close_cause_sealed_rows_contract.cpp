// B1-11 / E-10: document the existing legacy view boundary after real sealing.
// The header paragraph check is the red row on the base; runtime values stay fixed.
#include "closed_trade_sealed_fixture.hpp"

#include <cstring>
#include <exception>
#include <limits>

namespace {
using namespace sealed_accessors_test;

void documented_contract() {
    const auto header = header_text();
    const auto cause = paragraph(header, "PF_API int strategy_closed_trade_close_cause(");
    check(cause.find("This legacy accessor has no view of a sealed prefix row.")
              != std::string::npos,
          "close-cause doxygen explicitly names the sealed-row view boundary");
    check(cause.find("`-1` when @p s is NULL, or when @p trade_index is out of range")
              == std::string::npos,
          "close-cause doxygen no longer limits -1 to NULL/out-of-range");
    check(cause.find("It returns `-1`") != std::string::npos,
          "close-cause doxygen preserves the legacy -1 result");
    check(cause.find("read failure or other exception caught") != std::string::npos,
          "close-cause doxygen names caught read failures");
    check(cause.find("sink's T records") != std::string::npos,
          "close-cause doxygen directs sealed-row readers to T records");
    for (const auto* declaration : {
            "PF_API uint64_t strategy_closed_trade_entry_incarnation(",
            "PF_API const char* strategy_closed_trade_entry_id("}) {
        const auto doc = paragraph(header, declaration);
        check(doc.find("sealed prefix") != std::string::npos,
              std::string(declaration) + " documents sealed rows");
        check(doc.find("read failure") != std::string::npos,
              std::string(declaration) + " documents caught read failures");
        check(doc.find("sink's T records") != std::string::npos,
              std::string(declaration) + " names the sealed-row record source");
    }
}
} // namespace

int main() {
    using namespace sealed_accessors_test;
    try {
        Directory directory;
        SinkRun run(directory);
        const int base = static_cast<int>(run.host.sealed_base());
        const auto handle = run.host.handle();
        // 24 completed six-bar groups each close two lots; the last two lots
        // remain open at range end. Sealing must preserve both global counts.
        check(run.host.trade_count() == 48, "sealed rows still count as closed trades");
        check(run.host.report_trade_count() == 50, "report count includes sealed and range-end rows");
        absent_views(handle, 0);
        absent_views(handle, base - 1);
        check(run.host.closed_trade_close_cause(0) == -1, "engine sealed row 0 is -1");
        check(run.host.closed_trade_close_cause(base - 1) == -1, "engine sealed base-1 is -1");
        check(strategy_closed_trade_close_cause(handle, base) == PF_CLOSE_CAUSE_SCRIPT,
              "resident base row remains a script close");
        check(strategy_closed_trade_entry_incarnation(handle, base) != 0,
              "resident incarnation is available");
        const char* id = strategy_closed_trade_entry_id(handle, base);
        check(id != nullptr, "resident entry id is available");
        if (id)
            check(std::strcmp(id, base % 2 == 0 ? "entry-a" : "entry-b") == 0,
                  "resident global index keeps the scripted entry id");
        check(strategy_closed_trade_exit_id(handle, base) != nullptr,
              "resident exit id is available");
        check(strategy_closed_trade_exit_comment(handle, base) != nullptr,
              "resident empty exit comment is a string, not NULL");
        check(strategy_closed_trade_close_cause(handle, run.host.trade_count())
                  == PF_CLOSE_CAUSE_RANGE_END,
              "range-end row remains readable");
        absent_views(nullptr, 0);
        absent_views(handle, -1);
        absent_views(handle, run.host.report_trade_count());
        absent_views(handle, std::numeric_limits<int>::max());
        check(run.host.last_error().empty(), "sentinel queries leave last_error unchanged");
        documented_contract();
    } catch (const std::exception& error) {
        check(false, error.what());
    }
    return result("test_close_cause_sealed_rows_contract");
}
