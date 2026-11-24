#include <pineforge/source/pine_adapter.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {
namespace no = pineforge::native_order;
namespace source = pineforge::source;
namespace sd = source::detail;
using Status = pineforge::detail::ScratchStatus;
using Handles = std::array<no::RequestHandle, 2>;

struct Host final : pineforge::NativeStrategyHost {
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {}
};

template<class Expected, class Actual>
bool check(const char* verdict, const char* row, const char* field,
           const Expected& expected, const Actual& actual) {
    if (expected == actual) return true;
    std::cerr << verdict << ' ' << row << ' ' << field
              << ": expected=" << expected << " actual=" << actual << '\n';
    return false;
}

bool membership(const char* verdict, const char* row, const no::Owner& owner,
                const Handles& expected, bool external_only = false) {
    const auto* bind = std::get_if<no::BindOpenings>(&owner);
    if (!check(verdict, row, "BindOpenings owner", true, bind != nullptr)) return false;
    const auto* external = bind->external_openings.get();
    if ((external && !check(verdict, row, "inline count with external owner",
                            std::size_t{0}, bind->openings.size()))
        || !check(verdict, row, "cycle", 1, bind->cycle)
        || !check(verdict, row, "opening count", expected.size(),
                  no::opening_count(bind->openings, external))) return false;
    if (external_only && !check(verdict, row, "external input present", true, external != nullptr))
        return false;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        no::RequestHandle got;
        const auto status = no::opening_at(bind->openings, external, i, got);
        if (!check(verdict, row, "opening read status", unsigned(Status::ok), unsigned(status))
            || !check(verdict, row, "opening session", expected[i].run.session_key, got.run.session_key)
            || !check(verdict, row, "opening run", expected[i].run.run_number, got.run.run_number)
            || !check(verdict, row, "opening incarnation", expected[i].incarnation, got.incarnation))
            return false;
    }
    return true;
}

bool round_trip(sd::ExitScratchVersions& versions, std::uint64_t scope, const Handles& expected,
                bool pending, std::string_view mode) {
    const char* row = pending ? "pending" : "request";
    std::uint64_t version;
    {
        Handles input = expected;
        if (mode == "--wrong-input") ++input.back().incarnation;
        no::OpeningInputCursor cursor(mode == "--empty-input" ? 0 : input.size(), &input,
            [](void* ctx, std::uint64_t i, no::RequestHandle& out) {
                const auto& handles = *static_cast<const Handles*>(ctx);
                if (i >= handles.size()) return Status::not_found;
                out = handles[i];
                return Status::ok;
            });
        if (!check("NOT COMPARED", row, "input count", expected.size(), cursor.count())) return false;
        std::shared_ptr<const no::OpeningReadback> external;
        const auto status = no::freeze_openings(expected.front().run, cursor, external);
        if (!check("NOT COMPARED", row, "freeze status", unsigned(Status::ok), unsigned(status)))
            return false;
        no::Request request{no::Reduce{no::ExplicitUnits{1.0}}, row, ""};
        request.owner = no::BindOpenings{{}, 1, std::move(external)};
        if (!membership("NOT COMPARED", row, request.owner, expected, true)) return false;
        version = pending ? versions.pending(scope, request, source::PlacementSnapshot{}, "943-pending")
                          : versions.request(scope, request);
    } // Drop the caller's request and frozen file before either read.
    if (!check("FAIL", row, "nonzero version", true, version != 0)) return false;
    no::Request back;
    if (pending) {
        source::PlacementSnapshot placement;
        std::string key;
        std::as_const(versions).pending(version, back, placement, key);
        if (!check("FAIL", row, "replacement key", "943-pending", key)) return false;
    } else {
        back = versions.request(version);
    }
    const auto* reduce = std::get_if<no::Reduce>(&back.intent);
    const auto* units = reduce ? std::get_if<no::ExplicitUnits>(&reduce->size) : nullptr;
    return check("FAIL", row, "request label", row, back.label)
        && check("FAIL", row, "explicit reduce", true, units != nullptr)
        && check("FAIL", row, "units", 1.0, units->units)
        && membership("FAIL", row, back.owner, expected);
}

bool compare(std::string_view mode) {
    Host host;
    source::PineExecutionAdapter adapter(host);
    source::StagedConfiguration staged;
    staged.syminfo.mintick = 0.25;
    staged.syminfo.pointvalue = 1.0;
    const auto spec = adapter.project(source::PineStrategyConfig{}, staged, pineforge::NativeBeginArgs{});
    sd::ExitScratchRows<std::uint64_t> rows(adapter);
    auto& versions = sd::exit_scratch_versions(rows.context());
    // Different handles and labels make the preceding version a wrong answer for pending.
    const bool request = round_trip(versions, rows.scope(), {{{spec.identity, 17}, {spec.identity, 42}}},
                                    false, mode);
    const bool pending = round_trip(versions, rows.scope(), {{{spec.identity, 23}, {spec.identity, 55}}},
                                    true, mode);
    return request && pending;
}
} // namespace

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "";
    if (argc > 2 || (argc == 2 && mode != "--empty-input" && mode != "--wrong-input")) {
        std::cerr << "NOT COMPARED arguments: expected=no argument, --empty-input or --wrong-input"
                  << " actual=" << mode << " argument_count=" << argc - 1 << '\n';
        return 1;
    }
    try {
        if (!compare(mode)) return 1;
    } catch (const std::exception& error) {
        std::cerr << "NOT COMPARED exit scratch round trip: expected=readable versions actual="
                  << error.what() << '\n';
        return 1;
    }
    std::cout << "PASS request and pending restored both external openings and cycle 1\n";
}
