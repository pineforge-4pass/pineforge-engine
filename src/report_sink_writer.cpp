#include "report_sink_writer.hpp"
#include "report_value_invalid.hpp"
#include "sha256.hpp"

#include <pineforge/version.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <system_error>
#if defined(__linux__)
#include <pineforge/detail/scratch_directory.hpp>
#include <sys/vfs.h>
#endif
#include <unistd.h>
#include <utility>

namespace pineforge {
namespace report_sink {
namespace {

constexpr std::int64_t safe_integer = INT64_C(9007199254740991);
constexpr std::size_t max_line = 1024 * 1024;
constexpr std::size_t io_window = 64 * 1024;
constexpr std::size_t io_alignment = 4096;
std::atomic<std::uint64_t> private_sequence{0};
constexpr std::array<char, 7> tags{{'E', 'T', 'O', 'V', 'K', 'X', 'H'}};

[[noreturn]] void invalid(const char* message) {
    throw WriterError(WriterFailure::invalid_input, message);
}

// The existing wire contract bounds a line, including LF, to 1 MiB.
// Reserve that scratch once, and check before copying each field.
struct Line {
    std::array<char, max_line> bytes{};
    std::size_t used = 0;
    void clear() noexcept { used = 0; }
    void append(std::string_view value) {
        if (value.size() >= bytes.size() - used)
            invalid("report sink line exceeds 1 MiB including LF");
        if (!value.empty()) std::memcpy(bytes.data() + used, value.data(), value.size());
        used += value.size();
    }
    void append(char value) { append(std::string_view(&value, 1)); }
    std::string_view view() const { return {bytes.data(), used}; }
};

template<class Out>
void integer(Out& out, std::int64_t value) {
    if (value < -safe_integer || value > safe_integer)
        invalid("report sink i64 exceeds the exact JSON integer bound");
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc{}) invalid("report sink integer conversion failed");
    out.append(std::string_view(buffer, static_cast<std::size_t>(result.ptr - buffer)));
}

template<class Out>
void unsigned_integer(Out& out, std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(safe_integer))
        invalid("report sink u64 exceeds the exact JSON integer bound");
    integer(out, static_cast<std::int64_t>(value));
}

template<class Out>
void real(Out& out, double value) {
    if (!std::isfinite(value)) { out.append("null"); return; }
    char buffer[64];
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                         std::chars_format::scientific);
    if (converted.ec != std::errc{})
        invalid("report sink shortest scientific conversion failed");
    const std::string_view text(buffer, static_cast<std::size_t>(converted.ptr - buffer));
    const auto exponent_at = text.find('e');
    if (exponent_at == std::string_view::npos) invalid("report sink scientific exponent is absent");
    const bool negative = text.front() == '-';
    std::array<char, 24> digits{};
    std::size_t count = 0;
    for (std::size_t i = negative ? 1 : 0; i < exponent_at; ++i)
        if (text[i] != '.') digits[count++] = text[i];
    int exponent = 0;
    for (std::size_t i = exponent_at + 2; i < text.size(); ++i)
        exponent = 10 * exponent + (text[i] - '0');
    if (text[exponent_at + 1] == '-') exponent = -exponent;
    if (negative) out.append('-');
    if (exponent >= -4 && exponent < 16) {
        const int point = exponent + 1;
        if (point <= 0) {
            out.append("0.");
            for (int i = 0; i < -point; ++i) out.append('0');
            out.append(std::string_view(digits.data(), count));
        } else if (static_cast<std::size_t>(point) >= count) {
            out.append(std::string_view(digits.data(), count));
            for (auto i = count; i < static_cast<std::size_t>(point); ++i) out.append('0');
            out.append(".0");
        } else {
            out.append(std::string_view(digits.data(), static_cast<std::size_t>(point)));
            out.append('.');
            out.append(std::string_view(digits.data() + point, count - point));
        }
    } else {
        out.append(digits[0]);
        if (count > 1) {
            out.append('.');
            out.append(std::string_view(digits.data() + 1, count - 1));
        }
        out.append(exponent < 0 ? "e-" : "e+");
        const int magnitude = exponent < 0 ? -exponent : exponent;
        if (magnitude < 10) out.append('0');
        integer(out, magnitude);
    }
}

template<class Out>
void string(Out& out, std::string_view utf8) {
    static constexpr char hex[] = "0123456789abcdef";
    out.append('"');
    for (std::size_t i = 0; i < utf8.size();) {
        const auto byte = static_cast<unsigned char>(utf8[i]);
        if (byte < 0x20) {
            const char escaped[] = {'\\', 'u', '0', '0', hex[byte >> 4], hex[byte & 15]};
            out.append(std::string_view(escaped, sizeof(escaped)));
            ++i;
        } else if (byte < 0x80) {
            if (byte == '"' || byte == '\\') out.append('\\');
            out.append(static_cast<char>(byte));
            ++i;
        } else {
            std::size_t length = 0;
            if (byte >= 0xc2 && byte <= 0xdf) length = 2;
            else if (byte >= 0xe0 && byte <= 0xef) length = 3;
            else if (byte >= 0xf0 && byte <= 0xf4) length = 4;
            bool valid = length != 0 && length <= utf8.size() - i;
            for (std::size_t j = 1; valid && j < length; ++j) {
                const auto next = static_cast<unsigned char>(utf8[i + j]);
                valid = next >= 0x80 && next <= 0xbf;
            }
            if (valid) {
                const auto second = static_cast<unsigned char>(utf8[i + 1]);
                if ((byte == 0xe0 && second < 0xa0) || (byte == 0xed && second >= 0xa0)
                        || (byte == 0xf0 && second < 0x90) || (byte == 0xf4 && second >= 0x90))
                    valid = false;
            }
            if (valid) { out.append(utf8.substr(i, length)); i += length; }
            else { out.append("\xef\xbf\xbd"); ++i; }
        }
    }
    out.append('"');
}

void wire_string(Line& out, std::string_view value) {
    if (value.size() >= max_line) invalid("report sink string exceeds the line bound");
    string(out, value);
}

struct Column { std::string_view field, type, unit; };
constexpr Column equity_columns[] = {
    // v1 keeps f64? for byte compatibility; E never emits null, and converter/finalizer refuse it with exit 3 report_result_invalid.
    {"time_ms", "i64", "unix_ms"}, {"equity", "f64?", "account_ccy"},
    {"open_profit", "f64?", "account_ccy"},
};
constexpr Column trade_columns[] = {
    {"n", "i64", ""}, {"side", "str", ""},
    {"entry_time", "i64", "unix_ms"}, {"exit_time", "i64", "unix_ms"},
    {"entry_price", "f64?", "quote_ccy"}, {"exit_price", "f64?", "quote_ccy"},
    {"qty", "f64?", "qty"}, {"pnl", "f64?", "account_ccy"}, {"pnl_pct", "f64?", "percent"},
    {"max_runup", "f64?", "account_ccy"}, {"max_drawdown", "f64?", "account_ccy"},
    {"commission", "f64?", "account_ccy"}, {"entry_bar_index", "i64", "script_bar"},
    {"exit_bar_index", "i64", "script_bar"}, {"open_at_end", "bool", ""},
    {"entry_incarnation", "u64", ""}, {"close_cause", "i64", ""},
    {"entry_id", "str", ""}, {"exit_id", "str", ""}, {"exit_comment", "str", ""},
};
constexpr Column event_columns[] = {
    {"sequence", "u64", ""}, {"output_index", "i64", ""}, {"bar_index", "i64", ""},
    {"bar_open_ms", "i64?", "unix_ms"}, {"bar_close_ms", "i64?", "unix_ms"},
    {"ordinal_in_bar", "i64", ""}, {"phase", "i64", ""}, {"confirmed", "i64", ""},
    {"value", "f64?", ""}, {"message_hash64", "hex64", ""}, {"message", "str?", ""},
};
constexpr Column trace_columns[] = {
    {"timestamp", "i64", "unix_ms"}, {"bar_index", "i64", ""},
    {"name", "str", ""}, {"value", "f64?", ""},
};

bool enabled(char tag, const RecordSet& records) {
    if (tag == 'O' || tag == 'V' || tag == 'K') return records.outputs;
    if (tag == 'X') return records.trace;
    if (tag == 'H') return records.hashes;
    return true;
}

template<class Emit>
void columns(char tag, const RecordSet& records, Emit emit) {
    switch (tag) {
    case 'E': for (const auto& column : equity_columns) emit(column); break;
    case 'T': for (const auto& column : trade_columns) emit(column); break;
    case 'V': for (const auto& column : event_columns) emit(column); break;
    case 'X': for (const auto& column : trace_columns) emit(column); break;
    case 'H': emit(Column{"hash", "hex64", ""}); break;
    case 'O':
        emit(Column{"row", "i64", ""});
        emit(Column{"open_ms", "i64?", "unix_ms"});
        emit(Column{"close_ms", "i64?", "unix_ms"});
        break;
    }
    if (tag == 'O' || tag == 'K') {
        const auto count = tag == 'O' ? records.series_slots : records.constant_count;
        for (std::int64_t i = 0; i < count; ++i) {
            char name[32];
            name[0] = tag == 'O' ? 'v' : 'c';
            const auto end = std::to_chars(name + 1, name + sizeof(name), i);
            if (end.ec != std::errc{}) invalid("report sink integer conversion failed");
            emit(Column{std::string_view(name, static_cast<std::size_t>(end.ptr - name)), "f64?", ""});
        }
    }
}

void header(Line& line, const std::string& run_id, const RecordSet& records) {
    if (records.series_slots < 0 || records.constant_count < 0
            || records.series_slots > static_cast<std::int64_t>(max_line)
            || records.constant_count > static_cast<std::int64_t>(max_line))
        invalid("report sink descriptor count exceeds the line bound");
    line.clear();
    line.append("{\"schema\":\"pf-report-sink\",\"version\":1,\"run_id\":");
    wire_string(line, run_id);
    line.append(",\"engine\":"); string(line, PINEFORGE_VERSION_STRING);
    line.append(",\"records\":[");
    bool first_record = true;
    for (const char tag : tags) {
        if (!enabled(tag, records)) continue;
        if (!first_record) line.append(',');
        first_record = false;
        line.append("{\"type\":\""); line.append(tag); line.append('"');
        const char* keys[] = {",\"fields\":[", ",\"types\":[", ",\"units\":["};
        for (unsigned list = 0; list < 3; ++list) {
            line.append(keys[list]);
            bool first = true;
            columns(tag, records, [&](const Column& column) {
                if (!first) line.append(',');
                first = false;
                string(line, list == 0 ? column.field : list == 1 ? column.type : column.unit);
            });
            line.append(']');
        }
        line.append('}');
    }
    line.append("]}");
}

void nullable_time(Line& line, std::int64_t time) {
    if (time == INT64_MIN) line.append("null");
    else integer(line, time);
}

void hash64(Line& line, std::uint64_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    char text[] = "\"0000000000000000\"";
    for (int i = 16; i > 0; --i) {
        text[i] = digits[value & 15];
        value >>= 4;
    }
    line.append(std::string_view(text, 18));
}

void append_values(Line& line, const double* values, std::int64_t count, std::int64_t expected) {
    if (count != expected || count < 0 || (count > 0 && !values))
        invalid("report sink values differ from the header field count");
    for (std::int64_t i = 0; i < count; ++i) { line.append(','); real(line, values[i]); }
    line.append(']');
}

// Only the standalone public helpers return owning strings. The writer uses
// the same formatters directly with its fixed Line instead.
struct StringOutput {
    std::string value;
    void append(std::string_view text) { value.append(text.data(), text.size()); }
    void append(char text) { value += text; }
};
} // namespace

std::string f64(double value) {
    StringOutput out;
    real(out, value);
    return std::move(out.value);
}

std::string escaped_string(std::string_view utf8) {
    StringOutput out;
    string(out, utf8);
    return std::move(out.value);
}

struct LineWriter::State {
    enum class Phase { configured, writing, complete, failed };
    std::string path, run_id, scratch_dir;
    std::string private_path;
    std::size_t private_name = 0;
    int directory_descriptor = -1;
    bool private_owned = false;
    mode_t publication_permissions = 0600;
    int descriptor = -1;
    bool borrowed = false;
    bool socket = false;
    Phase phase = Phase::configured;
    RecordSet records{};
    Trailer trailer{};
    // Output-only provenance beside the existing trailer/hash. No trading
    // decision or broker-state hash reads this bounded record fold.
    pf_report_sink_diagnostics_v1 diagnostics{};
    Sha256 hash;
    Line scratch;
    std::array<char, io_window + io_alignment - 1> storage{};
    char* buffer;
    std::size_t used = 0;
    off_t offset = 0;
    bool unsynced = false;
    bool has_equity_time = false;
    std::int64_t equity_time = 0;

    explicit State(const pf_report_sink_v1& sink) {
        if (sink.struct_size != sizeof(pf_report_sink_v1) || sink.version != 1
                || !sink.path || !*sink.path)
            invalid("invalid report sink descriptor");
        path = sink.path;
        run_id = sink.run_id ? sink.run_id : "";
        if (sink.scratch_dir) scratch_dir = sink.scratch_dir;
        else {
            const auto slash = path.find_last_of('/');
            scratch_dir = slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
        }
        const auto address = reinterpret_cast<std::uintptr_t>(storage.data());
        buffer = reinterpret_cast<char*>((address + io_alignment - 1) & ~(io_alignment - 1));
    }

    explicit State(const pf_report_sink_fd_v1& sink) {
        if (sink.struct_size != sizeof(pf_report_sink_fd_v1) || sink.version != 1
                || sink.fd < 0)
            invalid("invalid report sink fd descriptor");
        descriptor = sink.fd;
        borrowed = true;
        run_id = sink.run_id ? sink.run_id : "";
        const auto address = reinterpret_cast<std::uintptr_t>(storage.data());
        buffer = reinterpret_cast<char*>((address + io_alignment - 1) & ~(io_alignment - 1));
    }

    ~State() {
        // Resource release only: abandoned buffered bytes never cause a
        // successful trailer or an unchecked destructor-time write.
        discard_private();
        if (directory_descriptor >= 0) ::close(directory_descriptor);
    }
    void discard_private() noexcept {
        const int saved = errno;
        if (!borrowed && descriptor >= 0) ::close(std::exchange(descriptor, -1));
        if (private_owned) {
            int result;
            do {
                result = ::unlinkat(directory_descriptor, private_path.c_str() + private_name, 0);
            } while (result < 0 && errno == EINTR);
            if (result == 0 || errno == ENOENT) private_owned = false;
        }
        if (!private_owned && directory_descriptor >= 0)
            ::close(std::exchange(directory_descriptor, -1));
        errno = saved;
    }
    void require_writing() const {
        if (phase != Phase::writing)
            throw WriterError(WriterFailure::invalid_state, "report sink is not accepting records");
    }
    void fail() noexcept {
        if (phase != Phase::complete) {
            phase = Phase::failed; used = 0;
            discard_private();
        }
    }
    [[noreturn]] void io_error(const char* operation, int number) {
        fail();
        throw WriterError(WriterFailure::io, std::string("report sink ") + operation + ": "
                          + std::error_code(number, std::generic_category()).message(), number);
    }

    void open_private() {
        // Validate without opening or truncating the public directory entry.
        struct stat info{};
        int result;
        do { result = ::stat(path.c_str(), &info); } while (result < 0 && errno == EINTR);
        if (result == 0 && S_ISDIR(info.st_mode)) io_error("open failed", EISDIR);
        if (result == 0 && !S_ISREG(info.st_mode))
            io_error("requires a disk-backed regular file", EINVAL);
        if (result < 0 && errno != ENOENT) io_error("open failed", errno);
        publication_permissions = result == 0 ? info.st_mode & 0777 : 0600;

        const auto slash = path.find_last_of('/');
        if (slash == std::string::npos) private_path = ".";
        else private_path.assign(path, 0, slash == 0 ? 1 : slash);
        // Keep a directory handle for cleanup even if the parent is moved.
#if defined(O_PATH)
        constexpr int directory_flags = O_PATH;
#elif defined(O_SEARCH)
        constexpr int directory_flags = O_SEARCH;
#else
        constexpr int directory_flags = O_RDONLY;
#endif
        do {
            directory_descriptor = ::open(private_path.c_str(),
                directory_flags | O_DIRECTORY | O_CLOEXEC);
        } while (directory_descriptor < 0 && errno == EINTR);
        if (directory_descriptor < 0) io_error("open failed", errno);
        if (private_path.back() != '/') private_path += '/';
        private_name = private_path.size();
        // Two uint64 decimal tokens and the fixed affixes fit in 80 bytes.
        private_path.reserve(private_name + 80);
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            private_path.resize(private_name);
            private_path += ".pf-report-sink-";
            std::array<char, 64> digits{};
            auto end = std::to_chars(digits.data(), digits.data() + digits.size(),
                                    static_cast<std::uint64_t>(::getpid())).ptr;
            private_path.append(digits.data(), end);
            private_path += '-';
            end = std::to_chars(digits.data(), digits.data() + digits.size(),
                private_sequence.fetch_add(1, std::memory_order_relaxed)).ptr;
            private_path.append(digits.data(), end);
            private_path += ".in-progress";
            do {
                descriptor = ::openat(directory_descriptor, private_path.c_str() + private_name,
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            } while (descriptor < 0 && errno == EINTR);
            if (descriptor >= 0) {
                private_owned = true;
                // A restrictive umask may remove owner bits; keep the
                // in-progress report exactly 0600 throughout record writing.
                if (::fchmod(descriptor, 0600) != 0) io_error("open failed", errno);
                uncached();
                return;
            }
            if (errno != EEXIST) io_error("open failed", errno);
        }
        io_error("open failed", EEXIST);
    }

    void prepare_publication() {
        if (!borrowed && publication_permissions != 0600
            && ::fchmod(descriptor, publication_permissions) != 0)
            io_error("permissions failed", errno);
    }

    void publish_private() {
        if (borrowed) return;
        // Check close once with its fd detached; retrying can close a reused fd.
        if (::close(std::exchange(descriptor, -1)) != 0) io_error("close failed", errno);
        if (::renameat(directory_descriptor, private_path.c_str() + private_name,
                       AT_FDCWD, path.c_str()) != 0) io_error("rename failed", errno);
        private_owned = false;
        // Releasing this lookup handle cannot fault an already-published run.
        ::close(std::exchange(directory_descriptor, -1));
    }

    void validate_stream() {
        struct stat info{};
        int result;
        do { result = ::fstat(descriptor, &info); } while (result < 0 && errno == EINTR);
        if (result < 0) io_error("fstat failed", errno);
        socket = S_ISSOCK(info.st_mode);
        if (!socket && !S_ISFIFO(info.st_mode))
            io_error("fd is not a FIFO or stream socket", EINVAL);
        if (socket) {
            int type = 0;
            socklen_t length = sizeof type;
            do { result = ::getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &type, &length); }
            while (result < 0 && errno == EINTR);
            if (result < 0) io_error("getsockopt failed", errno);
            if (type != SOCK_STREAM) io_error("fd is not a stream socket", EINVAL);
        }
    }

    void wait_writable() {
        pollfd ready{descriptor, POLLOUT, 0};
        int result;
        do { result = ::poll(&ready, 1, -1); } while (result < 0 && errno == EINTR);
        if (result < 0) io_error("poll failed", errno);
        if (ready.revents & POLLNVAL) io_error("poll invalid fd", EBADF);
        if (ready.revents & (POLLERR | POLLHUP)) io_error("stream peer closed", EPIPE);
    }

    void uncached() {
        struct stat info{};
        if (::fstat(descriptor, &info) != 0) io_error("stat failed", errno);
        if (!S_ISREG(info.st_mode)) io_error("requires a disk-backed regular file", EINVAL);
#if defined(__linux__)
        struct statfs filesystem{};
        if (::fstatfs(descriptor, &filesystem) != 0) io_error("filesystem query failed", errno);
        if (detail::scratch_filesystem_is_memory(filesystem.f_type))
            io_error("requires disk backing, not tmpfs or ramfs", EINVAL);
        const int flags = ::fcntl(descriptor, F_GETFL);
        if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_DIRECT) < 0)
            io_error("uncached I/O setup failed", errno);
#elif defined(__APPLE__)
        if (::fcntl(descriptor, F_NOCACHE, 1) < 0) io_error("uncached I/O setup failed", errno);
#else
        io_error("uncached I/O is unavailable", ENOTSUP);
#endif
    }
    void flush() {
        if (!used) return;
        const auto aligned = (used + io_alignment - 1) & ~(io_alignment - 1);
        std::memset(buffer + used, 0, aligned - used);
        std::size_t written = 0;
        const auto extent = borrowed ? used : aligned;
        while (written < extent) {
            const auto count = borrowed
                ? (socket
                    ? ::send(descriptor, buffer + written, extent - written, MSG_NOSIGNAL)
                    : ::write(descriptor, buffer + written, extent - written))
                : ::pwrite(descriptor, buffer + written, extent - written, offset + written);
            if (count < 0) {
                const int number = errno;
                if (number == EINTR) continue;
                if (number == EAGAIN || number == EWOULDBLOCK) {
                    wait_writable();
                    continue;
                }
                io_error("write failed", number);
            }
            if (count == 0) io_error("write made no progress", EIO);
            written += static_cast<std::size_t>(count);
        }
        if (borrowed) {
            used = 0;
            return;
        }
        if (::ftruncate(descriptor, offset + used) != 0) io_error("truncate failed", errno);
        // Direct I/O bypasses data cache; a returned pwrite has released the
        // window, so reuse needs no sync. No buffered fallback or eviction.
        unsynced = true;
        if (used == io_window) { offset += used; used = 0; }
        // Keep a partial window for an aligned rewrite on the next append;
        // padding never appears inside the logical stream.
    }
    // One fdatasync (fsync on other systems) makes every written window and the
    // file size durable; write_trailer and abandon call it before they return.
    void synchronize() {
        if (!unsynced) return;
        int status;
#if defined(__linux__)
        do { status = ::fdatasync(descriptor); } while (status < 0 && errno == EINTR);
#else
        do { status = ::fsync(descriptor); } while (status < 0 && errno == EINTR);
#endif
        if (status < 0) io_error("synchronize failed", errno);
        unsynced = false;
    }
    void append(std::string_view bytes) {
        while (!bytes.empty()) {
            const auto take = std::min(io_window - used, bytes.size());
            std::memcpy(buffer + used, bytes.data(), take);
            used += take;
            bytes.remove_prefix(take);
            if (used == io_window) flush();
        }
    }
    void line(bool hashed = true) {
        if (hashed) { hash.update(scratch.view()); hash.update("\n"); }
        append(scratch.view()); append("\n");
    }
    std::array<std::int64_t, 7> counts() const {
        return {{trailer.count_e, trailer.count_t, trailer.count_o, trailer.count_v,
                 trailer.count_k, trailer.count_x, trailer.count_h}};
    }
    template<class Build>
    void record(char tag, std::int64_t& count, Build build) {
        try {
            require_writing();
            if (!enabled(tag, records)) invalid("report sink record is absent from the header");
            if (count == safe_integer) invalid("report sink record count exceeds its bound");
            scratch.clear(); build(scratch); line(); ++count;
        } catch (...) { fail(); throw; }
    }
};

LineWriter::LineWriter(const pf_report_sink_v1& sink) : state_(std::make_unique<State>(sink)) {}
LineWriter::LineWriter(const pf_report_sink_fd_v1& sink) : state_(std::make_unique<State>(sink)) {}
LineWriter::~LineWriter() = default;

void LineWriter::write_header(const RecordSet& records) {
    auto& state = *state_;
    try {
        if (state.phase != State::Phase::configured)
            throw WriterError(WriterFailure::invalid_state, "report sink header was already attempted");
        header(state.scratch, state.run_id, records);
        if (state.borrowed) {
            state.validate_stream();
        } else {
            state.open_private();
        }
        state.records = records;
        state.phase = State::Phase::writing;
        state.line();
    } catch (...) { state.fail(); throw; }
}

void LineWriter::write_e(const pf_equity_point_t& point) {
    auto& state = *state_;
    try {
        state.require_writing();
        if (state.trailer.count_e == safe_integer) invalid("report sink E count exceeds its bound");
        if (state.has_equity_time && point.time_ms < state.equity_time)
            invalid("report sink equity times are not ordered");
        // Streaming precedence is the earliest released point, equity first; completed metrics check final open_profit first.
        (void)report::finite_value(point.equity, "equity_curve.equity");
        (void)report::finite_value(point.open_profit, "equity_curve.open_profit");
        auto& line = state.scratch;
        line.clear();
        line.append("[\"E\","); integer(line, point.time_ms);
        line.append(','); real(line, point.equity);
        line.append(','); real(line, point.open_profit);
        line.append(']'); state.line();
        if (state.trailer.count_e == 0) state.diagnostics.first = point;
        state.diagnostics.last = point;
        ++state.trailer.count_e;
        state.has_equity_time = true;
        state.equity_time = point.time_ms;
    } catch (...) { state.fail(); throw; }
}

void LineWriter::write_t(std::int64_t n, const pf_trade_t& trade,
                         std::uint64_t entry_incarnation, std::int64_t close_cause,
                         std::string_view entry_id, std::string_view exit_id,
                         std::string_view exit_comment) {
    auto& state = *state_;
    try {
        state.require_writing();
        if (state.trailer.count_t == safe_integer || n != state.trailer.count_t + 1)
            invalid("report sink trade row numbers must start at one and be contiguous");
        auto& line = state.scratch;
        line.clear();
        line.append("[\"T\","); integer(line, n);
        line.append(trade.is_long ? ",\"long\"," : ",\"short\",");
        integer(line, trade.entry_time); line.append(','); integer(line, trade.exit_time);
        const double values[] = {trade.entry_price, trade.exit_price, trade.qty, trade.pnl,
                                 trade.pnl_pct, trade.max_runup, trade.max_drawdown, trade.commission};
        for (const double value : values) { line.append(','); real(line, value); }
        line.append(','); integer(line, trade.entry_bar_index);
        line.append(','); integer(line, trade.exit_bar_index);
        line.append(trade.open_at_end ? ",true," : ",false,");
        unsigned_integer(line, entry_incarnation); line.append(','); integer(line, close_cause);
        for (const auto value : {entry_id, exit_id, exit_comment}) {
            line.append(','); wire_string(line, value);
        }
        line.append(']'); state.line(); ++state.trailer.count_t;
        if (trade.entry_bar_index <= 0) state.diagnostics.trade_at_start = 1;
    } catch (...) { state.fail(); throw; }
}

pf_report_sink_diagnostics_v1 LineWriter::diagnostics() const noexcept {
    pf_report_sink_diagnostics_v1 result{};
    if (state_->phase == State::Phase::complete) {
        result = state_->diagnostics;
        result.count_e = state_->trailer.count_e;
        result.complete = 1;
    }
    result.struct_size = sizeof result;
    result.version = 1;
    return result;
}

void LineWriter::abandon() {
    auto& state = *state_;
    try {
        state.require_writing();
        // line() can cross the fixed buffer boundary. Flush its retained
        // suffix here so EOF follows LF, never a partially emitted record.
        state.flush();
        state.synchronize();
        state.fail();
    } catch (...) {
        state.fail();
        throw;
    }
}

Trailer LineWriter::write_trailer() {
    auto& state = *state_;
    if (state.phase == State::Phase::complete) return state.trailer;
    try {
        state.require_writing();
        state.flush();
        const auto digest = state.hash.hex_digest();
        auto& line = state.scratch;
        line.clear();
        line.append("{\"trailer\":\"pf-report-sink\",\"version\":1,\"counts\":{");
        const auto counts = state.counts();
        bool first = true;
        for (std::size_t i = 0; i < tags.size(); ++i) {
            if (!enabled(tags[i], state.records)) continue;
            if (!first) line.append(',');
            first = false;
            line.append('"'); line.append(tags[i]); line.append("\":"); integer(line, counts[i]);
        }
        line.append("},\"sha256\":\""); line.append(digest); line.append("\"}");
        state.line(false); state.flush(); state.prepare_publication(); state.synchronize();
        state.publish_private();
        std::memcpy(state.trailer.sha256_hex, digest.c_str(), digest.size() + 1);
        state.phase = State::Phase::complete;
        return state.trailer;
    } catch (...) { state.fail(); throw; }
}

void LineWriter::write_o(std::int64_t row, std::int64_t open_ms, std::int64_t close_ms,
                         const double* values, std::int64_t count) {
    auto& state = *state_;
    state.record('O', state.trailer.count_o, [&](Line& line) {
        if (row != state.trailer.count_o)
            invalid("report sink output rows must start at zero and be contiguous");
        line.append("[\"O\","); integer(line, row);
        line.append(','); nullable_time(line, open_ms);
        line.append(','); nullable_time(line, close_ms);
        append_values(line, values, count, state.records.series_slots);
    });
}

void LineWriter::write_v(const pf_output_event_v1_t& event, std::string_view message) {
    auto& state = *state_;
    state.record('V', state.trailer.count_v, [&](Line& line) {
        line.append("[\"V\","); unsigned_integer(line, event.sequence);
        line.append(','); integer(line, event.output_index);
        line.append(','); integer(line, event.bar_index);
        line.append(','); nullable_time(line, event.bar_open_ms);
        line.append(','); nullable_time(line, event.bar_close_ms);
        line.append(','); integer(line, event.ordinal_in_bar);
        line.append(','); integer(line, event.phase);
        line.append(','); integer(line, event.confirmed);
        line.append(','); real(line, event.value);
        line.append(','); hash64(line, event.message_hash64);
        line.append(',');
        if (event.message) wire_string(line, message);
        else line.append("null");
        line.append(']');
    });
}

void LineWriter::write_k(const double* values, std::int64_t count) {
    auto& state = *state_;
    state.record('K', state.trailer.count_k, [&](Line& line) {
        if (state.trailer.count_k != 0) invalid("report sink constants were already written");
        line.append("[\"K\"");
        append_values(line, values, count, state.records.constant_count);
    });
}

void LineWriter::write_x(const pf_trace_entry_t& entry, std::string_view name) {
    auto& state = *state_;
    state.record('X', state.trailer.count_x, [&](Line& line) {
        line.append("[\"X\","); integer(line, entry.timestamp);
        line.append(','); integer(line, entry.bar_index);
        line.append(','); wire_string(line, name);
        line.append(','); real(line, entry.value);
        line.append(']');
    });
}

void LineWriter::write_h(std::uint64_t hash) {
    auto& state = *state_;
    state.record('H', state.trailer.count_h, [&](Line& line) {
        line.append("[\"H\","); hash64(line, hash); line.append(']');
    });
}

} // namespace report_sink
} // namespace pineforge
