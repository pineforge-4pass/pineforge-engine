#include <pineforge/detail/scratch_store.hpp>
#include <pineforge/source/market_admission.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {
namespace ad = pineforge::admission;
namespace pd = pineforge::detail;
using Status = pd::ScratchStatus;

constexpr std::size_t kPageBytes = 512;
constexpr std::uint64_t kTagOffset = 1024;
constexpr std::uint64_t kExpectedTag = 1;
constexpr std::string_view kFieldKey = "event.observation.quantity_type";

std::string_view name(Status status) {
    switch (status) {
    case Status::ok: return "ok";
    case Status::end: return "end";
    case Status::cancelled: return "cancelled";
    case Status::not_found: return "not_found";
    case Status::programming_error: return "programming_error";
    case Status::io_error: return "io_error";
    case Status::corrupt: return "corrupt";
    }
    return "unknown";
}

template<class T> bool expect(const char* verdict, const char* reason, const T& expected,
                              const T& observed) {
    if (observed == expected) return true;
    std::cerr << verdict << ": " << reason << ": expected " << expected
              << ", got " << observed << '\n';
    return false;
}

std::uint64_t word(const std::byte* data) noexcept {
    std::uint64_t result = 0;
    for (unsigned i = 0; i != 8; ++i)
        result |= std::uint64_t(std::to_integer<unsigned>(data[i])) << (8 * i);
    return result;
}

Status compare_sequences(void*, const pd::ScratchByteSource& a, const pd::ScratchByteSource& b,
                         int& order) noexcept {
    std::array<std::byte, 8> left{}, right{};
    if (a.size() != left.size() || b.size() != right.size()) return Status::programming_error;
    const auto x = a.read(0, {left.data(), left.size()});
    if (x != Status::ok) return x;
    const auto y = b.read(0, {right.data(), right.size()});
    if (y != Status::ok) return y;
    const auto lhs = word(left.data()), rhs = word(right.data());
    order = lhs < rhs ? -1 : lhs > rhs ? 1 : 0;
    return Status::ok;
}

class TagReadFault final : public pd::ScratchByteSource {
    const pd::ScratchByteSource& source_;
public:
    mutable std::uint64_t failed_at = 0;
    explicit TagReadFault(const pd::ScratchByteSource& source) : source_(source) {}
    std::uint64_t size() const noexcept override { return source_.size(); }
    Status read(std::uint64_t offset, pd::ScratchBuffer out) const noexcept override {
        if (offset < kTagOffset) return source_.read(offset, out);
        if (!failed_at) failed_at = offset;
        return Status::io_error;
    }
};

int check_tag_read() {
    std::array<std::byte, 64 * kPageBytes> storage{};
    pd::FixedMemoryScratchBacking backing({storage.data(), storage.size()}, kPageBytes);
    pd::CommandScratchStore store({2, kPageBytes, 1, 1, {939, nullptr, &compare_sequences},
                                   pd::ScratchOpenMode::create_empty}, &backing);
    if (!expect("NOT COMPARED", "open empty store", name(Status::ok), name(store.status()))
        || !expect("NOT COMPARED", "begin empty store transaction", name(Status::ok), name(store.begin()))
        || !expect("NOT COMPARED", "commit empty store", name(Status::ok),
                   name(store.commit(pd::ScratchDurability::buffered)))
        || !expect("NOT COMPARED", "store page bytes", kPageBytes, store.page_size())
        || !expect("NOT COMPARED", "initial store record count", std::uint64_t{0}, store.record_count()))
        return 2;

    ad::Journal journal;
    ad::bind_journal_codec(journal, store);
    const auto sequence = journal.next_sequence();
    if (!expect("NOT COMPARED", "first journal sequence", std::uint64_t{1}, sequence)) return 2;
    auto observation = std::make_shared<ad::CommandObservation>();
    observation->command = sequence;
    observation->id.assign(98, 'a');
    const ad::Event event = ad::CommandEvent{observation};
    auto encoded = ad::encode_journal_record(event);
    constexpr auto min_bytes = kTagOffset + 8;
    if (encoded.size() < min_bytes) {
        expect("NOT COMPARED", "minimum encoded record bytes", min_bytes, encoded.size());
        return 2;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(encoded.size()));
    if (!expect("NOT COMPARED", "copy the encoded record", name(Status::ok),
                name(encoded.read(0, {bytes.data(), bytes.size()})))) return 2;
    const std::string_view wire(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto key_at = wire.find(kFieldKey);
    if (key_at == std::string_view::npos) {
        expect("NOT COMPARED", "encoded quantity_type key", kFieldKey, std::string_view{"missing"});
        return 2;
    }
    if (!expect("NOT COMPARED", "quantity_type tag offset", kTagOffset,
                static_cast<std::uint64_t>(key_at + kFieldKey.size()))
        || !expect("NOT COMPARED", "encoded quantity_type tag", kExpectedTag,
                   word(bytes.data() + kTagOffset))) return 2;

    const pd::ScratchSpanSource source({bytes.data(), bytes.size()});
    std::array<std::byte, kPageBytes> page{};
    ad::Event decoded = ad::ReviewEvent{};
    if (!expect("NOT COMPARED", "decode the original record", name(Status::ok),
                name(ad::decode_journal_record(source, {page.data(), page.size()}, decoded))))
        return 2;
    const auto* command = std::get_if<ad::CommandEvent>(&decoded);
    if (!expect("NOT COMPARED", "decoded command observation present", true,
                command && bool(command->observation))
        || !expect("NOT COMPARED", "decoded command sequence", sequence, command->observation->command)
        || !expect("NOT COMPARED", "decoded command ID", observation->id, command->observation->id))
        return 2;

    const TagReadFault fault(source);
    const auto status = ad::append_journal_record(journal, fault);
    if (!expect("NOT COMPARED", "first failed read offset (0 if none)", kTagOffset, fault.failed_at))
        return 2;
    const bool read_status_ok = expect("FAIL", "append with a failed quantity_type tag read",
                                       name(Status::io_error), name(status));
    if (!expect("FAIL", "stored record count after failed read", std::uint64_t{0}, store.record_count())
        || !expect("FAIL", "journal event count after failed read", std::size_t{0}, journal.event_count())
        || !expect("FAIL", "active transaction after failed read", false, store.transaction_active()))
        return 1;

    auto wrong_bytes = bytes;
    wrong_bytes[kTagOffset] = std::byte{2};
    {
        const pd::ScratchSpanSource wrong_tag({wrong_bytes.data(), wrong_bytes.size()});
        if (!expect("FAIL", "append with a wrong but readable tag", name(Status::corrupt),
                    name(ad::append_journal_record(journal, wrong_tag)))) return 1;
    }
    wrong_bytes[kTagOffset] = bytes[kTagOffset];
    wrong_bytes[key_at] ^= std::byte{1};
    const pd::ScratchSpanSource bad_key({wrong_bytes.data(), wrong_bytes.size()});
    const TagReadFault wrong_key(bad_key);
    if (!expect("FAIL", "append with a wrong field key", name(Status::corrupt),
                name(ad::append_journal_record(journal, wrong_key)))
        || !expect("FAIL", "read fault after a wrong key", std::uint64_t{0}, wrong_key.failed_at))
        return 1;

    if (!expect("FAIL", "append with the original readable record", name(Status::ok),
                name(ad::append_journal_record(journal, source)))
        || !expect("FAIL", "store count after retry", std::uint64_t{1}, store.record_count())
        || !expect("FAIL", "journal event count after retry", std::size_t{1}, journal.event_count())
        || !read_status_ok) return 1;

    std::cout << "PASS: tag read returned io_error at byte 1024; only the valid retry stored a record\n";
    return 0;
}
} // namespace

int main() {
    try {
        return check_tag_read();
    } catch (const std::exception& error) {
        std::cerr << "NOT COMPARED: fixture setup: expected a usable journal, got "
                  << error.what() << '\n';
        return 2;
    }
}
