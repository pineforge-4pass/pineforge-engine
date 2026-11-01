// Frozen-opening storage follows reduce binding, retirement and shared-file
// lifetime as the test drops its owning pointers.
// Include the unmodified implementation to name its anonymous-namespace
// FrozenOpenings through direct native_order.cpp inclusion.
// Read the real OpeningFile::storage; no product accessor or layout mirror.
#include "../src/native_order.cpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <variant>
#include <vector>

namespace memory_spill_frozen_openings {
namespace no = pineforge::native_order;
using Status = pineforge::detail::ScratchStatus;
using Handle = no::RequestHandle;
using Word = std::uint64_t;

std::size_t checks = 0, failures = 0;
std::string trial;
struct PrerequisiteFailure final : std::exception {};

bool check(bool passed, const char* expression, int line) {
    ++checks;
    if (!passed) {
        ++failures;
        std::fprintf(stderr, "FAIL [frozen-openings] %s:%d: %s (trial=%s)\n",
                     __FILE__, line, expression, trial.c_str());
    }
    return passed;
}
#define MEM_CHECK(expression) check(bool(expression), #expression, __LINE__)
#define MEM_NEED(expression) do { \
    if (!check(bool(expression), "fixture prerequisite: " #expression, __LINE__)) \
        throw PrerequisiteFailure{}; \
} while (false)

template<class Action>
void run_trial(const std::string& name, Action&& action) {
    trial = name;
    try {
        action();
    } catch (const PrerequisiteFailure&) {
        // Reported above; other trials must still run.
    } catch (const std::exception& error) {
        check(false, error.what(), __LINE__);
    } catch (...) {
        check(false, "unknown exception", __LINE__);
    }
}

// C++ hosts supply physical opening observations through this API. Exercise
// enrollment, not hand-written frozen storage. High, nonconsecutive words
// exercise all eight serialized bytes; the callback supplies reverse order.
struct HostOpenings final : no::OpeningObservationReadback {
    no::RunIdentity run;
    std::vector<Handle> input;
    std::vector<Handle> model;
    std::size_t callback_reads = 0;
    bool readable = true;

    HostOpenings(no::RunIdentity identity, std::size_t members, std::size_t command)
        : run(std::move(identity)) {
        for (std::size_t i = 0; i != members; ++i)
            input.push_back({run, UINT64_C(0x1020304050607000)
                                 + command * UINT64_C(0x100000) + i * 257 + 1});
        std::reverse(input.begin(), input.end());
        model = input;
        std::sort(model.begin(), model.end(),
                  [](const Handle& a, const Handle& b) {
                      return a.incarnation < b.incarnation;
                  });
    }
    std::uint64_t count() const noexcept override { return model.size(); }
    Status observation_at(Word i, no::OpeningObservation& out) const noexcept override {
        if (!readable) return Status::io_error;
        if (i >= model.size()) return Status::not_found;
        out = {model[static_cast<std::size_t>(i)], 7,
               no::PositionNonflat{7, no::Side::Long}, true};
        return Status::ok;
    }
    static Status read(void* context, Word i, Handle& out) {
        auto& host = *static_cast<HostOpenings*>(context);
        ++host.callback_reads;
        if (!host.readable) return Status::io_error;
        if (i >= host.input.size()) return Status::not_found;
        out = host.input[static_cast<std::size_t>(i)];
        return Status::ok;
    }
};

struct Stored {
    Handle command;
    no::BindOpenings definition_owner;
    no::OpeningsClose authority_owner;
    std::vector<Handle> model;
    std::weak_ptr<const no::OpeningReadback> lifetime;
    int fd = -1; // Borrowed only, never dup'd or closed by the test.
    dev_t device{};
    ino_t inode{};
};

// Independent wire model: each incarnation is an unsigned 64-bit integer,
// eight base-256 digits, least significant first. Run identity is not repeated
// in the file. No production codec or measured extent supplies
// either the expected bytes or their count.
std::vector<unsigned char> wire_model(const std::vector<Handle>& model) {
    std::vector<unsigned char> bytes;
    for (const auto& handle : model) {
        Word value = handle.incarnation;
        for (unsigned digit = 0; digit != 8; ++digit) {
            bytes.push_back(static_cast<unsigned char>(value % 256));
            value /= 256;
        }
    }
    return bytes;
}

void inspect(const std::shared_ptr<const no::OpeningReadback>& external,
             const std::vector<Handle>& model) {
    MEM_NEED(external);
    const auto* frozen = dynamic_cast<const no::FrozenOpenings*>(external.get());
    MEM_NEED(frozen);
    const auto& storage = frozen->file.storage;
    // This is the ordinary anonymous-file route. Arena chunk occupancy is a
    // different measure and is deliberately not equated to logical wire bytes.
    MEM_NEED(storage.disk_backed() && !storage.arena_backed());
    const auto expected_bytes = wire_model(model);
    MEM_CHECK(storage.storage_bytes() == expected_bytes.size());
    MEM_CHECK(external->count() == model.size());
    std::vector<unsigned char> actual_bytes(expected_bytes.size());
    storage.read(0, actual_bytes.data(), actual_bytes.size());
    MEM_CHECK(actual_bytes == expected_bytes);
    std::vector<Handle> actual;
    for (std::size_t i = 0; i != model.size(); ++i) {
        Handle value;
        const auto status = external->opening_at(i, value);
        MEM_CHECK(status == Status::ok);
        actual.push_back(value);
    }
    MEM_CHECK(actual == model);
    Handle untouched{model.front().run, 999};
    const auto before = untouched;
    MEM_CHECK(external->opening_at(model.size(), untouched) == Status::not_found
              && untouched == before);
}

struct Population { std::size_t files = 0; Word bytes = 0; };

// Observe the actual files even when the test owns no shared_ptr. A weak_ptr
// does not extend their lives. No operation here opens a file, so a closed
// descriptor cannot be reused between the release and its probe. The recorded
// device/inode also catches an unexpected descriptor substitution.
Population observe(const std::vector<Stored>& stored, const std::vector<bool>& alive) {
    MEM_NEED(stored.size() == alive.size());
    Population actual, expected;
    for (std::size_t i = 0; i != stored.size(); ++i) {
        const auto& item = stored[i];
        if (alive[i]) {
            ++expected.files;
            expected.bytes += wire_model(item.model).size();
        }
        MEM_CHECK((!item.lifetime.expired()) == alive[i]);
        struct stat info{};
        errno = 0;
        const int result = ::fstat(item.fd, &info);
        const int error = errno;
        if (result == 0) {
            MEM_CHECK(info.st_dev == item.device && info.st_ino == item.inode);
            MEM_CHECK(info.st_size >= 0);
            ++actual.files;
            if (info.st_size >= 0) actual.bytes += static_cast<Word>(info.st_size);
        } else {
            MEM_CHECK(result == -1 && error == EBADF);
        }
    }
    MEM_CHECK(actual.files == expected.files);
    MEM_CHECK(actual.bytes == expected.bytes);
    return actual;
}

Population frozen_commands(std::size_t commands, std::size_t members) {
    const no::RunIdentity run{"frozen-openings", 1};
    no::WorkingRequestCore core(run);
    std::vector<Stored> stored;
    Word incarnation = 1, ordinal = 1;
    for (std::size_t q = 0; q != commands; ++q) {
        HostOpenings host(run, members, q);
        auto cursor = std::make_shared<no::OpeningInputCursor>(
            members, &host, HostOpenings::read);
        no::Request request{no::Reduce{no::ExplicitUnits{1}}, "frozen-close", "external"};
        request.owner = no::BindOpenings{{}, 7, cursor};
        no::CommandContext context;
        context.external_openings = &host;
        // The direct command validates the host observations, canonicalizes
        // its borrowed membership, and installs the real definition/authority.
        const auto installed = core.apply_submit(request, context, incarnation, ordinal);
        MEM_NEED(installed.result.status == no::SubmitStatus::Accepted
                  && installed.result.handle);
        const auto* live = core.find_live(*installed.result.handle);
        MEM_NEED(live);
        const auto* definition = std::get_if<no::BindOpenings>(&live->request().owner);
        const auto* authority = std::get_if<no::OpeningsClose>(&live->authority);
        MEM_NEED(definition && authority && definition->external_openings);
        MEM_CHECK(definition->openings.empty() && authority->openings.empty());
        MEM_CHECK(definition->external_openings == authority->external_openings
                  && definition->external_openings != cursor);
        MEM_CHECK(host.callback_reads >= members);
        const auto* frozen =
            dynamic_cast<const no::FrozenOpenings*>(definition->external_openings.get());
        MEM_NEED(frozen && frozen->file.storage.disk_backed());
        Stored item{*installed.result.handle, *definition, *authority, host.model,
                    definition->external_openings, frozen->file.storage.descriptor()};
        struct stat identity{};
        MEM_NEED(::fstat(item.fd, &identity) == 0);
        item.device = identity.st_dev;
        item.inode = identity.st_ino;
        for (const auto& older : stored) {
            MEM_CHECK(older.definition_owner.external_openings
                      != item.definition_owner.external_openings);
            MEM_CHECK(older.fd != item.fd);
        }
        // Both host callbacks now refuse reads. Real frozen-file reads must
        // still work, and the callback count must stay unchanged.
        host.readable = false;
        const auto reads = host.callback_reads;
        inspect(item.definition_owner.external_openings, item.model);
        inspect(item.authority_owner.external_openings, item.model);
        MEM_CHECK(host.callback_reads == reads);
        stored.push_back(std::move(item));
        const auto population = observe(stored, std::vector<bool>(stored.size(), true));
        // Each independently supplied command freezes one distinct membership:
        // q+1 files, members words/file, eight bytes/word. There is no cap.
        MEM_CHECK(population.files == q + 1);
        MEM_CHECK(population.bytes == (q + 1) * members * Word{8});
        incarnation = core.last_incarnation() + 1;
        ordinal = core.last_ordinal() + 1;
    }
    const auto peak = observe(stored, std::vector<bool>(commands, true));
    MEM_CHECK(core.live_size() == commands && core.history_base() == 0);
    for (const auto& item : stored) {
        ordinal = core.last_ordinal() + 1;
        MEM_CHECK(core.cancel(item.command, ordinal).status == no::CancelStatus::Cancelled);
    }
    MEM_CHECK(core.live_size() == 0);
    for (const auto& item : stored)
        inspect(item.definition_owner.external_openings, item.model);
    observe(stored, std::vector<bool>(commands, true)); // Cancellation did not shrink a file.

    // Leave all but the last membership owned ONLY by journal events.
    // The last has both actual owner variants copied from its installed row.
    for (std::size_t i = 0; i + 1 < commands; ++i) {
        stored[i].definition_owner.external_openings.reset();
        stored[i].authority_owner.external_openings.reset();
    }
    observe(stored, std::vector<bool>(commands, true)); // Journal still owns every file.
    const auto end = core.history_end();
    MEM_CHECK(end == commands * 2); // One AcceptedEvent and one CancelledEvent each.
    const auto retired = core.retire_history(core.last_ordinal());
    MEM_CHECK(retired == end && core.history_base() == end);
    std::vector<bool> last_only(commands, false);
    last_only.back() = true;
    observe(stored, last_only); // Retired definitions release commands-1 whole files.
    auto& last = stored.back();
    inspect(last.definition_owner.external_openings, last.model);
    last.definition_owner.external_openings.reset();
    observe(stored, last_only); // OpeningsClose still shares the full file.
    inspect(last.authority_owner.external_openings, last.model);
    last.authority_owner.external_openings.reset();
    observe(stored, std::vector<bool>(commands, false)); // Last holder: zero files/bytes.
    return peak;
}
} // namespace memory_spill_frozen_openings

int main() {
    using namespace memory_spill_frozen_openings;
    Population results[2][2]{};
    bool finished[2][2]{};
    // Independent axes: order commands 3/12 and openings per command 17/68.
    // At most 24 journal events, below the source's 32-slot resident chunk,
    // expose a missing resident-retirement step before core exit.
    for (std::size_t c = 0; c != 2; ++c) {
        for (std::size_t m = 0; m != 2; ++m) {
            const std::size_t commands = c == 0 ? 3 : 12;
            const std::size_t members = m == 0 ? 17 : 68;
            run_trial("commands=" + std::to_string(commands)
                      + ", openings=" + std::to_string(members), [&] {
                results[c][m] = frozen_commands(commands, members);
                finished[c][m] = true;
            });
        }
    }
    run_trial("compare measured command and opening growth axes", [&] {
        MEM_NEED(finished[0][0] && finished[0][1] && finished[1][0] && finished[1][1]);
        MEM_CHECK(results[1][0].files == 4 * results[0][0].files);
        MEM_CHECK(results[1][0].bytes == 4 * results[0][0].bytes);
        MEM_CHECK(results[0][1].files == results[0][0].files);
        MEM_CHECK(results[0][1].bytes == 4 * results[0][0].bytes);
        MEM_CHECK(results[1][1].bytes == 16 * results[0][0].bytes);
    });
    std::printf("memory proof spill_frozen_openings: %zu checks, %zu failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
