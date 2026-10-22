// Edit-only witness: the supervisor supplies frozen-header and runtime receipts.
#include <pineforge/engine.hpp>
#include <pineforge/run_failure.hpp>

#include <cstdio>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <vector>

namespace {
using namespace pineforge;

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

struct Host final : BacktestEngine {
    Host() : BacktestEngine(NativeConsumerBindTag{}) {}
    void on_bar(const Bar&) override {}
    using BacktestEngine::range_end_trades_;
    void add(unsigned i) {
        PyramidEntry lot{};
        lot.price = 100.0 + i;
        lot.time = 1000 + i;
        lot.qty = 1.0 + i;
        lot.entry_id = "lot-" + std::to_string(i);
        lot.entry_comment = "entry-" + std::to_string(i);
        lot.entry_bar_index = static_cast<int>(i);
        lot.entry_incarnation = 10 + i;
        lot.entry_commission_account = 0.0;
        fixture_open_lot(PositionSide::LONG, &lot);
    }
    void mark() {
        range_end_trades_.mark(*this, 110.0, 9000, 9, 1.0, pyramid_entries_.size());
    }
    void change_lot() { pyramid_entries_[1].price += 1.0; }
    const std::vector<Trade>& legacy_rows() const { return range_end_trades_; }
};

bool same_row(const Trade& a, const Trade& b) {
    return std::tie(a.entry_time, a.exit_time, a.entry_price, a.exit_price,
                    a.qty, a.pnl, a.pnl_pct, a.is_long, a.entry_bar_index,
                    a.exit_bar_index, a.entry_id, a.entry_comment, a.exit_comment,
                    a.exit_id, a.exit_from_bracket, a.max_runup, a.max_drawdown,
                    a.commission, a.entry_incarnation, a.open_at_end, a.close_cause)
        == std::tie(b.entry_time, b.exit_time, b.entry_price, b.exit_price,
                    b.qty, b.pnl, b.pnl_pct, b.is_long, b.entry_bar_index,
                    b.exit_bar_index, b.entry_id, b.entry_comment, b.exit_comment,
                    b.exit_id, b.exit_from_bracket, b.max_runup, b.max_drawdown,
                    b.commission, b.entry_incarnation, b.open_at_end, b.close_cause);
}

std::size_t clean_iteration() {
    Host host;
    for (unsigned i = 0; i < 3; ++i) host.add(i);
    host.mark();
    const auto& rows = host.range_end_trades_;
    static_assert(std::is_same_v<decltype(*rows.begin()), const Trade&>,
                  "terminal iterators expose only const rows");
    std::size_t index = 0;
    for (auto it = rows.begin(); it != rows.end(); ++it, ++index) {
        const Trade actual = *it;
        const Trade expected = rows.row(index);
        require(same_row(actual, expected), "every iterated row equals row(index)");
        require(same_row(actual, rows[index]) && same_row(actual, rows.at(index)),
                "indexed spellings preserve the whole row");
        require(actual.entry_incarnation == 10 + index, "lot order is unchanged");
    }
    require(index == rows.size() && index == 3 && !rows.empty(), "walk covers every row");
    std::vector<Trade> copied;
    copied.insert(copied.end(), rows.begin(), rows.end());
    const auto& legacy = host.legacy_rows();
    require(copied.size() == index && legacy.size() == index, "both legacy readers cover every row");
    for (std::size_t i = 0; i < index; ++i)
        require(same_row(copied[i], rows.row(i)) && same_row(legacy[i], copied[i]),
                "iterator insertion and stable vector reference preserve values");
    return index;
}

template<class Read>
void invariant(Read&& read, unsigned& throws) {
    bool caught = false;
    try { read(); }
    catch (const std::exception& error) {
        require(classify_run_failure(error).code == RunFailureCode::engine_invariant,
                "stale access has row()'s coded engine invariant");
        caught = true;
    }
    require(caught, "a changed book must fail loudly");
    ++throws;
}

unsigned changed_epoch() {
    Host host;
    for (unsigned i = 0; i < 3; ++i) host.add(i);
    host.mark();
    const auto& rows = host.range_end_trades_;
    auto it = rows.begin();
    const auto old_end = rows.end();
    require(it->entry_incarnation == 10, "iteration begins before mutation");
    ++it;
    require(it->entry_incarnation == 11, "mutation happens mid-iteration");
    host.change_lot(); // unchanged row count: only the pinned epoch can detect it
    unsigned throws = 0;
    invariant([&] { (void)*it; }, throws);
    invariant([&] { ++it; }, throws);
    invariant([&] { it++; }, throws);
    invariant([&] { (void)(it == old_end); }, throws);
    invariant([&] { (void)rows.end(); }, throws);
    invariant([&] { (void)rows.row(1); }, throws);
    host.mark();
    require(rows.size() == 3 && rows.row(1).entry_price == 102.0,
            "a new mark can read the changed book without a count change");
    invariant([&] { (void)*it; }, throws);
    invariant([&] { ++it; }, throws);
    return throws;
}

std::size_t synthetic_rows() {
    Host host;
    for (unsigned i = 0; i < 2; ++i) {
        Trade row{};
        row.entry_incarnation = 100 + i;
        row.open_at_end = true;
        host.range_end_trades_.push_back(row);
    }
    const auto& rows = host.legacy_rows();
    require(rows.size() == 2 && rows[0].entry_incarnation == 100
                && rows[1].entry_incarnation == 101, "legacy append retains both synthetic rows");
    return rows.size();
}
} // namespace

int main() {
    try {
        const auto clean = clean_iteration();
        const auto throws = changed_epoch();
        const auto synthetic = synthetic_rows();
        std::printf("clean_rows=%zu stale_throws=%u synthetic_rows=%zu\n", clean, throws, synthetic);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL terminal rows: %s\n", error.what());
        return 1;
    }
}
