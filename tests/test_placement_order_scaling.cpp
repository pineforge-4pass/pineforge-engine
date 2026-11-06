#include <pineforge/source/pine_adapter.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
using source::PlacementSnapshot;
using source::PlacementTable;

struct PlacementWorkTag {
    using type = std::uint64_t PlacementTable::*;
    friend type access(PlacementWorkTag);
};

struct LookupWorkTag {
    using type = std::uint64_t PlacementTable::*;
    friend type access(LookupWorkTag);
};

struct RemovalWorkTag {
    using type = std::uint64_t PlacementTable::*;
    friend type access(RemovalWorkTag);
};

struct OldOnlyTag {
    using type = bool PlacementTable::*;
    friend type access(OldOnlyTag);
};

template<class Tag, typename Tag::type Member>
struct PrivateAccess {
    friend typename Tag::type access(Tag) { return Member; }
};

template struct PrivateAccess<PlacementWorkTag, &PlacementTable::placement_work_>;
template struct PrivateAccess<LookupWorkTag, &PlacementTable::lookup_work_>;
template struct PrivateAccess<RemovalWorkTag, &PlacementTable::removal_work_>;
template struct PrivateAccess<OldOnlyTag, &PlacementTable::old_only_>;

struct Result {
    std::size_t live = 0;
    std::uint64_t work = 0;
};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

PlacementSnapshot resting_limit(std::size_t serial) {
    PlacementSnapshot row;
    row.family = source::PineOrderFamily::Order;
    row.source_id = "resting-book";
    row.comment = "never-fill-" + std::to_string(serial);
    row.requested_qty = 1.0;
    row.is_long = true;
    row.opening = true;
    row.exit_levels.limit = 1.0;
    row.command_sequence = static_cast<std::uint64_t>(serial + 1);
    return row;
}

std::uint64_t incarnation_for(std::size_t serial) {
    // The native handle space can contain gaps when other accepted commands
    // share a run. The gap grows with the input serial without using a
    // population-specific literal.
    const auto stride = serial / 100U + 1U;
    return 1U + static_cast<std::uint64_t>(serial) * stride;
}

Result place_book(std::size_t count) {
    PlacementTable table;
    for (std::size_t serial = 0; serial < count; ++serial) {
        const auto incarnation = incarnation_for(serial);
        const auto inserted = table.try_emplace(incarnation, resting_limit(serial));
        require(inserted.second, "every never-fill limit placement has a fresh incarnation");
    }

    require(table.size() == count,
            "live placement count follows the input: " + std::to_string(count));
    std::uint64_t previous = 0;
    std::size_t visited = 0;
    table.visit_rows([&](std::uint64_t incarnation, const PlacementSnapshot& row) {
        require(incarnation > previous, "placement rows remain incarnation ordered");
        require(row.family == source::PineOrderFamily::Order
                    && row.exit_levels.limit == 1.0
                    && row.comment == "never-fill-" + std::to_string(visited),
                "each input limit remains a distinct never-fill order");
        previous = incarnation;
        ++visited;
        return false;
    });
    require(visited == count, "every live order is present after placement");

    const auto work = table.*access(PlacementWorkTag{});
    std::printf("placement-order N=%zu live=%zu work=%llu per_order=%.3f\n",
                count, table.size(), static_cast<unsigned long long>(work),
                static_cast<double>(work) / static_cast<double>(count));
    return {table.size(), work};
}

void density_and_access_work(std::size_t count, bool sparse) {
    PlacementTable table;
    table.fixture_use_memory_auxiliary_tables();
    const std::uint64_t stride = sparse ? count + 1U : 1U;
    const auto key = [&](std::size_t serial) { return 1U + serial * stride; };
    for (std::size_t serial = 0; serial != count; ++serial) {
        require(table.try_emplace(key(serial), resting_limit(serial)).second,
                "density input must contain distinct rows");
        require((table.*access(OldOnlyTag{})) == (sparse && serial != 0),
                "only a sparse book should enter ordered storage");
    }

    // One comparison is counted at each binary-search predicate evaluation.
    // The bound comes from the input population, rather than a pinned result.
    std::uint64_t comparison_bound = 1;
    for (auto remaining = count; remaining > 1; remaining /= 2U) ++comparison_bound;
    const auto before_lookup = table.*access(LookupWorkTag{});
    const PlacementTable& rows = table;
    for (std::size_t serial = 0; serial != count; ++serial) {
        const auto found = rows.find(key(serial));
        require(found != rows.end() && found->second.comment == resting_limit(serial).comment,
                "lookup must return the input row");
        require(rows.contains(key(serial)), "contains must find every input row");
        require(rows.at(key(serial)).command_sequence == serial + 1U,
                "at must return the input sequence");
    }
    const auto lookup_work = (table.*access(LookupWorkTag{})) - before_lookup;
    require(sparse ? lookup_work >= 3U * count : lookup_work == 0,
            "ordered lookup comparisons must be measured; dense lookup stays direct");
    require(lookup_work <= 3U * count * comparison_bound, "lookup exceeds binary-search bound");

    const auto before_removal = table.*access(RemovalWorkTag{});
    const auto before_search = table.*access(LookupWorkTag{});
    const auto retired = count / 2U;
    std::uint64_t expected_removal_work = 0;
    for (std::size_t serial = 0; serial != retired; ++serial) {
        // Erasing the oldest row shifts every remaining vector element.
        // Count the removed element too; a dense slot removal costs one.
        expected_removal_work += sparse ? count - serial : 1U;
        require(table.erase(key(serial)), "oldest-half removal must succeed");
    }
    const auto removal_work = (table.*access(RemovalWorkTag{})) - before_removal;
    const auto removal_search = (table.*access(LookupWorkTag{})) - before_search;
    require(removal_work == expected_removal_work, "removal did not count actual shifted rows");
    require(sparse ? removal_search >= 2U * retired : removal_search == 0,
            "removal must measure its locate and take searches");
    require(removal_search <= 2U * retired * comparison_bound,
            "removal search exceeds binary-search bound");
    require(table.size() == count - retired && table.erased_rows() == retired,
            "removal must preserve the requested live and retired populations");
    for (std::size_t serial = 0; serial != count; ++serial)
        require(rows.contains(key(serial)) == (serial >= retired),
                "converted removal changed the wrong row");
    std::printf("placement-access N=%zu sparse=%u lookup=%llu removal_search=%llu removal=%llu\n",
                count, static_cast<unsigned>(sparse),
                static_cast<unsigned long long>(lookup_work),
                static_cast<unsigned long long>(removal_search),
                static_cast<unsigned long long>(removal_work));
}
} // namespace

int main() {
    try {
        for (const auto count : {258U, 516U, 1032U}) {
            density_and_access_work(count, false);
            density_and_access_work(count, true);
        }
        const std::vector<std::size_t> sizes{1000, 5000, 20000};
        std::vector<Result> results;
        results.reserve(sizes.size());
        for (const auto count : sizes) results.push_back(place_book(count));

        for (std::size_t index = 0; index < results.size(); ++index)
            require(results[index].live == sizes[index],
                    "the measured population is not the requested population");

        for (std::size_t index = 1; index < results.size(); ++index) {
            // The work per placement may vary by a constant representation
            // transition, but it cannot rise with the live book.
            require(results[index].work * sizes[0]
                        <= results[0].work * sizes[index] * 2U,
                    "placement work per order grows with the live order book");
            require(results[index].work <= sizes[index] * 4U,
                    "placement work exceeds a constant amortised bound");
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_placement_order_scaling: %s\n", error.what());
        return 1;
    }
}
