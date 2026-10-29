#include <pineforge/detail/run_quota.hpp>

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <map>
#include <new>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

using namespace pineforge;
using namespace pineforge::detail;

namespace {
std::atomic<bool> count_allocations{false};
std::atomic<std::size_t> allocations{0};
}  // namespace

void* operator new(std::size_t size) {
    if (count_allocations.load(std::memory_order_relaxed)) {
        allocations.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {

void unmanaged_without_allocations() {
    assert(mode() == QuotaMode::unmanaged);
    assert(std::strcmp(quota_line(), "quota=unmanaged") == 0);
    // A hostile in-process update cannot opt into management after startup.
    assert(::setenv(kRunQuotaModeEnv, "managed", 1) == 0);
    assert(::setenv(kRunQuotaNativeFdEnv, "0", 1) == 0);
    assert(::setenv(kRunQuotaPythonFdEnv, "0", 1) == 0);
    assert(::setenv("PF_RUN_SCRATCH_BYTES", "0", 1) == 0);
    assert(mode() == QuotaMode::unmanaged);
    // Prove the instrumentation observes a real allocation before measuring.
    count_allocations.store(true);
    void* probe = ::operator new(1);
    ::operator delete(probe);
    count_allocations.store(false);
    assert(allocations.load() == 1);
    allocations.store(0);
    count_allocations.store(true);
    {
        auto quota = RunQuota::from_environment();
        auto explicit_attachment = RunQuota::from_fd(-1);
        explicit_attachment.check();
        const QuotaObject object{UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
        const QuotaAmounts absurd{UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
        auto ticket = quota.reserve(object, absurd, UINT64_MAX, UINT64_MAX,
                                    QuotaStage::execution, QuotaArtifact::table);
        assert(ticket.id() == 0);
        ticket.commit(absurd, UINT64_MAX, UINT64_MAX, UINT64_MAX);
        auto moved = std::move(ticket);
        moved.cancel();
        moved.cancel();
        quota.release(object, absurd, UINT64_MAX, UINT64_MAX,
                      QuotaStage::execution, QuotaArtifact::table);
        const auto snapshot = quota.snapshot();
        const auto finished = quota.finish();
        assert(snapshot.failure.code == RunFailureCode::none);
        assert(finished.failure.code == RunFailureCode::none);
        assert(snapshot.message[QuotaSlot::used] == 0);
        assert(snapshot.message[QuotaSlot::reserved] == 0);
        assert(finished.message[QuotaSlot::peak] == 0);
        assert(!quota.refusal());
        quota.check();
        // FINISH does not introduce a refusal on an unmanaged caller.
        auto later = quota.reserve(object, absurd, UINT64_MAX, UINT64_MAX,
                                   QuotaStage::execution, QuotaArtifact::table);
        later.cancel();
        assert(run_quota_failure().code == RunFailureCode::none);
        assert(mode() == QuotaMode::unmanaged);
        assert(std::strcmp(quota_line(), "quota=unmanaged") == 0);
    }
    count_allocations.store(false);
    assert(allocations.load() == 0);
}

// Test support only. The getter includes the raw capture environment, pid,
// capture/record counters and fresh/cache origin from the recording call.
// Reading it must not recapture the marker or inspect a later environment.
void admission_diagnostic() {
    std::cerr << "admission_diagnostic: " << last_admission_diagnostic() << std::endl;
}

void failure(const RunFailureValue& value) {
    // Flush before stdout so the harness can read diagnostics with each result.
    admission_diagnostic();
    std::cout << "{\"code\":\"" << run_failure_code_name(value.code)
              << "\",\"args\":" << (value.args ? *value.args : "{}") << "}" << std::endl;
}

void usage(const QuotaSnapshot& snapshot) {
    const auto& message = snapshot.message;
    std::cout << "{\"used\":" << message[QuotaSlot::used]
              << ",\"committed\":" << message[QuotaSlot::committed]
              << ",\"reserved\":" << message[QuotaSlot::reserved]
              << ",\"peak\":" << message[QuotaSlot::peak]
              << ",\"limit\":" << message[QuotaSlot::limit] << "}" << std::endl;
}

void codec_and_catalog() {
    for (std::uint16_t opcode = 1; opcode <= 7; ++opcode) {
        for (const auto reply : {std::uint16_t{0}, std::uint16_t{0x8000}}) {
            QuotaMessage message;
            message.opcode = opcode | reply;
            message.sequence = UINT64_C(0xfedcba9876543210);
            for (std::size_t i = 0; i < 27; ++i) {
                message.slots[i] = UINT64_C(0x8102030405060708) + i;
            }
            const auto wire = encode_quota_message(message);
            assert(wire.size() == 256 && std::string(wire.begin(), wire.begin() + 4) == "PFQ1");
            assert(wire[4] == 1 && wire[5] == 0 && wire[6] == opcode && wire[7] == (reply ? 128 : 0));
            for (std::size_t i = 0; i < 30; ++i) {
                for (std::size_t byte = 0; byte < 8; ++byte) {
                    assert(wire[16 + 8 * i + byte] == ((message.slots[i] >> (8 * byte)) & 255));
                }
            }
            const auto decoded = decode_quota_message(wire.data(), wire.size());
            assert(decoded.opcode == message.opcode && decoded.sequence == message.sequence);
            assert(decoded.slots == message.slots);
        }
    }
    assert(quota_rounded_extent(0, 4096) == 0);
    assert(quota_rounded_extent(4097, 4096) == 8192);
    try {
        quota_rounded_extent((UINT64_C(1) << 53) - 1, 4096);
        assert(false);
    } catch (const coded<std::runtime_error>& error) {
        assert(error.run_failure_code() == RunFailureCode::run_quota_configuration_invalid);
        assert(*error.run_failure().args ==
               "{\"reason\":\"counter_overflow\",\"setting\":\"scratch_bytes\",\"stage\":\"admission\"}");
    }
    RunFailureArgs args{{"artifact", "table"}, {"resource", "scratch_bytes"}, {"stage", "execution"},
                        {"used", 7}, {"requested", 1}, {"limit", 7}, {"peak", 7}};
    auto value = make_run_failure(RunFailureCode::run_resource_quota_exceeded, args);
    assert(value.code == RunFailureCode::run_resource_quota_exceeded);
    assert(run_failure_code_class(value.code) == RunFailureClass::resource);
    assert(!run_failure_code_retryable(value.code));
    assert(static_cast<unsigned>(RunFailureCode::scratch_io_failed) == 69);
    args[0] = RunFailureArg("artifact", "unlisted");
    assert(make_run_failure(value.code, args).code == RunFailureCode::engine_invariant);
    args[0] = RunFailureArg("artifact", "table");
    args[3] = RunFailureArg("used", UINT64_C(1) << 53);
    assert(make_run_failure(value.code, args).code == RunFailureCode::engine_invariant);
    args[3] = RunFailureArg("used", -1);
    assert(make_run_failure(value.code, args).code == RunFailureCode::engine_invariant);
}

int wire_filter() {
    std::vector<unsigned char> data;
    char byte;
    while (data.size() <= 256 && std::cin.get(byte)) data.push_back(static_cast<unsigned char>(byte));
    try {
        const auto message = decode_quota_message(data.data(), data.size());
        const auto encoded = encode_quota_message(message);
        std::cout.write(reinterpret_cast<const char*>(encoded.data()), encoded.size());
        return std::cout ? 0 : 1;
    } catch (const coded<std::runtime_error>& error) {
        std::cerr << run_failure_code_name(error.run_failure_code()) << '\n';
        admission_diagnostic();
        return 1;
    }
}

int client() {
    try {
        assert(std::strcmp(quota_line(), mode() == QuotaMode::managed
            ? "quota=managed enforcement=cooperative" : "quota=unmanaged") == 0);
        assert(std::strcmp(kRunQuotaEnforcement, "cooperative") == 0);
        auto quota = RunQuota::from_environment();
        // Attach again to prove that HELLO is not repeated on a shared endpoint.
        auto same = RunQuota::from_environment();
        same.check();
        std::cout << "{\"ready\":true}" << std::endl;
        std::map<std::uint64_t, QuotaTicket> tickets;
        std::uint64_t next_handle = 1;
        std::string line;
        while (std::getline(std::cin, line)) {
            std::istringstream input(line);
            std::string operation;
            input >> operation;
            try {
                if (operation == "reserve" || operation == "release") {
                    QuotaObject object;
                    QuotaAmounts amounts;
                    std::uint64_t old_extent, new_extent, stage, artifact;
                    input >> object.id >> object.generation >> object.device >> object.inode
                          >> old_extent >> new_extent >> amounts.scratch_bytes >> amounts.scratch_files
                          >> amounts.output_bytes >> amounts.descriptors >> stage >> artifact;
                    assert(input);
                    if (operation == "reserve") {
                        auto ticket = quota.reserve(object, amounts, old_extent, new_extent,
                            static_cast<QuotaStage>(stage), static_cast<QuotaArtifact>(artifact));
                        const auto handle = next_handle++;
                        const auto number = ticket.id();
                        tickets.emplace(handle, std::move(ticket));
                        std::cout << "{\"ticket\":" << handle << ",\"wire_ticket\":" << number << "}" << std::endl;
                    } else {
                        quota.release(object, amounts, old_extent, new_extent,
                                      static_cast<QuotaStage>(stage), static_cast<QuotaArtifact>(artifact));
                        std::cout << "{\"ok\":true}" << std::endl;
                    }
                } else if (operation == "commit") {
                    std::uint64_t handle, extent, device, inode;
                    QuotaAmounts amounts;
                    input >> handle >> amounts.scratch_bytes >> amounts.scratch_files
                          >> amounts.output_bytes >> amounts.descriptors >> extent >> device >> inode;
                    assert(input);
                    auto found = tickets.find(handle);
                    assert(found != tickets.end());
                    found->second.commit(amounts, extent, device, inode);
                    std::cout << "{\"ok\":true}" << std::endl;
                } else if (operation == "cancel") {
                    std::uint64_t handle;
                    input >> handle;
                    assert(input);
                    auto found = tickets.find(handle);
                    assert(found != tickets.end());
                    // Move before use: the ticket must retain its original owner.
                    auto moved = std::move(found->second);
                    tickets.erase(found);
                    moved.cancel();
                    std::cout << "{\"ok\":true}" << std::endl;
                } else if (operation == "snapshot" || operation == "finish") {
                    unsigned resource;
                    input >> resource;
                    assert(input);
                    usage(operation == "snapshot" ? quota.snapshot(static_cast<QuotaResource>(resource))
                                                  : quota.finish(static_cast<QuotaResource>(resource)));
                } else if (operation == "attach") {
                    int descriptor;
                    input >> descriptor;
                    assert(input);
                    // A host-controlled second attempt, not a mode switch.
                    quota = RunQuota::from_fd(descriptor);
                    std::cout << "{\"ok\":true}" << std::endl;
                } else if (operation == "hostile_environment") {
                    std::string setting;
                    input >> setting;
                    assert(input);
                    const auto before = mode();
                    assert(::setenv(kRunQuotaModeEnv, setting.c_str(), 1) == 0);
                    assert(::unsetenv(kRunQuotaNativeFdEnv) == 0);
                    assert(::unsetenv(kRunQuotaPythonFdEnv) == 0);
                    assert(::setenv("PF_RUN_SCRATCH_BYTES", setting.c_str(), 1) == 0);
                    assert(mode() == before);
                    auto unchanged = RunQuota::from_environment();
                    unchanged.check();
                    assert(std::strcmp(quota_line(), before == QuotaMode::managed
                        ? "quota=managed enforcement=cooperative" : "quota=unmanaged") == 0);
                    std::cout << "{\"ok\":true}" << std::endl;
                } else if (operation == "replace_channel") {
                    int target, replacement;
                    input >> target >> replacement;
                    assert(input);
                    assert(::dup2(replacement, target) == target);
                    assert(mode() == QuotaMode::managed);
                    auto replaced = RunQuota::from_environment();
                    replaced.check();
                    assert(false && "replaced host channel was accepted");
                } else if (operation == "refusal") {
                    const auto snapshot = quota.refusal();
                    assert(snapshot);
                    failure(snapshot->failure);
                } else if (operation == "render") {
                    failure(classify_run_failure(std::runtime_error("translated I/O status")));
                } else if (operation == "check") {
                    quota.check();
                    std::cout << "{\"ok\":true}" << std::endl;
                } else {
                    assert(false && "unknown test-driver command");
                }
            } catch (const coded<std::runtime_error>& error) {
                failure(error.run_failure());
            }
        }
        return 0;
    } catch (const coded<std::runtime_error>& error) {
        failure(error.run_failure());
        return 1;
    }
}

}  // namespace

int main(int argc, char** argv) {
    // Freeze the real host environment explicitly, after runtime initialization
    // and before dispatch, including the unmanaged environment-mutation test.
    static_cast<void>(mode());
    if (argc == 2 && std::string(argv[1]) == "--unmanaged") {
        unmanaged_without_allocations();
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--wire") return wire_filter();
    if (argc == 2 && std::string(argv[1]) == "--client") return client();
    assert(argc == 1);
    codec_and_catalog();
    // One CTest row with a same-named C++ unit. Its real-socket tests are
    // driven by Python; children select --client or --wire, never this path.
    const std::string source = __FILE__;
    const auto slash = source.find_last_of('/');
    assert(slash != std::string::npos);
    const auto script = source.substr(0, slash + 1) + "test_quota_contract.py";
    ::execlp("python3", "python3", "-B", script.c_str(), "--native", argv[0],
             static_cast<char*>(nullptr));
    std::cerr << "cannot launch quota contract tests\n";
    return 1;
}
