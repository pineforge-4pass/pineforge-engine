#include <pineforge/source/pine_adapter.hpp>

#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
namespace sd = pineforge::source::detail;
using pineforge::native_order::Request;
using pineforge::source::PlacementSnapshot;
using Status = sd::ScratchStatus;
using Handle = sd::ScratchHandle;

template<class T, class U>
void require_equal(const char* field, const T& actual, const U& expected) {
    if (actual == expected) return;
    std::ostringstream message;
    message << field << ": expected=" << expected << " actual=" << actual;
    throw std::runtime_error(message.str());
}

std::string word(std::uint64_t n) {
    std::string bytes;
    for (unsigned i = 0; i < 8; ++i)
        bytes.push_back(static_cast<char>((n >> (8 * i)) & 0xff));
    return bytes;
}

// Read independently of the version decoder. The key stays in the store's native
// format; only the positional value below contains little-endian scalar words.
std::string read_value(sd::ExitScratchContext& context, Handle handle) {
    sd::ExitScratchText text(context.store, handle);
    std::string bytes(static_cast<std::size_t>(text.size()), '\0');
    sd::exit_scratch_check(text.read(0, {reinterpret_cast<std::byte*>(bytes.data()), bytes.size()}));
    return bytes;
}

void replace_value(sd::ExitScratchContext& context, Handle handle, const std::string& bytes) {
    const auto words = sd::exit_scratch_words(context, handle);
    const sd::ExitScratchKey key(words[0], words[1], words[2], words[3]);
    Handle written = 0;
    sd::exit_scratch_edit(context, [&] {
        return context.store.put(key, sd::exit_scratch_text(bytes), written);
    });
    require_equal("replaced version handle", written, handle);
    const auto stored = read_value(context, handle);
    require_equal("stored payload size", stored.size(), bytes.size());
    require_equal("stored bytes match replacement", stored == bytes, true);
}

bool check_reader(sd::ExitScratchContext& context, bool pending) {
    const char* reader = pending ? "pending" : "placement";
    auto& writer = sd::exit_scratch_versions(context);
    const auto& versions = writer; // select the pending read overload
    sd::ExitScratchScope scope(context);
    PlacementSnapshot placed;
    placed.source_id = "exit-980";
    placed.legs.set_stop_price(95.0);
    Request request;
    request.label = "request-980";
    const std::string key = "key-980";

    const auto save = [&] {
        return pending ? writer.pending(scope.scope(), request, placed, key)
                       : writer.placement(scope.scope(), placed);
    };
    const auto restore = [&](Handle version, Request& req, PlacementSnapshot& out, std::string& k) {
        if (pending) versions.pending(version, req, out, k);
        else out = versions.placement(version);
    };
    const auto check_valid = [&](Handle handle, std::uint64_t incarnation, std::int64_t owner) {
        require_equal("valid nonzero version handle", handle != 0, true);
        Request req;
        PlacementSnapshot out;
        std::string k;
        restore(handle, req, out, k);
        require_equal("source id", out.source_id, placed.source_id);
        require_equal("stop price", out.legs.prices().stop_price, 95.0);
        require_equal("target incarnation", out.legs.target().incarnation, incarnation);
        require_equal("definition incarnation", out.legs.current_definition().incarnation(), incarnation);
        require_equal("target owner", out.legs.target().owner, owner);
        require_equal("lifecycle revision", out.legs.revision(), std::uint64_t{1});
        if (pending) {
            require_equal("request label", req.label, request.label);
            require_equal("pending key", k, key);
        }
    };
    // Both zero identities are valid before attachment, even with saved prices.
    check_valid(save(), 0, 0);
    placed.legs.attach(41, 0);
    const auto handle = save();
    check_valid(handle, 41, 0);
    const auto original = read_value(context, handle);

    // Locate exactly one target(41, 0), revision 1, definition(41) in a
    // known-good record, without assuming the size of the preceding fields.
    const auto prefix = word(41) + word(0) + word(1) + word(41);
    const auto pos = original.find(prefix);
    require_equal("lifecycle prefix present", pos != std::string::npos, true);
    require_equal("unique lifecycle prefix offset", original.rfind(prefix), pos);
    auto changed = original;
    changed.replace(pos, prefix.size(), word(42) + word(7) + word(1) + word(42));
    replace_value(context, handle, changed);
    check_valid(handle, 42, 7);

    bool passed = true;
    const auto expect_error = [&](const char* row, Handle version, Status expected) {
        PlacementSnapshot out;
        out.source_id = "old-placement";
        Request req;
        req.label = "old-request";
        std::string k = "old-key";
        try {
            restore(version, req, out, k);
        } catch (const sd::ExitScratchError& e) {
            if (e.status() == expected && out.source_id == "old-placement"
                && req.label == "old-request" && k == "old-key") return;
            std::cerr << "FAIL " << reader << '/' << row << ": expected status="
                      << static_cast<unsigned>(expected) << " actual=" << static_cast<unsigned>(e.status())
                      << "; expected outputs=old-placement,old-request,old-key actual="
                      << out.source_id << ',' << req.label << ',' << k << '\n';
            passed = false;
            return;
        } catch (const std::exception& e) {
            std::cerr << "FAIL " << reader << '/' << row << ": expected ExitScratchError, actual="
                      << e.what() << '\n';
            passed = false;
            return;
        }
        std::cerr << "FAIL " << reader << '/' << row << ": expected "
                  << (expected == Status::corrupt ? "corrupt" : "programming_error")
                  << ", restored target=" << out.legs.target().incarnation
                  << " definition=" << out.legs.current_definition().incarnation()
                  << " owner=" << out.legs.target().owner << '\n';
        passed = false;
    };

    changed = original;
    changed.replace(pos + 3 * 8, 8, word(42));
    replace_value(context, handle, changed);
    expect_error("definition", handle, Status::corrupt);

    changed = original;
    changed.replace(pos + 8, 8, word(UINT64_MAX)); // signed owner -1
    replace_value(context, handle, changed);
    expect_error("owner", handle, Status::corrupt);

    replace_value(context, handle, {});
    expect_error("empty payload", handle, Status::corrupt);
    const auto other = writer.request(scope.scope(), request);
    require_equal("wrong-kind control request", versions.request(other).label, request.label);
    expect_error("request handle", other, Status::programming_error);
    return passed;
}
} // namespace

int main() {
    try {
        pineforge::source::PineExecutionAdapter adapter;
        adapter.project({}, {}, {});
        auto& context = sd::exit_scratch(adapter);
        bool passed = true;
        for (bool pending : {false, true}) {
            try {
                if (!check_reader(context, pending)) passed = false;
            } catch (const std::exception& e) {
                std::cerr << "NOT COMPARED " << (pending ? "pending" : "placement")
                          << ": expected usable fixture; actual=" << e.what() << '\n';
                passed = false;
            }
        }
        if (!passed) return 1;
        std::cout << "PASS placement and pending reject inconsistent exit-leg identities\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "NOT COMPARED run setup: expected projected exit scratch; actual=" << e.what() << '\n';
        return 1;
    }
}
