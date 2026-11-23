#include <pineforge/source/pine_adapter.hpp>

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace {
using namespace pineforge;
namespace sd = source::detail;
using Status = sd::ScratchStatus;
using Snapshot = source::PlacementSnapshot;
using Word = std::uint64_t;

std::string row = "run setup";
int failures = 0;
struct NotCompared {};

template<class T, class U>
void need(const std::string& reason, const T& actual, const U& expected) {
    if (actual == expected) return;
    std::cerr << "NOT COMPARED " << row << ": " << reason << " actual=" << actual
              << " expected=" << expected << '\n';
    throw NotCompared{};
}

std::string status_name(Status s) {
    switch (s) {
    case Status::ok: return "ok";
    case Status::corrupt: return "corrupt";
    case Status::programming_error: return "programming_error";
    case Status::not_found: return "not_found";
    default: return "scratch status " + std::to_string(static_cast<unsigned>(s));
    }
}
void ok(const char* operation, Status s) { need(operation, status_name(s), "ok"); }

// The positional codec uses eight little-endian bytes per scalar, including
// presence flags. Locate complete, unique field groups in a real saved value.
std::string words(std::initializer_list<Word> values) {
    std::string bytes;
    for (auto value : values)
        for (unsigned i = 0; i < 8; ++i)
            bytes.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
    return bytes;
}
std::size_t locate(const std::string& bytes, const std::string& group, const char* field) {
    const auto at = bytes.find(group);
    const int matches = at == std::string::npos ? 0 :
        (bytes.find(group, at + 1) == std::string::npos ? 1 : 2);
    need(std::string(field) + " signature matches", matches, 1);
    return at;
}

struct Record { sd::ScratchHandle handle; std::string key, bytes; const char* expected; };
Record record(const sd::CommandScratchStore& store, sd::ScratchHandle handle,
              const char* expected = nullptr) {
    sd::ScratchRecordInfo info{};
    ok("point", store.point(handle, info));
    need("record handle", info.handle, handle);
    auto read = [&](sd::ScratchField field, std::size_t size) {
        std::string bytes(size, '\0');
        if (size) ok("read field", store.read(handle, field, 0,
            {reinterpret_cast<std::byte*>(bytes.data()), size}));
        return bytes;
    };
    return {handle, read(sd::ScratchField::key, info.key_bytes),
            read(sd::ScratchField::value, info.value_bytes), expected};
}

void replace(sd::CommandScratchStore& store, const Record& original, const std::string& bytes) {
    ok("begin", store.begin());
    sd::ScratchHandle handle = 0;
    ok("put", store.put(sd::exit_scratch_text(original.key), sd::exit_scratch_text(bytes), handle));
    ok("commit", store.commit(sd::ScratchDurability::buffered));
    need("replacement handle", handle, original.handle);
    const auto stored = record(store, handle);
    need("committed payload length", stored.bytes.size(), bytes.size());
    need("committed payload matches this edit", stored.bytes == bytes, true);
}

struct Output {
    native_order::Request request;
    Snapshot placement;
    std::string key = "caller key";
    Output() { request.label = "caller request"; placement.source_id = "caller placement"; }
};
void restore(const sd::ExitScratchVersions& reader, bool pending, sd::ScratchHandle handle,
             Output& out) {
    if (pending) reader.pending(handle, out.request, out.placement, out.key);
    else out.placement = reader.placement(handle);
}

std::string describe(const Snapshot& p) {
    std::ostringstream out;
    out << p.source_id;
    if (const auto& c = p.reservation_expansion.capture())
        out << " capture=" << c->position_cycle << '/' << static_cast<int>(c->side) << '/'
            << (c->first_later_admission ? std::to_string(*c->first_later_admission) : "absent")
            << (p.reservation_expansion.population_open() ? ":open" : ":closed");
    if (auto owner = p.reservation_growth_source.reservation_owner()) out << " owner=" << *owner;
    if (const auto& o = p.market_admission.observation())
        out << " observation=" << o->id << '/' << o->command << '@' << o->bar;
    if (const auto& r = p.market_admission.review())
        out << " review=" << r->sequence << '/' << static_cast<int>(r->checkpoint)
            << '/' << r->target_command << '@' << r->bar;
    if (const auto& r = p.market_admission.sizing_revision())
        out << " sizing=" << r->sequence << '/' << r->cause_fill
            << '/' << r->target_command << '@' << r->bar;
    return out.str();
}

void refuse(const sd::ExitScratchVersions& reader, bool pending, sd::ScratchHandle handle,
            Status expected) {
    Output out;
    std::string actual = "restored";
    try { restore(reader, pending, handle, out); }
    catch (const sd::ExitScratchError& error) { actual = status_name(error.status()); }
    catch (const std::exception& error) { actual = std::string("exception: ") + error.what(); }
    if (actual != status_name(expected)) {
        std::cerr << "FAIL " << row << ": actual=" << actual
                  << " expected=" << status_name(expected) << '\n';
        ++failures;
    } else if (pending && (out.request.label != "caller request" ||
                           out.placement.source_id != "caller placement" || out.key != "caller key")) {
        std::cerr << "FAIL " << row << ": outputs actual=" << out.request.label << '/'
                  << out.placement.source_id << '/' << out.key
                  << " expected=caller request/caller placement/caller key\n";
        ++failures;
    }
}

void exercise(sd::ExitScratchContext& context, bool pending) {
    const std::string path = pending ? "pending/" : "placement/";
    row = path + "setup";
    sd::ExitScratchScope scope(context);
    auto& versions = sd::exit_scratch_versions(context);
    native_order::Request request;
    request.label = "request-979";
    const std::string key = "key-979";
    auto seed = [&](const Snapshot& p, const char* expected) {
        row = path + p.source_id;
        const auto handle = pending ? versions.pending(scope.scope(), request, p, key)
                                    : versions.placement(scope.scope(), p);
        need("issued a nonzero version", handle != 0, true);
        Output out;
        restore(versions, pending, handle, out);
        need("valid round trip", describe(out.placement), expected);
        if (pending) {
            need("request label", out.request.label, request.label);
            need("pending key", out.key, key);
        }
        return record(context.store, handle, expected);
    };
    auto damaged = [&](const char* name, const Record& original, const std::string& bytes) {
        row = path + name;
        need("edit differs from original", bytes != original.bytes, true);
        replace(context.store, original, bytes);
        refuse(versions, pending, original.handle, Status::corrupt);
        // Reuse the same handle for its valid payload. A stale refusal must
        // not count as a comparison of the next, independently altered field.
        replace(context.store, original, original.bytes);
        Output repaired;
        restore(versions, pending, original.handle, repaired);
        need("repaired placement", describe(repaired.placement), original.expected);
    };

    Snapshot p;
    p.source_id = "no optional state";
    seed(p, "no optional state"); // even an absent capture and draft must preserve the identity
    auto o = std::make_shared<admission::CommandObservation>();
    o->command = 100; o->bar = 10; o->id = "order";
    p.market_admission.bind(o);
    p.source_id = "bound";
    p.reservation_expansion.capture(70, 9, PositionSide::SHORT, 4);
    const auto bound = seed(p, "bound capture=9/2/absent:open observation=order/100@10");
    p.market_admission.sizing_revised({102, 11, 12, 100});
    seed(p, "bound capture=9/2/absent:open observation=order/100@10 sizing=102/11/100@12");

    p.source_id = "full";
    p.reservation_expansion = {};
    p.reservation_expansion.capture(50, 7, PositionSide::LONG, 10);
    p.reservation_expansion.close_population(51);
    p.reservation_growth_source.assign_capture(41, 50);
    p.market_admission.reviewed({101, admission::Checkpoint::ExplicitPair, 10, 100});
    const auto full = seed(p, "full capture=7/1/51:closed owner=50 observation=order/100@10"
                              " review=101/1/100@10 sizing=102/11/100@12");

    const auto capture = locate(full.bytes, words({1, 7, 1, 1, 51, 1, 50}), "capture and owner");
    const auto origin_tag = words({1, 100, static_cast<Word>(admission::CommandKind::Entry)});
    const auto origin = locate(full.bytes, origin_tag, "observation");
    const auto review_words = words({1, 101, 1, 10, 100});
    const auto sizing_words = words({1, 102, 11, 12, 100});
    const auto review = locate(full.bytes, review_words, "review");
    const auto sizing = locate(full.bytes, sizing_words, "sizing");
    need("observation precedes receipts", origin < review, true);
    need("sizing tag follows review", sizing, review + review_words.size());

    struct Change { const char* name; std::size_t offset; Word replacement; };
    const Change changes[] = {
        {"zero owner", capture + 6 * 8, 0},
        {"zero cycle", capture + 8, 0}, {"negative cycle", capture + 8, Word(-1)},
        {"flat side", capture + 2 * 8, static_cast<Word>(PositionSide::FLAT)},
        {"zero closing admission", capture + 4 * 8, 0},
        {"review target", review + 4 * 8, 99}, {"review sequence", review + 8, 100},
        {"review bar", review + 3 * 8, 9}, {"review checkpoint", review + 2 * 8, 99},
        {"sizing target", sizing + 4 * 8, 99}, {"sizing sequence", sizing + 8, 100},
        {"sizing bar", sizing + 3 * 8, 9}, {"zero cause fill", sizing + 2 * 8, 0},
    };
    for (const auto& c : changes) {
        auto bytes = full.bytes;
        bytes.replace(c.offset, 8, words({c.replacement}));
        damaged(c.name, full, bytes);
    }
    // Keep exactly one receipt and omit the entire observation (including its
    // variable-size body). Neither orphan is a truncated or misaligned record.
    auto orphan = [&](const std::string& receipts) {
        auto bytes = full.bytes;
        bytes.replace(origin, sizing + sizing_words.size() - origin, words({0}) + receipts);
        return bytes;
    };
    damaged("review without observation", full, orphan(review_words + words({0})));
    damaged("sizing without observation", full, orphan(words({0}) + sizing_words));
    auto zero = bound.bytes; // no receipts: only bind() can reject this command
    zero.replace(locate(zero, origin_tag, "bound observation") + 8, 8, words({0}));
    damaged("zero command", bound, zero);
    damaged("empty payload", full, {});
    row = path + "wrong record kind";
    refuse(versions, pending, versions.request(scope.scope(), request), Status::programming_error);
}
} // namespace

int main() {
    try {
        struct Host final : pineforge::NativeStrategyHost {
            void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {}
        } host;
        source::PineExecutionAdapter adapter(host);
        source::StagedConfiguration staged;
        staged.syminfo.mintick = 0.25;
        staged.syminfo.pointvalue = 1;
        adapter.project({}, staged, {});
        auto& context = sd::exit_scratch(adapter);
        ok("run store", context.store.status());
        exercise(context, false);
        exercise(context, true);
    } catch (const NotCompared&) { return 1; }
    catch (const std::exception& error) {
        std::cerr << "NOT COMPARED " << row << ": actual=" << error.what()
                  << " expected=successful fixture setup and valid readback\n";
        return 1;
    }
    if (failures) return 1;
    std::cout << "PASS #979: placement and pending reject damaged reservation and admission state\n";
    return 0;
}
