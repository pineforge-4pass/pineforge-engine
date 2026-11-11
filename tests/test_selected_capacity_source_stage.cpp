#include "../src/selected_capacity_source_stage.hpp"
#include "selected_capacity_fixture.hpp"

#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

namespace {
namespace sc = pineforge::detail::selected_capacity;
const std::string kHeader = "timestamp,open,high,low,close,volume\n";
const std::vector<std::string> kRecords{
    "-60000,11,13,9,12,101\n", "0,21,23,19,22,102\n",
    "60000,31,33,29,32,103\n", "900000,41,43,39,42,104\n"};
const std::string kSource = [] {
    std::string text = kHeader;
    for (const auto& record : kRecords) text += record;
    return text;
}();
const std::string kPrimary = kHeader + "0,51,53,49,52,201\n60000,61,63,59,62,202\n";

struct Source {
    const std::string& text;
    std::size_t position = 0;
    std::string forwarded;
    std::vector<std::string> writes;
    static std::ptrdiff_t read(void* ctx, char* out, std::size_t cap) {
        auto& self = *static_cast<Source*>(ctx);
        const auto count = std::min(cap, self.text.size() - self.position);
        std::memcpy(out, self.text.data() + self.position, count);
        self.position += count;
        return static_cast<std::ptrdiff_t>(count);
    }
    static bool forward(void* ctx, const char* data, std::size_t count) {
        auto& self = *static_cast<Source*>(ctx);
        self.writes.emplace_back(data, count);
        self.forwarded.append(data, count);
        return true;
    }
};

struct Failure {
    pineforge::RunFailureCode code = pineforge::RunFailureCode::none;
    std::string args;
};

struct SelectedRun {
    Failure failure;
    sc::DriverResult driver{};
    bool completed = false;
    std::size_t counted = 0;
    std::streampos primary_position{};
};

SelectedRun native_selected(const sc::Limits& limits) {
    std::istringstream source(kSource), primary(kPrimary);
    const sc::StreamInput inputs[] = {
        {sc::Role::Source, source}, {sc::Role::Primary, primary}};
    SelectedRun selected;
    try {
        selected.driver = sc::materialize_selected_capacity(
            inputs, 2, pineforge::SelectedPlanRequest{}, limits,
            [](void* ctx, const pineforge::Bar&) { ++*static_cast<std::size_t*>(ctx); },
            &selected.counted);
        assert(selected.driver.status == sc::DriverStatus::Completed);
        selected.completed = true;
    } catch (const pineforge::coded<std::runtime_error>& error) {
        assert(error.run_failure().args);
        selected.failure = {error.run_failure_code(), *error.run_failure().args};
    }
    selected.primary_position = primary.tellg();
    return selected;
}

void early_stop_and_selected_parity() {
    std::vector<sc::Limits> cases;
    for (const auto limit : {std::uint64_t{4}, std::uint64_t{3}, std::uint64_t{2}, std::uint64_t{0}}) {
        sc::Limits limits;
        limits.source_rows = {true, limit};
        cases.push_back(limits);
    }
    for (const auto limit : {std::uint64_t{kSource.size()}, std::uint64_t{kSource.size() - 1},
                            std::uint64_t{kHeader.size()}, std::uint64_t{0}}) {
        sc::Limits limits;
        limits.bytes = {true, limit};
        cases.push_back(limits);
    }
    sc::Limits simultaneous;
    simultaneous.source_rows = {true, kRecords.size() - 1};
    simultaneous.bytes = {true, kSource.size() - kRecords.back().size()};
    cases.push_back(simultaneous);
    for (const auto& limits : cases) {
        Source source{kSource};
        const sc::SourceStageInput input{&Source::read, &source, "source"};
        Failure actual;
        const auto selected = native_selected(limits);
        std::uint64_t expected_rows = 0, expected_bytes = 0;
        std::string expected_forward;
        Failure expected;
        std::vector<std::string> pieces{kHeader};
        pieces.insert(pieces.end(), kRecords.begin(), kRecords.end());
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            const auto requested = pieces[i].size();
            if (limits.bytes.present && expected_bytes + requested > limits.bytes.value) {
                expected.code = pineforge::RunFailureCode::selected_bytes_limit_exceeded;
                expected.args = "{\"limit\":\"" + std::to_string(limits.bytes.value)
                    + "\",\"requested\":\"" + std::to_string(requested)
                    + "\",\"used\":\"" + std::to_string(expected_bytes) + "\"}";
                break;
            }
            if (i && limits.source_rows.present && expected_rows == limits.source_rows.value) {
                expected.code = pineforge::RunFailureCode::selected_source_rows_limit_exceeded;
                expected.args = "{\"limit\":\"" + std::to_string(limits.source_rows.value)
                    + "\",\"requested\":\"1\",\"used\":\"" + std::to_string(expected_rows) + "\"}";
                break;
            }
            expected_forward += pieces[i];
            if (limits.bytes.present) expected_bytes += requested;
            if (i && limits.source_rows.present) ++expected_rows;
        }
        sc::SourceStageResult source_result;
        try {
            source_result = sc::admit_source_stage(
                &input, 1, limits, {4096, 65536}, &Source::forward, &source);
            const auto& result = source_result;
            assert(expected.code == pineforge::RunFailureCode::none);
            assert(result.status == sc::DriverStatus::Completed);
            assert(result.reason == sc::SourceStageReason::None);
            assert(result.state.source_rows == expected_rows);
            assert(result.state.bytes == expected_bytes);
            assert(result.coverage.source_rows == kRecords.size());
            assert(result.coverage.source_bytes == kSource.size());
            assert(result.forwarded_bytes == expected_forward.size());
        } catch (const pineforge::coded<std::runtime_error>& error) {
            assert(error.run_failure().args);
            actual = {error.run_failure_code(), *error.run_failure().args};
        }
        assert(actual.code == expected.code && actual.args == expected.args);
        if (actual.code != pineforge::RunFailureCode::none) {
            assert(actual.code == selected.failure.code && actual.args == selected.failure.args);
            assert(selected.primary_position == std::streampos(0));
        } else if (limits.source_rows.present) {
            assert(selected.completed && selected.failure.code == pineforge::RunFailureCode::none);
            assert(source_result.state.source_rows == selected.driver.state.source_rows);
            assert(source_result.coverage.source_rows == selected.driver.coverage.source_rows);
            assert(source_result.coverage.source_bytes == selected.driver.coverage.source_bytes);
        } else {
            assert(limits.bytes.present && limits.bytes.value == kSource.size());
            assert(source_result.status == sc::DriverStatus::Completed);
            assert(source_result.state.bytes == kSource.size());
            assert(source_result.forwarded_bytes == kSource.size());
            assert(!selected.completed);
            assert(selected.failure.code == pineforge::RunFailureCode::selected_bytes_limit_exceeded);
            const auto header_end = kPrimary.find('\n');
            assert(header_end != std::string::npos);
            const auto primary_header_bytes = kPrimary.substr(0, header_end + 1).size();
            const auto args = "{\"limit\":\"" + std::to_string(limits.bytes.value)
                + "\",\"requested\":\"" + std::to_string(primary_header_bytes)
                + "\",\"used\":\"" + std::to_string(source_result.state.bytes) + "\"}";
            assert(selected.failure.args == args);
            assert(selected.counted == 0);
            assert(selected.primary_position == std::streampos(primary_header_bytes));
        }
        assert(source.forwarded == expected_forward);
        assert(!source.writes.empty() || expected_forward.empty());
        for (std::size_t i = 0; i < source.writes.size(); ++i)
            assert(source.writes[i] == pieces[i]);
    }
}

void source_only_and_inapplicable() {
    Source source{kHeader};
    const sc::SourceStageInput input{&Source::read, &source, "empty"};
    sc::Limits limits;
    limits.source_rows = {true, 0};
    const auto header = sc::admit_source_stage(
        &input, 1, limits, {4096, 65536}, &Source::forward, &source);
    assert(header.status == sc::DriverStatus::Completed);
    assert(source.forwarded == kHeader && header.coverage.source_rows == 0);
    assert(header.coverage.primary_rows == 0 && header.forwarded_bytes == kHeader.size());
    source.position = 0;
    source.forwarded.clear();
    limits = {};
    limits.primary_rows = {true, 0};
    const auto inactive = sc::admit_source_stage(
        &input, 1, limits, {4096, 65536}, &Source::forward, &source);
    assert(inactive.status == sc::DriverStatus::NotAdmitted);
    assert(inactive.reason == sc::SourceStageReason::NoStageFields);
    assert(source.position == 0 && source.forwarded.empty());
}
}  // namespace

int main() {
    early_stop_and_selected_parity();
    source_only_and_inapplicable();
    std::cout << "source stage scenarios: 2 complete\n";
}
