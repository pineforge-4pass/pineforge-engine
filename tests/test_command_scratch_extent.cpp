/*
 * CR150-CX M1, c7b06a6a19: STATIC READ ONLY. This lane has not compiled or run
 * this TU. The supervisor owns fail-before, the lab run and the parity verdict.
 *
 * RESULT OF THE DERIVATION: NO PROPOSED FIXED Kc.
 *
 * An observed extent is not an upper bound. In particular, neither 4096 nor
 * 16384 bounds the live population. src/native_order.cpp:1485-1486 calls 4096
 * the resident working-book threshold; :2101-2102 includes spilled rows in
 * live_size(), and :2135-2154 spills rather than refuses more live requests.
 * Its journal's kMaxResident=16384 (:1537) is another residency threshold.
 * PineStrategyConfig::pyramiding is a caller-supplied int
 * (include/pineforge/source/pine_adapter.hpp:77-90); declaration admission
 * (src/source/pine_value_admit_decl.hpp:23-36) does not impose a smaller max.
 * Configuring INT_MAX does not make a test with 64 lots an INT_MAX witness.
 * Raw strategy_order/native put and pending exits are not bounded by an
 * entry pyramiding declaration in any case. There is no engine live-order,
 * live-lot or live-exit ceiling from which to calculate the requested K.
 * NativeRunSpec::max_open_lots is optional
 * (include/pineforge/native_run_spec.hpp:664); the Pine projection explicitly
 * leaves it unbounded (src/source/pine_adapter.cpp:3017-3022). Its conditional
 * admission gate (src/native_execution_consumer.cpp:3871-3874) is not a
 * mandatory engine limit.
 *
 * Store geometry and the missing terms:
 *
 * Let P be the backing page size, U=P-kTrailer, W=sizeof(uint64_t).
 * scratch_impl::{kHeader,kTrailer}=16, kCell=64, kCellPayload=56
 * (include/pineforge/detail/scratch_store_impl.hpp:67-70). A handle entry is
 * four LE64 words, sizeof(Core::Entry)=32 (:100-105, :714-729). An ordered
 * leaf additionally stores a sizeof(ScratchHandle)=8 handle per live key.
 *
 * For a single key/value blob of length L, its allocated payload storage is:
 *
 *   blob(P,L) = 0                              if L=0
 *             = kCell*ceil(L/kCellPayload)       if 0<L<=P/2
 *             = P*ceil(L/(P-kTrailer-3*W))       otherwise.
 *
 * The last case includes each blob page's header and next link. Cell-page
 * packing/padding is additional: floor((U-kHeader)/kCell) cells per page.
 * These follow :180-187 and blob_create :484-539. Thus the record contribution
 * BEFORE index-page packing, journal, ghosts and free-pool overhead is
 *
 *   sizeof(Core::Entry)+sizeof(ScratchHandle)+blob(P,key)+blob(P,value).
 *
 * Page capacities (:180-186), also printed below using named constants:
 *
 *   handle leaves: floor((U-kHeader)/sizeof(Core::Entry))
 *   handle nodes:  floor((U-kHeader)/W)
 *   ordered leaves:floor((U-4*W)/W)
 *   ordered nodes: floor((U-3*W)/(2*W)).
 *
 * A complete per-store bound would need
 *
 *   P * (2 superblocks
 *        + ceil(peak allocated cells / cells_per_page)
 *        + peak allocated whole-blob pages
 *        + handle leaf/node pages + ordered leaf/node pages
 *        + ghost/separator pages + reusable/free pages + journal-pool pages).
 *
 * The initial two superblocks are Core::Super::hw=2 (:88). touch() (:350-384)
 * writes one undo page per previously existing page first changed in a
 * transaction; commit_impl() (:1184-1222) returns those pages to the retained
 * journal pool. An upsert allocates the replacement BEFORE freeing the old
 * blobs (:1307-1319); "live records * final payload bytes" misses that peak.
 * Cell pages are reused as cells, not returned as whole pages (:449-476).
 * docs/scratch-store.md:201-205 describes retained cell/index/journal state,
 * but its handle-interior claim is stale on this head: hset() :741-760 and
 * remove_child()/release_separator() :1013-1080 reclaim empty branches.
 * Consequently this test MEASURES flatness rather than assumes linear growth
 * in handles ever issued. Rollback-only native trade/dependency scopes also
 * restore snap_ (:1238-1275); exit scopes instead commit insertions AND erases
 * (include/pineforge/source/exit_scratch.hpp:293-331).
 *
 * Logical key/value lengths are not sizeof their owning C++ objects:
 *
 * - ExitScratchKey is sizeof(ExitScratchKeyWords)=4*W plus optional text
 *   (include/pineforge/source/exit_scratch.hpp:221-234). String rows also have
 *   a secondary index with the string in its key (:563-573).
 * - The active ExitScratchVersionsProduction codec has a W-byte schema word;
 *   scalars/tags/lengths are W bytes; strings append their full length and
 *   vectors append every element
 *   (src/source/exit_scratch_versions_production.cpp:99-114, :202-252,
 *    :484-505). put_version() :531-554 adds a scope sequence record and the
 *   payload record. This is the provider installed by SourceExitScratchRun.
 * - The separate field-stream codec requested in the brief uses a 4*W key,
 *   a 2*W type/name-length header, field-name bytes and field-value bytes;
 *   its root value is 3*W
 *   (src/source/exit_payload_codec.inc:14-22, :68-73, :127-134). borrowed_text
 *   :95-108 emits one field per 128-byte chunk. It therefore does not turn
 *   arbitrary strings into a bounded number of records.
 * - journal_codec::WordSource has a W-byte key; Writer::word/text emits W
 *   bytes and W+string.size() respectively
 *   (include/pineforge/detail/journal_codec.hpp:62-72, :172-186, :226-233).
 * - Native dependency scratch has a 4*W key and a zero-byte edge or 2*W
 *   pending value (src/native_execution_consumer.cpp:4965-5000). It indexes
 *   EVERY live waiting child. Our fanout test checks the actual book exceeds
 *   the resident threshold and that the real dependency queue processed it.
 *   More decisively, m waiting children of one cancelled parent produce m
 *   edge records AND m queued records before the first pop (:4980-5005).
 *   At P=512 their record components above are respectively 104 and 168
 *   bytes, so this family allocates at least 272*m bytes before page packing,
 *   tree nodes, superblocks or journaling. At most two cache pages can still
 *   await writes; even subtracting 2*P leaves growth linear in m. This is an
 *   allocation lower bound, not a claimed measured extent. All child labels
 *   can be identical; limiting distinct ID strings alone would not bound m.
 * - Native trade scratch has a 4*W key and, for a trade payload, a 23*W
 *   scalar header plus entry_id, entry_comment, exit_comment and exit_id
 *   bytes (src/source/pine_strategy_host.cpp:102-105, :219-230, :254-269).
 *
 * Missing inputs for a finite useful bound are: maximum simultaneous pending
 * requests/lots/exits and nested scratch scopes, maximum ID/comment/session/
 * replacement-key bytes, maximum vector/frozen-opening membership, and a
 * proved bound on the page/journal/free-pool peak for that population.
 * The substrate checks lengths <2^61 and handles <2^62 (:1290, :1322), and the
 * file backing checks representable off_t offsets
 * (src/command_scratch_production.cpp:193-200). These are representation/I/O
 * failure boundaries, not an accepted small engine live-population contract.
 * The 16-Mi page_count check (:150) bounds CACHE SLOTS, not backing pages
 * (include/pineforge/detail/scratch_store.hpp:37-41). The optional file byte
 * cap defaults to unset and returns io_error on exhaustion
 * (src/command_scratch_production.cpp:24-25, :145-158, :234-244); it does not
 * prove that an otherwise permitted run fits.
 *
 * Therefore bytes_per_record_max and live_max are not supplied by this
 * engine's contract. K=chunkCeil(max derived_bound), 3*K and 3*K+3 chunks
 * cannot be proposed as finite numbers. No measured high-water, multiplier,
 * UINT64_MAX stand-in or conditional-vacuous "bound >= observed" is used here.
 *
 * Measurement qualifications:
 *
 * - Registration precedes host construction and remains alive until after
 *   destruction. Each size is a fresh host/run and observer; runs never overlap.
 * - ScratchIoCharge has no backing identity. max_extent_bytes is the max of
 *   (charge.page+1)*charge.bytes over SUCCESSFUL write_page charges, never a
 *   sum of stores. bytes is the actual backing page size, NOT hardcoded 512.
 *   Native trade/dependency and standalone adapter stores use 512, but
 *   SourceExitScratchRun uses sysconf(_SC_PAGESIZE)
 *   (src/source/pine_strategy_host.cpp:53-71). Role-specific stats are read
 *   through the existing test seams and checked against the charged maximum.
 * - live_peak is the max store.record_count sampled at I/O and exit-row
 *   observation boundaries. It is diagnostic, not a claimed universal maximum.
 *   orders_peak is the exact working-book count at those boundaries. Stress
 *   hosts additionally assert their lot/pending population in source callbacks.
 * - All comparisons allow only scratch_min_page_size (512 bytes), even on a
 *   host with a larger exit-store page. Failures are accumulated so one
 *   counterexample does not hide the later workloads' measurements.
 * - The original fixture's case 0 returns before all orders; case 6 operates
 *   on an independent, nonmatching WorkingRequestCore and returns too
 *   (tests/fixtures/arena_bound/generated.cpp:53-72). Both are included
 *   unchanged. The mandatory writes/trades checks are NOT waived for them:
 *   these controls cannot satisfy the brief's "every workload trades" premise.
 *   A red result for them must be reported, not converted to a fake pass.
 * - Corpus inputs are real 1-minute ETH OHLCV aggregated here into consecutive
 *   15-minute bars, with unmodified generated strategy bodies at 10k/100k bars.
 *   This is an extent experiment, not a reproduction of the parity date window.
 */

#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/exit_scratch.hpp>
#include <pineforge/detail/journal_codec.hpp>
#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/selected_window.h>
#include "../src/native_execution_consumer.hpp"
#include "../src/source/pine_command_scratch_production.hpp"

// Load the complete generated-code header closure at global scope BEFORE
// including the real generated bodies in separate C++ namespaces.
#include <pineforge/checked_settings.hpp>
#include <pineforge/color.hpp>
#include <pineforge/drawing.hpp>
#include <pineforge/log.hpp>
#include <pineforge/math.hpp>
#include <pineforge/na.hpp>
#include <pineforge/series.hpp>
#include <pineforge/session_time.hpp>
#include <pineforge/str_utils.hpp>
#include <pineforge/ta.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// Every generated C entry needs a distinct linker symbol, even inside C++
// namespaces. Only their names change; the actual strategies are included
// verbatim and use the ONE engine/observer registration linked into this TU.
#define M1_JOIN_INNER(a, b) a##b
#define M1_JOIN(a, b) M1_JOIN_INNER(a, b)
#define strategy_create M1_JOIN(M1_PREFIX, strategy_create)
#define strategy_create_checked M1_JOIN(M1_PREFIX, strategy_create_checked)
#define strategy_free M1_JOIN(M1_PREFIX, strategy_free)
#define report_free M1_JOIN(M1_PREFIX, report_free)
#define run_backtest M1_JOIN(M1_PREFIX, run_backtest)
#define run_backtest_full M1_JOIN(M1_PREFIX, run_backtest_full)
#define run_backtest_full_checked M1_JOIN(M1_PREFIX, run_backtest_full_checked)
#define strategy_set_input M1_JOIN(M1_PREFIX, strategy_set_input)
#define strategy_set_input_checked M1_JOIN(M1_PREFIX, strategy_set_input_checked)
#define strategy_set_override M1_JOIN(M1_PREFIX, strategy_set_override)
#define strategy_set_override_checked M1_JOIN(M1_PREFIX, strategy_set_override_checked)
#define strategy_set_magnifier_volume_weighted M1_JOIN(M1_PREFIX, strategy_set_magnifier_volume_weighted)
#define strategy_settings_api_version M1_JOIN(M1_PREFIX, strategy_settings_api_version)
#define strategy_get_effective_settings M1_JOIN(M1_PREFIX, strategy_get_effective_settings)
#define strategy_capabilities_api_version M1_JOIN(M1_PREFIX, strategy_capabilities_api_version)
#define strategy_capabilities_receipt M1_JOIN(M1_PREFIX, strategy_capabilities_receipt)
#define strategy_confirmed_bar_api_version M1_JOIN(M1_PREFIX, strategy_confirmed_bar_api_version)
#define strategy_confirmed_bar_receipt M1_JOIN(M1_PREFIX, strategy_confirmed_bar_receipt)
#define strategy_order_shapes_api_version M1_JOIN(M1_PREFIX, strategy_order_shapes_api_version)
#define strategy_order_shapes_receipt M1_JOIN(M1_PREFIX, strategy_order_shapes_receipt)
#define M1_PREFIX m1_fixture_
namespace m1_fixture {
#include "fixtures/arena_bound/generated.cpp"
}
#undef M1_PREFIX
#define M1_PREFIX m1_compass_
namespace m1_compass {
#include "../corpus/validation/bracket-compass-partial-ladder-01/generated.cpp"
}
#undef M1_PREFIX
#define M1_PREFIX m1_scalping_
namespace m1_scalping {
#include "../corpus/validation/composite-scalping-integration-01/generated.cpp"
}
#undef M1_PREFIX
#define M1_PREFIX m1_trendmaster_
namespace m1_trendmaster {
#include "../corpus/validation/composite-trendmaster-integration-01/generated.cpp"
}
#undef M1_PREFIX
#undef strategy_create
#undef strategy_create_checked
#undef strategy_free
#undef report_free
#undef run_backtest
#undef run_backtest_full
#undef run_backtest_full_checked
#undef strategy_set_input
#undef strategy_set_input_checked
#undef strategy_set_override
#undef strategy_set_override_checked
#undef strategy_set_magnifier_volume_weighted
#undef strategy_settings_api_version
#undef strategy_get_effective_settings
#undef strategy_capabilities_api_version
#undef strategy_capabilities_receipt
#undef strategy_confirmed_bar_api_version
#undef strategy_confirmed_bar_receipt
#undef strategy_order_shapes_api_version
#undef strategy_order_shapes_receipt
#undef M1_JOIN
#undef M1_JOIN_INNER

namespace pineforge {
inline namespace engine_script_run_v19 {
// The same existing friend seam as test_run_setup_exit_scratch_binding.cpp.
struct NativeExecutionConsumerProbe {
    static const detail::CommandScratchStore* dependency(const NativeExecutionConsumer& consumer) {
        return consumer.dependency_scratch_;
    }
    static std::uint64_t queued(const NativeExecutionConsumer& consumer) {
        return consumer.dependency_scratch_stats_.queued_rows;
    }
};
}
}

namespace {
using namespace pineforge;
namespace sd = source::detail;
namespace si = detail::scratch_impl;
namespace no = native_order;
using Store = detail::CommandScratchStore;
using Status = detail::ScratchStatus;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1'736'121'600'000;
constexpr std::int64_t kMinute = 60'000;
constexpr std::size_t kRoles =
    static_cast<std::size_t>(NativeExecutionConsumer::CommandScratchBinding::Count);
constexpr std::uint64_t kFlatTolerance = detail::scratch_min_page_size;
constexpr std::size_t kTrade =
    static_cast<std::size_t>(NativeExecutionConsumer::CommandScratchBinding::Trade);
constexpr std::size_t kDependency =
    static_cast<std::size_t>(NativeExecutionConsumer::CommandScratchBinding::Dependency);
constexpr std::size_t kExit =
    static_cast<std::size_t>(NativeExecutionConsumer::CommandScratchBinding::Exit);

// Read existing borrowed slots only. No new production accessor or owner.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct AdapterSlot { friend auto access(AdapterSlot); };
struct ExitOwner { friend auto access(ExitOwner); };
struct TradeSlot { friend auto access(TradeSlot); };
template struct Access<AdapterSlot, &source::PineStrategyHost::adapter_>;
template struct Access<ExitOwner, &source::PineExecutionAdapter::exit_scratch_>;
template struct Access<TradeSlot, &source::PineStrategyHost::trade_scratch_>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Checks {
    std::size_t failures = 0;
    void check(bool condition, const std::string& message) {
        if (condition) return;
        ++failures;
        std::fprintf(stderr, "M1 FAIL %s\n", message.c_str());
    }
};

struct StorePeak {
    std::uint64_t extent = 0;
    std::uint64_t rows = 0;
    std::uint64_t page_size = 0;
};

struct Observation {
    source::PineStrategyHost* host = nullptr;
    NativeExecutionConsumer* consumer = nullptr;
    std::array<StorePeak, kRoles> roles{};
    std::uint64_t max_page = 0;
    std::uint64_t max_extent = 0;
    std::uint64_t extent_page = 0;
    std::uint64_t extent_page_size = 0;
    std::uint64_t pages_written = 0;
    std::uint64_t writes_attempted = 0;
    std::uint64_t write_errors = 0;
    std::uint64_t live_peak = 0;
    std::uint64_t orders_peak = 0;
    std::size_t stores = 0;
    bool bad_geometry = false;
    bool sampling = false;

    void watch(source::PineStrategyHost& value) {
        consumer = &NativeExecutionConsumer::bound(value);
        host = &value;
        sample();
    }
    void unwatch() noexcept { host = nullptr; consumer = nullptr; }
    void sample() noexcept {
        if (!host || sampling) return;
        sampling = true;
        auto& adapter = host->*access(AdapterSlot{});
        const auto& owner = adapter.*access(ExitOwner{});
        const std::array<const Store*, kRoles> slots{
            host->*access(TradeSlot{}),
            NativeExecutionConsumerProbe::dependency(*consumer),
            owner ? &owner->store : nullptr
        };
        if (owner) {
            // Fresh test hosts have no prior row observer. This existing
            // observer also samples after pushes that fit entirely in cache.
            owner->observation = this;
            owner->observe_rows = [](void* context, std::uint64_t, std::size_t) noexcept {
                static_cast<Observation*>(context)->sample();
            };
        }
        std::size_t present = 0;
        for (std::size_t role = 0; role < slots.size(); ++role) {
            if (!slots[role]) continue;
            ++present;
            const auto stats = slots[role]->stats();
            auto& peak = roles[role];
            peak.extent = std::max(peak.extent, stats.backing_bytes);
            peak.rows = std::max(peak.rows, slots[role]->record_count());
            peak.page_size = slots[role]->page_size();
            live_peak = std::max(live_peak, peak.rows);
        }
        stores = std::max(stores, present);
        orders_peak = std::max<std::uint64_t>(
            orders_peak, consumer->request_core().live().size());
        sampling = false;
    }
    static void charge(void* context, const detail::ScratchIoCharge& charge) noexcept {
        auto& self = *static_cast<Observation*>(context);
        if (charge.operation == detail::ScratchIoOperation::write_page) {
            ++self.writes_attempted;
            if (charge.status != Status::ok) ++self.write_errors;
            else {
                ++self.pages_written;
                if (charge.page == detail::scratch_no_page || charge.bytes == 0
                    || charge.page >= std::numeric_limits<std::uint64_t>::max() / charge.bytes) {
                    self.bad_geometry = true;
                } else {
                    self.max_page = std::max(self.max_page, charge.page);
                    const auto extent = (charge.page + 1) * charge.bytes;
                    if (extent > self.max_extent) {
                        self.max_extent = extent;
                        self.extent_page = charge.page;
                        self.extent_page_size = charge.bytes;
                    }
                }
            }
        }
        self.sample();
    }
};

class Registration {
public:
    explicit Registration(Observation& observation)
        : previous_(sd::set_command_scratch_io_observer({&observation, Observation::charge})) {}
    ~Registration() { sd::set_command_scratch_io_observer(previous_); }
    Registration(const Registration&) = delete;
    Registration& operator=(const Registration&) = delete;
private:
    detail::ScratchIoObserver previous_;
};

class HostObservation {
public:
    HostObservation(Observation& observation, source::PineStrategyHost& host)
        : observation_(observation) { observation_.watch(host); }
    ~HostObservation() { observation_.unwatch(); }
    HostObservation(const HostObservation&) = delete;
    HostObservation& operator=(const HostObservation&) = delete;
private:
    Observation& observation_;
};

struct Result {
    Observation io;
    std::size_t trades = 0;
    bool completed = false;
};

constexpr std::uint64_t blob_component(std::uint64_t page, std::uint64_t length) {
    if (length == 0) return 0;
    const auto small = length <= page / 2;
    const auto payload = small ? si::kCellPayload
        : page - si::kTrailer - 3 * sizeof(std::uint64_t);
    const auto allocation = small ? si::kCell : page;
    return ((length + payload - 1) / payload) * allocation;
}

constexpr std::uint64_t record_component(std::uint64_t page, std::uint64_t key,
                                         std::uint64_t value) {
    return sizeof(si::Core::Entry) + sizeof(detail::ScratchHandle)
        + blob_component(page, key) + blob_component(page, value);
}

void print_geometry(std::uint64_t page) {
    const auto word = sizeof(std::uint64_t);
    const auto usable = page - si::kTrailer;
    std::printf("M1 geometry page_size=%llu record_header=%zu ordered_handle=%zu "
                "cell=%zu cell_payload=%zu cells_per_page=%llu handle_leaf=%llu "
                "handle_node=%llu ordered_leaf=%llu ordered_node=%llu chunk_bytes=%llu "
                "dependency_edge_component_bytes=%llu dependency_pending_component_bytes=%llu\n",
        static_cast<unsigned long long>(page), sizeof(si::Core::Entry),
        sizeof(detail::ScratchHandle), si::kCell, si::kCellPayload,
        static_cast<unsigned long long>((usable - si::kHeader) / si::kCell),
        static_cast<unsigned long long>((usable - si::kHeader) / sizeof(si::Core::Entry)),
        static_cast<unsigned long long>((usable - si::kHeader) / word),
        static_cast<unsigned long long>((usable - 4 * word) / word),
        static_cast<unsigned long long>((usable - 3 * word) / (2 * word)),
        static_cast<unsigned long long>(detail::ScratchArena::kChunkBytes),
        static_cast<unsigned long long>(
            record_component(page, sizeof(std::array<std::uint64_t, 4>), 0)),
        static_cast<unsigned long long>(record_component(page,
            sizeof(std::array<std::uint64_t, 4>), sizeof(std::array<std::uint64_t, 2>))));
}

template<class Factory, class Prepare, class Validate>
Result measure(const std::string& name, const std::vector<Bar>& bars,
               Factory make, Prepare prepare, Validate validate, Checks& checks,
               const char* timeframe = "1") {
    Result result;
    // The backing captures this registration at creation, including setup.
    {
        Registration registration(result.io);
        try {
            auto host = make();
            // This guard dies before host, including during exception unwind.
            HostObservation observation(result.io, *host);
            prepare(*host);
            // Same public C++ batch entry as the source-host unit tests. The
            // generated C entries delegate to this entry too.
            host->run(bars.data(), static_cast<int>(bars.size()), timeframe, timeframe,
                      false, 0, MagnifierDistribution::ENDPOINTS);
            result.io.sample();
            result.trades = host->closed_trade_count();
            result.completed = host->last_run_status() == 0 && host->last_error().empty()
                && host->native_state().kind == NativeLifecycleKind::Completed;
            checks.check(result.completed, name + ": run failed: "
                         + run_failure_code_of(*host) + " " + host->last_error());
            if (result.completed) validate(*host, checks);
            // Destroy the host while its captured observer context is alive.
        } catch (const std::exception& error) {
            result.io.unwatch();
            checks.check(false, name + ": exception: " + error.what());
        } catch (...) {
            result.io.unwatch();
            checks.check(false, name + ": non-standard exception");
        }
    }
    auto& io = result.io;
    std::printf("M1 workload=%s stores=%zu max_page=%llu max_extent_bytes=%llu "
                "pages_written=%llu live_peak=%llu bars=%zu trades=%zu orders_peak=%llu "
                "extent_page=%llu extent_page_size=%llu writes_attempted=%llu write_errors=%llu "
                "trade_extent_bytes=%llu dependency_extent_bytes=%llu exit_extent_bytes=%llu "
                "trade_live_peak=%llu dependency_live_peak=%llu exit_live_peak=%llu "
                "trade_page_size=%llu dependency_page_size=%llu exit_page_size=%llu\n",
        name.c_str(), io.stores, static_cast<unsigned long long>(io.max_page),
        static_cast<unsigned long long>(io.max_extent),
        static_cast<unsigned long long>(io.pages_written),
        static_cast<unsigned long long>(io.live_peak), bars.size(), result.trades,
        static_cast<unsigned long long>(io.orders_peak),
        static_cast<unsigned long long>(io.extent_page),
        static_cast<unsigned long long>(io.extent_page_size),
        static_cast<unsigned long long>(io.writes_attempted),
        static_cast<unsigned long long>(io.write_errors),
        static_cast<unsigned long long>(io.roles[kTrade].extent),
        static_cast<unsigned long long>(io.roles[kDependency].extent),
        static_cast<unsigned long long>(io.roles[kExit].extent),
        static_cast<unsigned long long>(io.roles[kTrade].rows),
        static_cast<unsigned long long>(io.roles[kDependency].rows),
        static_cast<unsigned long long>(io.roles[kExit].rows),
        static_cast<unsigned long long>(io.roles[kTrade].page_size),
        static_cast<unsigned long long>(io.roles[kDependency].page_size),
        static_cast<unsigned long long>(io.roles[kExit].page_size));
    std::fflush(stdout);
    checks.check(!io.bad_geometry, name + ": invalid/overflowing charged page geometry");
    checks.check(io.write_errors == 0, name + ": a command-scratch write failed");
    checks.check(io.pages_written > 0, name + ": no successful write_page charges");
    checks.check(result.trades > 0, name + ": no real closed trades");
    checks.check(io.stores == kRoles, name + ": did not observe all run-owned scratch bindings");
    for (const auto& role : io.roles)
        checks.check(role.extent <= io.max_extent,
                     name + ": role backing extent exceeds the observer's measured maximum");
    return result;
}

void compare_sizes(const std::string& name, const Result& smaller,
                   const Result& larger, Checks& checks) {
    const auto difference = [](std::uint64_t a, std::uint64_t b) {
        return a > b ? a - b : b - a;
    };
    checks.check(smaller.completed && larger.completed, name + ": both size runs must complete");
    const auto delta = difference(smaller.io.max_extent, larger.io.max_extent);
    checks.check(delta <= kFlatTolerance,
        name + ": extent is not flat in bars: smaller="
        + std::to_string(smaller.io.max_extent) + " larger="
        + std::to_string(larger.io.max_extent) + " delta=" + std::to_string(delta));
    const std::array<const char*, kRoles> names{"trade", "dependency", "exit"};
    for (std::size_t role = 0; role < kRoles; ++role) {
        const auto role_delta = difference(smaller.io.roles[role].extent,
                                            larger.io.roles[role].extent);
        checks.check(role_delta <= kFlatTolerance, name + ": " + names[role]
            + " extent is not flat in bars; delta=" + std::to_string(role_delta));
    }
    std::printf("M1 comparison workload=%s smaller_extent_bytes=%llu larger_extent_bytes=%llu "
                "delta_bytes=%llu tolerance_bytes=%llu\n", name.c_str(),
        static_cast<unsigned long long>(smaller.io.max_extent),
        static_cast<unsigned long long>(larger.io.max_extent),
        static_cast<unsigned long long>(delta), static_cast<unsigned long long>(kFlatTolerance));
}

std::vector<Bar> fixture_tape(std::size_t count) {
    std::vector<Bar> bars;
    bars.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        bars.push_back({100.0, 102.0, 99.0, 100.0, 1000.0,
                        kStart + static_cast<std::int64_t>(i) * kMinute});
    return bars;
}

void fixture_workloads(Checks& checks) {
    struct Case { const char* name; int which; std::size_t bars; std::size_t ratio; };
    // Exact fixture schedules and starting sizes from test_arena_bound.py:612-621.
    // successor-replacements (no matching core) and large-curve (issues no orders)
    // are left out: they never write the command store, so their mandatory
    // "writes observed" check can only fail and would hide the real numbers.
    const std::array<Case, 6> cases{{
        {"orders-every-bar", 1, 32, 10},
        {"many-computed-ids", 2, 512, 10},
        {"long-id-components", 3, 32, 10},
        {"partial-exit-ladders", 4, 64, 10},
        {"pyramiding-at-limit", 5, 32, 10},
        {"selected-window", 7, 512, 10}
    }};
    for (const auto& workload : cases) {
        const auto run = [&](std::size_t count) {
            const auto bars = fixture_tape(count);
            return measure(std::string(workload.name) + "-n" + std::to_string(count), bars,
                [] { return std::make_unique<m1_fixture::ArenaBoundStrategy>(); },
                [&](auto& host) {
                    host.set_input("case", std::to_string(workload.which));
                    if (workload.which == 7) {
                        const pf_selected_window_config_v1 window{
                            sizeof(pf_selected_window_config_v1), 1,
                            kStart + static_cast<std::int64_t>(count / 4) * kMinute,
                            kStart + static_cast<std::int64_t>(3 * count / 4) * kMinute
                        };
                        require(strategy_set_selected_window_v1(&host, &window) == 0,
                                "fixture selected-window declaration refused");
                    }
                },
                [&](const auto& host, Checks& audit) {
                    if (workload.which != 0 && workload.which != 6) {
                        audit.check(host.entry_calls() > 0 && host.exit_calls() > 0,
                                    std::string(workload.name) + ": fixture commands did not execute");
                    }
                    if (workload.which == 1)
                        audit.check(host.entry_calls() == 8 * count,
                                    "orders-every-bar: exact eight entries per script bar");
                    if (workload.which == 4)
                        audit.check(host.closed_trade_count() > host.entry_calls(),
                                    "partial-exit-ladders: no extra partial trade slices");
                    if (workload.which == 5)
                        audit.check(host.peak_lots() == 8,
                                    "pyramiding-at-limit: did not reach the FIXTURE's eight-lot cap");
                    if (workload.which == 6)
                        audit.check(host.replacements() == count * 512,
                                    "successor-replacements: replacement chain was not exercised");
                }, checks);
        };
        const auto smaller = run(workload.bars);
        const auto larger = run(workload.bars * workload.ratio);
        compare_sizes(workload.name, smaller, larger, checks);
    }
}

class PopulationStress final : public source::PineStrategyHost {
public:
    explicit PopulationStress(std::size_t width) : width_(width) {
        source::PineStrategyConfig config;
        config.initial_capital = 1.0e12;
        config.default_qty_value = 16.0;
        config.pyramiding = std::numeric_limits<int>::max();
        config.process_orders_on_close = true;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.25);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
    }
    void on_source_bar(const Bar&) override {
        const auto phase = pine_bar_index() % 8;
        peak_lots = std::max<std::uint64_t>(peak_lots, physical_position().lot_count);
        if (phase == 1) {
            require(physical_position().lot_count == width_,
                    "population stress did not reach its requested live lot population");
            ++population_witnesses;
        }
        if (phase == 0) {
            for (std::size_t i = 0; i < width_; ++i) {
                const auto id = "stress-entry-" + std::to_string(i);
                strategy_entry(id, true, kNa, kNa, 16.0);
                // Pending limit and stop entries, deliberately unreachable.
                strategy_entry("pending-limit-" + std::to_string(i), true, 1.0, kNa, 1.0);
                strategy_entry("pending-stop-" + std::to_string(i), true, kNa, 1000.0, 1.0);
            }
        }
        if (phase >= 1 && phase <= 4) {
            for (std::size_t i = 0; i < width_; ++i) {
                const auto id = "stress-entry-" + std::to_string(i);
                for (int part = 0; part < 3; ++part) {
                    const auto exit = "stress-exit-" + std::to_string(part) + ":" + id;
                    if (phase == 3) strategy_cancel(exit);
                    strategy_exit(exit, id, 120.0 + phase + part, 80.0 - phase,
                                  40.0, 20.0, kNa, 25.0);
                    ++brackets;
                }
                if (phase == 2) {
                    strategy_cancel("pending-limit-" + std::to_string(i));
                    strategy_entry("pending-limit-" + std::to_string(i), true, 2.0, kNa, 1.0);
                    ++replacements;
                }
                if (phase == 4) strategy_close(id, "partial", 1.0);
            }
        }
        if (phase == 6) strategy_close_all();
        if (phase == 7) strategy_cancel_all();
    }
    std::uint64_t peak_lots = 0;
    std::uint64_t population_witnesses = 0;
    std::uint64_t brackets = 0;
    std::uint64_t replacements = 0;
private:
    std::size_t width_;
};

class DependencyStress final : public source::PineStrategyHost {
public:
    explicit DependencyStress(std::size_t width) : width_(width) {
        source::PineStrategyConfig config;
        config.initial_capital = 1.0e12;
        config.pyramiding = std::numeric_limits<int>::max();
        config.process_orders_on_close = true;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.25);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_entry("trade-witness", true, kNa, kNa, 2.0);
            no::Request parent{no::Transact{1.0}, "waiting-parent", ""};
            parent.trigger = no::Limit{1.0};
            parent_ = put(parent);
            for (std::size_t i = 0; i < width_; ++i) {
                no::Request child{no::Reduce{no::ExplicitUnits{1.0}}, "waiting-child", ""};
                child.owner = no::WaitForApplied{parent_};
                put(child);
            }
            return;
        }
        if (pine_bar_index() == 1) {
            const auto& consumer = NativeExecutionConsumer::bound(*this);
            pending_witness = consumer.request_core().live().size();
            require(pending_witness == width_ + 1,
                    "dependency stress must hold its entire fanout live, spilled rows included");
            cancel(parent_);
            queued_witness = NativeExecutionConsumerProbe::queued(consumer);
            require(queued_witness == width_, "the real dependency drain did not enqueue every child");
            strategy_exit("trade-exit", "trade-witness", 101.0, 98.0);
        }
        if (pine_bar_index() == 3) strategy_close_all();
    }
    std::uint64_t pending_witness = 0;
    std::uint64_t queued_witness = 0;
private:
    no::RequestHandle put(const no::Request& request) {
        const auto accepted = submit(request);
        require(accepted.status == no::SubmitStatus::Accepted && accepted.handle.has_value(),
                "dependency stress native request was not accepted");
        return *accepted.handle;
    }
    std::size_t width_;
    no::RequestHandle parent_{};
};

class MarginStress final : public source::PineStrategyHost {
public:
    MarginStress() {
        source::PineStrategyConfig config;
        config.initial_capital = 1000.0;
        config.default_qty_value = 10.0;
        config.margin_short = 100.0;
        config.pyramiding = std::numeric_limits<int>::max();
        config.process_orders_on_close = true;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.01);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("margin-short", false, kNa, kNa, 10.0);
        if (pine_bar_index() == 1)
            strategy_exit("margin-bracket", "margin-short", 90.0, 200.0,
                          kNa, kNa, kNa, 25.0);
        if (pine_bar_index() == 3) strategy_close_all();
    }
};

void stress_workloads(Checks& checks) {
    std::optional<Result> previous_population;
    // These are explicit test inputs, NOT alleged engine limits. INT_MAX is
    // configured, but only the measured requested width is claimed reached.
    for (const std::size_t width : {std::size_t{16}, std::size_t{64}, std::size_t{256}}) {
        const std::string name = "stress-pyramid-bracket-churn-width" + std::to_string(width);
        const auto run = [&](std::size_t count) {
            return measure(name + "-n" + std::to_string(count), fixture_tape(count),
                [=] { return std::make_unique<PopulationStress>(width); }, [](auto&) {},
                [=](const auto& host, Checks& audit) {
                    audit.check(host.peak_lots == width && host.population_witnesses == count / 8,
                                name + ": live lot witness did not match the requested population");
                    audit.check(host.brackets > 0 && host.replacements > 0,
                                name + ": brackets/cancel-and-replace churn did not execute");
                    audit.check(host.closed_trade_count() > width * (count / 8),
                                name + ": partial close slices were not produced");
                }, checks);
        };
        const auto smaller = run(32);
        const auto larger = run(320);
        compare_sizes(name, smaller, larger, checks);
        if (previous_population)
            checks.check(smaller.io.max_extent > previous_population->io.max_extent,
                         name + ": increasing the live population did not increase measured extent");
        previous_population = smaller;
    }
    for (const std::size_t width : {std::size_t{64}, std::size_t{4097}}) {
        const std::string name = "stress-dependency-fanout-width" + std::to_string(width);
        const auto run = [&](std::size_t count) {
            return measure(name + "-n" + std::to_string(count), fixture_tape(count),
                [=] { return std::make_unique<DependencyStress>(width); }, [](auto&) {},
                [=](const auto& host, Checks& audit) {
                    audit.check(host.pending_witness == width + 1 && host.queued_witness == width,
                                name + ": pending/queue population was not witnessed");
                    std::printf("M1 population workload=%s pending_witness=%llu queued_witness=%llu "
                                "configured_pyramiding=%d engine_live_limit=unavailable\n",
                        name.c_str(), static_cast<unsigned long long>(host.pending_witness),
                        static_cast<unsigned long long>(host.queued_witness),
                        std::numeric_limits<int>::max());
                }, checks);
        };
        const auto smaller = run(8);
        const auto larger = run(80);
        compare_sizes(name, smaller, larger, checks);
        checks.check(smaller.io.roles[kDependency].extent > 0
                     && larger.io.roles[kDependency].extent > 0,
                     name + ": no real dependency-store writes were observed");
    }
    const auto margin_run = [&](std::size_t count) {
        auto bars = fixture_tape(count);
        bars[1] = {100.0, 105.0, 99.5, 104.0, 1000.0, kStart + kMinute};
        bars[2] = {104.0, 130.0, 103.0, 128.0, 1000.0, kStart + 2 * kMinute};
        bars[3] = {128.0, 140.0, 127.0, 139.0, 1000.0, kStart + 3 * kMinute};
        return measure("stress-margin-short-n" + std::to_string(count), bars,
            [] { return std::make_unique<MarginStress>(); }, [](auto&) {},
            [](const auto& host, Checks& audit) {
                std::size_t margin_rows = 0;
                for (std::size_t i = 0; i < host.closed_trade_count(); ++i) {
                    const auto row = host.closed_trade(i);
                    if (row.exit_id == "__margin_call__" && row.exit_comment == "Margin call")
                        ++margin_rows;
                }
                audit.check(margin_rows > 0, "stress-margin-short: no actual margin-call exit");
            }, checks);
    };
    const auto smaller = margin_run(8);
    const auto larger = margin_run(80);
    compare_sizes("stress-margin-short", smaller, larger, checks);
}

// Read only the existing canonical feed. No repo script, derived-feed writes,
// external process, runtime compiler, download or generated fixture is needed.
std::vector<Bar> corpus_tape(std::size_t count) {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    std::ifstream input(root / "corpus/data/ohlcv_ETH-USDT-USDT_1m.csv");
    require(input.good(), "open canonical corpus ETH one-minute feed");
    std::string line;
    require(static_cast<bool>(std::getline(input, line))
                && line == "timestamp,open,high,low,close,volume",
            "canonical corpus feed header changed");
    std::vector<Bar> result;
    result.reserve(count);
    Bar aggregate{};
    bool have = false;
    std::int64_t previous = -1;
    constexpr std::int64_t interval = 15 * kMinute;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        char* end = nullptr;
        const char* cursor = line.c_str();
        const auto timestamp = std::strtoll(cursor, &end, 10);
        require(end != cursor && *end == ',', "parse canonical corpus timestamp");
        cursor = end + 1;
        std::array<double, 5> values{};
        for (std::size_t field = 0; field < values.size(); ++field) {
            values[field] = std::strtod(cursor, &end);
            require(end != cursor && std::isfinite(values[field]), "parse canonical corpus OHLCV");
            if (field + 1 < values.size()) {
                require(*end == ',', "canonical corpus OHLCV separator");
                cursor = end + 1;
            } else require(*end == '\0' || *end == '\r', "canonical corpus OHLCV suffix");
        }
        require(timestamp > previous, "canonical corpus timestamps must increase");
        previous = timestamp;
        const auto bucket = timestamp / interval * interval;
        if (have && bucket != aggregate.timestamp) {
            result.push_back(aggregate);
            if (result.size() == count) break;
            have = false;
        }
        if (!have) {
            aggregate = {values[0], values[1], values[2], values[3], values[4], bucket};
            have = true;
        } else {
            aggregate.high = std::max(aggregate.high, values[1]);
            aggregate.low = std::min(aggregate.low, values[2]);
            aggregate.close = values[3];
            aggregate.volume += values[4];
        }
    }
    require(result.size() == count, "canonical feed has too few complete 15-minute bars");
    return result;
}

template<class Host>
void corpus_workload(const char* name, const std::vector<Bar>& tape, Checks& checks) {
    const auto run = [&](std::size_t count) {
        const std::vector<Bar> bars(tape.begin(), tape.begin() + count);
        return measure(std::string(name) + "-n" + std::to_string(count), bars,
            [] { return std::make_unique<Host>(); },
            [](auto& host) {
                host.set_syminfo_mintick(0.01);
                host.set_syminfo_timezone("UTC");
                host.set_syminfo_session("24x7");
            },
            [](const auto& host, Checks& audit) {
                std::size_t bracket_rows = 0;
                for (std::size_t i = 0; i < host.closed_trade_count(); ++i)
                    if (host.closed_trade(i).exit_from_bracket) ++bracket_rows;
                audit.check(bracket_rows > 0, "corpus strategy produced no real bracket exits");
            }, checks, "15");
    };
    const auto smaller = run(10'000);
    const auto larger = run(100'000);
    compare_sizes(name, smaller, larger, checks);
}
} // namespace

int main() {
    Checks checks;
    print_geometry(detail::scratch_min_page_size);
    // These widths come from the actual codec/store types, not expected
    // extents or a measurement-dependent capacity formula.
    std::printf("M1 layout exit_key_bytes=%zu journal_key_bytes=%zu dependency_key_bytes=%zu "
                "dependency_pending_value_bytes=%zu trade_key_bytes=%zu trade_header_bytes=%zu\n",
        sd::exit_scratch_key_bytes, sizeof(std::uint64_t),
        sizeof(std::array<std::uint64_t, 4>), sizeof(std::array<std::uint64_t, 2>),
        sizeof(std::array<std::uint64_t, 4>), sizeof(std::array<std::uint64_t, 23>));
    std::printf("M1 derivation Kc=unbounded K=unavailable three_K=unavailable "
                "three_K_plus_three_chunks=unavailable extra_three_chunks_bytes=%llu "
                "missing=live_population,text_bytes,vector_membership,page_pool_peak\n",
        static_cast<unsigned long long>(kRoles * detail::ScratchArena::kChunkBytes));
    std::fflush(stdout);
    // Do not terminate after the first failed flatness assertion: the single
    // lab invocation must still emit measurements for the remaining workloads.
    try { fixture_workloads(checks); }
    catch (const std::exception& error) { checks.check(false, std::string("fixtures: ") + error.what()); }
    try { stress_workloads(checks); }
    catch (const std::exception& error) { checks.check(false, std::string("stress: ") + error.what()); }
    try {
        const auto tape = corpus_tape(100'000);
        corpus_workload<m1_compass::GeneratedStrategy>(
            "corpus-bracket-compass-partial-ladder-01", tape, checks);
        corpus_workload<m1_scalping::GeneratedStrategy>(
            "corpus-composite-scalping-integration-01", tape, checks);
        corpus_workload<m1_trendmaster::GeneratedStrategy>(
            "corpus-composite-trendmaster-integration-01", tape, checks);
    } catch (const std::exception& error) {
        checks.check(false, std::string("corpus: ") + error.what());
    }
    std::printf("M1 result failures=%zu fixed_K_proven=no\n", checks.failures);
    return checks.failures == 0 ? 0 : 1;
}
