#pragma once

#include <pineforge/run_failure.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace pineforge::detail {

inline constexpr std::uint64_t kRunQuotaMax = (std::uint64_t{1} << 53) - 1;
inline constexpr std::size_t kRunQuotaWireSize = 256;
inline constexpr std::uint16_t kRunQuotaVersion = 1;
inline constexpr std::uint16_t kRunQuotaReply = 0x8000;

inline constexpr char kRunQuotaModeEnv[] = "PF_RUN_QUOTA_MODE";
inline constexpr char kRunQuotaNativeFdEnv[] = "PF_RUN_QUOTA_NATIVE_FD";
inline constexpr char kRunQuotaPythonFdEnv[] = "PF_RUN_QUOTA_PYTHON_FD";
inline constexpr char kRunQuotaEnforcement[] = "cooperative";
inline constexpr char kRunQuotaManagedLine[] = "quota=managed enforcement=cooperative";
inline constexpr char kRunQuotaUnmanagedLine[] = "quota=unmanaged";

enum class QuotaMode { unmanaged, managed };
// Captured once at the first mode/attachment call, before any strategy code.
// The host must call during trusted startup. Any marker component means
// managed; invalid/partial markers refuse on attach.
// No request, strategy setting or later environment edit changes this mode.
QuotaMode mode() noexcept;
// No newline, allocation or output. The wrapper owns stderr/receipt emission.
const char* quota_line() noexcept;
// Test support only: the last predicate plus its environment/capture snapshot.
// No output; never part of typed failure args, receipts, wire or product text.
// The returned pointer lasts until the next call on this thread.
const char* last_admission_diagnostic() noexcept;

enum class QuotaOpcode : std::uint16_t {
    hello = 1, reserve, commit, cancel, release, snapshot, finish,
};
enum class QuotaStage : std::uint64_t {
    admission = 1, transpile, compile, execution, assembly, finalizer, converter, publication,
};
enum class QuotaArtifact : std::uint64_t {
    workdir = 1, compiler, table, ledger, arena, curve, sink, spool,
    column_index, column_data, json, stdout_stream, conversion, diagnostic,
};
enum class QuotaRole : std::uint64_t { host = 1, compiler, worker, finalizer, converter, reaper };
enum class QuotaResource : std::uint64_t { scratch_bytes = 1, scratch_files, output_bytes, descriptors };
// Configuration replies use the resource slot for a QuotaSetting.
enum class QuotaSetting : std::uint64_t {
    scratch_bytes = 1, scratch_files, output_bytes, descriptors,
    compiler, facts, workdir, channel, profile, protocol,
};
enum class QuotaStatus : std::uint64_t {
    ok = 0, resource_exceeded, descriptors_exceeded, missing_limit, malformed_limit,
    counter_overflow, protocol, containment_unavailable, unmanaged_path, unknown_bound,
    unsupported_route,
};
enum class QuotaSlot : std::size_t {
    nonce_hi, nonce_lo, object, generation, ticket, stage, artifact, role, resource,
    old_extent, new_extent, delta_disk, delta_files, delta_output, delta_fds, status,
    used, requested, limit, peak, committed, reserved, soft_nofile, pid, tid, device,
    inode, zero0, zero1, zero2,
};

// Encoding is explicitly little endian; this type is never sent as a struct.
// decode accepts requests and replies, validates framing and the three zero
// slots, and leaves opcode-specific field validation to the endpoint.
struct QuotaMessage {
    std::uint16_t opcode = static_cast<std::uint16_t>(QuotaOpcode::hello);
    std::uint64_t sequence = 0;
    std::array<std::uint64_t, 30> slots{};

    std::uint64_t& operator[](QuotaSlot slot) noexcept {
        return slots[static_cast<std::size_t>(slot)];
    }
    std::uint64_t operator[](QuotaSlot slot) const noexcept {
        return slots[static_cast<std::size_t>(slot)];
    }
};
using QuotaWire = std::array<unsigned char, kRunQuotaWireSize>;
QuotaWire encode_quota_message(const QuotaMessage&);
QuotaMessage decode_quota_message(const unsigned char*, std::size_t);

struct QuotaAmounts {
    std::uint64_t scratch_bytes = 0;
    std::uint64_t scratch_files = 0;
    std::uint64_t output_bytes = 0;
    std::uint64_t descriptors = 0;
};
struct QuotaObject {
    // Opaque identities are full u64s, never JSON numbers. generation belongs
    // to the object, not its fd number. A new backing must use a new identity.
    std::uint64_t id = 0;
    std::uint64_t generation = 0;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
};
struct QuotaSnapshot {
    QuotaMessage message;
    RunFailureValue failure;
};
struct RunQuotaEndpoint;

// A ticket keeps its endpoint alive through moves, old readers and later
// attempts. Destruction deliberately leaves unreported growth charged:
// destruction cannot prove that a failed syscall realized zero bytes.
class QuotaTicket {
public:
    QuotaTicket() = default;
    QuotaTicket(const QuotaTicket&) = delete;
    QuotaTicket& operator=(const QuotaTicket&) = delete;
    QuotaTicket(QuotaTicket&&) noexcept = default;
    QuotaTicket& operator=(QuotaTicket&&) noexcept = default;

    std::uint64_t id() const noexcept { return ticket_; }
    // Cumulative realization of this reservation, not another delta. Report
    // the host-policy-rounded real storage extent after partial write/error.
    // The host must confirm a new inode before accepting its file COMMIT.
    // Call cancel afterwards to retire even a fully realized ticket.
    void commit(QuotaAmounts realized, std::uint64_t actual_extent,
                std::uint64_t device = 0, std::uint64_t inode = 0);
    void cancel();  // Retires the ticket; returns only its unrealized portion.

private:
    friend class RunQuota;
    std::shared_ptr<RunQuotaEndpoint> endpoint_;
    QuotaMessage reservation_;
    std::uint64_t ticket_ = 0;
};

class RunQuota {
public:
    // Duplicates a pre-opened, connected AF_UNIX SOCK_SEQPACKET endpoint.
    // Repeated attachment to the same socket shares its mutex and sequence.
    static RunQuota from_fd(int fd, QuotaRole role = QuotaRole::worker);
    // With no host marker, returns a no-op client (including absurd sizes).
    // Partial/invalid markers refuse. The host alone parses limits; HELLO
    // authenticates the attempt. Call before loading any strategy code.
    static RunQuota from_environment();

    QuotaTicket reserve(QuotaObject, QuotaAmounts, std::uint64_t old_extent,
                        std::uint64_t new_extent, QuotaStage, QuotaArtifact);
    // Call only AFTER successful shrink/last-close. Output is never refunded.
    void release(QuotaObject, QuotaAmounts, std::uint64_t old_extent,
                 std::uint64_t new_extent, QuotaStage, QuotaArtifact);
    QuotaSnapshot snapshot(QuotaResource = QuotaResource::scratch_bytes);
    QuotaSnapshot finish(QuotaResource = QuotaResource::scratch_bytes);
    std::shared_ptr<const QuotaSnapshot> refusal() const;
    void check() const;

private:
    explicit RunQuota(std::shared_ptr<RunQuotaEndpoint> endpoint)
        : endpoint_(std::move(endpoint)) {}
    std::shared_ptr<RunQuotaEndpoint> endpoint_;
};

// The most recently attached environment endpoint is the active attempt for
// failure rendering. Explicit from_fd clients do not change that binding.
// The latch survives clear_run_failure and status-only exception translation.
RunFailureValue run_quota_failure() noexcept;

// Managed callers only: checked before rounding or constructing a request.
// Unmanaged callers retain their existing sizing path.
std::uint64_t quota_rounded_extent(std::uint64_t extent, std::uint64_t block_size);

}  // namespace pineforge::detail
