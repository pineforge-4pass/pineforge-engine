#include "../src/native_cohort_target_work.hpp"
#include "native_cohort_target_fixture.hpp"

namespace {
using namespace cohort_target_test;

void read_after_mutation(Host& h, const no::LiveRequest& request, bool indexed) {
    const std::size_t L = h.native_open_lots(100).size();
    const auto expected = reference(h, request);
#ifdef PINEFORGE_COHORT_TARGET_OBSERVER
    Counts counts;
    no::TargetObservation got;
    {
        work::Scope counter({&counts, Counts::charge});
        got = Probe::read(h.consumer(), h, request);
    }
    print(counts, L, 1, "invalidation");
    require(counts.get(work::Event::Target) == 1, "mutation row skipped its target read");
    require(counts.get(work::Event::Canonical) == L,
            "mutation did not recompute the physical opening view");
    require(counts.get(work::Event::Contains) == (indexed ? 0 : L),
            "input window used the wrong target reader");
#else
    (void)L; (void)indexed;
    const auto got = Probe::read(h.consumer(), h, request);
#endif
    require(same(got, expected), "mutated cohort target differs from complete reader");
}

void commands_and_fill() {
    Host h;
    std::size_t reads = 0;
    h.calculation = [&](Host& host) {
        if (host.bars != 1) return;
        auto p = populate(host, 5, true);
        const auto witness = live(host, p.closes.front());
        judge(host, witness);
        auto unrelated = put(host, {no::Transact{1}, "unrelated pending", ""});
        read_after_mutation(host, witness, true); ++reads;
        require(host.cancel(unrelated).status == no::CancelStatus::Cancelled, "cancel did not apply");
        read_after_mutation(host, witness, true); ++reads;
        require(same(Probe::cached(host.consumer(), host, witness), reference(host, witness)),
                "small target cache did not select the original roster");
        host.cohort_remove(p.cohorts.front(), p.roots.front());
        read_after_mutation(host, witness, true); ++reads;
        require(Probe::read(host.consumer(), host, witness).openings.empty(),
                "removed origin stayed selected");
        require(same(Probe::cached(host.consumer(), host, witness), reference(host, witness)),
                "roster removal left the small target cache stale");
        // Close a different cohort's physical opening through its public
        // BindOpening interface, keeping a nonflat position and live siblings.
        no::Request reduce{no::Reduce{no::ExplicitUnits{1}}, "scoped fill", ""};
        reduce.owner = no::BindOpening{p.openings.back(), host.native_open_lots(100).front().cycle};
        const auto target = put(host, reduce);
        judge(host, witness); // Warm AFTER submit, immediately before settlement.
        require(apply(host, target).closed_units == 1, "scoped close did not settle");
        const auto closed_cohort = live(host, p.closes.back());
        read_after_mutation(host, closed_cohort, true); ++reads;
        require(Probe::read(host.consumer(), host, closed_cohort).openings.empty(),
                "closed opening remained in the target");
        require(host.physical_position().lot_count + 1 == p.openings.size(),
                "fill witness did not remove exactly one physical lot");
    };
    run(h, spec(), tape(1));
    require(reads == 4 && h.fills > 0, "mutation schedule was not executed");
}

void reuse() {
    Host h;
    Population p;
    std::size_t begins = 0;
    h.beginning = [&](Host& host) {
        ++begins;
        require(!Probe::valid(host.consumer()), "index survived public begin");
        require(host.physical_position().lot_count == 0, "begin retained a physical lot");
    };
    h.calculation = [&](Host& host) {
        if (host.bars == 1) p = populate(host, 3, true);
        if (host.bars == 2) {
            judge(host, live(host, p.closes.front()));
#ifdef PINEFORGE_HAS_COHORT_OPENING_INDEX
            require(Probe::valid(host.consumer()), "reuse row never populated the index");
#endif
        }
    };
    run(h, spec(1), tape(2));
    run(h, spec(2), tape(2));
    require(begins == 2, "public host was not reused");
}

void close_one(Host& h, const Population& p) {
    no::Request r{no::Reduce{no::ExplicitUnits{1}}, "window close", ""};
    r.owner = no::BindOpening{p.openings.front(), h.native_open_lots(100).front().cycle};
    require(apply(h, put(h, r)).closed_units == 1, "window did not settle a real close");
}

void intrabar() {
    Host h;
    std::size_t checked = 0;
    h.calculation = [&](Host& host) {
        if (host.bars != 1) return;
        const auto p = populate(host, 4, true);
        auto witness = live(host, p.closes.front());
        read_after_mutation(host, witness, false);
        read_after_mutation(host, witness, false); // Warm read MUST still bypass.
        close_one(host, p);
        read_after_mutation(host, witness, false);
        require(Probe::read(host.consumer(), host, witness).openings.empty(),
                "intrabar fill kept a stale target");
        require(!Probe::valid(host.consumer()), "intrabar input populated an ordinary-point index");
        ++checked;
    };
    auto s = spec(); s.intrabar.value = IntrabarPath::synthesized{};
    run(h, s, tape(2));
    require(checked == 1, "intrabar stale window was not reached");
}

void observed_ticks() {
    Host h;
    std::size_t checked = 0;
    h.tick = [&](Host& host) {
        if (checked != 0) return;
        const auto p = populate(host, 4, true);
        const auto witness = live(host, p.closes.front());
        read_after_mutation(host, witness, false);
        read_after_mutation(host, witness, false);
        close_one(host, p);
        read_after_mutation(host, witness, false);
        require(Probe::read(host.consumer(), host, witness).openings.empty(),
                "observed-tick fill kept a stale target");
        require(!Probe::valid(host.consumer()), "observed ticks populated the ordinary-point index");
        ++checked;
    };
    configure(h, spec());
    const Bar warmup{100, 100, 100, 100, 1, start - 60000};
    require(h.stream_begin(&warmup, 1, "1", "1"), "tick stream begin failed");
    if (!h.stream_push_tick({start, 1, 100, 1}))
        throw std::runtime_error("observed print failed: " + h.last_error());
    require(h.stream_end(false), "tick stream end failed");
    require(checked == 1 && h.fills > 0, "tick stale window was not exercised");
}

void applied_host_write() {
    Host h;
    std::optional<no::LiveRequest> witness;
    std::size_t retired = 0, after = 0;
    h.calculation = [&](Host& host) {
        if (host.bars == 1) {
            const auto tiny = put(host, {no::Transact{0x1p-40}, "small physical lot", ""});
            const auto cohort = host.cohort_open();
            host.cohort_add(cohort, tiny);
            (void)apply(host, tiny);
            (void)populate(host, 2, true);
            witness = live(host, put(host, close(host, cohort, "small lot close")));
            require(reference(host, *witness).openings.size() == 1, "small lot was never opened");
            judge(host, *witness); // Populate before the deferred notification.
#ifdef PINEFORGE_HAS_COHORT_OPENING_INDEX
            require(Probe::valid(host.consumer()), "host-write row never populated the index");
#endif
        } else {
            judge(host, *witness);
            require(Probe::read(host.consumer(), host, *witness).openings.empty(),
                    "index reused a physical lot retired by the applied host");
            ++after;
        }
    };
    h.notification = [&](Host& host, const no::ExecutionAppliedEvent&) {
        if (retired) return;
        // Hand arithmetic: 2^-40 < 2^-35, while both other lots have 1 unit.
        // This host write therefore retires exactly the small opening.
        const auto before = host.physical_position().lot_count;
        host.retire_lots_at_most(0x1p-35);
        require(host.physical_position().lot_count + 1 == before, "host writer was not reached");
        require(!Probe::valid(host.consumer()), "host writer kept the pre-callback generation valid");
        judge(host, *witness);
        require(Probe::read(host.consumer(), host, *witness).openings.empty(),
                "applied callback served its pre-write target");
        ++retired;
    };
    run(h, spec(), tape(2));
    require(retired == 1 && after == 1, "host-write boundary was not observed on both sides");
}

enum class Fault { Settlement, DirectInstall, PreparedInstall };
void failed_settlement_window(Fault fault) {
    Host h;
    h.consumer().set_direct_mutation(fault != Fault::PreparedInstall);
    h.consumer().set_settlement_carry(false);
    std::optional<no::LiveRequest> witness;
    no::RequestHandle closing;
    std::uint64_t before = 0;
    std::size_t injected = 0;
    bool armed = false;
    h.precommit = [&](Host& host, const NativePrecommitView& view) {
        if (!armed || view.target != closing) return NativePrecommitVerdict::Admit;
        require(view.settlement_readiness == execution::Status::Applied,
                "fault setup never reached a successful real preview");
        judge(host, *witness);
#ifdef PINEFORGE_HAS_COHORT_OPENING_INDEX
        require(Probe::valid(host.consumer()), "failure window did not start with a populated index");
#endif
        before = Probe::settlements(host.consumer());
        if (fault == Fault::Settlement) host.invalidate_accounting();
        else Probe::interrupt_install(host.consumer(), closing, fault == Fault::DirectInstall);
        ++injected;
        return NativePrecommitVerdict::Admit;
    };
    h.calculation = [&](Host& host) {
        const auto p = populate(host, 3, true);
        witness = live(host, p.closes.front());
        closing = put(host, {no::Flatten{}, "faulted close", ""});
        armed = true;
        (void)host.execute_current({closing, NativeCurrentPriceRule::AsPresented});
    };
    configure(h, spec());
    const auto feed = tape(1);
    h.run(feed.data(), static_cast<int>(feed.size()));
    if (injected == 0 && h.native_state().kind == NativeLifecycleKind::Failed)
        throw std::runtime_error("fault setup failed: " + h.last_error());
    require(injected == 1, "fault was not injected at the real precommit boundary");
    require(Probe::settlements(h.consumer()) == before + 1, "settlement site was not reached");
    const auto state = h.native_state();
    require(state.kind == NativeLifecycleKind::Failed, "failure window did not latch failure");
    require(state.failure.operation == NativeFailureOperation::Settlement, "wrong failure operation");
    if (fault == Fault::Settlement) {
        require(state.failure.code == NativeFailureCode::SettlementFailure
                && state.failure.discriminator == static_cast<std::uint32_t>(execution::Status::InvalidAccounting),
                "real settlement did not return InvalidAccounting");
    } else {
        require(state.failure.code == NativeFailureCode::Contract
                && state.failure.discriminator == static_cast<std::uint32_t>(no::InstallError::StalePreparation),
                "real post-settlement install did not reject stale state");
        require(h.physical_position().lot_count == 0, "failed install did not follow a real settlement");
    }
    require(!Probe::valid(h.consumer()), "index survived a failed settlement/install window");
    // The public failure latch is also checked: no target read follows.
#ifdef PINEFORGE_COHORT_TARGET_OBSERVER
    Counts after;
    work::Scope counter({&after, Counts::charge});
#endif
    const auto result = h.inspect_current_execution({closing, NativeCurrentPriceRule::AsPresented});
    require(result.refusal == NativeCurrentRefusal::NoExecutionContext, "failed run resumed reading");
#ifdef PINEFORGE_COHORT_TARGET_OBSERVER
    require(after.get(work::Event::Target) == 0, "public failure latch allowed a target read");
#endif
}
}

int main() {
    row("installed commands cancel roster and fill", commands_and_fill);
    row("index does not survive begin", reuse);
    row("intrabar fill window", intrabar);
    row("observed tick fill window", observed_ticks);
    row("applied host retires a physical opening", applied_host_write);
    row("non-applied settlement window", [] { failed_settlement_window(Fault::Settlement); });
    row("direct install fails after settlement", [] { failed_settlement_window(Fault::DirectInstall); });
    row("prepared install fails after settlement", [] { failed_settlement_window(Fault::PreparedInstall); });
    return failures ? 1 : 0;
}
