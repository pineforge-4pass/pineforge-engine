// R5 lane PERF-P23: the Pine adapter's host view against dynamic_cast's, run by
// run.
//
// PineExecutionAdapter took dynamic_cast<PineStrategyHost*> of its bound host
// at 25 sites, several of them every bar (the account-FX lookup, the
// percent-commission sizing basis, the leg order). The lane memoises that
// answer per thread, keyed by the host's address, the address of its complete
// object and its dynamic type (pine_view in src/source/pine_adapter.cpp); the
// adapter gains no member, because it is a by-value member of
// PineStrategyHost. The memo has to answer as dynamic_cast did whichever host
// asks, in whatever order. This witness runs two Pine host classes that reach
// those sites on every bar -- percent-of-equity sizing under a percent
// commission with pyramiding, over a staged account-FX series in batch --
// alone as a batch and as a stream, as two live streams interleaved bar by bar, three
// streams at once in a round robin, and rebuilt in turn in one block of
// storage. Every run's trades, net profit and broker-state hash are pinned,
// harvested at the lane's base fc7aad62 (PF_HOST_VIEW_MEMO_DUMP=1), and every
// shared or rebuilt run must also equal its host class's run alone.
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

int passed = 0;
int failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            ++passed;                                                            \
        } else {                                                                 \
            ++failed;                                                            \
            std::fprintf(stderr, "CHECK FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

constexpr std::int64_t kT0 = 1704067200000LL;  // 2024-01-01 00:00 UTC
constexpr std::int64_t kMinute = 60000;
constexpr int kBars = 160;
constexpr int kWarmup = 20;

struct Fnv {
    std::uint64_t h = 1469598103934665603ULL;
    void bytes(const void* p, std::size_t n) {
        const auto* c = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h ^= c[i];
            h *= 1099511628211ULL;
        }
    }
    template <class T> void v(T x) { bytes(&x, sizeof x); }
    void s(const std::string& text) {
        v(text.size());
        bytes(text.data(), text.size());
    }
};

class ViewHost : public source::PineStrategyHost {
public:
    ViewHost(int variant, bool fx_series) : variant_(variant) {
        source::PineStrategyConfig config;
        config.initial_capital = 100000;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 10;
        config.pyramiding = 3;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.1;
        configure_pine_strategy(config);
        syminfo_mintick_ = 0.01;
        // A staged series is batch-only (streaming refuses the setter
        // ingress); a streamed host reads the scalar through the same lookup.
        if (fx_series) {
            const std::int64_t stamps[] = {kT0, kT0 + 60 * kMinute, kT0 + 110 * kMinute};
            const double rates[] = {1.0, 1.25, 0.8};
            set_account_currency_fx_series(stamps, rates, 3);
        }
    }

    void on_source_bar(const Bar&) override {
        const int i = bars_++;
        if (i % 7 == variant_) strategy_entry("L", true);
        if (i % 11 == 3 + variant_) strategy_close("L", "close-L");
        if (variant_ == 1 && i % 13 == 5) strategy_entry("S", false);
        if (variant_ == 1 && i % 17 == 9) strategy_close("S", "close-S");
    }

    std::string fingerprint() const {
        Fnv f;
        for (const Trade& t : trades_) {
            f.v(t.entry_time);
            f.v(t.exit_time);
            f.v(t.entry_price);
            f.v(t.exit_price);
            f.v(t.qty);
            f.v(t.pnl);
            f.v(t.commission);
            f.v(t.is_long);
        }
        char text[200];
        std::snprintf(text, sizeof text, "trades=%zu fnv=%016llx net=%.17g hash=%llu",
                      trades_.size(), static_cast<unsigned long long>(f.h), net_profit_sum_,
                      static_cast<unsigned long long>(broker_state_hash()));
        return std::string(text) + " error='" + last_error() + "'";
    }

private:
    int variant_ = 0;
    int bars_ = 0;
};

// Two distinct dynamic types over the same behaviour family.
class HostA final : public ViewHost {
public:
    explicit HostA(bool fx_series) : ViewHost(0, fx_series) {}
};
class HostB final : public ViewHost {
public:
    explicit HostB(bool fx_series) : ViewHost(1, fx_series) {}
};

std::vector<Bar> tape() {
    std::vector<Bar> bars;
    std::uint64_t state = 0x2545F4914F6CDD1DULL;
    double price = 100.0;
    for (int i = 0; i < kBars; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        const double step = (static_cast<double>(state % 2001) - 1000.0) / 1000.0;
        const double open = price;
        const double close = std::max(20.0, price + step);
        const double high = std::max(open, close) + 0.35;
        const double low = std::min(open, close) - 0.35;
        bars.push_back(Bar{open, high, low, close, 10.0, kT0 + i * kMinute});
        price = close;
    }
    return bars;
}

void batch(ViewHost& host, const std::vector<Bar>& bars) {
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
}

bool open_stream(ViewHost& host, const std::vector<Bar>& bars) {
    return host.stream_begin(bars.data(), kWarmup, "1", "1");
}

struct Pinned {
    const char* run;
    const char* fingerprint;
};

#include "test_adapter_host_view_memo_data.hpp"

std::vector<std::pair<std::string, std::string>> observed;

void record(const std::string& run, const std::string& fingerprint) {
    observed.emplace_back(run, fingerprint);
}

void solo_runs(const std::vector<Bar>& bars, std::string& batch_a, std::string& batch_b,
               std::string& stream_a, std::string& stream_b) {
    {
        HostA a(true);
        batch(a, bars);
        batch_a = a.fingerprint();
    }
    {
        HostB b(true);
        batch(b, bars);
        batch_b = b.fingerprint();
    }
    {
        HostA a(false);
        CHECK(open_stream(a, bars));
        for (int i = kWarmup; i < kBars; ++i) CHECK(a.stream_push_bar(bars[i]));
        CHECK(a.stream_end(false));
        stream_a = a.fingerprint();
    }
    {
        HostB b(false);
        CHECK(open_stream(b, bars));
        for (int i = kWarmup; i < kBars; ++i) CHECK(b.stream_push_bar(bars[i]));
        CHECK(b.stream_end(false));
        stream_b = b.fingerprint();
    }
    record("batch/A", batch_a);
    record("batch/B", batch_b);
    record("stream/A", stream_a);
    record("stream/B", stream_b);
}

// Two live hosts of different types, their bars pushed alternately: every bar
// of one is followed by a bar of the other on the same thread.
void interleaved(const std::vector<Bar>& bars, const std::string& stream_a,
                 const std::string& stream_b) {
    HostA a(false);
    HostB b(false);
    CHECK(open_stream(a, bars));
    CHECK(open_stream(b, bars));
    for (int i = kWarmup; i < kBars; ++i) {
        CHECK(a.stream_push_bar(bars[i]));
        CHECK(b.stream_push_bar(bars[i]));
    }
    CHECK(a.stream_end(false));
    CHECK(b.stream_end(false));
    record("interleaved/A", a.fingerprint());
    record("interleaved/B", b.fingerprint());
    CHECK(a.fingerprint() == stream_a);
    CHECK(b.fingerprint() == stream_b);
}

// Three live hosts, two of one type, in a round robin.
void round_robin(const std::vector<Bar>& bars, const std::string& stream_a,
                 const std::string& stream_b) {
    HostA a1(false);
    HostB b(false);
    HostA a2(false);
    ViewHost* hosts[] = {&a1, &b, &a2};
    for (ViewHost* host : hosts) CHECK(open_stream(*host, bars));
    for (int i = kWarmup; i < kBars; ++i) {
        for (ViewHost* host : hosts) CHECK(host->stream_push_bar(bars[i]));
    }
    for (ViewHost* host : hosts) CHECK(host->stream_end(false));
    record("round_robin/A1", a1.fingerprint());
    record("round_robin/B", b.fingerprint());
    record("round_robin/A2", a2.fingerprint());
    CHECK(a1.fingerprint() == stream_a);
    CHECK(b.fingerprint() == stream_b);
    CHECK(a2.fingerprint() == stream_a);
}

// One block of storage, rebuilt as A, B, A, B: each host lives at the address
// the previous one, of the other type, died at.
void rebuilt_in_place(const std::vector<Bar>& bars, const std::string& batch_a,
                      const std::string& batch_b) {
    const std::size_t size = std::max(sizeof(HostA), sizeof(HostB));
    const std::size_t align = std::max(alignof(HostA), alignof(HostB));
    void* storage = ::operator new(size, std::align_val_t{align});
    for (int round = 0; round < 4; ++round) {
        std::string fingerprint;
        ViewHost* host = nullptr;
        if (round % 2 == 0) {
            host = new (storage) HostA(true);
        } else {
            host = new (storage) HostB(true);
        }
        CHECK(static_cast<void*>(host) == storage);
        batch(*host, bars);
        fingerprint = host->fingerprint();
        host->~ViewHost();
        record("rebuilt/" + std::to_string(round), fingerprint);
        CHECK(fingerprint == (round % 2 == 0 ? batch_a : batch_b));
    }
    ::operator delete(storage, std::align_val_t{align});
}

}  // namespace

int main() {
    const auto bars = tape();
    std::string batch_a, batch_b, stream_a, stream_b;
    solo_runs(bars, batch_a, batch_b, stream_a, stream_b);
    interleaved(bars, stream_a, stream_b);
    round_robin(bars, stream_a, stream_b);
    rebuilt_in_place(bars, batch_a, batch_b);
    // The two classes do trade, and differently: the witness is not vacuous.
    CHECK(batch_a != batch_b);
    CHECK(batch_a.rfind("trades=0 ", 0) != 0);
    CHECK(batch_b.rfind("trades=0 ", 0) != 0);

    if (std::getenv("PF_HOST_VIEW_MEMO_DUMP") != nullptr) {
        for (const auto& row : observed)
            std::printf("    {\"%s\", \"%s\"},\n", row.first.c_str(), row.second.c_str());
    }
    const std::size_t pinned = sizeof(kPinned) / sizeof(kPinned[0]);
    CHECK(observed.size() == pinned);
    for (std::size_t i = 0; i < observed.size() && i < pinned; ++i) {
        const bool same = observed[i].first == kPinned[i].run
            && observed[i].second == kPinned[i].fingerprint;
        if (!same) {
            std::fprintf(stderr, "  %s\n    pinned:   %s: %s\n    observed: %s\n",
                         observed[i].first.c_str(), kPinned[i].run, kPinned[i].fingerprint,
                         observed[i].second.c_str());
        }
        CHECK(same);
    }
    std::printf("  %zu runs pinned; batch A %s\n", observed.size(), batch_a.c_str());
    std::printf("test_adapter_host_view_memo: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
