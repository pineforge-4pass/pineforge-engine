// Edit-only witness. Counts/extent are measured, never implementation constants.
#include <pineforge/detail/working_members.hpp>
#include <pineforge/detail/native_working_codec.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace pineforge::detail;
namespace no = pineforge::native_order;
constexpr std::size_t page_size = 512;

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

ScratchStatus lexical(void*, const ScratchByteSource& a, const ScratchByteSource& b,
                      int& order) noexcept {
    std::array<std::byte, 32> left{}, right{};
    const auto common = std::min(a.size(), b.size());
    for (std::uint64_t at = 0; at < common;) {
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(left.size(), common - at));
        auto status = a.read(at, {left.data(), n});
        if (status != ScratchStatus::ok) return status;
        status = b.read(at, {right.data(), n});
        if (status != ScratchStatus::ok) return status;
        const int compared = std::memcmp(left.data(), right.data(), n);
        if (compared) { order = compared < 0 ? -1 : 1; return ScratchStatus::ok; }
        at += n;
    }
    order = a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
    return ScratchStatus::ok;
}

struct Rig {
    std::vector<std::byte> storage = std::vector<std::byte>(128 * 1024 * 1024);
    FixedMemoryScratchBacking backing{{storage.data(), storage.size()}, page_size};
    ScratchStore store{{2, page_size, 1, 1, {0x4f315334, nullptr, &lexical},
                        ScratchOpenMode::create_empty}, &backing};
    Rig() { working_store::check(store.status()); }
};

struct Row {
    std::uint64_t rank = 0;
    std::string text;
    template<class Ar> friend void scratch_fields(Ar& ar, Row& row) { ar(row.rank, row.text); }
};
Row row(std::uint64_t rank) { return {rank, std::string(320, static_cast<char>('a' + rank))}; }

struct Sample { std::uint64_t bytes = 0, records = 0; };
Sample sample(const ScratchStore& store) { return {store.stats().backing_bytes, store.record_count()}; }
void flat(Sample early, Sample late, const char* label) {
    std::printf("%s N=64,512 bytes=%llu,%llu records=%llu,%llu growth_bytes=%llu\n",
                label, static_cast<unsigned long long>(early.bytes),
                static_cast<unsigned long long>(late.bytes),
                static_cast<unsigned long long>(early.records),
                static_cast<unsigned long long>(late.records),
                static_cast<unsigned long long>(late.bytes >= early.bytes ? late.bytes - early.bytes : 0));
    require(early.bytes != 0 && late.bytes <= early.bytes + page_size,
            "512 cycles use at most one page more backing than 64 cycles");
    require(late.records == early.records, "no payload or dead owner count accumulates");
}

void rewrites_erase_clear() {
    Rig rig;
    WorkingSequence<Row> rows;
    rows.bind(rig.store, 62, 1);
    Sample early;
    for (unsigned n = 1; n <= 512; ++n) {
        rows.push_back(row(3)); rows.push_back(row(1)); rows.push_back(row(2));
        rows.set(0, row(4));
        rows[1].text.front() = 'z';
        const auto writes = rig.store.stats().page_writes;
        require(rows.observe_at(1).text.front() == 'z'
                    && rig.store.stats().page_writes == writes,
                "a read observes a dirty frame without publishing it");
        rows.flush();
        rows.insert(rows.begin() + 1, row(0));
        rows.stable_sort([](const Row& a, const Row& b) { return a.rank < b.rank; });
        const auto& read = rows;
        for (std::size_t i = 0; i < 4; ++i) {
            const std::array<std::uint64_t, 4> expected{{0, 1, 2, 4}};
            require(read[i].rank == expected[i], "sort/shift preserve independent input order");
            const std::string expected_text = i == 1 ? "z" + std::string(319, 'b') : row(expected[i]).text;
            require(read[i].text == expected_text, "every shifted payload keeps its value");
        }
        rows.erase(rows.begin() + 1, rows.begin() + 3);
        require(rows.size() == 2 && read[0].rank == 0 && read[1].rank == 4,
                "erase preserves the surviving values");
        rows.clear();
        require(rows.empty(), "each cycle ends with the same live population");
        if (n == 64) early = sample(rig.store);
    }
    require(rig.store.stats().page_writes != 0, "the test exercised the selected backing");
    flat(early, sample(rig.store), "rewrite/shift/sort/erase/clear");
}

class Openings final : public no::OpeningReadback {
public:
    Openings() {
        for (std::size_t i = 0; i < rows_.size(); ++i)
            rows_[i] = {{"retained-opening", 7}, 100 + i};
    }
    std::uint64_t count() const noexcept override { return rows_.size(); }
    ScratchStatus opening_at(std::uint64_t index, no::RequestHandle& out) const noexcept override {
        if (index >= count()) return ScratchStatus::not_found;
        out = rows_[static_cast<std::size_t>(index)];
        return ScratchStatus::ok;
    }
private:
    std::array<no::RequestHandle, 3> rows_{};
};
no::Request request(const char* label) {
    no::Request value{no::Reduce{no::ExplicitUnits{1.0}}, label, ""};
    value.owner = no::BindOpenings{{}, 1, std::make_shared<Openings>()};
    return value;
}
void check_openings(const no::Request& value) {
    const auto& owner = std::get<no::BindOpenings>(value.owner);
    require(owner.external_openings && owner.external_openings->count() == 3, "decoded lazy reader is present");
    for (std::uint64_t i = 0; i < 3; ++i) {
        no::RequestHandle actual;
        working_store::check(owner.external_openings->opening_at(i, actual));
        require(actual == no::RequestHandle{{"retained-opening", 7}, 100 + i},
                "a live reader preserves every full opening handle after replacement");
    }
}

void live_reader_retention() {
    Rig rig;
    WorkingSequence<no::Request> rows;
    rows.bind(rig.store, 62, 2);
    rows.push_back(request("warm")); rows.clear();
    const auto baseline = rig.store.record_count();
    unsigned retained_readers = 0;
    {
        rows.push_back(request("old"));
        const auto held = rows.value_at(0);
        const auto copy = held;
        rows.set(0, request("replacement"));
        rows.insert(rows.begin(), request("first"));
        rows.stable_sort([](const no::Request& a, const no::Request& b) { return a.label > b.label; });
        rows.erase(rows.begin());
        rows.clear();
        require(rig.store.record_count() > baseline, "the live reader's old payload is retained");
        check_openings(held); ++retained_readers;
        check_openings(copy); ++retained_readers;
    }
    rows.clear(); // explicit write boundary after the last old reader releases
    require(rig.store.record_count() == baseline, "released payload versions are reclaimed");
    rows.push_back(request("committed"));
    {
        const auto held = rows.value_at(0);
        working_store::check(rig.store.begin());
        rows.set(0, request("aborted"));
        working_store::check(rig.store.rollback());
        rows.bind(rig.store, 62, 2);
        check_openings(held);
    }
    rows.flush();
    require(rows.value_at(0).label == "committed", "reclamation cannot erase a rollback-restored root");
    check_openings(rows.value_at(0));
    rows.clear();
    require(rig.store.record_count() == baseline, "rollback exercise also drains to the baseline");
    std::printf("retained_readers=%u released_extra_records=%llu rollback_read=1\n",
                retained_readers, static_cast<unsigned long long>(rig.store.record_count() - baseline));
}

void drained_owners() {
    Rig rig;
    WorkingSequence<std::uint64_t> member;
    member.bind(rig.store, 62, 6);
    Sample early;
    for (unsigned n = 1; n <= 512; ++n) {
        member.push_back(n);
        {
            WorkingSequence<std::uint64_t> queued;
            queued.swap(member);
            auto storage = queued.recycle_on_exit(member);
            require(queued.value_at(0) == n && member.empty(), "swap drains exactly this bar's input");
            if (n % 2 == 0) member.push_back(1000 + n);
        }
        if (n % 2 == 0)
            require(member.size() == 1 && member.value_at(0) == 1000 + n,
                    "requeued commands survive old-owner cleanup");
        member.clear();
        if (n == 64) early = sample(rig.store);
    }
    flat(early, sample(rig.store), "per-bar swap/requeue");
}

void const_pending_view() {
    namespace ps = pineforge::source;
    struct Host final : ps::PineStrategyHost {
        void on_source_bar(const pineforge::Bar&) override {}
        using ps::PineStrategyHost::source_pending_view;
        ps::PineExecutionAdapter& adapter() { return adapter_; }
    };
    Rig rig;
    Host host;
    host.adapter().bind_scratch_store(rig.store);
    for (unsigned i = 0; i < 2; ++i) {
        no::Request value{no::Transact{2.0 + i}, "entry-" + std::to_string(i), ""};
        ps::PlacementSnapshot placement;
        placement.source_id = value.label;
        placement.requested_qty = 2.0 + i;
        host.adapter().fixture_queue_state(1, &value, &placement, {}, {});
    }
    std::array<std::byte, page_size> page{};
    const auto digest = [&] {
        std::uint64_t value = 0;
        working_store::check(rig.store.state_digest({page.data(), page.size()}, value));
        return value;
    };
    const auto before = digest();
    const auto writes = rig.store.stats().page_writes;
    const auto observed = static_cast<const Host&>(host).source_pending_view();
    require(!observed.bound() && observed.size() == 2, "const view owns only in-memory rows");
    for (std::size_t i = 0; i < observed.size(); ++i)
        require(observed[i].id == "entry-" + std::to_string(i) && observed[i].qty == 2.0 + i,
                "const view keeps input values and order");
    require(digest() == before && rig.store.stats().page_writes == writes,
            "const view changes neither scratch bytes nor page writes");
    const auto const_writes = rig.store.stats().page_writes - writes;
    const auto& written = host.source_pending_view();
    require(written.bound() && written.size() == observed.size(), "writer retains its selected-store cache");
    for (std::size_t i = 0; i < observed.size(); ++i)
        require(written[i].id == observed[i].id && written[i].qty == observed[i].qty,
                "writer and const projections agree");
    std::printf("const_pending_rows=%zu const_pending_writes=%llu\n", observed.size(),
                static_cast<unsigned long long>(const_writes));
}
} // namespace

int main() {
    try {
        rewrites_erase_clear();
        live_reader_retention();
        drained_owners();
        const_pending_view();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL backing flatness: %s\n", error.what());
        return 1;
    }
}
