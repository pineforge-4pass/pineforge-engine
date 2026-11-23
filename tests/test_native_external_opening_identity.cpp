// #976: enroll a close with explicit observations in the host's order core.
// Vary its accepted definition and authority independently to test the
// continuation hash and NetOfSiblings sizing against inline controls.
#include "../src/native_execution_consumer.hpp"

#include <cstdio>
#include <iostream>
#include <new>

namespace pineforge {
inline namespace engine_script_run_v20 {
struct NativeExecutionConsumerProbe {
    static native_order::WorkingRequestCore& core(NativeExecutionConsumer& c) {
        return c.requests_;
    }
    static void committed(NativeExecutionConsumer& c, native_order::EventRange range) {
        c.note_committed_events(range);
    }
    static std::optional<double> size(const NativeStrategyHost& host,
                                      const native_order::LiveRequest& net) {
        NativeExecutionTermsFacts facts{};
        facts.scope_exposure_units = 10;
        return NativeExecutionConsumer::bound(host).resolve_sized_units(host, net, facts);
    }
};
}
}

namespace {
using namespace pineforge;
namespace no = native_order;
using Probe = NativeExecutionConsumerProbe;
using Rows = std::vector<no::RequestHandle>;
using Status = detail::ScratchStatus;
constexpr std::int64_t cycle = 7;
int failures = 0;
struct NotCompared {};

template<class A, class B>
void require(const char* row, const A& actual, const B& expected) {
    if (actual == expected) return;
    std::cerr << "NOT COMPARED " << row << ": actual=" << actual << " expected=" << expected << '\n';
    throw NotCompared{};
}
template<class T>
void check(const char* row, T actual, T expected) {
    if (actual == expected) return;
    std::cerr << "FAIL " << row << ": actual=" << actual << " expected=" << expected << '\n';
    ++failures;
}

enum class Fault { none, io, allocation };
struct Input {
    Rows rows;
    Fault fault = Fault::none;
    unsigned faults = 0;

    no::BindOpenings external() {
        return {{}, cycle, std::make_shared<no::OpeningInputCursor>(rows.size(), this, read)};
    }
    static Status read(void* context, std::uint64_t index, no::RequestHandle& out) {
        auto& input = *static_cast<Input*>(context);
        if (index >= input.rows.size()) return Status::not_found;
        if (index == 1 && input.fault != Fault::none) {
            ++input.faults;
            if (input.fault == Fault::allocation) throw std::bad_alloc{};
            return Status::io_error;
        }
        out = input.rows[index];
        return Status::ok;
    }
};

struct Host final : NativeStrategyHost {
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

struct Book {
    Host host;
    NativeExecutionConsumer& consumer = NativeExecutionConsumer::bound(host);
    no::WorkingRequestCore& core = Probe::core(consumer);
    const no::RunIdentity run{"issue-976", 1};
    const no::BindOpenings a{{{run, 11}, {run, 22}}, cycle, {}};
    const no::BindOpenings b{{{run, 11}, {run, 33}}, cycle, {}};
    no::RequestHandle sibling, net;
    no::BindOpenings frozen;

    no::SubmitResult submit(no::BindOpenings owner, no::ReductionSize size, const Rows& observed) {
        no::CommandContext ctx;
        for (const auto& h : observed)
            ctx.openings.push_back({h, cycle, no::PositionNonflat{cycle, no::Side::Long}, true});
        no::Request request{no::Reduce{size}, "close", ""};
        request.owner = std::move(owner);
        auto incarnation = 100 + core.last_incarnation();
        auto ordinal = 1 + core.last_ordinal();
        auto result = core.apply_submit(request, ctx, incarnation, ordinal);
        Probe::committed(consumer, result.events);
        return result.result;
    }
    no::LiveRequest& row(const no::RequestHandle& h) {
        const auto* live = core.find_live(h);
        require("resident request found", live != nullptr, true);
        require("request handle", live->handle() == h, true);
        // The underlying row is mutable and resident. Only the test replaces
        // its storage spelling, between commands; definitions are copied.
        return const_cast<no::LiveRequest&>(*live);
    }
    Book() {
        NativeRunSpec spec;
        spec.identity = run;
        spec.input_tf = spec.script_tf = "5";
        spec.tickerid = "TEST:976";
        spec.timezone = "UTC+0";
        spec.session = "24x7";
        spec.initial_capital = 1000;
        spec.point_value = spec.account_fx = 1;
        require("configure", int(host.configure_native(spec).status), int(NativeSetupStatus::Applied));
        core.reset(run);

        Input descending{{a.openings.back(), a.openings.front()}};
        auto claim = submit(descending.external(), no::ExplicitUnits{5}, a.openings);
        auto reduce = submit(a, no::ScopeFraction{0.5, no::ScopeClaim::NetOfSiblings}, a.openings);
        for (const auto* result : {&claim, &reduce}) {
            require("close accepted", int(result->status), int(no::SubmitStatus::Accepted));
            require("accepted handle present", result->handle.has_value(), true);
        }
        sibling = *claim.handle;
        net = *reduce.handle;
        require("distinct requests", net != sibling, true);
        require("live requests", core.live().size(), 2U);
        require("spilled rows", core.book_spilled(), false);
        const auto& live = row(sibling);
        require("fixed owner", std::holds_alternative<no::BindOpenings>(live.request().owner), true);
        require("fixed authority", std::holds_alternative<no::OpeningsClose>(live.authority), true);
        require("fraction authority", std::holds_alternative<no::OpeningsClose>(row(net).authority), true);
        frozen = std::get<no::BindOpenings>(live.request().owner);
        const auto& close = std::get<no::OpeningsClose>(live.authority);
        require("inline owner count", frozen.openings.size(), 0U);
        require("inline authority count", close.openings.size(), 0U);
        require("external owner present", bool(frozen.external_openings), true);
        require("authority shares frozen owner", close.external_openings == frozen.external_openings, true);
        require("frozen count", frozen.external_openings->count(), 2U);
        for (unsigned i = 0; i != 2; ++i) {
            no::RequestHandle h;
            require("frozen read", int(frozen.external_openings->opening_at(i, h)), int(Status::ok));
            require("canonical member", h.incarnation, a.openings[i].incarnation);
            require("canonical run", h.run == run, true);
        }
    }
    void authority(const no::RequestHandle& h, const no::BindOpenings& members) {
        auto& close = std::get<no::OpeningsClose>(row(h).authority);
        close.openings = members.openings;
        close.external_openings = members.external_openings;
        close.cycle = members.cycle;
    }
    std::uint64_t hash(const no::BindOpenings& owner, const no::BindOpenings& close) {
        auto& live = row(sibling);
        auto def = std::make_shared<no::RequestDefinition>(*live.definition);
        def->request.owner = owner;
        live.definition = std::move(def);
        authority(sibling, close);
        return host.native_continuation_hash();
    }
    double sized(const no::BindOpenings& scope, const no::BindOpenings& other) {
        authority(net, scope);
        authority(sibling, other);
        const auto result = Probe::size(host, row(net));
        if (!result) {
            std::cerr << "FAIL net sizing: actual=no units expected=positive units\n";
            ++failures;
        }
        return result.value_or(-1);
    }
};

void membership() {
    Book book;
    Input different{book.b.openings};
    const auto external_b = different.external();
    const auto aa = book.hash(book.a, book.a);
    const auto ba = book.hash(book.b, book.a);
    const auto ab = book.hash(book.a, book.b);
    require("inline owner distinguishes member 22 from 33", aa != ba, true);
    require("inline authority distinguishes member 22 from 33", aa != ab, true);

    const auto owner_a = book.hash(book.frozen, book.a);
    const auto owner_b = book.hash(external_b, book.a);
    check("owner hash distinguishes {11,22} from {11,33}", owner_a != owner_b, true);
    check("same owner hash across storage", owner_a, aa);
    check("changed owner hash across storage", owner_b, ba);
    const auto authority_a = book.hash(book.a, book.frozen);
    const auto authority_b = book.hash(book.a, external_b);
    check("authority hash distinguishes {11,22} from {11,33}", authority_a != authority_b, true);
    check("same authority hash across storage", authority_a, aa);
    check("changed authority hash across storage", authority_b, ab);

    Input separate{book.a.openings};
    check("inline/inline claim", book.sized(book.a, book.a), 2.5);
    check("inline/external claim", book.sized(book.a, book.frozen), 2.5);
    check("external/inline claim", book.sized(book.frozen, book.a), 2.5);
    check("separate external lists, same members", book.sized(separate.external(), book.frozen), 2.5);
    check("separate external lists, different members", book.sized(book.frozen, external_b), 5.0);
    Input shorter{{book.a.openings.front()}};
    check("different count", book.sized(book.frozen, shorter.external()), 5.0);
    auto other_cycle = book.frozen;
    ++other_cycle.cycle;
    check("different cycle", book.sized(book.frozen, other_cycle), 5.0);

    Input empty{};
    Input foreign{{no::RequestHandle{{"other-run", 1}, 22}}};
    for (auto* invalid : {&empty, &foreign}) {
        const auto result = book.submit(invalid->external(), no::ExplicitUnits{5}, invalid->rows);
        const char* name = invalid == &empty ? "empty membership" : "foreign-run member";
        check(name, int(result.status), int(no::SubmitStatus::Rejected));
        check(name, result.reason ? int(*result.reason) : -1, int(no::RequestRejectReason::InvalidOwner));
        check(name, result.handle.has_value(), false);
        check(name, book.core.live().size(), std::size_t{2});
    }
}

enum class Read { owner, authority, claim };
void failed_read(Read where, Fault fault) {
    Book book;
    Input broken{book.a.openings, fault};
    const auto bad = broken.external();
    const auto expected = fault == Fault::io ? RunFailureCode::scratch_io_failed
                                             : RunFailureCode::out_of_memory;
    const char* label = where == Read::owner ? "owner read" :
                        where == Read::authority ? "authority read" : "claim read";
    try {
        if (where == Read::claim) (void)book.sized(book.a, bad);
        else if (where == Read::owner) (void)book.hash(bad, bad); // Shared reader: stop at the owner.
        else (void)book.hash(book.a, bad);
        std::cerr << "FAIL " << label << ": actual=returned expected=" << run_failure_code_name(expected) << '\n';
        ++failures;
    } catch (const RunFailureInfo& e) {
        check(label, int(e.run_failure_code()), int(expected));
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << label << ": actual=" << e.what()
                  << " expected=" << run_failure_code_name(expected) << '\n';
        ++failures;
    }
    check("fault reached at second member", broken.faults, 1U);
}
}

int main() {
    try {
        membership();
        for (auto where : {Read::owner, Read::authority, Read::claim})
            for (auto fault : {Fault::io, Fault::allocation}) failed_read(where, fault);
    } catch (const NotCompared&) {
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "NOT COMPARED unexpected exception: actual=" << e.what()
                  << " expected=completed comparison\n";
        return 2;
    }
    if (failures) return 1;
    std::puts("PASS: external opening identity, sibling claims and read failures");
    return 0;
}
