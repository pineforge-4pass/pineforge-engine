#include <pineforge/source/pine_adapter.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <string>

namespace {
using namespace pineforge;
namespace sd = source::detail;
using Status = sd::ScratchStatus;
using Snapshot = source::PlacementSnapshot;

struct NotCompared {};

template<class A, class B>
void need(const char* field, const A& actual, const B& expected) {
    if (actual == expected) return;
    std::cerr << "NOT COMPARED " << field << ": expected=" << expected << " actual=" << actual << '\n';
    throw NotCompared{};
}

void ok(const char* operation, Status status) {
    need(operation, static_cast<unsigned>(status), static_cast<unsigned>(Status::ok));
}

std::string record_field(const sd::CommandScratchStore& store, sd::ScratchHandle version,
                         sd::ScratchField field) {
    sd::ScratchRecordInfo info{};
    ok("locate version", store.point(version, info));
    need("located handle", info.handle, version);
    const auto n = field == sd::ScratchField::key ? info.key_bytes : info.value_bytes;
    std::string bytes(static_cast<std::size_t>(n), '\0');
    ok("read record field", store.read(version, field, 0,
        {reinterpret_cast<std::byte*>(bytes.data()), bytes.size()}));
    return bytes;
}

void replace(sd::CommandScratchStore& store, sd::ScratchHandle version,
             const std::string& key, const std::string& bytes) {
    sd::ScratchHandle written = 0;
    ok("begin edit", store.begin());
    ok("put same key", store.put(sd::exit_scratch_text(key), sd::exit_scratch_text(bytes), written));
    ok("commit edit", store.commit(sd::ScratchDurability::buffered));
    need("edited handle", written, version);
    need("committed payload equals edit", record_field(store, version, sd::ScratchField::value) == bytes, true);
}

std::string le64(std::uint64_t word) {
    std::string bytes(8, '\0');
    for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<char>(word >> (8 * i));
    return bytes;
}

std::uint64_t bits(double value) {
    std::uint64_t word;
    static_assert(sizeof(word) == sizeof(value));
    std::memcpy(&word, &value, sizeof(word));
    return word;
}

Snapshot restore(const sd::ExitScratchVersions& versions, sd::ScratchHandle version, bool pending) {
    if (!pending) return versions.placement(version);
    native_order::Request request;
    Snapshot row;
    std::string key;
    versions.pending(version, request, row, key);
    need("restored request", request.label, "request-978");
    need("restored pending key", key, "key-978");
    return row;
}

const char* name(Status status) {
    switch (status) {
    case Status::ok:                return "ok";
    case Status::end:               return "end";
    case Status::cancelled:         return "cancelled";
    case Status::not_found:         return "not_found";
    case Status::programming_error: return "programming_error";
    case Status::io_error:          return "io_error";
    case Status::corrupt:           return "corrupt";
    }
    return "unknown";
}

bool refused(const sd::ExitScratchVersions& versions, sd::ScratchHandle version, bool pending,
             const char* field, Status expected = Status::corrupt) {
    const char* path = pending ? "pending" : "placement";
    try {
        const auto row = restore(versions, version, pending);
        need("identity of unexpectedly restored record", row.comment, "exit-978");
        std::cerr << "FAIL " << path << ' ' << field << ": expected=" << name(expected)
                  << " actual=restored\n";
    } catch (const sd::ExitScratchError& error) {
        if (error.status() == expected) return true;
        std::cerr << "FAIL " << path << ' ' << field << ": expected=" << name(expected)
                  << " actual=" << name(error.status()) << '\n';
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << path << ' ' << field << ": expected=" << name(expected)
                  << " actual=" << error.what() << '\n';
    }
    return false;
}

bool check_reader(sd::ExitScratchContext& scratch, bool pending) {
    auto& versions = sd::exit_scratch_versions(scratch);
    native_order::Request request;
    request.label = "request-978";
    const std::string key = "key-978";
    auto save = [&](const Snapshot& row) {
        return pending ? versions.pending(1, request, row, key) : versions.placement(1, row);
    };

    Snapshot input;
    input.comment = "unbound-978";
    const auto unbound = restore(versions, save(input), pending);
    need("unbound identity", unbound.comment, input.comment);
    need("unbound bounds", unbound.leg_activation.bounds().has_value(), false);
    need("absent evidence", unbound.exit_activation.evidence().has_value(), false);

    input.comment = "exit-978";
    input.leg_activation.bind({978, 13, 14});
    input.exit_activation = compat::pine::ExitActivationPolicy({978, 12, 1, 100., 95., 105., {}});
    const auto version = save(input);
    const auto valid = restore(versions, version, pending);
    need("placement identity", valid.comment, input.comment);
    const auto& bounds = valid.leg_activation.bounds();
    const auto& evidence = valid.exit_activation.evidence();
    need("bounds present", bounds.has_value(), true);
    need("evidence present", evidence.has_value(), true);
    need("bound cycle", bounds->position_cycle, 978);
    need("stop first bar", bounds->stop_first_bar, 13);
    need("limit first bar", bounds->limit_first_bar, 14);
    need("placement cycle", evidence->position_cycle, 978);
    need("entry bar", evidence->entry_bar, 12);
    need("direction", evidence->direction, 1);
    need("cursor price", evidence->cursor_price, 100.);
    need("stop level", evidence->stop_level, 95.);
    need("limit level", evidence->limit_level, 105.);

    const auto stored_key = record_field(scratch.store, version, sd::ScratchField::key);
    const auto original = record_field(scratch.store, version, sd::ScratchField::value);
    need("key size", stored_key.size(), sd::exit_scratch_key_bytes);
    // Locate both complete schema-2 optionals, including their presence words.
    // No absolute offset or assumption that strings have word-aligned lengths.
    const auto activation = le64(1) + le64(978) + le64(13) + le64(14)
                          + le64(1) + le64(978) + le64(12) + le64(1) + le64(bits(100.))
                          + le64(bits(95.)) + le64(bits(105.)) + le64(0);
    const auto at = original.find(activation);
    need("activation sequence found", at != std::string::npos, true);
    need("second activation sequence", original.find(activation, at + 1), std::string::npos);
    auto edit = [&](unsigned word, std::uint64_t replacement) {
        auto changed = original;
        changed.replace(at + 8 * word, 8, le64(replacement));
        replace(scratch.store, version, stored_key, changed);
    };

    // A legal change at the direction word must reach the very same reader.
    edit(7, static_cast<std::uint64_t>(-1));
    const auto short_row = restore(versions, version, pending);
    need("edited placement identity", short_row.comment, input.comment);
    need("edited evidence present", short_row.exit_activation.evidence().has_value(), true);
    need("edited direction", short_row.exit_activation.evidence()->direction, -1);
    need("short holds stop", short_row.exit_activation.holds_stop(), true);
    need("short holds limit", short_row.exit_activation.holds_limit(), true);

    bool passed = refused(versions, version, !pending, "wrong record kind", Status::programming_error);
    const auto negative = static_cast<std::uint64_t>(-1);
    struct Edit { const char* field; unsigned word; std::uint64_t value; };
    const Edit cases[] = {
        {"bounds.position_cycle=0", 1, 0}, {"bounds.stop_first_bar=-1", 2, negative},
        {"bounds.limit_first_bar=-1", 3, negative}, {"evidence.position_cycle=0", 5, 0},
        {"evidence.entry_bar=-1", 6, negative}, {"evidence.direction=0", 7, 0},
        {"evidence.cursor_price=inf", 8, bits(std::numeric_limits<double>::infinity())},
        {"evidence.cursor_price=NaN", 8, bits(std::numeric_limits<double>::quiet_NaN())},
    };
    for (const auto& c : cases) {
        edit(c.word, c.value);
        if (!refused(versions, version, pending, c.field)) passed = false;
    }
    replace(scratch.store, version, stored_key, {});
    if (!refused(versions, version, pending, "empty payload")) passed = false;
    return passed;
}
} // namespace

int main() {
    try {
        source::PineExecutionAdapter adapter;
        source::StagedConfiguration staged;
        staged.syminfo.mintick = 0.25;
        staged.syminfo.pointvalue = 1.0;
        adapter.project({}, staged, {});
        auto& scratch = sd::exit_scratch(adapter);
        ok("open run store", scratch.store.status());
        need("initial records", scratch.store.record_count(), 0u);
        const bool placement = check_reader(scratch, false);
        const bool pending = check_reader(scratch, true);
        if (!placement || !pending) return 1;
        std::cout << "PASS placement and pending reject invalid exit activation\n";
        return 0;
    } catch (const NotCompared&) {
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "NOT COMPARED scratch fixture: expected=valid record actual=" << error.what() << '\n';
        return 1;
    }
}
