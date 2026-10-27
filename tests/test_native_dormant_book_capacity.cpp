// REV-CX K4. Public batch run with unique ids at the requested long horizon,
// twice on the same host (HPO reuse). No lifetime order-count cap is raised.
// Fixture sizes and cost expectations are layout estimates from source, not
// measured. Only the OHLCV tape scales; the observer/manifest are fixed size.
#include "../src/native_execution_consumer.hpp"
#include "../src/command_scratch_production.hpp"

#include <pineforge/detail/scratch_slab.hpp>
#include <pineforge/native_host.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;
namespace detail = pineforge::detail;
using Binding = NativeExecutionConsumer::CommandScratchBinding;
constexpr Binding book_binding = Binding::Book; // Intentional base diagnostic.
constexpr std::size_t bars_count = 200000;
constexpr std::int64_t start = 1736121600000LL;

void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

std::array<std::uint64_t, 11> manifest_words(const detail::ScratchManifest& m) {
    return {{m.K_q, m.B_q, m.K_e, m.B_e, m.K_c, m.B_c,
             m.U, m.B_U, m.V, m.B_V, m.R}};
}

struct Io {
    std::uint64_t errors = 0;
    std::uint64_t writes = 0;
    static void charge(void* context, const detail::ScratchIoCharge& c) noexcept {
        auto& self = *static_cast<Io*>(context);
        if (c.status != detail::ScratchStatus::ok) ++self.errors;
        if (c.operation == detail::ScratchIoOperation::write_page) ++self.writes;
    }
};

struct Observer {
    detail::CommandScratchByteCap previous_cap;
    detail::ScratchIoObserver previous_observer;
    explicit Observer(Io& io)
        : previous_cap(detail::set_command_scratch_byte_cap(std::uint64_t{0})),
          previous_observer(detail::set_command_scratch_io_observer({&io, Io::charge})) {}
    ~Observer() {
        detail::set_command_scratch_io_observer(previous_observer);
        detail::set_command_scratch_byte_cap(std::move(previous_cap));
    }
};

class Host final : public NativeStrategyHost {
public:
    std::array<std::uint64_t, 11> manifest{};
    std::size_t placed = 0;
    std::size_t late_checks = 0;
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
    void prepare_native_begin(const NativeBeginArgs& args) override {
        require(!args.is_stream && args.n == static_cast<int>(bars_count),
                "K4 did not use the declared finite batch horizon");
        const auto& slab = scratch_tables_for_run();
        require(static_cast<bool>(slab), "K4 bounded default manifest was not installed");
        manifest = manifest_words(slab->manifest());
        // Hand oracle: the existing finite-batch root reserve is 2 * bars.
        require(slab->manifest().R == 2 * bars_count,
                "K4 finite-batch manifest did not scale with the input bars");
    }
    void on_native_run_begin() override {
        placed = 0;
        late_checks = 0;
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (placed > 4096) {
            require(consumer().fast_points() > 0 && consumer().eager_points() == 0
                        && consumer().fast_path_exits() == 0,
                    "K4 unique-id run left the fast path");
            require(consumer().dormant_rows() > 0,
                    "K4 long run did not join dormant rows");
            require(consumer().grid_points() == 0,
                    "K4 unquantized long run used a price grid");
            ++late_checks;
        }
        // Labels are distinct and every accepted incarnation is distinct;
        // neither a modulo id nor a workload literal reaches production.
        no::Request r{no::Transact{(placed & 1) ? -1.0 : 1.0},
                      "unique-" + std::to_string(placed), ""};
        const double offset = static_cast<double>(placed) / static_cast<double>(bars_count);
        const bool buy = (placed & 1) == 0;
        const bool stop = (placed & 2) != 0;
        const double level = buy == stop ? 200 + offset : 50 - offset;
        if (stop) r.trigger = no::Stop{level};
        else r.trigger = no::Limit{level};
        require(submit(r).handle.has_value(), "K4 long unique-id placement refused");
        ++placed;
    }
};
} // namespace

int main() {
    try {
        Io io;
        Observer observer(io);
        Host h;
        pf_scratch_tables_v1 tables{};
        tables.struct_size = static_cast<std::uint32_t>(sizeof(tables));
        tables.version = 1;
        tables.backend = PF_SCRATCH_TABLE_BOUNDED_MEMORY;
        require(h.set_scratch_tables_v1(&tables) == 0, "K4 bounded policy refused");
        NativeRunSpec s;
        s.identity = {"c3-dormant-capacity", 1};
        s.input_tf = "5"; s.script_tf = "5";
        s.tickerid = "TEST:C3"; s.timezone = "UTC"; s.session = "24x7";
        s.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
        s.initial_capital = 1.0e9; s.point_value = 1; s.account_fx = 1;
        s.report_policy = NativeReportPolicy::HostRecorded;
        require(h.configure_native(s).status == NativeSetupStatus::Applied,
                "K4 configure refused");
        std::vector<Bar> bars(bars_count);
        for (std::size_t i = 0; i < bars.size(); ++i)
            bars[i] = {100, 100, 100, 100, 1,
                       start + static_cast<std::int64_t>(i) * 300000};
        std::array<std::uint64_t, 11> first_manifest{};
        std::uint64_t first_book = 0;
        for (int trial = 0; trial < 2; ++trial) {
            h.run(bars.data(), static_cast<int>(bars.size()));
            require(h.native_state().kind == NativeLifecycleKind::Completed, h.last_error().c_str());
            require(h.placed == bars.size() && h.late_checks == bars.size() - 4097,
                    "K4 long horizon did not execute fully");
            require(h.consumer().request_core().live_size() == bars.size()
                        && h.consumer().request_core().book_spilled() && h.trade_count() == 0,
                    "K4 never-filled long book changed membership or traded");
            require(h.consumer().eager_points() == 0 && h.consumer().fast_path_exits() == 0,
                    "K4 final point poisoned or latched the fast path");
            require(h.consumer().index_poisonings() == 0
                        && h.consumer().dormant_setup_failures() == 0,
                    "K4 long Book run reported index poison or setup degradation");
            require(io.errors == 0 && io.writes > 0,
                    "K4 Book backing refused work or was never written");
            const auto book = h.consumer().released_command_scratch(book_binding);
            require(book && book->status == detail::ScratchStatus::ok,
                    "K4 long Book index did not release a healthy snapshot");
            if (trial == 0) {
                first_manifest = h.manifest;
                first_book = book->digest;
            } else {
                require(h.manifest == first_manifest,
                        "K4 same horizon produced different manifest rows across HPO trials");
                require(book->digest == first_book,
                        "K4 identical HPO trials produced different Book index contents");
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "test_native_dormant_book_capacity: %s\n", e.what());
        return 1;
    }
}
