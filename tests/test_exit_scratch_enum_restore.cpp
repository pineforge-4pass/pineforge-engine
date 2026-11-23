#include <pineforge/source/pine_adapter.hpp>
#include "../src/source/exit_scratch_versions_production.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
namespace pd = pineforge::detail;
namespace sd = source::detail;
namespace no = native_order;
namespace legs = exit_legs;
using Status = pd::ScratchStatus;
using Bytes = std::vector<std::byte>;
using Word = std::array<std::byte, 8>;

struct NotCompared {};
template<class A, class B>
void same(const std::string& what, const A& actual, const B& expected) {
    if (actual == expected) return;
    std::cerr << "NOT COMPARED " << what << ": expected=" << expected << " actual=" << actual << '\n';
    throw NotCompared{};
}

std::string status_name(Status s) {
    switch (s) {
    case Status::ok: return "ok";
    case Status::corrupt: return "corrupt";
    case Status::programming_error: return "programming_error";
    case Status::not_found: return "not_found";
    default: return "status " + std::to_string(static_cast<unsigned>(s));
    }
}

Word le64(std::uint64_t n) {
    Word bytes{};
    for (auto& b : bytes) { b = static_cast<std::byte>(n & 0xff); n >>= 8; }
    return bytes;
}

// Distinct real field values locate nested fields without duplicating the
// variable-length placement schema. Require exactly one match in the record.
constexpr std::uint64_t tag(unsigned n) { return UINT64_C(0x1020304050607000) + n; }

struct Record {
    pd::CommandScratchStore& store;
    pd::ScratchHandle handle;
    std::array<std::byte, sd::exit_scratch_key_bytes> key{};
    Bytes original;

    pd::ScratchRecordInfo info() const {
        pd::ScratchRecordInfo result{};
        same("point at frozen record", status_name(store.point(handle, result)), "ok");
        same("frozen handle", result.handle, handle);
        return result;
    }
    Record(pd::CommandScratchStore& s, pd::ScratchHandle h) : store(s), handle(h) {
        const auto row = info();
        same("frozen key size", row.key_bytes, key.size());
        same("read key", status_name(store.read(handle, pd::ScratchField::key, 0,
             {key.data(), key.size()})), "ok");
        original.resize(static_cast<std::size_t>(row.value_bytes));
        same("read frozen payload", status_name(store.read(handle, pd::ScratchField::value, 0,
             {original.data(), original.size()})), "ok");
        check_word("frozen schema", 0, 2);
    }
    void check_word(const std::string& row, std::size_t offset, std::uint64_t expected) const {
        std::uint64_t actual = 0;
        for (unsigned i = 0; i < 8; ++i)
            actual |= std::to_integer<std::uint64_t>(original.at(offset + i)) << (8 * i);
        same(row + " original word", actual, expected);
    }
    std::size_t at_tag(unsigned n) const {
        const auto word = le64(tag(n));
        const auto end = original.end();
        const auto first = std::search(original.begin(), end, word.begin(), word.end());
        const auto label = "marker " + std::to_string(n);
        same(label + " present", first != end, true);
        same(label + " unique", std::search(first + 1, end, word.begin(), word.end()) == end, true);
        return static_cast<std::size_t>(first - original.begin());
    }
    void replace(const std::string& row, const Bytes& bytes) const {
        pd::ScratchHandle written = 0;
        same(row + " begin", status_name(store.begin()), "ok");
        same(row + " put", status_name(store.put(pd::ScratchSpanSource({key.data(), key.size()}),
             pd::ScratchSpanSource({bytes.data(), bytes.size()}), written)), "ok");
        same(row + " commit", status_name(store.commit(pd::ScratchDurability::buffered)), "ok");
        same(row + " replaced handle", written, handle);
        same(row + " committed size", info().value_bytes, bytes.size());
        Bytes actual(bytes.size());
        same(row + " read back", status_name(store.read(handle, pd::ScratchField::value, 0,
             {actual.data(), actual.size()})), "ok");
        const auto diff = std::mismatch(actual.begin(), actual.end(), bytes.begin());
        if (diff.first != actual.end())
            same(row + " committed byte " + std::to_string(diff.first - actual.begin()),
                 std::to_integer<unsigned>(*diff.first), std::to_integer<unsigned>(*diff.second));
    }
};

struct Field { const char* name; std::size_t offset; std::int64_t good, bad; };
struct Checks {
    int compared = 0, failures = 0;
    template<class Restore> void refuses(const std::string& row, Status expected, Restore restore) {
        std::string actual = "restored";
        try { restore(); }
        catch (const sd::ExitScratchError& e) {
            if (e.status() == expected) { ++compared; return; }
            actual = status_name(e.status());
        }
        catch (const std::exception& e) { actual = e.what(); }
        std::cerr << "FAIL " << row << ": expected=" << status_name(expected) << " actual=" << actual << '\n';
        ++failures;
    }
    template<class Restore> void fields(const Record& record, std::initializer_list<Field> cases,
                                         Restore restore) {
        for (const auto& f : cases) {
            const auto row = std::string(f.name) + "=" + std::to_string(f.bad);
            record.check_word(row, f.offset, f.good);
            Bytes bytes = record.original;
            const auto word = le64(static_cast<std::uint64_t>(f.bad));
            std::copy(word.begin(), word.end(), bytes.begin() + f.offset);
            record.replace(row, bytes);
            refuses(row, Status::corrupt, restore);
        }
    }
};

source::PlacementSnapshot placement_fixture() {
    source::PlacementSnapshot p;
    p.family = source::PineOrderFamily::Risk;
    p.source_id = "exit-977";
    p.birth = OrderBirth::fill_evaluation(3, tag(1),
        BirthCursor::point(BirthCursorDomain::HistoricalPath, 1, 4), 100, 5, 5, 1);
    p.birth_reach = compat::pine::HistoricalBirthReach::ExtremeWaypoints;
    p.exit_activation = compat::pine::ExitActivationPolicy({2, 3, 1, 100, 99, 101,
        compat::pine::LimitContinuation{compat::pine::LimitContinuationCause::FirstHighRecross, tag(2)}});
    p.reservation_expansion.capture(1, tag(3), PositionSide::SHORT, 2.0);
    p.cancellation.cause = source::PineCancellationCause::Explicit;
    p.cancellation.source_incarnation = tag(4);
    auto obs = std::make_shared<admission::CommandObservation>();
    obs->command = 17;
    obs->kind = admission::CommandKind::CancelAll;
    obs->birth = OrderBirth::direct_command(3, tag(5));
    p.market_admission.bind(obs);
    p.market_admission.reviewed({tag(6), admission::Checkpoint::TerminalGross, 3, obs->command});
    p.legs.attach(12, 1);
    const legs::Frame suspended{tag(7), 2, legs::Domain::Magnifier, legs::Phase::Observation};
    const legs::Frame observed{tag(8), 3, legs::Domain::RawTicks, legs::Phase::AfterMargin};
    const auto apply = [&](const legs::Action& action) {
        same("lifecycle fixture action", static_cast<int>(p.legs.apply(p.legs.target(), action)),
             static_cast<int>(legs::Result::Applied));
    };
    apply({p.legs.target(), p.legs.revision(), suspended,
           legs::Suspend{{legs::Leg::Trail}, {}, legs::ObservationWindow{}, {}}});
    apply({p.legs.target(), p.legs.revision(), observed, legs::Observe{101, 99, 1, legs::Fold::Continue}});
    return p;
}

int run() {
    Bytes arena(512 * 1024);
    pd::FixedMemoryScratchBacking backing({arena.data(), arena.size()}, 512);
    pd::CommandScratchStore store({2, 512, 1, 1, sd::exit_scratch_key_order(),
                                   pd::ScratchOpenMode::create_empty}, &backing);
    same("open store", status_name(store.status()), "ok");
    sd::ExitScratchVersionsProduction versions(store);
    const sd::ExitScratchVersions& read = versions;
    Checks check;

    const auto p = placement_fixture();
    const Record placed(store, versions.placement(1, p));
    const auto valid = read.placement(placed.handle);
    same("placement source id", valid.source_id, p.source_id);
    same("placement family", static_cast<int>(valid.family), 8);
    check.refuses("request(placement handle)", Status::programming_error,
                  [&] { (void)read.request(placed.handle); });
    const auto birth = placed.at_tag(1), observed = placed.at_tag(8);
    check.fields(placed, {
        {"placement.family", 8, 8, 9}, {"placement.family", 8, 8, 255},
        {"placement.family", 8, 8, 256}, // already outside uint8_t on the base
        {"placement.birth.cause", birth - 16, 3, 4},
        {"placement.birth.cursor.domain", birth + 8, 1, 3},
        {"placement.birth.cursor.position", birth + 16, 1, 3},
        {"placement.birth_reach", birth + 72, 1, 2},
        {"placement.continuation.cause", placed.at_tag(2) - 8, 1, -1},
        {"placement.capture.side", placed.at_tag(3) + 8, 2, -1},
        {"placement.cancellation.cause", placed.at_tag(4) - 24, 4, 5},
        {"placement.observation.kind", placed.at_tag(5) - 24, 3, 4},
        {"placement.review.checkpoint", placed.at_tag(6) + 8, 2, -1},
        {"placement.suspension.leg", placed.at_tag(7) + 40, 2, 3},
        {"placement.action.domain", observed + 16, 4, 5},
        {"placement.action.phase", observed + 24, 1, 2},
        {"placement.observe.fold", observed + 64, 1, 2},
    }, [&] { (void)read.placement(placed.handle); });

    no::Request r;
    r.label = "sized-977";
    r.intent = no::Sized{no::Side::Short, no::CashValue{20}, no::SizeTime::AtAcceptance,
                         no::SizePrice::SignalOnTick, no::ExecutionGridPolicy::ExplicitUnits, true};
    const Record sized(store, versions.request(1, r));
    const auto request = read.request(sized.handle);
    same("request label", request.label, r.label);
    same("sized price", static_cast<int>(std::get<no::Sized>(request.intent).price), 2);
    check.fields(sized, {
        {"request.side", 16, 1, 2}, {"request.time", 40, 1, 2},
        {"request.price", 48, 2, 3}, {"request.grid_policy", 56, 1, 2},
    }, [&] { (void)read.request(sized.handle); });

    r.intent = no::HostSized{no::HostSizedKind::Close, {}};
    r.trigger = no::Limit{};
    r.owner = no::WaitForApplied{{{"orders", 1}, tag(9)}, no::NativeArmVisibility::PendingUntilArmed,
                                 no::NativeArmFirstMatch::AfterArmPrint, no::NativeArmScope::Book};
    r.group = no::Member{tag(10), 1, no::GroupEffect::Reduce};
    r.anchor = no::FromOwnerFill{1.0, false, no::NativeAnchorRounding::Directional};
    const Record child(store, versions.request(1, r));
    same("child label", read.request(child.handle).label, r.label);
    const auto owner = child.at_tag(9);
    check.fields(child, {
        {"request.host_sized.kind", 16, 1, 2},
        {"request.owner.visibility", owner + 8, 1, 2}, {"request.owner.first_match", owner + 16, 1, 2},
        {"request.owner.scope", owner + 24, 1, 2}, {"request.group.effect", child.at_tag(10) + 16, 1, 2},
        {"request.anchor.rounding", child.original.size() - 8, 2, 3},
    }, [&] { (void)read.request(child.handle); });

    no::Request reduce;
    reduce.label = "reduce-977";
    reduce.intent = no::Reduce{no::ScopeFraction{0.5, no::ScopeClaim::NetOfSiblings,
                                                 no::ScopeBasis::AtAcceptance}};
    const Record fraction(store, versions.request(1, reduce));
    same("reduction label", read.request(fraction.handle).label, reduce.label);
    check.fields(fraction, {{"request.fraction.claim", 32, 1, 2}, {"request.fraction.basis", 40, 1, 2}},
                 [&] { (void)read.request(fraction.handle); });

    const Record pending(store, versions.pending(1, r, p, "pending-977"));
    no::Request saved;
    source::PlacementSnapshot snapshot;
    std::string key;
    auto restore_pending = [&] { read.pending(pending.handle, saved, snapshot, key); };
    restore_pending();
    same("pending key", key, "pending-977");
    same("pending request", saved.label, r.label);
    same("pending placement", snapshot.source_id, p.source_id);
    check.fields(pending, {{"pending.request.kind", 16, 1, 2},
                           {"pending.placement.family", child.original.size(), 8, 9}}, restore_pending);
    placed.replace("empty placement", {});
    check.refuses("empty placement", Status::corrupt, [&] { (void)read.placement(placed.handle); });

    if (check.failures) return 1;
    same("completed refusal checks", check.compared, 32);
    std::cout << "PASS exit-scratch enum restore: " << check.compared << " refusals\n";
    return 0;
}
} // namespace

int main() {
    try { return run(); }
    catch (const NotCompared&) { return 1; }
    catch (const std::exception& e) {
        std::cerr << "NOT COMPARED exit-scratch fixture: expected=valid record actual=" << e.what() << '\n';
        return 1;
    }
}
