// Issue #975: follow the same fixed selection from a native host command to
// the actual closed rows. The inline spelling is the reported workaround.
#include "native_current_fixture.hpp"

#include <array>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace r4_test;
using Status = detail::ScratchStatus;
using Handles = std::vector<no::RequestHandle>;

struct Mismatch {};
template<class Actual, class Expected>
void compare(const char* verdict, const char* what, const Actual& actual,
             const Expected& expected, const char* dependent = nullptr) {
    if (actual == expected) return;
    std::cerr << verdict << ' ' << what << ": expected=" << expected
              << " actual=" << actual << '\n';
    if (dependent) std::cerr << "NOT COMPARED " << dependent << ": expected=" << expected
                             << " actual=" << actual << '\n';
    throw Mismatch{};
}

Status member_at(void* context, std::uint64_t i, no::RequestHandle& out) {
    const auto& members = *static_cast<const Handles*>(context);
    if (i >= members.size()) return Status::not_found;
    out = members[i];
    return Status::ok;
}

std::string reason(const std::optional<no::RequestRejectReason>& value) {
    if (!value) return "";
    return *value == no::RequestRejectReason::InvalidOwner ? "/InvalidOwner"
        : "/reason-" + std::to_string(static_cast<int>(*value));
}

std::string result(const no::SubmitResult& r) {
    return std::string(r.status == no::SubmitStatus::Accepted ? "Accepted" : "Rejected")
        + reason(r.reason);
}

std::string result(const no::ReplaceResult& r) {
    const char* name = "InvalidHandle";
    if (r.status == no::ReplaceStatus::Replaced) name = "Replaced";
    else if (r.status == no::ReplaceStatus::ReplaceRejected) name = "ReplaceRejected";
    else if (r.status == no::ReplaceStatus::NotWorking) name = "NotWorking";
    return std::string(name) + reason(r.reason);
}

void exercise(bool external, bool replace_close) {
    Host host;
    const auto config = spec("issue-975");
    const auto setup = host.configure_native(config);
    compare("NOT COMPARED", "configure status", int(setup.status), int(NativeSetupStatus::Applied));
    const Bar warmup{100, 100, 100, 100, 1, T - 60000};
    compare("NOT COMPARED", "stream begin", host.stream_begin(&warmup, 1, "1", "1"), true);
    const std::array<double, 3> qty{1, 3, 2};
    std::array<no::RequestHandle, 3> opened;
    for (std::size_t i = 0; i < opened.size(); ++i) {
        const auto r = host.submit(tx(qty[i]));
        compare("NOT COMPARED", "opening submit", result(r), "Accepted");
        compare("NOT COMPARED", "opening has nonzero handle", r.handle && r.handle->incarnation, true);
        opened[i] = *r.handle;
    }
    compare("NOT COMPARED", "opening tick", host.stream_push_tick({T, 1, 100, 1}), true);
    compare("NOT COMPARED", "opening lots", host.lots().size(), opened.size());
    for (std::size_t i = 0; i < opened.size(); ++i) {
        compare("NOT COMPARED", "live opening", host.lots()[i].entry_incarnation, opened[i].incarnation);
        compare("NOT COMPARED", "opening units", host.lots()[i].qty, qty[i]);
    }
    const auto cycle = host.cycle();
    compare("NOT COMPARED", "positive cycle", cycle > 0, true);
    compare("NOT COMPARED", "closed rows before close", host.rows().size(), 0u);

    no::RequestHandle predecessor;
    if (replace_close) {
        auto resting = flat("predecessor");
        resting.trigger = no::Limit{200};
        resting.owner = no::BindOpening{opened[1], cycle};
        const auto r = host.submit(resting);
        compare("NOT COMPARED", "resting predecessor", result(r), "Accepted");
        compare("NOT COMPARED", "predecessor has nonzero handle", r.handle && r.handle->incarnation, true);
        predecessor = *r.handle;
    }
    Handles members{opened[2], opened[0]};  // reverse the physical FIFO order
    auto close = [&](std::int64_t binding_cycle) {
        auto request = flat("selected");
        if (replace_close) {
            request.intent = no::Reduce{no::ScopeFraction{1, no::ScopeClaim::Gross,
                                                          no::ScopeBasis::AtAcceptance}};
        }
        request.owner = external
            ? no::BindOpenings{{}, binding_cycle,
                  std::make_shared<no::OpeningInputCursor>(members.size(), &members, member_at)}
            : no::BindOpenings{members, binding_cycle};
        return request;
    };
    const char* verdict = external ? "FAIL" : "NOT COMPARED";
    auto rejected = [&](const char* what, std::int64_t binding_cycle) {
        if (replace_close) {
            const auto r = host.replace(predecessor, close(binding_cycle));
            compare(verdict, what, result(r), "ReplaceRejected/InvalidOwner");
            compare(verdict, "refused replacement has successor", r.successor.has_value(), false);
            compare(verdict, "refused replacement recorded", r.event_ordinal > 0, true);
        } else {
            const auto r = host.submit(close(binding_cycle));
            compare(verdict, what, result(r), "Rejected/InvalidOwner");
            compare(verdict, "refused submit has handle", r.handle.has_value(), false);
            compare(verdict, "refused submit recorded", r.event_ordinal > 0, true);
        }
    };
    rejected("wrong cycle", cycle + 1);
    members[0].incarnation += 1000;  // same run and cycle, but no such live lot
    rejected("one missing opening", cycle);
    members[0] = members[1];
    rejected("duplicate opening", cycle);
    members.clear();
    rejected("empty selection", cycle);
    members = {opened[2], opened[0]};

    no::RequestHandle target;
    if (replace_close) {
        const auto r = host.replace(predecessor, close(cycle));
        compare(verdict, "replace live selection", result(r), "Replaced", "match needs replacement");
        compare(verdict, "replacement has nonzero successor", r.successor && r.successor->incarnation, true);
        target = *r.successor;
    } else {
        const auto r = host.submit(close(cycle));
        compare(verdict, "submit live selection", result(r), "Accepted", "match needs accepted close");
        compare(verdict, "close has nonzero handle", r.handle && r.handle->incarnation, true);
        target = *r.handle;
    }
    members = {opened[1]};  // the call-local input no longer describes the close
    compare(verdict, "matching tick", host.stream_push_tick({T + 1, 2, 110, 1}), true);
    compare(verdict, "closed rows", host.rows().size(), 2u);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto row = host.rows()[i];
        compare(verdict, "FIFO opening", row.entry_incarnation, opened[2 * i].incarnation);
        compare(verdict, "closed units", row.qty, qty[2 * i]);
        compare(verdict, "closing price", row.exit_price, 110.0);
    }
    compare(verdict, "remaining lots", host.lots().size(), 1u);
    compare(verdict, "unselected opening", host.lots()[0].entry_incarnation, opened[1].incarnation);
    compare(verdict, "unselected units", host.lots()[0].qty, qty[1]);
    unsigned matches = 0;
    for (const auto& fill : events<no::ExecutionAppliedEvent>(host)) {
        compare(verdict, "fill has definition", bool(fill.definition), true);
        if (fill.handle() == target) {
            ++matches;
            compare(verdict, "units closed by accepted handle", fill.closed_units, 3.0);
        }
    }
    compare(verdict, "fills for accepted handle", matches, 1u);
    compare(verdict, "stream end", host.stream_end(false), true);
    compare(verdict, "run status", int(host.native_state().kind), int(NativeLifecycleKind::Completed));
}
} // namespace

int main() {
    bool ok = true;
    for (bool replace_close : {false, true}) {
        for (bool external : {false, true}) {
            try {
                exercise(external, replace_close);
            } catch (const Mismatch&) {
                ok = false;
                // An invalid inline control cannot support a comparison.
                if (!external) break;
            } catch (const std::exception& e) {
                std::cerr << "NOT COMPARED " << (external ? "external " : "inline ")
                          << (replace_close ? "replace" : "submit")
                          << ": expected=completed run actual=" << e.what() << '\n';
                ok = false;
                if (!external) break;
            }
        }
    }
    if (ok) std::cout << "PASS inline and external submit/replace closed only the two selected openings\n";
    return ok ? 0 : 1;
}
