#include <pineforge/detail/run_quota.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace pineforge::detail {
namespace {

struct SocketIdentity {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
};

constexpr std::size_t kAdmissionPredicateSize = 1024;
constexpr std::size_t kAdmissionDiagnosticSize = 3072;
constexpr std::size_t kMarkerEnvironmentLimit = 32;
constexpr std::size_t kQuotedMarkerEnvironmentSize = 4 * kMarkerEnvironmentLimit + 3;

struct HostMarker {
    // Only capture may select unmanaged. An uncaptured object must fail closed.
    bool captured = false;
    QuotaMode mode = QuotaMode::managed;
    QuotaStatus status = QuotaStatus::protocol;
    int native_fd = -1;
    int python_fd = -1;
    SocketIdentity native_socket;
    SocketIdentity python_socket;
    std::uint64_t capture_number = 0;
    std::uint64_t capture_pid = 0;
    unsigned environment_present = 0;
    unsigned environment_truncated = 0;
    char raw_mode[kQuotedMarkerEnvironmentSize] = "\"<unread>\"";
    char raw_native_fd[kQuotedMarkerEnvironmentSize] = "\"<unread>\"";
    char raw_python_fd[kQuotedMarkerEnvironmentSize] = "\"<unread>\"";
    char capture_refusal[kAdmissionPredicateSize] = "marker_not_captured";

    HostMarker() noexcept = default;
    HostMarker(const HostMarker&) = delete;
    HostMarker& operator=(const HostMarker&) = delete;
};

std::atomic<std::uint64_t> host_marker_capture_count{0};

struct AdmissionDiagnosticContext {
    const HostMarker* marker = nullptr;
    const char* source = "none";
    const char* call = "none";
    char* capture_refusal = nullptr;
};
thread_local AdmissionDiagnosticContext admission_diagnostic_context;

class AdmissionDiagnosticScope {
public:
    AdmissionDiagnosticScope(const HostMarker& marker, const char* source, const char* call,
                             char* capture_refusal = nullptr) noexcept
        : previous_(admission_diagnostic_context) {
        admission_diagnostic_context = {&marker, source, call, capture_refusal};
    }
    ~AdmissionDiagnosticScope() { admission_diagnostic_context = previous_; }
    AdmissionDiagnosticScope(const AdmissionDiagnosticScope&) = delete;
    AdmissionDiagnosticScope& operator=(const AdmissionDiagnosticScope&) = delete;

private:
    AdmissionDiagnosticContext previous_;
};

// Test support only. Initialize storage on demand, after the marker's storage
// exists. Unmanaged admission never touches diagnostics, locks or getpid.
struct AdmissionDiagnosticState {
    std::mutex mutex;
    char text[kAdmissionDiagnosticSize] = "none";
    std::uint64_t records = 0;
};

AdmissionDiagnosticState& admission_diagnostic_state() noexcept {
    static AdmissionDiagnosticState state;
    return state;
}

template <typename... Args>
void record_admission_diagnostic(const char* format, Args... args) noexcept {
    const int saved_errno = errno;
    char predicate[kAdmissionPredicateSize]{};
    std::snprintf(predicate, sizeof(predicate), format, args...);
    const auto& context = admission_diagnostic_context;
    const auto* marker = context.marker;
    if (context.capture_refusal) {
        std::memcpy(context.capture_refusal, predicate, sizeof(predicate));
    }
#if defined(__unix__) || defined(__APPLE__)
    const auto pid = static_cast<std::uint64_t>(::getpid());
#else
    const std::uint64_t pid = 0;
#endif
    auto& diagnostic = admission_diagnostic_state();
    {
        std::lock_guard<std::mutex> lock(diagnostic.mutex);
        std::snprintf(diagnostic.text, sizeof(diagnostic.text),
            "%s diagnostic_revision=6 record=%" PRIu64 " call=%s pid=%" PRIu64
            " capture_pid=%" PRIu64 " capture_count=%" PRIu64 " marker_capture=%" PRIu64
            " marker_source=%s marker_captured=%d marker_native_fd=%d marker_python_fd=%d"
            " env_present=%u env_truncated=%u env_mode=%s env_native_fd=%s env_python_fd=%s",
            predicate, ++diagnostic.records, context.call, pid,
            marker ? marker->capture_pid : 0,
            host_marker_capture_count.load(std::memory_order_relaxed),
            marker ? marker->capture_number : 0, context.source,
            marker && marker->captured ? 1 : 0,
            marker ? marker->native_fd : -1, marker ? marker->python_fd : -1,
            marker ? marker->environment_present : 0,
            marker ? marker->environment_truncated : 0,
            marker ? marker->raw_mode : "\"<unread>\"",
            marker ? marker->raw_native_fd : "\"<unread>\"",
            marker ? marker->raw_python_fd : "\"<unread>\"");
    }
    errno = saved_errno;
}

bool marker_is_captured(const HostMarker& marker, const char* source, const char* call) noexcept {
    if (marker.captured && marker.capture_number != 0) return true;
    const AdmissionDiagnosticScope diagnostic(marker, source, call);
    record_admission_diagnostic("marker_not_captured native_fd=%d python_fd=%d",
                                marker.native_fd, marker.python_fd);
    return false;
}

void capture_environment_text(const char* text, char* quoted, unsigned bit,
                              HostMarker& marker) noexcept {
    if (!text) {
        std::memcpy(quoted, "\"<unset>\"", sizeof("\"<unset>\""));
        return;
    }
    marker.environment_present |= bit;
    constexpr char hex[] = "0123456789abcdef";
    char* out = quoted;
    *out++ = '"';
    std::size_t count = 0;
    for (; count < kMarkerEnvironmentLimit && text[count]; ++count) {
        const auto byte = static_cast<unsigned char>(text[count]);
        if (byte == '"' || byte == '\\') {
            *out++ = '\\';
            *out++ = static_cast<char>(byte);
        } else if (byte < 0x20 || byte > 0x7e) {
            *out++ = '\\';
            *out++ = 'x';
            *out++ = hex[byte >> 4];
            *out++ = hex[byte & 15];
        } else {
            *out++ = static_cast<char>(byte);
        }
    }
    if (text[count]) marker.environment_truncated |= bit;
    *out++ = '"';
    *out = '\0';
}

bool same_socket(SocketIdentity left, SocketIdentity right) noexcept {
    return left.device == right.device && left.inode == right.inode;
}

bool socket_identity(int fd, SocketIdentity& identity, const char* phase) noexcept {
#if defined(__unix__) || defined(__APPLE__)
    int type = 0;
    socklen_t type_size = sizeof(type);
    struct sockaddr_storage peer{};
    socklen_t peer_size = sizeof(peer);
    struct stat metadata{};
    if (fd < 3) {
        record_admission_diagnostic("socket_fd_below_min phase=%s fd=%d min_fd=3", phase, fd);
        return false;
    }
    if (::fcntl(fd, F_GETFD) < 0) {
        record_admission_diagnostic("socket_getfd_failed phase=%s fd=%d errno=%d", phase, fd, errno);
        return false;
    }
    if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_size) != 0) {
        record_admission_diagnostic("socket_getsockopt_failed phase=%s fd=%d errno=%d type=%d type_size=%u",
                                    phase, fd, errno, type, static_cast<unsigned>(type_size));
        return false;
    }
    if (type != SOCK_SEQPACKET) {
        record_admission_diagnostic("socket_type_mismatch phase=%s fd=%d type=%d expected_type=%d type_size=%u",
                                    phase, fd, type, SOCK_SEQPACKET, static_cast<unsigned>(type_size));
        return false;
    }
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peer_size) != 0) {
        record_admission_diagnostic("socket_getpeername_failed phase=%s fd=%d errno=%d type=%d",
                                    phase, fd, errno, type);
        return false;
    }
    if (peer.ss_family != AF_UNIX) {
        record_admission_diagnostic("socket_peer_family_mismatch phase=%s fd=%d family=%u expected_family=%d peer_size=%u",
                                    phase, fd, static_cast<unsigned>(peer.ss_family), AF_UNIX,
                                    static_cast<unsigned>(peer_size));
        return false;
    }
    if (::fstat(fd, &metadata) != 0) {
        record_admission_diagnostic("socket_fstat_failed phase=%s fd=%d errno=%d type=%d", phase, fd, errno, type);
        return false;
    }
    if (!S_ISSOCK(metadata.st_mode)) {
        record_admission_diagnostic("socket_mode_not_socket phase=%s fd=%d type=%d mode=%" PRIo64
                                    " dev=%" PRIu64 " ino=%" PRIu64, phase, fd, type,
                                    static_cast<std::uint64_t>(metadata.st_mode),
                                    static_cast<std::uint64_t>(metadata.st_dev),
                                    static_cast<std::uint64_t>(metadata.st_ino));
        return false;
    }
    identity = {static_cast<std::uint64_t>(metadata.st_dev),
                static_cast<std::uint64_t>(metadata.st_ino)};
    return true;
#else
    (void)identity;
    record_admission_diagnostic("socket_platform_unsupported phase=%s fd=%d", phase, fd);
    return false;
#endif
}

const char* marker_fd(const char* text, int& fd) noexcept {
    if (!text) return "marker_fd_missing";
    if (!*text) return "marker_fd_empty";
    fd = 0;
    for (const char* at = text; *at; ++at) {
        if (*at < '0' || *at > '9') return "marker_fd_nondigit";
        if (fd > (INT_MAX - (*at - '0')) / 10) return "marker_fd_overflow";
        fd = fd * 10 + (*at - '0');
    }
    return fd >= 3 ? nullptr : "marker_fd_below_min";
}

void capture_host_marker(HostMarker& marker) noexcept {
    marker.capture_number = host_marker_capture_count.fetch_add(1, std::memory_order_relaxed) + 1;
    const char* setting = std::getenv(kRunQuotaModeEnv);
    const char* native = std::getenv(kRunQuotaNativeFdEnv);
    const char* python = std::getenv(kRunQuotaPythonFdEnv);
    // getenv inspects process memory. No descriptors, locks, heap or syscalls
    // are touched when all three marker components are absent.
    if (!setting && !native && !python) {
        marker.mode = QuotaMode::unmanaged;
        marker.status = QuotaStatus::ok;
        marker.captured = true;
        return;
    }
    capture_environment_text(setting, marker.raw_mode, 1, marker);
    capture_environment_text(native, marker.raw_native_fd, 2, marker);
    capture_environment_text(python, marker.raw_python_fd, 4, marker);
#if defined(__unix__) || defined(__APPLE__)
    marker.capture_pid = static_cast<std::uint64_t>(::getpid());
#endif
    marker.captured = true;
    const AdmissionDiagnosticScope diagnostic(marker, "fresh", "capture_host_marker", marker.capture_refusal);
    // Parse both numbers for diagnostics even if an earlier marker component
    // is invalid. The admission predicates and their refusal order stay fixed.
    const char* native_error = marker_fd(native, marker.native_fd);
    const char* python_error = marker_fd(python, marker.python_fd);
    if (!setting || std::strcmp(setting, "managed") != 0) {
        record_admission_diagnostic("marker_mode_invalid mode_present=%d native_fd=%d python_fd=%d native_parse=%s python_parse=%s",
                                    setting ? 1 : 0, marker.native_fd, marker.python_fd,
                                    native_error ? native_error : "ok", python_error ? python_error : "ok");
        return;
    }
    if (native_error || python_error) {
        record_admission_diagnostic("%s channel=%s native_fd=%d python_fd=%d native_parse=%s python_parse=%s",
                                    native_error ? native_error : python_error,
                                    native_error ? "native" : "python", marker.native_fd, marker.python_fd,
                                    native_error ? native_error : "ok", python_error ? python_error : "ok");
        return;
    }
    if (!marker_is_captured(marker, "fresh", "capture_host_marker")) return;
    if (marker.native_fd < 3 || marker.python_fd < 3) {
        record_admission_diagnostic("marker_parsed_fd_invalid native_fd=%d python_fd=%d native_parse=ok python_parse=ok",
                                    marker.native_fd, marker.python_fd);
        return;
    }
    if (marker.native_fd == marker.python_fd) {
        record_admission_diagnostic("marker_fds_same_number native_fd=%d python_fd=%d",
                                    marker.native_fd, marker.python_fd);
        return;
    }
#if defined(__unix__) || defined(__APPLE__)
    if (!socket_identity(marker.native_fd, marker.native_socket, "marker_native")
        || !socket_identity(marker.python_fd, marker.python_socket, "marker_python")) return;
    if (same_socket(marker.native_socket, marker.python_socket)) {
        record_admission_diagnostic("marker_fds_same_socket native_fd=%d python_fd=%d native_dev=%" PRIu64
                                    " native_ino=%" PRIu64 " python_dev=%" PRIu64 " python_ino=%" PRIu64,
                                    marker.native_fd, marker.python_fd,
                                    marker.native_socket.device, marker.native_socket.inode,
                                    marker.python_socket.device, marker.python_socket.inode);
        return;
    }
    marker.status = QuotaStatus::ok;
#else
    marker.status = QuotaStatus::containment_unavailable;
    record_admission_diagnostic("marker_platform_unsupported native_fd=%d python_fd=%d",
                                marker.native_fd, marker.python_fd);
#endif
}

struct HostMarkerCache {
    HostMarker marker;
    HostMarkerCache() noexcept { capture_host_marker(marker); }
};

// The host must call mode/attach before untrusted code. Construct the mutable
// storage first and capture in place: no pre-main const initialization, return
// copy, move or later default initialization can replace the captured marker.
const HostMarker& host_marker() noexcept {
    static HostMarkerCache cache;
    return cache.marker;
}

constexpr const char* kStages[] = {
    "", "admission", "transpile", "compile", "execution", "assembly", "finalizer",
    "converter", "publication",
};
constexpr const char* kArtifacts[] = {
    "", "workdir", "compiler", "table", "ledger", "arena", "curve", "sink", "spool",
    "column_index", "column_data", "json", "stdout", "conversion", "diagnostic",
};
constexpr const char* kRoles[] = {"", "host", "compiler", "worker", "finalizer", "converter", "reaper"};
constexpr const char* kSettings[] = {
    "", "scratch_bytes", "scratch_files", "output_bytes", "descriptors", "compiler",
    "facts", "workdir", "channel", "profile", "protocol",
};
constexpr const char* kReasons[] = {
    "", "", "", "missing_limit", "malformed_limit", "counter_overflow", "protocol",
    "containment_unavailable", "unmanaged_path", "unknown_bound", "unsupported_route",
};

RunFailureValue configuration(QuotaStatus status, QuotaSetting setting,
                              QuotaStage stage = QuotaStage::admission) {
    return make_run_failure(RunFailureCode::run_quota_configuration_invalid,
        {{"stage", kStages[static_cast<std::size_t>(stage)]},
         {"reason", kReasons[static_cast<std::size_t>(status)]},
         {"setting", kSettings[static_cast<std::size_t>(setting)]}});
}

[[noreturn]] void invalid(QuotaStatus status = QuotaStatus::protocol,
                          QuotaSetting setting = QuotaSetting::protocol) {
    throw coded<std::runtime_error>(configuration(status, setting), "run quota configuration invalid");
}

void validate_host_marker(const HostMarker& marker) {
    if (!marker_is_captured(marker, "cache", "validate_host_marker")) {
        invalid(QuotaStatus::protocol, QuotaSetting::channel);
    }
    if (marker.status != QuotaStatus::ok) {
        // Replay this marker's predicate, rather than an unrelated last record.
        record_admission_diagnostic("%s", marker.capture_refusal);
        invalid(marker.status, QuotaSetting::channel);
    }
    SocketIdentity native, python;
    if (!socket_identity(marker.native_fd, native, "validate_native")
        || !socket_identity(marker.python_fd, python, "validate_python")) {
        invalid(QuotaStatus::protocol, QuotaSetting::channel);
    }
    if (!same_socket(native, marker.native_socket)) {
        record_admission_diagnostic("marker_native_identity_changed native_fd=%d python_fd=%d retained_dev=%" PRIu64
                                    " retained_ino=%" PRIu64 " current_dev=%" PRIu64 " current_ino=%" PRIu64,
                                    marker.native_fd, marker.python_fd,
                                    marker.native_socket.device, marker.native_socket.inode,
                                    native.device, native.inode);
        invalid(QuotaStatus::protocol, QuotaSetting::channel);
    }
    if (!same_socket(python, marker.python_socket)) {
        record_admission_diagnostic("marker_python_identity_changed native_fd=%d python_fd=%d retained_dev=%" PRIu64
                                    " retained_ino=%" PRIu64 " current_dev=%" PRIu64 " current_ino=%" PRIu64,
                                    marker.native_fd, marker.python_fd,
                                    marker.python_socket.device, marker.python_socket.inode,
                                    python.device, python.inode);
        invalid(QuotaStatus::protocol, QuotaSetting::channel);
    }
}

void validate_opcode(std::uint16_t opcode) {
    const auto operation = static_cast<std::uint16_t>(opcode & ~kRunQuotaReply);
    if (operation < 1 || operation > 7) invalid();
}

template <typename UInt>
void put_le(unsigned char* out, UInt value) noexcept {
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        out[i] = static_cast<unsigned char>(value & 0xff);
        value >>= 8;
    }
}

template <typename UInt>
UInt get_le(const unsigned char* in) noexcept {
    UInt value = 0;
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        value |= static_cast<UInt>(in[i]) << (8 * i);
    }
    return value;
}

QuotaMessage config_message(QuotaStatus status, QuotaSetting setting,
                            QuotaStage stage = QuotaStage::admission) {
    QuotaMessage message;
    message[QuotaSlot::status] = static_cast<std::uint64_t>(status);
    message[QuotaSlot::resource] = static_cast<std::uint64_t>(setting);
    message[QuotaSlot::stage] = static_cast<std::uint64_t>(stage);
    message[QuotaSlot::artifact] = static_cast<std::uint64_t>(QuotaArtifact::workdir);
    message[QuotaSlot::role] = static_cast<std::uint64_t>(QuotaRole::worker);
    return message;
}

QuotaSnapshot read_snapshot(const QuotaMessage& message) {
    const auto status = message[QuotaSlot::status];
    if (status > static_cast<std::uint64_t>(QuotaStatus::unsupported_route)) invalid();
    for (const auto slot : {QuotaSlot::used, QuotaSlot::requested, QuotaSlot::limit,
                            QuotaSlot::peak, QuotaSlot::committed, QuotaSlot::reserved,
                            QuotaSlot::soft_nofile}) {
        if (message[slot] > kRunQuotaMax) invalid();
    }
    const auto committed = message[QuotaSlot::committed];
    const auto reserved = message[QuotaSlot::reserved];
    if (reserved > kRunQuotaMax - committed
        || message[QuotaSlot::used] != committed + reserved
        || message[QuotaSlot::used] > message[QuotaSlot::peak]
        || message[QuotaSlot::peak] > message[QuotaSlot::limit]) invalid();
    QuotaSnapshot snapshot{message, {}};
    if (status == 0) return snapshot;
    const auto stage = message[QuotaSlot::stage];
    const auto artifact = message[QuotaSlot::artifact];
    const auto role = message[QuotaSlot::role];
    const auto resource = message[QuotaSlot::resource];
    if (stage < 1 || stage > 8 || artifact < 1 || artifact > 14 || role < 1 || role > 6) invalid();
    RunFailureArgs args{{"stage", kStages[stage]}};
    if (status == static_cast<std::uint64_t>(QuotaStatus::resource_exceeded)
        || status == static_cast<std::uint64_t>(QuotaStatus::descriptors_exceeded)) {
        const bool descriptors = status == static_cast<std::uint64_t>(QuotaStatus::descriptors_exceeded);
        if (resource < 1 || resource > 4 || descriptors != (resource == 4)) invalid();
        args.emplace_back("used", message[QuotaSlot::used]);
        args.emplace_back("requested", message[QuotaSlot::requested]);
        args.emplace_back("limit", message[QuotaSlot::limit]);
        args.emplace_back("peak", message[QuotaSlot::peak]);
        if (descriptors) {
            args.emplace_back("process_role", kRoles[role]);
            args.emplace_back("soft_nofile", message[QuotaSlot::soft_nofile]);
        } else {
            args.emplace_back("resource", kSettings[resource]);
            args.emplace_back("artifact", kArtifacts[artifact]);
        }
        snapshot.failure = make_run_failure(
            descriptors ? RunFailureCode::run_descriptor_limit_exceeded
                        : RunFailureCode::run_resource_quota_exceeded, args);
    } else {
        if (resource < 1 || resource > 10) invalid();
        args.emplace_back("reason", kReasons[status]);
        args.emplace_back("setting", kSettings[resource]);
        snapshot.failure = make_run_failure(RunFailureCode::run_quota_configuration_invalid, args);
    }
    return snapshot;
}

void validate_reply_fields(const QuotaMessage& reply, QuotaOpcode opcode) {
    std::array<bool, 30> allowed{};
    const auto allow = [&](QuotaSlot slot) { allowed[static_cast<std::size_t>(slot)] = true; };
    allow(QuotaSlot::nonce_hi);
    allow(QuotaSlot::nonce_lo);
    if (reply[QuotaSlot::status]) {
        for (const auto slot : {QuotaSlot::status, QuotaSlot::stage, QuotaSlot::artifact,
                                QuotaSlot::role, QuotaSlot::resource, QuotaSlot::used,
                                QuotaSlot::requested, QuotaSlot::limit, QuotaSlot::peak,
                                QuotaSlot::committed, QuotaSlot::reserved, QuotaSlot::soft_nofile}) allow(slot);
    } else if (opcode == QuotaOpcode::hello) {
        for (const auto slot : {QuotaSlot::generation, QuotaSlot::role, QuotaSlot::pid,
                                QuotaSlot::soft_nofile}) allow(slot);
    } else if (opcode == QuotaOpcode::reserve) {
        allow(QuotaSlot::ticket);
    } else if (opcode == QuotaOpcode::snapshot || opcode == QuotaOpcode::finish) {
        for (const auto slot : {QuotaSlot::resource, QuotaSlot::used, QuotaSlot::limit,
                                QuotaSlot::peak, QuotaSlot::committed, QuotaSlot::reserved}) allow(slot);
    }
    for (std::size_t i = 0; i < allowed.size(); ++i) {
        if (!allowed[i] && reply.slots[i]) invalid();
    }
}

void set_amounts(QuotaMessage& message, QuotaAmounts amounts) {
    message[QuotaSlot::delta_disk] = amounts.scratch_bytes;
    message[QuotaSlot::delta_files] = amounts.scratch_files;
    message[QuotaSlot::delta_output] = amounts.output_bytes;
    message[QuotaSlot::delta_fds] = amounts.descriptors;
}

QuotaMessage object_message(QuotaOpcode opcode, QuotaObject object, QuotaAmounts amounts,
                            std::uint64_t old_extent, std::uint64_t new_extent,
                            QuotaStage stage, QuotaArtifact artifact) {
    QuotaMessage message;
    message.opcode = static_cast<std::uint16_t>(opcode);
    message[QuotaSlot::object] = object.id;
    message[QuotaSlot::generation] = object.generation;
    message[QuotaSlot::device] = object.device;
    message[QuotaSlot::inode] = object.inode;
    message[QuotaSlot::old_extent] = old_extent;
    message[QuotaSlot::new_extent] = new_extent;
    message[QuotaSlot::stage] = static_cast<std::uint64_t>(stage);
    message[QuotaSlot::artifact] = static_cast<std::uint64_t>(artifact);
    set_amounts(message, amounts);
    return message;
}

std::mutex endpoints_mutex;
using EndpointKey = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;
std::map<EndpointKey, std::weak_ptr<RunQuotaEndpoint>> endpoints;
std::shared_ptr<RunQuotaEndpoint> active_endpoint;

}  // namespace

QuotaMode mode() noexcept {
    const auto& marker = host_marker();
    if (!marker_is_captured(marker, "cache", "mode")) return QuotaMode::managed;
    return marker.mode;
}

const char* quota_line() noexcept {
    return mode() == QuotaMode::managed ? kRunQuotaManagedLine : kRunQuotaUnmanagedLine;
}

const char* last_admission_diagnostic() noexcept {
    thread_local char snapshot[kAdmissionDiagnosticSize];
    const int saved_errno = errno;
    auto& diagnostic = admission_diagnostic_state();
    {
        std::lock_guard<std::mutex> lock(diagnostic.mutex);
        std::memcpy(snapshot, diagnostic.text, sizeof(snapshot));
    }
    errno = saved_errno;
    return snapshot;
}

QuotaWire encode_quota_message(const QuotaMessage& message) {
    validate_opcode(message.opcode);
    if (message[QuotaSlot::zero0] || message[QuotaSlot::zero1] || message[QuotaSlot::zero2]) invalid();
    QuotaWire wire{};
    std::memcpy(wire.data(), "PFQ1", 4);
    put_le(wire.data() + 4, kRunQuotaVersion);
    put_le(wire.data() + 6, message.opcode);
    put_le(wire.data() + 8, message.sequence);
    for (std::size_t i = 0; i < message.slots.size(); ++i) {
        put_le(wire.data() + 16 + 8 * i, message.slots[i]);
    }
    return wire;
}

QuotaMessage decode_quota_message(const unsigned char* data, std::size_t size) {
    if (size != kRunQuotaWireSize || !data || std::memcmp(data, "PFQ1", 4) != 0
        || get_le<std::uint16_t>(data + 4) != kRunQuotaVersion) invalid();
    QuotaMessage message;
    message.opcode = get_le<std::uint16_t>(data + 6);
    message.sequence = get_le<std::uint64_t>(data + 8);
    validate_opcode(message.opcode);
    for (std::size_t i = 0; i < message.slots.size(); ++i) {
        message.slots[i] = get_le<std::uint64_t>(data + 16 + 8 * i);
    }
    if (message[QuotaSlot::zero0] || message[QuotaSlot::zero1] || message[QuotaSlot::zero2]) invalid();
    return message;
}

struct RunQuotaEndpoint {
    int fd = -1;
    std::uint64_t process = 0;
    QuotaRole role = QuotaRole::worker;
    std::mutex mutex;
    std::uint64_t sequence = 0;
    std::uint64_t nonce_hi = 0, nonce_lo = 0;
    bool finished = false;
    std::shared_ptr<const QuotaSnapshot> refusal;

    ~RunQuotaEndpoint() {
#if defined(__unix__) || defined(__APPLE__)
        if (fd >= 0) ::close(fd);
#endif
    }

    [[noreturn]] void fail(QuotaSnapshot snapshot) {
        if (!refusal) refusal = std::make_shared<const QuotaSnapshot>(std::move(snapshot));
        throw coded<std::runtime_error>(refusal->failure, "run quota refused");
    }

    [[noreturn]] void fail_configuration(QuotaStatus status, QuotaSetting setting,
                                        QuotaStage stage = QuotaStage::admission) {
        fail(read_snapshot(config_message(status, setting, stage)));
    }

    void validate_request(const QuotaMessage& request, QuotaStage fallback) {
        const auto stage_value = request[QuotaSlot::stage];
        const auto stage = stage_value >= 1 && stage_value <= 8
            ? static_cast<QuotaStage>(stage_value) : fallback;
        for (const auto slot : {QuotaSlot::old_extent, QuotaSlot::new_extent,
                                QuotaSlot::delta_disk, QuotaSlot::delta_files,
                                QuotaSlot::delta_output, QuotaSlot::delta_fds}) {
            if (request[slot] > kRunQuotaMax) {
                const auto setting = slot == QuotaSlot::delta_files ? QuotaSetting::scratch_files
                    : slot == QuotaSlot::delta_output ? QuotaSetting::output_bytes
                    : slot == QuotaSlot::delta_fds ? QuotaSetting::descriptors : QuotaSetting::scratch_bytes;
                fail_configuration(QuotaStatus::counter_overflow, setting, stage);
            }
        }
        const auto op = static_cast<QuotaOpcode>(request.opcode);
        if ((op == QuotaOpcode::reserve || op == QuotaOpcode::release)
            && (stage_value < 1 || stage_value > 8 || request[QuotaSlot::artifact] < 1
                || request[QuotaSlot::artifact] > 14)) {
            fail_configuration(QuotaStatus::protocol, QuotaSetting::protocol);
        }
    }

    // The caller holds mutex across send AND receive; a sequence has exactly
    // one reply, even when several native threads share the same endpoint.
    QuotaMessage exchange_locked(QuotaMessage request, QuotaStage context = QuotaStage::admission) {
        const AdmissionDiagnosticScope diagnostic(host_marker(), "cache", "exchange_locked");
        const bool growth = request.opcode == static_cast<std::uint16_t>(QuotaOpcode::reserve);
        if (growth && refusal) {
            throw coded<std::runtime_error>(refusal->failure, "run quota refused");
        }
        if (growth && finished) {
            record_admission_diagnostic("channel_growth_after_finish fd=%d opcode=%u", fd,
                                        static_cast<unsigned>(request.opcode));
            fail_configuration(QuotaStatus::protocol, QuotaSetting::channel);
        }
        validate_request(request, context);
#if defined(__unix__) || defined(__APPLE__)
        if (process != static_cast<std::uint64_t>(::getpid())) {
            record_admission_diagnostic("channel_process_mismatch fd=%d expected_pid=%" PRIu64
                                        " current_pid=%" PRIu64 " opcode=%u", fd, process,
                                        static_cast<std::uint64_t>(::getpid()), static_cast<unsigned>(request.opcode));
            fail_configuration(QuotaStatus::protocol, QuotaSetting::channel);
        }
        if (sequence == UINT64_MAX) fail_configuration(QuotaStatus::counter_overflow, QuotaSetting::protocol);
        request.sequence = ++sequence;
        request[QuotaSlot::role] = static_cast<std::uint64_t>(role);
        const bool hello = request.opcode == static_cast<std::uint16_t>(QuotaOpcode::hello);
        if (!hello) {
            request[QuotaSlot::nonce_hi] = nonce_hi;
            request[QuotaSlot::nonce_lo] = nonce_lo;
        }
        const QuotaWire wire = encode_quota_message(request);
        int send_flags = 0;
#ifdef MSG_NOSIGNAL
        send_flags = MSG_NOSIGNAL;
#endif
        ssize_t sent;
        do { sent = ::send(fd, wire.data(), wire.size(), send_flags); } while (sent < 0 && errno == EINTR);
        if (sent != static_cast<ssize_t>(wire.size())) {
            record_admission_diagnostic("%s fd=%d errno=%d sent=%jd expected=%zu sequence=%" PRIu64,
                                        hello ? "hello_send_failed" : "channel_send_failed", fd,
                                        sent < 0 ? errno : 0, static_cast<std::intmax_t>(sent), wire.size(), request.sequence);
            fail_configuration(QuotaStatus::protocol, QuotaSetting::channel);
        }
        std::array<unsigned char, kRunQuotaWireSize + 1> incoming{};
        struct iovec vector{incoming.data(), incoming.size()};
        struct msghdr header{};
        header.msg_iov = &vector;
        header.msg_iovlen = 1;
        ssize_t received;
        do { received = ::recvmsg(fd, &header, 0); } while (received < 0 && errno == EINTR);
        if (received != static_cast<ssize_t>(kRunQuotaWireSize)
            || (header.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
            record_admission_diagnostic("%s fd=%d errno=%d received=%jd expected=%zu flags=%d sequence=%" PRIu64,
                                        hello ? "hello_receive_failed" : "channel_receive_failed", fd,
                                        received < 0 ? errno : 0, static_cast<std::intmax_t>(received),
                                        kRunQuotaWireSize, header.msg_flags, request.sequence);
            fail_configuration(QuotaStatus::protocol, QuotaSetting::channel);
        }
        QuotaMessage reply;
        QuotaSnapshot snapshot;
        const char* reply_check = "decode";
        try {
            reply = decode_quota_message(incoming.data(), static_cast<std::size_t>(received));
            reply_check = "opcode";
            if (reply.opcode != (request.opcode | kRunQuotaReply)) invalid();
            reply_check = "sequence";
            if (reply.sequence != request.sequence) invalid();
            reply_check = "nonce";
            if (!hello && (reply[QuotaSlot::nonce_hi] != nonce_hi
                           || reply[QuotaSlot::nonce_lo] != nonce_lo)) invalid();
            reply_check = "unused_fields";
            validate_reply_fields(reply, static_cast<QuotaOpcode>(request.opcode));
            reply_check = "resource";
            if ((request.opcode == static_cast<std::uint16_t>(QuotaOpcode::snapshot)
                 || request.opcode == static_cast<std::uint16_t>(QuotaOpcode::finish))
                && !reply[QuotaSlot::status] && reply[QuotaSlot::resource] != request[QuotaSlot::resource]) invalid();
            reply_check = "snapshot";
            snapshot = read_snapshot(reply);
        } catch (const coded<std::runtime_error>&) {
            record_admission_diagnostic("%s check=%s fd=%d request_opcode=%u reply_opcode=%u request_sequence=%" PRIu64
                                        " reply_sequence=%" PRIu64, hello ? "hello_reply_invalid" : "channel_reply_invalid",
                                        reply_check, fd, static_cast<unsigned>(request.opcode),
                                        static_cast<unsigned>(get_le<std::uint16_t>(incoming.data() + 6)),
                                        request.sequence, get_le<std::uint64_t>(incoming.data() + 8));
            fail_configuration(QuotaStatus::protocol, QuotaSetting::channel);
        }
        if (snapshot.failure.code != RunFailureCode::none) {
            if (hello) {
                record_admission_diagnostic("hello_host_refusal fd=%d status=%" PRIu64 " setting=%" PRIu64
                                            " stage=%" PRIu64, fd, reply[QuotaSlot::status],
                                            reply[QuotaSlot::resource], reply[QuotaSlot::stage]);
            }
            fail(std::move(snapshot));
        }
        if (hello) {
            nonce_hi = reply[QuotaSlot::nonce_hi];
            nonce_lo = reply[QuotaSlot::nonce_lo];
            const char* identity_error = (!nonce_hi && !nonce_lo) ? "hello_nonce_missing"
                : !reply[QuotaSlot::generation] ? "hello_generation_missing"
                : reply[QuotaSlot::pid] != process ? "hello_pid_mismatch"
                : reply[QuotaSlot::role] != static_cast<std::uint64_t>(role) ? "hello_role_mismatch" : nullptr;
            if (identity_error) {
                record_admission_diagnostic("%s fd=%d nonce_hi=%" PRIu64 " nonce_lo=%" PRIu64
                                            " generation=%" PRIu64 " reply_pid=%" PRIu64 " expected_pid=%" PRIu64
                                            " reply_role=%" PRIu64 " expected_role=%" PRIu64,
                                            identity_error, fd, nonce_hi, nonce_lo,
                                            reply[QuotaSlot::generation], reply[QuotaSlot::pid], process,
                                            reply[QuotaSlot::role], static_cast<std::uint64_t>(role));
                fail_configuration(QuotaStatus::protocol, QuotaSetting::channel);
            }
        }
        return reply;
#else
        record_admission_diagnostic("channel_platform_unsupported fd=%d opcode=%u", fd,
                                    static_cast<unsigned>(request.opcode));
        fail_configuration(QuotaStatus::containment_unavailable, QuotaSetting::channel);
#endif
    }

    QuotaMessage exchange(QuotaMessage request, QuotaStage context = QuotaStage::admission) {
        std::lock_guard<std::mutex> lock(mutex);
        return exchange_locked(std::move(request), context);
    }
};

RunQuota RunQuota::from_fd(int fd, QuotaRole role) {
    const auto& marker = host_marker();
    if (!marker_is_captured(marker, "cache", "from_fd")) {
        invalid(QuotaStatus::protocol, QuotaSetting::channel);
    }
    if (marker.mode == QuotaMode::unmanaged) return RunQuota(nullptr);
    const AdmissionDiagnosticScope diagnostic(marker, "cache", "from_fd");
    validate_host_marker(marker);
#if defined(__unix__) || defined(__APPLE__)
    int type = 0;
    socklen_t type_size = sizeof(type);
    struct sockaddr_storage peer{};
    socklen_t peer_size = sizeof(peer);
    struct stat metadata{};
    if (fd < 0) {
        record_admission_diagnostic("attach_fd_negative fd=%d", fd);
        invalid(QuotaStatus::protocol, QuotaSetting::channel);
    }
    if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_size) != 0) {
        record_admission_diagnostic("attach_getsockopt_failed fd=%d errno=%d type=%d type_size=%u",
                                    fd, errno, type, static_cast<unsigned>(type_size));
        invalid(QuotaStatus::protocol, QuotaSetting::channel);
    }
    if (type != SOCK_SEQPACKET) {
        record_admission_diagnostic("attach_socket_type_mismatch fd=%d type=%d expected_type=%d type_size=%u",
                                    fd, type, SOCK_SEQPACKET, static_cast<unsigned>(type_size));
        invalid(QuotaStatus::protocol, QuotaSetting::channel);
    }
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peer_size) != 0) {
        record_admission_diagnostic("attach_getpeername_failed fd=%d errno=%d type=%d", fd, errno, type);
        invalid(QuotaStatus::protocol, QuotaSetting::channel);
    }
    if (peer.ss_family != AF_UNIX) {
        record_admission_diagnostic("attach_peer_family_mismatch fd=%d family=%u expected_family=%d peer_size=%u",
                                    fd, static_cast<unsigned>(peer.ss_family), AF_UNIX, static_cast<unsigned>(peer_size));
        invalid(QuotaStatus::protocol, QuotaSetting::channel);
    }
    if (::fstat(fd, &metadata) != 0) {
        record_admission_diagnostic("attach_fstat_failed fd=%d errno=%d type=%d", fd, errno, type);
        invalid(QuotaStatus::protocol, QuotaSetting::channel);
    }
    if (static_cast<std::uint64_t>(role) < 1 || static_cast<std::uint64_t>(role) > 6) {
        record_admission_diagnostic("attach_role_invalid fd=%d role=%" PRIu64, fd, static_cast<std::uint64_t>(role));
        invalid(QuotaStatus::protocol, QuotaSetting::channel);
    }
    const EndpointKey key{static_cast<std::uint64_t>(::getpid()),
                          static_cast<std::uint64_t>(metadata.st_dev),
                          static_cast<std::uint64_t>(metadata.st_ino)};
    std::lock_guard<std::mutex> lock(endpoints_mutex);
    for (auto it = endpoints.begin(); it != endpoints.end();) {
        if (it->second.expired()) it = endpoints.erase(it);
        else ++it;
    }
    auto& cached = endpoints[key];
    if (auto endpoint = cached.lock()) {
        if (endpoint->role != role) {
            record_admission_diagnostic("attach_cached_role_mismatch fd=%d role=%" PRIu64 " cached_role=%" PRIu64
                                        " dev=%" PRIu64 " ino=%" PRIu64, fd, static_cast<std::uint64_t>(role),
                                        static_cast<std::uint64_t>(endpoint->role),
                                        static_cast<std::uint64_t>(metadata.st_dev),
                                        static_cast<std::uint64_t>(metadata.st_ino));
            invalid(QuotaStatus::protocol, QuotaSetting::channel);
        }
        return RunQuota(std::move(endpoint));
    }
    auto endpoint = std::make_shared<RunQuotaEndpoint>();
    endpoint->fd = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (endpoint->fd < 0) {
        record_admission_diagnostic("dup_failed inherited_fd=%d dup_fd=%d errno=%d", fd, endpoint->fd, errno);
        invalid(QuotaStatus::protocol, QuotaSetting::channel);
    }
    const int flags = ::fcntl(endpoint->fd, F_GETFD);
    if (flags < 0) {
        record_admission_diagnostic("dup_getfd_failed inherited_fd=%d dup_fd=%d errno=%d", fd, endpoint->fd, errno);
        invalid(QuotaStatus::protocol, QuotaSetting::channel);
    }
    if ((flags & FD_CLOEXEC) == 0) {
        record_admission_diagnostic("dup_cloexec_missing inherited_fd=%d dup_fd=%d flags=%d", fd, endpoint->fd, flags);
        invalid(QuotaStatus::protocol, QuotaSetting::channel);
    }
    SocketIdentity duplicate;
    if (!socket_identity(endpoint->fd, duplicate, "duplicate")) {
        invalid(QuotaStatus::protocol, QuotaSetting::channel);
    }
    if (!same_socket(duplicate, {static_cast<std::uint64_t>(metadata.st_dev),
                                 static_cast<std::uint64_t>(metadata.st_ino)})) {
        record_admission_diagnostic("dup_identity_mismatch inherited_fd=%d dup_fd=%d expected_dev=%" PRIu64
                                    " expected_ino=%" PRIu64 " current_dev=%" PRIu64 " current_ino=%" PRIu64,
                                    fd, endpoint->fd, static_cast<std::uint64_t>(metadata.st_dev),
                                    static_cast<std::uint64_t>(metadata.st_ino), duplicate.device, duplicate.inode);
        invalid(QuotaStatus::protocol, QuotaSetting::channel);
    }
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    if (::setsockopt(endpoint->fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
        record_admission_diagnostic("dup_nosigpipe_failed inherited_fd=%d dup_fd=%d errno=%d", fd, endpoint->fd, errno);
        invalid(QuotaStatus::protocol, QuotaSetting::channel);
    }
#endif
    endpoint->process = static_cast<std::uint64_t>(::getpid());
    endpoint->role = role;
    endpoint->exchange(QuotaMessage{});
    cached = endpoint;
    return RunQuota(std::move(endpoint));
#else
    record_admission_diagnostic("attach_platform_unsupported fd=%d role=%" PRIu64, fd, static_cast<std::uint64_t>(role));
    invalid(QuotaStatus::containment_unavailable, QuotaSetting::channel);
#endif
}

RunQuota RunQuota::from_environment() {
    const auto& marker = host_marker();
    if (!marker_is_captured(marker, "cache", "from_environment")) {
        invalid(QuotaStatus::protocol, QuotaSetting::channel);
    }
    if (marker.mode == QuotaMode::unmanaged) return RunQuota(nullptr);
    // A private CLOEXEC duplicate protects against accidental close/reuse of
    // the inherited number. In-process code can still close arbitrary fds;
    // preventing that requires host containment, not an fd flag.
    auto quota = from_fd(marker.native_fd);
    {
        std::lock_guard<std::mutex> lock(endpoints_mutex);
        active_endpoint = quota.endpoint_;
    }
    quota.check();
    return quota;
}

QuotaTicket RunQuota::reserve(QuotaObject object, QuotaAmounts amounts,
                             std::uint64_t old_extent, std::uint64_t new_extent,
                             QuotaStage stage, QuotaArtifact artifact) {
    if (!endpoint_) return {};
    const auto request = object_message(QuotaOpcode::reserve, object, amounts, old_extent,
                                        new_extent, stage, artifact);
    const auto reply = endpoint_->exchange(request);
    QuotaTicket ticket;
    ticket.endpoint_ = endpoint_;
    ticket.reservation_ = request;
    ticket.ticket_ = reply[QuotaSlot::ticket];
    if (!ticket.ticket_) {
        std::lock_guard<std::mutex> lock(endpoint_->mutex);
        endpoint_->fail_configuration(QuotaStatus::protocol, QuotaSetting::protocol);
    }
    return ticket;
}

void QuotaTicket::commit(QuotaAmounts realized, std::uint64_t actual_extent,
                         std::uint64_t device, std::uint64_t inode) {
    if (!endpoint_) return;
    if (!ticket_) {
        std::lock_guard<std::mutex> lock(endpoint_->mutex);
        endpoint_->fail_configuration(QuotaStatus::protocol, QuotaSetting::protocol);
    }
    QuotaMessage message;
    message.opcode = static_cast<std::uint16_t>(QuotaOpcode::commit);
    message[QuotaSlot::ticket] = ticket_;
    message[QuotaSlot::new_extent] = actual_extent;
    message[QuotaSlot::device] = device;
    message[QuotaSlot::inode] = inode;
    set_amounts(message, realized);
    endpoint_->exchange(message, static_cast<QuotaStage>(reservation_[QuotaSlot::stage]));
}

void QuotaTicket::cancel() {
    if (!endpoint_) return;
    if (!ticket_) {
        std::lock_guard<std::mutex> lock(endpoint_->mutex);
        endpoint_->fail_configuration(QuotaStatus::protocol, QuotaSetting::protocol);
    }
    QuotaMessage message;
    message.opcode = static_cast<std::uint16_t>(QuotaOpcode::cancel);
    message[QuotaSlot::ticket] = ticket_;
    endpoint_->exchange(message);
    ticket_ = 0;
}

void RunQuota::release(QuotaObject object, QuotaAmounts amounts,
                       std::uint64_t old_extent, std::uint64_t new_extent,
                       QuotaStage stage, QuotaArtifact artifact) {
    if (!endpoint_) return;
    endpoint_->exchange(object_message(QuotaOpcode::release, object, amounts, old_extent,
                                       new_extent, stage, artifact));
}

QuotaSnapshot RunQuota::snapshot(QuotaResource resource) {
    if (!endpoint_) return {};
    QuotaMessage message;
    message.opcode = static_cast<std::uint16_t>(QuotaOpcode::snapshot);
    message[QuotaSlot::resource] = static_cast<std::uint64_t>(resource);
    return read_snapshot(endpoint_->exchange(message));
}

QuotaSnapshot RunQuota::finish(QuotaResource resource) {
    if (!endpoint_) return {};
    std::lock_guard<std::mutex> lock(endpoint_->mutex);
    QuotaMessage message;
    message.opcode = static_cast<std::uint16_t>(QuotaOpcode::finish);
    message[QuotaSlot::resource] = static_cast<std::uint64_t>(resource);
    endpoint_->finished = true;
    const auto result = read_snapshot(endpoint_->exchange_locked(message));
    if (endpoint_->refusal) {
        throw coded<std::runtime_error>(endpoint_->refusal->failure, "run quota refused");
    }
    return result;
}

std::shared_ptr<const QuotaSnapshot> RunQuota::refusal() const {
    if (!endpoint_) return {};
    std::lock_guard<std::mutex> lock(endpoint_->mutex);
    return endpoint_->refusal;
}

void RunQuota::check() const {
    if (!endpoint_) return;
    std::lock_guard<std::mutex> lock(endpoint_->mutex);
    if (endpoint_->refusal) {
        throw coded<std::runtime_error>(endpoint_->refusal->failure, "run quota refused");
    }
    if (endpoint_->finished) {
        const AdmissionDiagnosticScope diagnostic(host_marker(), "cache", "check");
        record_admission_diagnostic("channel_check_after_finish fd=%d", endpoint_->fd);
        endpoint_->fail_configuration(QuotaStatus::protocol, QuotaSetting::channel);
    }
}

RunFailureValue run_quota_failure() noexcept {
    if (mode() == QuotaMode::unmanaged) return {};
    std::shared_ptr<RunQuotaEndpoint> endpoint;
    {
        std::lock_guard<std::mutex> lock(endpoints_mutex);
        endpoint = active_endpoint;
    }
    if (endpoint) {
        std::lock_guard<std::mutex> lock(endpoint->mutex);
        if (endpoint->refusal) return endpoint->refusal->failure;
    }
    return {};
}

std::uint64_t quota_rounded_extent(std::uint64_t extent, std::uint64_t block_size) {
    if (extent > kRunQuotaMax || block_size > kRunQuotaMax) {
        invalid(QuotaStatus::counter_overflow, QuotaSetting::scratch_bytes);
    }
    if (block_size == 0) invalid(QuotaStatus::malformed_limit, QuotaSetting::scratch_bytes);
    const auto remainder = extent % block_size;
    const auto extra = remainder ? block_size - remainder : 0;
    if (extra > kRunQuotaMax - extent) invalid(QuotaStatus::counter_overflow, QuotaSetting::scratch_bytes);
    return extent + extra;
}

}  // namespace pineforge::detail
