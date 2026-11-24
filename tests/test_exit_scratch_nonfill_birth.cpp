#include <pineforge/source/pine_adapter.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
namespace sd = source::detail;
using Bytes = std::vector<std::byte>;

struct Host final : NativeStrategyHost {
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

struct NotCompared : std::runtime_error { using std::runtime_error::runtime_error; };
struct WrongVerdict : std::runtime_error { using std::runtime_error::runtime_error; };

template<class Actual, class Expected>
void need(const char* reason, Actual actual, Expected expected) {
    if (actual != expected)
        throw NotCompared(std::string(reason) + ": expected=" + std::to_string(expected)
                          + " actual=" + std::to_string(actual));
}

void ok(const char* reason, sd::ScratchStatus status) {
    need(reason, static_cast<unsigned>(status), static_cast<unsigned>(sd::ScratchStatus::ok));
}

std::uint64_t bits(double price) {
    std::uint64_t word;
    static_assert(sizeof(word) == sizeof(price));
    std::memcpy(&word, &price, sizeof(word));
    return word;
}

// These are the eleven LE64 words in the birth, not an offset into PlacementSnapshot.
using Words = std::array<std::uint64_t, 11>;
Words words(const OrderBirth& b) {
    const auto& c = b.cursor();
    return {static_cast<unsigned>(b.cause()), static_cast<std::uint64_t>(b.bar()),
            static_cast<std::uint64_t>(b.timestamp()), static_cast<unsigned>(c.domain()),
            static_cast<unsigned>(c.position()), static_cast<std::uint64_t>(c.index()),
            static_cast<std::uint64_t>(c.count()), bits(b.cursor_price()),
            b.first_fill(), b.last_fill(), b.evaluation_ordinal()};
}

Bytes read_field(const sd::CommandScratchStore& store, sd::ScratchHandle handle,
                 sd::ScratchField field) {
    sd::ScratchRecordInfo info{};
    ok("point", store.point(handle, info));
    need("record handle", info.handle, handle);
    const auto size = field == sd::ScratchField::key ? info.key_bytes : info.value_bytes;
    if (!size) throw NotCompared("record field bytes: expected=nonempty actual=0");
    Bytes bytes(static_cast<std::size_t>(size));
    ok("read field", store.read(handle, field, 0, {bytes.data(), bytes.size()}));
    return bytes;
}

void check() {
    Host host;
    source::PineExecutionAdapter adapter(host);
    source::StagedConfiguration staged;
    staged.syminfo.mintick = 0.25;
    staged.syminfo.pointvalue = 1.0;
    adapter.project({}, staged, NativeBeginArgs{});
    sd::ExitScratchRows<std::uint64_t> rows(adapter);
    auto& context = rows.context();
    auto& versions = sd::exit_scratch_versions(context);
    auto& store = context.store;

    enum { direct, chart, fill, unattributed };
    const std::array<OrderBirth, 4> births{
        OrderBirth::direct_command(3, 1000), OrderBirth::chart_evaluation(3, 1000),
        OrderBirth::fill_evaluation(3, 1000,
            BirthCursor::point(BirthCursorDomain::HistoricalPath, 0, 4), 100.0, 7, 7, 1),
        OrderBirth{}};
    std::array<sd::ScratchHandle, 4> ids{};
    std::array<Bytes, 4> values;
    for (std::size_t i = 0; i < births.size(); ++i) {
        source::PlacementSnapshot placed{};
        placed.birth = births[i];
        ids[i] = versions.placement(rows.scope(), placed);
        if (!ids[i]) throw NotCompared("placement handle: expected=nonzero actual=0");
        for (std::size_t j = 0; j < i; ++j) {
            if (ids[i] == ids[j])
                throw NotCompared("placement handle: expected=different from " + std::to_string(ids[j])
                                  + " actual=" + std::to_string(ids[i]));
        }
        values[i] = read_field(store, ids[i], sd::ScratchField::value);
        need("placement length", values[i].size(), values[direct].size());
        const auto actual = words(versions.placement(ids[i]).birth);
        const auto expected = words(births[i]);
        for (std::size_t w = 0; w < expected.size(); ++w)
            need("unmodified birth word", actual[w], expected[w]);
    }

    std::size_t k = 0, differences = 0;
    for (std::size_t i = 0; i < values[direct].size(); ++i) {
        if (values[direct][i] == values[chart][i]) continue;
        k = i;
        ++differences;
    }
    need("differing direct/chart bytes", differences, std::size_t{1});
    constexpr auto birth_bytes = 8 * Words{}.size();
    if (values[direct].size() - k < birth_bytes)
        throw NotCompared("birth bytes: expected=at least " + std::to_string(birth_bytes)
                          + " actual=" + std::to_string(values[direct].size() - k));
    need("DirectCommand cause byte", std::to_integer<unsigned>(values[direct][k]), 1U);
    need("ChartEvaluation cause byte", std::to_integer<unsigned>(values[chart][k]), 2U);
    need("FillEvaluation cause byte", std::to_integer<unsigned>(values[fill][k]), 3U);
    for (std::size_t i = 0; i < births.size(); ++i) {
        const auto expected = words(births[i]);
        for (std::size_t w = 0; w < expected.size(); ++w) {
            std::uint64_t actual = 0;
            for (unsigned n = 0; n < 8; ++n)
                actual |= std::to_integer<std::uint64_t>(values[i][k + 8 * w + n]) << (8 * n);
            need("stored birth word", actual, expected[w]);
        }
    }

    const auto key = read_field(store, ids[fill], sd::ScratchField::key);
    auto reject = [&](const char* row, const Bytes& value) {
        sd::ScratchHandle written = 0;
        sd::exit_scratch_edit(context, [&] {
            return store.put(sd::ScratchSpanSource({key.data(), key.size()}),
                             sd::ScratchSpanSource({value.data(), value.size()}), written);
        });
        need("replacement handle", written, ids[fill]);
        const auto saved = read_field(store, ids[fill], sd::ScratchField::value);
        need("committed length", saved.size(), value.size());
        if (saved != value) {
            const auto mismatch = std::mismatch(saved.begin(), saved.end(), value.begin());
            need("committed byte", std::to_integer<unsigned>(*mismatch.first),
                 std::to_integer<unsigned>(*mismatch.second));
        }
        try {
            (void)versions.placement(ids[fill]);
        } catch (const sd::ExitScratchError& error) {
            if (error.status() == sd::ScratchStatus::corrupt) return;
            throw WrongVerdict(std::string(row) + ": expected=corrupt actual=ScratchStatus("
                               + std::to_string(static_cast<unsigned>(error.status())) + ")");
        } catch (const std::exception& error) {
            throw WrongVerdict(std::string(row) + ": expected=corrupt actual=" + error.what());
        }
        throw WrongVerdict(std::string(row) + ": expected=corrupt actual=accepted");
    };

    auto changed = values[fill];
    changed[k] = values[direct][k]; // #940: the only changed byte is the cause.
    reject("fill relabeled DirectCommand", changed);
    changed[k] = values[chart][k];
    reject("fill relabeled ChartEvaluation", changed);

    // Leave all other words canonical so no field can hide a missing check on another.
    auto copy_words = [&](const char* row, int original, int donor, std::size_t w, int count) {
        auto value = values[original];
        const auto offset = k + 8 * w;
        std::copy_n(values[donor].begin() + offset, 8 * count, value.begin() + offset);
        need("mutation differs from original", value != values[original], true);
        reject(row, value);
    };
    copy_words("point cursor only", direct, fill, 3, 4);
    copy_words("point position on an empty cursor", chart, fill, 4, 1);
    copy_words("finite price only", direct, fill, 7, 1);
    copy_words("first fill only", direct, fill, 8, 1);
    copy_words("last fill only", direct, fill, 9, 1);
    copy_words("evaluation ordinal only", direct, fill, 10, 1);
    copy_words("unattributed bar only", unattributed, direct, 1, 1);
    copy_words("unattributed timestamp only", unattributed, direct, 2, 1);
    changed = values[unattributed];
    changed[k + 7 * 8] ^= std::byte{1}; // Different quiet NaN with the same numeric classification.
    reject("alternate NaN price only", changed);
}
} // namespace

int main() {
    try {
        check();
    } catch (const NotCompared& error) {
        std::cerr << "NOT COMPARED " << error.what() << '\n';
        return 1;
    } catch (const sd::ExitScratchError& error) {
        std::cerr << "NOT COMPARED scratch setup: expected=ok actual=ScratchStatus("
                  << static_cast<unsigned>(error.status()) << ")\n";
        return 1;
    } catch (const WrongVerdict& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "NOT COMPARED fixture: expected=ready actual=" << error.what() << '\n';
        return 1;
    }
    std::cout << "PASS four canonical births round-tripped; eleven malformed non-fill births rejected\n";
}
