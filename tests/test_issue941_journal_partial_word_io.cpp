#include <pineforge/detail/scratch_store.hpp>
#include <pineforge/source/market_admission.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {
namespace ad = pineforge::admission;
namespace pd = pineforge::detail;
using Status = pd::ScratchStatus;

constexpr std::size_t page_size = 512;
constexpr std::size_t quantity_offset = 1020;
constexpr std::size_t fault_offset = 1024;
constexpr std::size_t word_size = 8;

std::string_view status_name(Status status) {
    switch (status) {
    case Status::ok: return "ok";
    case Status::end: return "end";
    case Status::cancelled: return "cancelled";
    case Status::not_found: return "not_found";
    case Status::programming_error: return "programming_error";
    case Status::io_error: return "io_error";
    case Status::corrupt: return "corrupt";
    }
    return "unknown status";
}

template<class Actual, class Expected>
bool require_equal(std::string_view reason, const Actual& actual, const Expected& expected) {
    if (actual == expected) return true;
    std::cerr << "NOT COMPARED: " << reason << ": expected " << expected
              << ", got " << actual << '\n';
    return false;
}

bool require_ok(std::string_view reason, Status actual) {
    return require_equal(reason, status_name(actual), status_name(Status::ok));
}

Status compare_keys(void*, const pd::ScratchByteSource& a,
                    const pd::ScratchByteSource& b, int& order) noexcept {
    std::array<std::byte, word_size> left{}, right{};
    if (a.size() != left.size() || b.size() != right.size()) return Status::programming_error;
    auto status = a.read(0, {left.data(), left.size()});
    if (status != Status::ok) return status;
    status = b.read(0, {right.data(), right.size()});
    if (status != Status::ok) return status;
    order = 0;
    for (std::size_t i = left.size(); i-- != 0;) {
        if (left[i] == right[i]) continue;
        order = left[i] < right[i] ? -1 : 1;
        break;
    }
    return Status::ok;
}

class FaultedSource final : public pd::ScratchByteSource {
public:
    explicit FaultedSource(pd::ScratchBytes bytes) : source_(bytes) {}
    std::uint64_t size() const noexcept override { return source_.size(); }
    Status read(std::uint64_t offset, pd::ScratchBuffer out) const noexcept override {
        if (reads < offsets.size()) offsets[reads] = offset;
        ++reads;
        if (offset >= fault_offset) return Status::io_error;
        return source_.read(offset, out);
    }

    mutable std::array<std::uint64_t, 3> offsets{};
    mutable std::size_t reads = 0;
private:
    const pd::ScratchSpanSource source_;
};
} // namespace

int main(int argc, char** argv) try {
    // Optional controls exercise the fixture guards without adding CTest rows.
    const std::string_view mode = argc > 1 ? argv[1] : "";
    if (argc > 2 || (argc == 2 && mode != "--empty" && mode != "--wrong-input")) {
        std::cerr << "NOT COMPARED: input mode: expected no argument, --empty or --wrong-input; got "
                  << argc - 1 << " argument(s), first '" << mode << "'\n";
        return 1;
    }

    if (!require_equal("signed int value bits", std::numeric_limits<int>::digits, 31)) return 1;
    std::array<std::byte, 16 * page_size> storage{};
    pd::FixedMemoryScratchBacking backing({storage.data(), storage.size()}, page_size);
    pd::ScratchStoreConfig config{2, page_size, 1, 1, {941, nullptr, &compare_keys},
                                  pd::ScratchOpenMode::create_empty};
    pd::CommandScratchStore store(config, &backing);
    if (!require_ok("scratch backing", backing.status())
        || !require_ok("scratch store", store.status())
        || !require_equal("scratch page size", store.page_size(), page_size)
        || !require_ok("begin empty store", store.begin())
        || !require_ok("commit empty store", store.commit(pd::ScratchDurability::buffered))
        || !require_equal("committed record count", store.record_count(), std::uint64_t{0}))
        return 1;

    ad::Journal journal;
    ad::bind_journal_codec(journal, store);
    const auto seq = journal.next_sequence();
    if (!require_equal("issued command sequence", seq, std::uint64_t{1})) return 1;
    const std::string id(86, 'x');
    auto observation = std::make_shared<ad::CommandObservation>();
    observation->command = seq;
    observation->id = id;
    ad::CommandEvent command;
    command.observation = observation;
    const ad::Event event = command;
    const auto encoded = ad::encode_journal_record(event);
    const auto length = mode == "--empty" ? std::uint64_t{0} : encoded.size();
    if (length < quantity_offset + word_size) {
        std::cerr << "NOT COMPARED: encoded record length: expected at least "
                  << quantity_offset + word_size << ", got " << length << '\n';
        return 1;
    }

    std::vector<std::byte> wire(static_cast<std::size_t>(length));
    if (!require_ok("copy encoded record", encoded.read(0, {wire.data(), wire.size()})))
        return 1;
    if (mode == "--wrong-input") wire[quantity_offset] = std::byte{0};

    constexpr std::string_view key = "event.observation.quantity_type";
    const std::string_view wire_text(reinterpret_cast<const char*>(wire.data()), wire.size());
    if (!require_equal("quantity_type key offset", wire_text.find(key),
                       quantity_offset - word_size - key.size()))
        return 1;
    std::uint64_t quantity_word = 0;
    for (std::size_t i = 0; i < word_size; ++i)
        quantity_word |= std::to_integer<std::uint64_t>(wire[quantity_offset + i]) << (8 * i);
    if (quantity_word != 0xffffffffffffffffULL) {
        std::cerr << "NOT COMPARED: quantity_type word at byte 1020: expected "
                     "0xffffffffffffffff, got 0x" << std::hex << quantity_word << '\n';
        return 1;
    }

    // First prove the intact source decodes to this specific observation.
    const pd::ScratchSpanSource intact({wire.data(), wire.size()});
    std::array<std::byte, page_size> page{};
    ad::Event decoded = ad::ReviewEvent{};
    if (!require_ok("decode intact record",
                    ad::decode_journal_record(intact, {page.data(), page.size()}, decoded)))
        return 1;
    const auto* decoded_command = std::get_if<ad::CommandEvent>(&decoded);
    if (!require_equal("decoded command has an observation",
                       decoded_command && bool(decoded_command->observation), true)
        || !require_equal("decoded command sequence", decoded_command->observation->command, seq)
        || !require_equal("decoded command id", decoded_command->observation->id, id)
        || !require_equal("decoded quantity_type", decoded_command->observation->quantity_type, -1))
        return 1;

    const FaultedSource source({wire.data(), wire.size()});
    const auto status = ad::append_journal_record(journal, source);
    if (!require_equal("append source read count", source.reads, source.offsets.size())
        || !require_equal("first refill offset", source.offsets[0], std::uint64_t{0})
        || !require_equal("second refill offset", source.offsets[1], std::uint64_t{page_size})
        || !require_equal("failed refill offset", source.offsets[2], std::uint64_t{fault_offset}))
        return 1;
    if (status != Status::io_error) {
        std::cerr << "FAIL #941: quantity_type refill at byte 1024: expected io_error, got "
                  << status_name(status) << '\n';
        return 1;
    }
    std::cout << "PASS #941: quantity_type refill at byte 1024 returned io_error\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "NOT COMPARED: fixture setup: expected no exception, got " << error.what() << '\n';
    return 1;
}
