// A-12: retain followed by a fold must refold before the first retirement.
#include <pineforge/source/market_admission.hpp>
#include <pineforge/detail/scratch_backing.hpp>
#include <pineforge/detail/scratch_store.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {
namespace ad = pineforge::admission;
namespace pd = pineforge::detail;

pd::ScratchStatus numeric_order(void*, const pd::ScratchByteSource& left,
                                const pd::ScratchByteSource& right,
                                int& result) noexcept {
    if (left.size() != sizeof(std::uint64_t) || right.size() != sizeof(std::uint64_t))
        return pd::ScratchStatus::programming_error;
    std::array<std::byte, sizeof(std::uint64_t)> a{}, b{};
    auto status = left.read(0, {a.data(), a.size()});
    if (status != pd::ScratchStatus::ok)
        return status;
    status = right.read(0, {b.data(), b.size()});
    if (status != pd::ScratchStatus::ok)
        return status;
    result = 0;
    for (std::size_t index = a.size(); index-- != 0;) {
        if (a[index] == b[index])
            continue;
        result = std::to_integer<unsigned>(a[index]) < std::to_integer<unsigned>(b[index])
            ? -1 : 1;
        break;
    }
    return pd::ScratchStatus::ok;
}

void append_one(ad::Journal& journal) {
    {
        auto allocation = journal.reserve();
        auto observation = std::make_shared<ad::CommandObservation>();
        observation->command = allocation.sequence();
        observation->bar = 0;
        ad::CommandEvent event;
        event.observation = std::move(observation);
        journal.append(std::move(event));
    }
    const auto before = journal.folded_window();
    if (before.count != 1 || journal.event_count() != 1)
        throw 1;
    journal.retain({});
    const auto after = journal.folded_window();
    if (after.count != 0 || journal.event_count() != 0)
        throw 2;
}
}

int main() {
    try {
        ad::Journal ordinary;
        append_one(ordinary);

        std::vector<std::byte> bytes(32 * 1024 * 1024);
        pd::FixedMemoryScratchBacking backing({bytes.data(), bytes.size()}, 512);
        pd::CommandScratchStore store(
            {2, 512, 1, 1, {0x70326175786a6f01ULL, nullptr, &numeric_order},
             pd::ScratchOpenMode::create_empty},
            &backing);
        if (backing.status() != pd::ScratchStatus::ok
            || store.status() != pd::ScratchStatus::ok)
            return 1;
        ad::Journal codec_journal;
        ad::bind_journal_codec(codec_journal, store);
        append_one(codec_journal);
    } catch (...) {
        return 1;
    }
    return 0;
}
