#include "sink_v2_index_heap.hpp"

#include <iostream>

namespace {
using namespace sink2_test;

struct Key { std::uint64_t bits; bool valid; };
using Keys = std::array<Key, 5>;
void check_ranks(const Bytes& file, const std::vector<Keys>& keys) {
    const auto tail = file.size() - 160;
    const auto root_at = load(file.data() + tail, 8);
    const auto* root = file.data() + root_at;
    require(load(file.data() + tail + 8, 4) == 4096 && load(root + 6, 2) == 0, "rank test needs FULL");
    require(digest_at(sha(bytes(root, 4096)), file.data() + tail + 24), "rank root SHA");
    require(digest_at(sha(bytes(file.data(), tail)), file.data() + tail + 88), "rewritten footer prefix SHA");
    for (unsigned id = 8; id < 18; ++id) {
        const auto key = (id - 8) / 2;
        const bool descending = (id & 1) != 0;
        std::vector<std::uint64_t> expected(keys.size());
        std::iota(expected.begin(), expected.end(), 0);
        std::stable_sort(expected.begin(), expected.end(), [&](std::uint64_t a, std::uint64_t b) {
            const auto& x = keys[a][key];
            const auto& y = keys[b][key];
            if (x.valid != y.valid) return x.valid;
            if (!x.valid) return false;
            if (key == 0) return descending ? signed_value(x.bits) > signed_value(y.bits)
                                          : signed_value(x.bits) < signed_value(y.bits);
            const double xv = key == 4 ? -std::fabs(real(x.bits)) : real(x.bits);
            const double yv = key == 4 ? -std::fabs(real(y.bits)) : real(y.bits);
            return descending ? xv > yv : xv < yv;
        });
        const auto* locator = root + 512 + 64 * id;
        const auto offset = load(locator, 8), pages = load(locator + 8, 4);
        require(load(locator + 12, 4) == 4 && load(locator + 16, 8) == keys.size()
                && load(locator + 28, 4) == 1, "required rank table locator");
        std::size_t ordinal = 0;
        for (std::uint64_t p = 0; p < pages; ++p) {
            const auto length = p + 1 == pages ? load(locator + 24, 4) : 65536;
            require(offset + p * 65536 + length <= root_at, "rank page bounds");
            const auto* page = file.data() + offset + p * 65536;
            require(std::memcmp(page, "IDX2", 4) == 0 && page[4] == id, "rank page header");
            const auto used = std::min<std::uint64_t>(15360, keys.size() - ordinal);
            require(length == 4096 + 4 * used && load(page + 12, 4) == used, "rank short final page");
            std::string checked(bytes(page, 4092));
            checked.append(bytes(page + 4096, length - 4096));
            require(crc(checked) == load(page + 4092, 4), "rank page CRC");
            std::string leaf("PFS2-INDEX\0", 11);
            leaf.append(bytes(page, 24)); leaf.append(bytes(page + 4096, length - 4096));
            auto hash = sha(leaf);
            require(digest_at(hash, page + 24), "rank leaf SHA");
            unsigned depth = 0;
            for (std::uint64_t n = 1; n < pages; n *= 2) ++depth;
            require(page[6] == depth, "rank proof depth");
            for (unsigned level = 0; level < depth; ++level) {
                std::string node("PFS2-NODE\0", 10);
                const auto sibling = bytes(page + 56 + 32 * level, 32);
                if ((p >> level) & 1) { node.append(sibling); node.append(bytes(hash.data(), 32)); }
                else { node.append(bytes(hash.data(), 32)); node.append(sibling); }
                hash = sha(node);
            }
            require(digest_at(hash, locator + 32), "rank Merkle proof");
            for (std::uint64_t slot = 0; slot < used; ++slot)
                require(load(page + 4096 + 4 * slot, 4) == expected[ordinal++], "brute-force rank order");
        }
        require(ordinal == expected.size(), "rank population lost");
    }
}

struct Counted : sink::BlockMemory { std::size_t allocated_peak; };
Counted counted(unsigned count) {
    Directory output, scratch;
    const auto path = output.file("fixed-memory");
    sink::BlockPolicy policy;
    policy.force_full = true; // exercise the same sorter at BOTH sizes
    sink::BlockWriter writer(descriptor(path, scratch.path), policy, std::make_unique<StoredCodec>());
    writer.write_header({false, false, false, 0, 0});
    std::vector<Keys> reference;
    reference.reserve(count);
    for (unsigned row = 0; row < count; ++row) {
        pf_trade_t t{};
        t.entry_time = row;
        t.exit_time = (count - row) % 17;
        t.entry_price = 100.0; t.exit_price = 101.0; t.qty = 1.0;
        t.pnl = row % 31 == 0 ? std::numeric_limits<double>::quiet_NaN()
            : row % 7 == 0 ? -0.0 : static_cast<double>(static_cast<int>(row % 19) - 9);
        t.pnl_pct = row % 3 == 0 ? std::numeric_limits<double>::infinity() : t.pnl / 100.0;
        t.max_runup = (count - row) % 23;
        // Positive and negative raw drawdowns must both compare as -abs.
        t.max_drawdown = row % 11 == 0 ? std::numeric_limits<double>::infinity()
            : row % 2 ? t.max_runup : -t.max_runup;
        reference.push_back({{{static_cast<std::uint64_t>(t.exit_time), true},
            {word(t.pnl), std::isfinite(t.pnl)}, {word(t.pnl_pct), std::isfinite(t.pnl_pct)},
            {word(t.max_runup), true}, {word(t.max_drawdown), std::isfinite(t.max_drawdown)}}});
        writer.write_t(row + 1, t, row + 1, 0, "", "", "");
    }
    writer.write_trailer();
    require(std::filesystem::is_empty(scratch.path), "writing left descriptor/key scratch");
    const auto before = read_file(path);
    {
        const final_heap::Interval interval;
        writer.finalize(metadata_for(writer.receipt()));
    }
    const auto allocated_peak = final_heap::peak.load();
    const auto file = read_file(path);
    require(std::equal(before.begin(), before.end(), file.begin()), "rank construction rewrote immutable bytes");
    check_ranks(file, reference);
    require(std::filesystem::is_empty(scratch.path), "rank construction leaked work files");
    const auto memory = writer.memory();
    require(memory.finalization_bytes != 0 && memory.finalization_bytes + memory.work_file_bytes
            <= sink::BlockMemory::finalization_allowance, "counted finalization exceeds F");
    require(memory.work_file_bytes <= 2 * 65536 && memory.decoder_bytes <= 2 * sink::BlockPolicy::max_block_bytes,
            "fixed work files/reused decoder bound");
    // Include the separately charged control/stack term. Allocator headers
    // are counted above; host/cgroup RSS and zstd workspace remain lab gates.
    require(allocated_peak + 65536 + memory.work_file_bytes <= 1836032,
            "observed live finalization allocations exceed F");
    return {memory, allocated_peak};
}

} // namespace

int main() {
    try {
        const auto small = counted(1000), large = counted(400000);
        require(small.finalization_bytes == large.finalization_bytes
                && small.work_file_bytes == large.work_file_bytes, "sort state grows with T");
        require(small.finalization_bytes + small.work_file_bytes <= 1836032
                && large.finalization_bytes + large.work_file_bytes <= 1836032,
                "fixed allowance differs from section 10.7");
        const auto difference = large.allocated_peak > small.allocated_peak
            ? large.allocated_peak - small.allocated_peak : small.allocated_peak - large.allocated_peak;
        require(difference <= 2 * 65536, "new allocations grow beyond a fixed block between 1k and 400k");
        std::cout << "sink v2 index: in-output merge, ten independent rank oracles and fixed F at 1k/400k passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sink v2 index: " << error.what() << '\n';
        return 1;
    }
}
