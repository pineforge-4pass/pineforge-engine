#include <pineforge/source/pine_adapter.hpp>

#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

namespace {
namespace no = pineforge::native_order;
using Adapter = pineforge::source::PineExecutionAdapter;
using Status = pineforge::detail::ScratchStatus;
using Handles = std::vector<no::RequestHandle>;

// The stock adapter queues only inline bindings. Use the same private queue
// access as test_pending_observer_walk; hash it through the production method.
struct Brackets { friend auto member(Brackets); };
template<class Tag, auto Member> struct Access {
    friend auto member(Tag) { return Member; }
};
template struct Access<Brackets, &Adapter::pending_bracket_legs_>;

class Members final : public no::OpeningReadback {
public:
    explicit Members(Handles rows, Status last = Status::ok)
        : rows_(std::move(rows)), last_(last) {}
    std::uint64_t count() const noexcept override { return rows_.size(); }
    Status opening_at(std::uint64_t i, no::RequestHandle& out) const noexcept override {
        if (i >= count()) return Status::not_found;
        if (i + 1 == count() && last_ != Status::ok) return last_;
        out = rows_[i];
        return Status::ok;
    }
private:
    Handles rows_;
    Status last_;
};

bool ready(const no::OpeningReadback& reader, const Handles& expected) {
    if (reader.count() != expected.size()) {
        std::cerr << "NOT COMPARED: external count: expected=" << expected.size()
                  << " actual=" << reader.count() << '\n';
        return false;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        no::RequestHandle got;
        const auto status = reader.opening_at(i, got);
        const auto& want = expected[i];
        if (status != Status::ok || got != want) {
            std::cerr << "NOT COMPARED: external opening " << i << ": expected="
                      << want.run.session_key << '/' << want.run.run_number << '/' << want.incarnation
                      << " (ok) actual=" << got.run.session_key << '/' << got.run.run_number
                      << '/' << got.incarnation << " (status " << static_cast<int>(status) << ")\n";
            return false;
        }
    }
    return true;
}

bool compare(const char* row, std::uint64_t left, std::uint64_t right, bool expect_equal) {
    if ((left == right) == expect_equal) return true;
    std::cerr << "FAIL: " << row << ": expected " << (expect_equal ? "equal" : "different")
              << " hashes, got " << (left == right ? "equal" : "different")
              << "\n  left=" << left << " right=" << right << '\n';
    return false;
}

int run() {
    const no::RunIdentity run{"issue-945", 1};
    const Handles a{{run, 11}, {run, 22}}, b{{run, 11}, {run, 23}};
    const auto ext_a = std::make_shared<Members>(a), ext_b = std::make_shared<Members>(b);
    if (!ready(*ext_a, a) || !ready(*ext_b, b)) return 1;

    Adapter adapter;
    auto& leg = (adapter.*member(Brackets{})).emplace_back();
    leg.request = {no::Reduce{no::ExplicitUnits{1.0}}, "exit", ""};
    leg.request.trigger = no::Limit{105.0};
    const auto hash = [&](no::BindOpenings binding) {
        leg.request.owner = std::move(binding);
        pineforge::BrokerStateHashSink sink;
        adapter.hash_state(sink);
        return sink.h;
    };
    const auto empty = hash({{}, 7, {}});
    const auto inline_a = hash({a, 7, {}}), inline_b = hash({b, 7, {}});
    if (inline_a == inline_b || inline_a == empty || inline_b == empty) {
        std::cerr << "NOT COMPARED: expected three distinct inline hashes: [11,22]="
                  << inline_a << " [11,23]=" << inline_b << " []=" << empty << '\n';
        return 1;
    }

    const auto external_a = hash({{}, 7, ext_a}), external_b = hash({{}, 7, ext_b});
    bool ok = compare("external [11,22] vs [11,23]", external_a, external_b, false);
    ok &= compare("external [11,22] vs empty", external_a, empty, false);
    ok &= compare("[11,22] external vs inline", external_a, inline_a, true);
    ok &= compare("[11,23] external vs inline", external_b, inline_b, true);
    ok &= compare("same openings, different cycle", external_a, hash({{}, 8, ext_a}), false);

    // The first member is readable. A failure reading the second must not
    // produce a hash of the prefix or a default handle.
    try {
        const auto digest = hash({{}, 7, std::make_shared<Members>(a, Status::io_error)});
        std::cerr << "FAIL: unreadable second opening: expected=scratch_io_failed actual=hash "
                  << digest << '\n';
        ok = false;
    } catch (const pineforge::RunFailureInfo& error) {
        if (error.run_failure_code() != pineforge::RunFailureCode::scratch_io_failed) {
            std::cerr << "FAIL: unreadable second opening: expected=scratch_io_failed actual="
                      << pineforge::run_failure_code_name(error.run_failure_code()) << '\n';
            ok = false;
        }
    }
    if (ok) std::cout << "PASS: queued BindOpenings hashes its opening membership\n";
    return ok ? 0 : 1;
}
} // namespace

int main() {
    try {
        return run();
    } catch (const std::exception& error) {
        std::cerr << "NOT COMPARED: expected a completed comparison, actual=" << error.what() << '\n';
        return 1;
    }
}
