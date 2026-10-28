#include <pineforge/script_string.hpp>
#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace pineforge {
namespace {
using detail::o1::Id;
using detail::o1::Store;
using Size = ScriptString::size_type;
thread_local Store* active_string_store = nullptr;

struct StringRoot {
    Id payload;
    Size references;
    Size used;
    Size capacity;
};

[[noreturn]] void string_invariant(const char* message) {
    throw coded<std::runtime_error>(RunFailureCode::engine_invariant, {}, message);
}
void exact_read(const Store& store, Id id, Size off, void* dst, std::size_t n) {
    if (store.read(id, off, dst, n) != n) string_invariant("short script-string store read");
}
void exact_write(Store& store, Id id, Size off, const void* src, std::size_t n) {
    if (store.write(id, off, src, n) != n) string_invariant("short script-string store write");
}
StringRoot root_read(const Store& store, Id root) {
    StringRoot result{};
    exact_read(store, root, 0, &result, sizeof result);
    return result;
}
void root_write(Store& store, Id root, const StringRoot& value) {
    exact_write(store, root, 0, &value, sizeof value);
}
void abandon_object(Store& store, Id id) noexcept {
    try {
        if (id != detail::o1::kNullId && store.alive(id)) store.release(id);
    } catch (...) {
        // Main's Store rejects cleanup mutations while rollback-only. Keep
        // the original failure; the transaction/run owner owns recovery.
    }
}
Size sum(Size a, Size b) {
    if (b > std::numeric_limits<Size>::max() - a)
        throw std::length_error("script string byte size overflow");
    return a + b;
}
Size grown_capacity(Size old, Size required) {
    return std::max(required, old <= UINT64_MAX / 2 ? old * 2 : UINT64_MAX);
}

// At most one page per reader, even when a scan spans the entire payload.
class Reader {
public:
    explicit Reader(const ScriptString& text) : text_(text) {}
    unsigned char operator[](Size pos) {
        if (pos >= text_.size()) return 0;
        if (first_ == ScriptString::npos || pos < first_ || pos - first_ >= used_) {
            first_ = pos - pos % buffer_.size();
            used_ = text_.read(first_, buffer_.data(), buffer_.size());
        }
        return static_cast<unsigned char>(buffer_[static_cast<std::size_t>(pos - first_)]);
    }
private:
    const ScriptString& text_;
    std::array<char, ScriptString::chunk_bytes> buffer_{};
    Size first_ = ScriptString::npos;
    std::size_t used_ = 0;
};

template <class OtherRead>
Size find_bytes(const ScriptString& source, Size pattern_size, OtherRead&& other, Size pos) {
    if (pos > source.size()) return ScriptString::npos;
    if (pattern_size == 0) return pos;
    if (pattern_size > source.size() - pos) return ScriptString::npos;
    Reader input(source);
    const auto first = other(0);
    const Size last = source.size() - pattern_size;
    for (Size start = pos; start <= last; ++start) {
        if (input[start] != first) continue;
        Size matched = 1;
        while (matched < pattern_size && input[start + matched] == other(matched)) ++matched;
        if (matched == pattern_size) return start;
    }
    return ScriptString::npos;
}

template <class Read>
int compare_bytes(const ScriptString& source, Size other_size, Read&& other) {
    Reader input(source);
    const Size common = std::min(source.size(), other_size);
    for (Size i = 0; i < common; ++i) {
        const auto a = input[i], b = other(i);
        if (a != b) return static_cast<int>(a) - static_cast<int>(b);
    }
    return source.size() < other_size ? -1 : source.size() > other_size ? 1 : 0;
}
} // namespace

ScriptStringScope::ScriptStringScope(Store& store) noexcept : previous_(active_string_store) {
    active_string_store = &store;
}
ScriptStringScope::~ScriptStringScope() { active_string_store = previous_; }
Store& ScriptStringScope::current() {
    if (!active_string_store) string_invariant("script string construction requires a Store scope");
    return *active_string_store;
}
Store& ScriptString::required_store() const { return store_ ? *store_ : ScriptStringScope::current(); }

ScriptString::ScriptString(Store& store, std::string_view bytes, Size bound) : store_(&store) {
    if (bytes.empty() && bound == 0) return;
    const Size capacity = std::max(bound, static_cast<Size>(bytes.size()));
    const Id data = store.create(capacity, 1);
    Id root = detail::o1::kNullId;
    try {
        if (!bytes.empty()) exact_write(store, data, 0, bytes.data(), bytes.size());
        root = store.create(sizeof(StringRoot), alignof(StringRoot));
        root_write(store, root, StringRoot{data, 1, static_cast<Size>(bytes.size()), capacity});
    } catch (...) {
        abandon_object(store, root);
        abandon_object(store, data);
        throw;
    }
    handle_ = Handle{root, 0, static_cast<Size>(bytes.size())};
}
ScriptString::ScriptString(const char* value) : ScriptString(std::string_view(value)) {}
ScriptString::ScriptString(const std::string& value) : ScriptString(std::string_view(value)) {}
ScriptString::ScriptString(std::string_view value) {
    if (!value.empty()) {
        ScriptString temporary(ScriptStringScope::current(), value);
        swap(temporary);
    }
}
ScriptString::ScriptString(const ScriptString& value)
    : store_(value.store_), handle_(value.handle_), owner_(value.owner_) {
    retain();
}
ScriptString::ScriptString(ScriptString&& value) noexcept
    : store_(value.store_), handle_(value.handle_), owner_(value.owner_) {
    value.handle_ = {};
}
ScriptString& ScriptString::operator=(const ScriptString& value) {
    if (this != &value) { ScriptString copy(value); swap(copy); }
    return *this;
}
ScriptString& ScriptString::operator=(ScriptString&& value) {
    if (this != &value) {
        release();
        store_ = value.store_;
        handle_ = value.handle_;
        owner_ = value.owner_;
        value.handle_ = {};
    }
    return *this;
}
ScriptString::~ScriptString() {
    try {
        release();
    } catch (...) {
        // A failed read poisons the outer transaction on main. Unwinding a
        // temporary must not terminate before that owner can roll it back.
    }
}
void ScriptString::swap(ScriptString& other) noexcept {
    std::swap(store_, other.store_);
    std::swap(handle_, other.handle_);
    owner_.swap(other.owner_);
}
void ScriptString::retain() const {
    if (handle_.root == detail::o1::kNullId) return;
    auto root = root_read(*store_, handle_.root);
    root.references = sum(root.references, 1);
    root_write(*store_, handle_.root, root);
}
void ScriptString::unpin(Store& store, Handle handle) {
    if (handle.root == detail::o1::kNullId) return;
    auto root = root_read(store, handle.root);
    if (root.references == 0) string_invariant("script string reference underflow");
    if (--root.references == 0) {
        store.release(root.payload);
        store.release(handle.root);
    } else {
        root_write(store, handle.root, root);
    }
}
void ScriptString::release() {
    if (handle_.root != detail::o1::kNullId) unpin(*store_, handle_);
    handle_ = {};
}
void ScriptString::clear() { release(); }
ScriptString::Handle ScriptString::pin() const { retain(); return handle_; }
ScriptString ScriptString::from_handle(Store& store, Handle handle) {
    ScriptString result;
    if (handle.root != detail::o1::kNullId) {
        const auto root = root_read(store, handle.root);
        if (handle.offset > root.used || handle.length > root.used - handle.offset)
            string_invariant("script string handle crosses payload end");
    } else if (handle.offset != 0 || handle.length != 0) {
        string_invariant("nonempty null script string handle");
    }
    result.store_ = &store;
    result.handle_ = handle;
    result.retain();
    return result;
}

void ScriptString::reserve(Size bound) {
    if (bound <= size()) return;
    prepare_append(bound - size());
}
void ScriptString::prepare_append(Size extra) {
    const Size required = sum(size(), extra);
    if (handle_.root == detail::o1::kNullId) {
        ScriptString replacement(required_store(), {}, required);
        replacement.owner_ = owner_;
        swap(replacement);
        return;
    }
    auto root = root_read(*store_, handle_.root);
    if (sum(handle_.offset, size()) != root.used) {
        // A branch of an older prefix must keep the newer branch immutable.
        // Copy to the external backing with a single fixed scratch page.
        ScriptString replacement(*store_, {}, required);
        replacement.owner_ = owner_;
        std::array<char, chunk_bytes> buffer{};
        for (Size offset = 0; offset < size();) {
            const auto count = read(offset, buffer.data(), buffer.size());
            replacement.append(std::string_view(buffer.data(), count));
            offset += count;
        }
        swap(replacement);
        return;
    }
    const Size end = sum(root.used, extra);
    if (end > root.capacity) {
        root.capacity = grown_capacity(root.capacity, end);
        if (!store_->resize(root.payload, root.capacity))
            string_invariant("script string backing resize failed");
        root_write(*store_, handle_.root, root);
    }
}
ScriptString& ScriptString::append(std::string_view bytes) {
    if (bytes.empty()) return *this;
    prepare_append(bytes.size());
    auto root = root_read(*store_, handle_.root);
    exact_write(*store_, root.payload, root.used, bytes.data(), bytes.size());
    root.used += bytes.size();
    root_write(*store_, handle_.root, root);
    handle_.length += bytes.size();
    return *this;
}
ScriptString& ScriptString::append(const ScriptString& value) {
    if (value.empty()) return *this;
    ScriptString source(value); // also makes self-append safe across a fork
    if (!store_) { store_ = source.store_; owner_ = source.owner_; }
    prepare_append(source.size());
    auto root = root_read(*store_, handle_.root);
    std::array<char, chunk_bytes> buffer{};
    for (Size offset = 0; offset < source.size();) {
        const auto count = source.read(offset, buffer.data(), buffer.size());
        exact_write(*store_, root.payload, root.used + offset, buffer.data(), count);
        offset += count;
    }
    root.used += source.size();
    root_write(*store_, handle_.root, root);
    handle_.length += source.size();
    return *this;
}

std::size_t ScriptString::read(Size offset, void* dst, std::size_t bytes) const {
    if (offset >= size() || bytes == 0) return 0;
    const auto count = static_cast<std::size_t>(std::min<Size>(bytes, size() - offset));
    const auto root = root_read(*store_, handle_.root);
    exact_read(*store_, root.payload, handle_.offset + offset, dst, count);
    return count;
}
char ScriptString::operator[](Size pos) const {
    if (pos == size()) return '\0'; // std::string's terminating byte
    return at(pos);
}
char ScriptString::at(Size pos) const {
    if (pos >= size()) throw std::out_of_range("script string index");
    char value;
    read(pos, &value, 1);
    return value;
}
ScriptString ScriptString::substr(Size pos, Size count) const {
    if (pos > size()) throw std::out_of_range("script string substring");
    ScriptString result(*this);
    result.handle_.offset += pos;
    result.handle_.length = std::min(count, size() - pos);
    return result;
}
int ScriptString::compare(const ScriptString& other) const {
    Reader input(other);
    return compare_bytes(*this, other.size(), [&](Size i) { return input[i]; });
}
int ScriptString::compare(std::string_view other) const {
    return compare_bytes(*this, other.size(), [&](Size i) { return static_cast<unsigned char>(other[i]); });
}
int ScriptString::compare(Size pos, Size count, const ScriptString& other) const {
    return substr(pos, count).compare(other);
}
ScriptString::size_type ScriptString::find(const ScriptString& pattern, Size pos) const {
    Reader input(pattern);
    return find_bytes(*this, pattern.size(), [&](Size i) { return input[i]; }, pos);
}
ScriptString::size_type ScriptString::find(std::string_view pattern, Size pos) const {
    return find_bytes(*this, pattern.size(), [&](Size i) { return static_cast<unsigned char>(pattern[i]); }, pos);
}
ScriptString::size_type ScriptString::find(char pattern, Size pos) const {
    return find(std::string_view(&pattern, 1), pos);
}
std::string ScriptString::to_std_string() const {
    std::string result;
    if (size() > result.max_size()) throw std::length_error("std::string boundary size overflow");
    result.resize(static_cast<std::size_t>(size()));
    for (Size offset = 0; offset < size();) {
        const auto count = static_cast<std::size_t>(std::min<Size>(chunk_bytes, size() - offset));
        read(offset, result.data() + offset, count);
        offset += count;
    }
    return result;
}

ScriptStringParts::iterator::iterator(const ScriptStringParts* owner, Size start)
    : owner_(owner), start_(start) {
    if (start != ScriptString::npos)
        end_ = owner->separator_.empty() ? ScriptString::npos : owner->source_.find(owner->separator_, start);
}
ScriptString ScriptStringParts::iterator::operator*() const {
    if (!owner_ || start_ == ScriptString::npos) throw std::out_of_range("split iterator end");
    return owner_->source_.substr(start_, end_ == ScriptString::npos ? end_ : end_ - start_);
}
ScriptStringParts::iterator& ScriptStringParts::iterator::operator++() {
    if (start_ == ScriptString::npos) throw std::out_of_range("split iterator end");
    if (end_ == ScriptString::npos) {
        start_ = ScriptString::npos;
    } else {
        start_ = end_ + owner_->separator_.size();
        end_ = owner_->source_.find(owner_->separator_, start_);
    }
    return *this;
}
ScriptString::size_type ScriptStringParts::size() const {
    Size count = 0;
    for (auto it = begin(); it != end(); ++it) ++count;
    return count;
}
ScriptString ScriptStringParts::operator[](Size index) const {
    for (auto it = begin(); it != end(); ++it) {
        if (index-- == 0) return *it;
    }
    throw std::out_of_range("split part index");
}

namespace {
template <class Word>
Word word(Reader& input, Size offset) {
    std::array<unsigned char, sizeof(Word)> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = input[offset + i];
    Word value;
    std::memcpy(&value, bytes.data(), sizeof value);
    return value;
}
#if defined(_LIBCPP_VERSION)
// CityHash64's long-input recurrence, used by libc++'s string/string_view
// hash. Short inputs below go directly to std::hash on a fixed stack page.
std::uint64_t rotate(std::uint64_t value, unsigned shift) {
    return (value >> shift) | (value << (64 - shift));
}
std::uint64_t mix(std::uint64_t value) { return value ^ (value >> 47); }
std::uint64_t hash16(std::uint64_t first, std::uint64_t second) {
    constexpr std::uint64_t multiplier = UINT64_C(0x9ddfea08eb382d69);
    auto a = (first ^ second) * multiplier;
    a ^= a >> 47;
    auto b = (second ^ a) * multiplier;
    b ^= b >> 47;
    return b * multiplier;
}
std::pair<std::uint64_t, std::uint64_t> weak32(Reader& bytes, Size off,
                                            std::uint64_t a, std::uint64_t b) {
    const auto w = word<std::uint64_t>(bytes, off);
    const auto x = word<std::uint64_t>(bytes, off + 8);
    const auto y = word<std::uint64_t>(bytes, off + 16);
    const auto z = word<std::uint64_t>(bytes, off + 24);
    a += w;
    b = rotate(b + a + z, 21);
    const auto c = a;
    a += x + y;
    b += rotate(a, 44);
    return {a + z, b + c};
}
#endif
} // namespace

std::size_t ScriptString::hash() const {
    if (size() <= chunk_bytes) {
        std::array<char, chunk_bytes> bytes{};
        read(0, bytes.data(), static_cast<std::size_t>(size()));
        return std::hash<std::string_view>{}(std::string_view(bytes.data(), static_cast<std::size_t>(size())));
    }
    Reader bytes(*this);
#if defined(_MSC_VER)
    std::size_t state = sizeof(std::size_t) == 8 ? UINT64_C(14695981039346656037) : UINT32_C(2166136261);
    const std::size_t prime = sizeof(std::size_t) == 8 ? UINT64_C(1099511628211) : UINT32_C(16777619);
    for (Size i = 0; i < size(); ++i) state = (state ^ bytes[i]) * prime;
    return state;
#elif defined(__GLIBCXX__) || defined(_LIBCPP_VERSION)
    if constexpr (sizeof(std::size_t) == 4) {
        constexpr std::uint32_t multiplier = UINT32_C(0x5bd1e995);
#if defined(__GLIBCXX__)
        std::uint32_t state = UINT32_C(0xc70f6907) ^ static_cast<std::uint32_t>(size());
#else
        std::uint32_t state = static_cast<std::uint32_t>(size());
#endif
        Size offset = 0;
        for (; size() - offset >= 4; offset += 4) {
            auto block = word<std::uint32_t>(bytes, offset) * multiplier;
            block ^= block >> 24;
            state = state * multiplier ^ block * multiplier;
        }
        if (offset != size()) {
            for (unsigned shift = 0; offset < size(); ++offset, shift += 8)
                state ^= static_cast<std::uint32_t>(bytes[offset]) << shift;
            state *= multiplier;
        }
        state ^= state >> 13;
        state *= multiplier;
        return state ^ (state >> 15);
    } else {
#if defined(__GLIBCXX__)
        constexpr std::uint64_t multiplier = UINT64_C(0xc6a4a7935bd1e995);
        std::uint64_t state = UINT64_C(0xc70f6907) ^ size() * multiplier;
        Size offset = 0;
        for (; size() - offset >= 8; offset += 8) {
            auto block = word<std::uint64_t>(bytes, offset) * multiplier;
            block ^= block >> 47;
            state = (state ^ block * multiplier) * multiplier;
        }
        if (offset != size()) {
            for (unsigned shift = 0; offset < size(); ++offset, shift += 8)
                state ^= static_cast<std::uint64_t>(bytes[offset]) << shift;
            state *= multiplier;
        }
        state ^= state >> 47;
        state *= multiplier;
        return state ^ (state >> 47);
#else
        constexpr std::uint64_t k1 = UINT64_C(0xb492b66fbe98f273);
        auto x = word<std::uint64_t>(bytes, size() - 40);
        auto y = word<std::uint64_t>(bytes, size() - 16) + word<std::uint64_t>(bytes, size() - 56);
        auto z = hash16(word<std::uint64_t>(bytes, size() - 48) + size(), word<std::uint64_t>(bytes, size() - 24));
        auto v = weak32(bytes, size() - 64, size(), z);
        auto w = weak32(bytes, size() - 32, y + k1, x);
        x = x * k1 + word<std::uint64_t>(bytes, 0);
        const Size end = (size() - 1) & ~Size{63};
        for (Size offset = 0; offset < end; offset += 64) {
            x = rotate(x + y + v.first + word<std::uint64_t>(bytes, offset + 8), 37) * k1;
            y = rotate(y + v.second + word<std::uint64_t>(bytes, offset + 48), 42) * k1;
            x ^= w.second;
            y += v.first + word<std::uint64_t>(bytes, offset + 40);
            z = rotate(z + w.first, 33) * k1;
            v = weak32(bytes, offset, v.second * k1, x + w.first);
            w = weak32(bytes, offset + 32, z + w.second, y + word<std::uint64_t>(bytes, offset + 16));
            std::swap(z, x);
        }
        return hash16(hash16(v.first, w.first) + mix(y) * k1 + z, hash16(v.second, w.second) + x);
#endif
    }
#else
#error "Implement the platform string hash recurrence before enabling ScriptString"
#endif
}

namespace {
double parse_buffer(const char* input) {
    char* end = nullptr;
    const int previous_errno = errno;
    errno = 0;
    const double value = std::strtod(input, &end);
    const int conversion_errno = errno;
    if (errno == 0) errno = previous_errno;
    return end == input || conversion_errno == ERANGE ? na<double>() : value;
}
int digit(unsigned char ch, bool hex) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (hex && ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (hex && ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

// Enough magnitude to cancel every possible input-length decimal/hex scale.
// Values >= 16*2^64 are indistinguishable for a binary64 conversion, even if
// all UINT64_MAX input bytes were fraction digits. No string length is capped.
struct Magnitude {
    std::uint64_t high = 0, low = 0;
};
int compare(Magnitude a, Magnitude b) {
    if (a.high != b.high) return a.high < b.high ? -1 : 1;
    return a.low < b.low ? -1 : a.low > b.low ? 1 : 0;
}
Magnitude add(Magnitude a, Magnitude b) {
    const auto low = a.low + b.low;
    return {a.high + b.high + (low < a.low), low};
}
Magnitude subtract(Magnitude a, Magnitude b) {
    return {a.high - b.high - (a.low < b.low), a.low - b.low};
}
Magnitude times_four(Size value) { return {value >> 62, value << 2}; }
Magnitude decimal_digit(Magnitude value, unsigned d) {
    if (value.high >= 16) return {16, 0};
    const std::uint64_t lower = (value.low & UINT64_C(0xffffffff)) * 10 + d;
    const std::uint64_t upper = (value.low >> 32) * 10 + (lower >> 32);
    const auto high = value.high * 10 + (upper >> 32);
    return high >= 16 ? Magnitude{16, 0}
                      : Magnitude{high, (upper << 32) | (lower & UINT64_C(0xffffffff))};
}
struct Scale {
    Magnitude value{};
    bool negative = false;
    void accumulate(Magnitude term, bool minus) {
        if (negative == minus) value = add(value, term);
        else if (compare(value, term) >= 0) value = subtract(value, term);
        else { value = subtract(term, value); negative = minus; }
    }
    int bounded() const {
        const int amount = value.high || value.low > 10000 ? 10000 : static_cast<int>(value.low);
        return negative ? -amount : amount;
    }
};

bool token(Reader& input, Size offset, const char* spelling) {
    for (; *spelling; ++spelling, ++offset)
        if (std::tolower(input[offset]) != *spelling) return false;
    return true;
}
double long_special(Reader& input, Size start, bool negative) {
    if (token(input, start, "inf")) return negative ? -INFINITY : INFINITY;
    if (!token(input, start, "nan")) return na<double>();
    // C strtod accepts nan(n-char-sequence); its numeric payload follows the
    // platform's strtoull base-0 spelling. Normalize that integer, preserving
    // overflow saturation and the platform's own payload/sign construction.
    std::uint64_t payload = 0;
    Size p = start + 3;
    const bool parenthesis = input[p] == '(';
    if (parenthesis) ++p;
    unsigned base = 10;
    if (parenthesis && input[p] == '0') {
        base = 8;
        if (input[p + 1] == 'x' || input[p + 1] == 'X') { base = 16; p += 2; }
    }
    bool any = false, overflow = false, numeric = parenthesis;
    bool closed = false;
    while (parenthesis) {
        const auto ch = input[p];
        if (ch == ')') { closed = true; break; }
        const bool nchar = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z')
                         || (ch >= 'A' && ch <= 'Z') || ch == '_';
        if (!nchar) break;
        const int d = digit(ch, true);
        numeric = numeric && d >= 0 && static_cast<unsigned>(d) < base;
        if (numeric) {
            any = true;
            if (payload > (UINT64_MAX - static_cast<unsigned>(d)) / base) overflow = true;
            payload = overflow ? UINT64_MAX : payload * base + static_cast<unsigned>(d);
        }
        ++p;
    }
    std::array<char, 64> spelling{};
    char* output = spelling.data();
    if (negative) *output++ = '-';
    *output++ = 'n'; *output++ = 'a'; *output++ = 'n';
    if (closed && any && (numeric || overflow)) {
        *output++ = '(';
        output = std::to_chars(output, spelling.data() + spelling.size() - 2, payload).ptr;
        // Preserve libc's errno policy as well as its saturated payload. Some
        // strtod implementations expose strtoull's ERANGE for a NaN payload;
        // std::stod then returns na via the generated out_of_range catch.
        if (overflow) *output++ = '0';
        // A closed but nonnumeric payload may still expose an overflowing
        // numeric prefix through errno. An unclosed payload never parses it.
        if (!numeric) *output++ = '_';
        *output++ = ')';
    }
    *output = '\0';
    return parse_buffer(spelling.data());
}
} // namespace

double str_tonumber(const ScriptString& text) {
    // This also preserves the exact platform spellings/NaN payloads for normal
    // inputs, without allocating even a temporary std::string.
    constexpr std::size_t retained = 2048;
    std::array<char, ScriptString::chunk_bytes + 1> output{};
    if (text.size() <= ScriptString::chunk_bytes) {
        text.read(0, output.data(), static_cast<std::size_t>(text.size()));
        return parse_buffer(output.data());
    }
    Reader input(text);
    Size p = 0;
    while (std::isspace(input[p])) ++p;
    bool negative = input[p] == '-';
    if (negative || input[p] == '+') ++p;
    if (token(input, p, "inf") || token(input, p, "nan")) return long_special(input, p, negative);

    bool hex = input[p] == '0' && (input[p + 1] == 'x' || input[p + 1] == 'X');
    if (hex) p += 2;
    const char* point = std::localeconv()->decimal_point;
    const std::size_t point_size = std::strlen(point);
    bool after_point = false, any = false, significant = false, sticky = false;
    Size fraction = 0, significant_digits = 0;
    std::array<char, retained + 1> digits{};
    std::size_t kept = 0;
    for (;;) {
        const int d = digit(input[p], hex);
        if (d >= 0) {
            any = true;
            if (after_point) ++fraction;
            significant = significant || d != 0;
            if (significant) {
                ++significant_digits;
                if (kept < retained) digits[kept++] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
                else sticky = sticky || d != 0;
            }
            ++p;
            continue;
        }
        bool is_point = !after_point && point_size != 0;
        for (std::size_t i = 0; is_point && i < point_size; ++i) is_point = input[p + i] == static_cast<unsigned char>(point[i]);
        if (!is_point) break;
        after_point = true;
        p += point_size;
    }
    if (!any) return hex ? (negative ? -0.0 : 0.0) : na<double>(); // "0x!" still parses its leading zero
    if (!significant) return negative ? -0.0 : 0.0;

    Scale scale;
    const bool exponent_marker = hex ? input[p] == 'p' || input[p] == 'P' : input[p] == 'e' || input[p] == 'E';
    if (exponent_marker) {
        ++p;
        scale.negative = input[p] == '-';
        if (scale.negative || input[p] == '+') ++p;
        while (input[p] >= '0' && input[p] <= '9') {
            scale.value = decimal_digit(scale.value, input[p] - '0');
            ++p;
        }
    }
    const Size omitted = significant_digits - kept;
    scale.accumulate(hex ? times_four(omitted) : Magnitude{0, omitted}, false);
    scale.accumulate(hex ? times_four(fraction) : Magnitude{0, fraction}, true);
    if (sticky) {
        digits[kept++] = '1';
        scale.accumulate(Magnitude{0, hex ? 4U : 1U}, true);
    }
    // Every binary64 rounding boundary has at most 1075 binary fractional
    // places (< 2048 significant decimal or hexadecimal digits). Once those
    // digits are retained, a nonzero discarded tail only decides the side of
    // an exact tie. One sticky digit preserves that side; libc still performs
    // the final conversion and underflow/overflow decision under the current
    // rounding mode. No input-length-sized numeric temporary is needed.
    char* out = output.data();
    if (negative) *out++ = '-';
    if (hex) { *out++ = '0'; *out++ = 'x'; }
    std::memcpy(out, digits.data(), kept);
    out += kept;
    *out++ = hex ? 'p' : 'e';
    out = std::to_chars(out, output.data() + output.size() - 1, scale.bounded()).ptr;
    *out = '\0';
    return parse_buffer(output.data());
}

} // namespace pineforge
