// #981: no stock producer gives these readers external membership. Install
// the same accepted close in a C host's core and a source adapter's queue.
#include <pineforge/native_c_api.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/native_execution_consumer.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <variant>
#include <vector>

namespace {
namespace no = pineforge::native_order;
namespace source = pineforge::source;
using Adapter = source::PineExecutionAdapter;
using Status = pineforge::detail::ScratchStatus;
using Handle = no::RequestHandle;

// The same explicit-instantiation seam as test_origin_roster_retirement.
// Only fixture setup and observation need private access.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Core { friend auto access(Core); };
struct Queue { friend auto access(Queue); };
struct Placements { friend auto access(Placements); };
struct Cohorts { friend auto access(Cohorts); };
struct Gate { friend auto access(Gate); };
struct Sweep { friend auto access(Sweep); };
struct Prefix { friend auto access(Prefix); };
template struct Access<Core, &pineforge::NativeExecutionConsumer::requests_>;
template struct Access<Queue, &Adapter::pending_bracket_legs_>;
template struct Access<Placements, &Adapter::placement_>;
template struct Access<Cohorts, &Adapter::cohorts_by_id_>;
template struct Access<Gate, &Adapter::retirement_gate_>;
template struct Access<Sweep, &Adapter::erase_retired_rows>;
template struct Access<Prefix, &Adapter::retire_origin_prefixes>;

struct PineHost final : source::PineStrategyHost {
    void on_source_bar(const pineforge::Bar&) override {}
};

struct NotCompared {};
struct Checks {
    const char* form;
    bool ok = true;

    void require(const char* field, std::int64_t got, std::int64_t expected) const {
        if (got == expected) return;
        std::cerr << "NOT COMPARED " << form << ' ' << field
                  << ": expected=" << expected << " actual=" << got << '\n';
        throw NotCompared{};
    }
    void check(const char* field, std::int64_t got, std::int64_t expected) {
        if (got == expected) return;
        std::cerr << "FAIL " << form << ' ' << field
                  << ": expected=" << expected << " actual=" << got << '\n';
        ok = false;
    }
};

using Pair = std::array<Handle, 2>;
Status read_pair(void* context, std::uint64_t i, Handle& out) {
    const auto& pair = *static_cast<const Pair*>(context);
    if (i >= pair.size()) return Status::not_found;
    out = pair[i];
    return Status::ok;
}

void source_readers(Checks& c, const no::Request& close, const no::RunIdentity& run) {
    PineHost host;
    Adapter adapter(host);
    auto& source_core = pineforge::NativeExecutionConsumer::bound(host).*access(Core{});
    source_core.reset(run);
    adapter.fixture_use_memory_auxiliary_tables();
    auto& queue = adapter.*access(Queue{});
    queue.emplace_back().request = close;
    const auto population = adapter.retirement_population();
    c.require("queued requests", population.pending, 1);
    c.check("retirement owners", population.owners, 2);

    // The three old rows have no pin except the queued request.
    auto& placements = adapter.*access(Placements{});
    source::PlacementSnapshot old;
    old.family = source::PineOrderFamily::Close;
    for (const auto id : {17u, 29u, 41u})
        c.require("fresh placement", placements.try_emplace(id, old).second, true);
    (adapter.*access(Sweep{}))(pineforge::NativeDecisionContext{});
    const auto& roots = (adapter.*access(Gate{})).current.roots;
    c.check("captured roots", roots.size(), 2);
    c.check("exact captured roots", roots == std::vector<std::uint64_t>{17, 41}, true);
    c.check("placement 17 retained", placements.contains(17), true);
    c.check("placement 41 retained", placements.contains(41), true);
    c.check("unrelated placement 29 retained", placements.contains(29), false);

    // Without placements, only the queued owner can keep either origin.
    for (const auto id : {17u, 41u})
        if (placements.contains(id)) placements.erase(id);
    c.require("remaining placements before prefix retirement", placements.size(), 0);
    auto& cohorts = adapter.*access(Cohorts{});
    auto origin = [&](const char* key, std::uint64_t id) {
        auto row = cohorts.emplace(key, {source_core.cohort_open()});
        c.require("fresh origin roster", row.second, true);
        row.first->second.cycle = 7;
        row.first->second.origins.push_back({run, id});
        c.require("initial origin count", row.first->second.origins.members().size(), 1);
        return row.first;
    };
    const auto first = origin("first", 17);
    const auto other = origin("unrelated", 29);
    const auto last = origin("last", 41);
    (adapter.*access(Prefix{}))();
    c.check("origin 17 retained", first->second.origins.members().size(), 1);
    c.check("origin 41 retained", last->second.origins.members().size(), 1);
    c.check("unrelated origin 29 retained", other->second.origins.members().size(), 0);
}

bool compare(bool external) {
    Checks c{external ? "external" : "inline"};
    pf_native_callbacks_v1 callbacks{};
    callbacks.struct_size = sizeof(callbacks);
    callbacks.version = PF_NATIVE_API_VERSION;
    std::unique_ptr<void, decltype(&strategy_native_host_free)> host(
        strategy_native_host_create_v1(&callbacks), &strategy_native_host_free);
    c.require("C host created", host != nullptr, true);
    auto& core = pineforge::NativeExecutionConsumer::bound(
        *static_cast<pineforge::BacktestEngine*>(host.get())).*access(Core{});
    const no::RunIdentity run{"issue-981", 1};
    core.reset(run);
    Pair pair{{{run, 41}, {run, 17}}}; // Enrollment puts 17 first.
    auto cursor = std::make_shared<no::OpeningInputCursor>(pair.size(), &pair, read_pair);
    no::Request close{no::Reduce{no::ExplicitUnits{1}}, "close", ""};
    no::CommandContext context;
    for (const auto& handle : pair)
        context.openings.push_back({handle, 7, no::PositionNonflat{7, no::Side::Long}, true});
    std::uint64_t incarnation = 100, ordinal = 1;
    const auto submit = [&] {
        ordinal = core.last_ordinal() + 1;
        return core.apply_submit(close, context, incarnation, ordinal).result;
    };

    // Empty membership and both representations together are not working
    // requests. They cannot be substituted for the measured row below.
    for (const bool mixed : {false, true}) {
        no::BindOpenings invalid{{}, 7};
        if (mixed) {
            invalid.openings.assign(pair.begin(), pair.end());
            invalid.external_openings = cursor;
        }
        close.owner = invalid;
        const auto refusal = submit();
        c.require(mixed ? "mixed owner refused" : "empty owner refused",
                  refusal.reason == no::RequestRejectReason::InvalidOwner && !refusal.handle, true);
    }
    c.require("empty C working list", strategy_native_working_len_v1(host.get()), 0);
    pf_native_working_v1 row{};
    row.struct_size = sizeof(row);
    c.require("absent C row", strategy_native_working_get_v1(host.get(), 0, &row),
              PF_NATIVE_E_ARGUMENT);

    no::BindOpenings owner{{}, 7};
    if (external) owner.external_openings = cursor;
    else owner.openings.assign(pair.begin(), pair.end());
    close.owner = owner;
    const auto accepted = submit();
    c.require("kernel accepted", accepted.status == no::SubmitStatus::Accepted, true);
    c.require("accepted incarnation", accepted.handle ? accepted.handle->incarnation : 0, 100);
    const auto* live = core.find_live(*accepted.handle);
    c.require("live definition", live && live->definition, true);
    const auto definition = live->definition;
    const auto* held = std::get_if<no::BindOpenings>(&definition->request.owner);
    c.require("kernel owner is BindOpenings", held != nullptr, true);
    c.require("inline members", held->openings.size(), external ? 0 : 2);
    c.require("external storage", held->external_openings != nullptr, external);
    c.require("kernel members", no::opening_count(held->openings, held->external_openings.get()), 2);
    for (unsigned i = 0; i < 2; ++i) {
        Handle got;
        const auto status = no::opening_at(held->openings, held->external_openings.get(), i, got);
        c.require("kernel read status", static_cast<int>(status), static_cast<int>(Status::ok));
        c.require("kernel member run", got.run == run, true);
        c.require("kernel member incarnation", got.incarnation, i == 0 ? 17 : 41);
    }
    c.require("C working rows", strategy_native_working_len_v1(host.get()), 1);
    c.require("C getter", strategy_native_working_get_v1(host.get(), 0, &row), PF_NATIVE_OK);
    c.require("C row incarnation", row.incarnation, 100);
    c.require("C owner tag", row.owner, PF_NATIVE_OWNER_BIND_OPENINGS);
    c.require("C owner cycle", row.owner_cycle, 7);
    c.check("C owner_n", row.owner_n, 2);
    c.check("C owner_handle", row.owner_handle, 17);
    for (const auto size : {PF_NATIVE_WORKING_V1_BASE_SIZE, PF_NATIVE_WORKING_V1_ARM_SIZE}) {
        pf_native_working_v1 prefix{};
        prefix.struct_size = size;
        prefix.owner_handle = 29;
        c.require("older C getter", strategy_native_working_get_v1(host.get(), 0, &prefix), PF_NATIVE_OK);
        c.require("older C row incarnation", prefix.incarnation, 100);
        c.require("older C owner tag", prefix.owner, PF_NATIVE_OWNER_BIND_OPENINGS);
        c.check("older C row length", prefix.struct_size, size);
        c.check("untouched relation tail", prefix.owner_handle, 29);
    }
    source_readers(c, definition->request, run);
    return c.ok;
}
} // namespace

int main() {
    try {
        const bool inline_ok = compare(false);
        const bool external_ok = compare(true);
        if (!inline_ok || !external_ok) return 1;
        std::cout << "PASS inline and external: C owner_n=2 owner_handle=17 cycle=7; "
                     "retirement counted and kept 17,41 and released 29\n";
        return 0;
    } catch (const NotCompared&) {
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "NOT COMPARED reader fixture: expected=completed actual=" << error.what() << '\n';
        return 2;
    }
}
