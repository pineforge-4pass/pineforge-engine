#include <pineforge/source/pine_strategy_host.hpp>

#include <cstddef>
#include <cstdio>
#include <memory>
#include <type_traits>

namespace abi_layout_probe {
using Adapter = pineforge::source::PineExecutionAdapter;
using Host = pineforge::source::PineStrategyHost;

// Explicit member instantiation follows the adapter storage tests' access
// convention without changing the classes or their access specifiers.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct RetiredScratch { friend auto access(RetiredScratch); };
struct BeforeScratch { friend auto access(BeforeScratch); };
struct AfterScratch { friend auto access(AfterScratch); };
struct HostAdapter { friend auto access(HostAdapter); };
template struct Access<RetiredScratch, &Adapter::retired_row_scratch_>;
template struct Access<BeforeScratch, &Adapter::pine_view_>;
template struct Access<AfterScratch, &Adapter::staged_scopes_>;
template struct Access<HostAdapter, &Host::adapter_>;

class ProbeHost final : public Host {
public:
    void on_source_bar(const pineforge::Bar&) override {}
};

template<class Object, class Member>
void print_offset(const char* name, const Object& object, Member Object::*member) {
    // Use actual subobject addresses so non-standard-layout hosts need no offsetof.
    const auto* base = reinterpret_cast<const unsigned char*>(std::addressof(object));
    const auto* field = reinterpret_cast<const unsigned char*>(std::addressof(object.*member));
    std::printf("abi offset %s=%zu\n", name, static_cast<std::size_t>(field - base));
}

// The same probe also reads headers that still expose the roster workspace.
template<class Scratch, class = void>
struct HasRosterRoots : std::false_type {};
template<class Scratch>
struct HasRosterRoots<Scratch, std::void_t<decltype(&Scratch::roster_roots)>>
    : std::true_type {};

template<class Scratch>
void print_scratch(const Scratch& scratch) {
    std::printf("abi sizeof RetiredRowScratch=%zu\n", sizeof(Scratch));
    print_offset("RetiredRowScratch::roots", scratch, &Scratch::roots);
    print_offset("RetiredRowScratch::askable_origins", scratch, &Scratch::askable_origins);
    print_offset("RetiredRowScratch::live_groups", scratch, &Scratch::live_groups);
    print_offset("RetiredRowScratch::source_bars", scratch, &Scratch::source_bars);
    print_offset("RetiredRowScratch::candidates", scratch, &Scratch::candidates);
    print_offset("RetiredRowScratch::candidate_names", scratch, &Scratch::candidate_names);
    print_offset("RetiredRowScratch::first_reissues", scratch, &Scratch::first_reissues);
    print_offset("RetiredRowScratch::origin_pins", scratch, &Scratch::origin_pins);
    print_offset("RetiredRowScratch::doomed", scratch, &Scratch::doomed);
    print_offset("RetiredRowScratch::held", scratch, &Scratch::held);
    print_offset("RetiredRowScratch::released", scratch, &Scratch::released);
    print_offset("RetiredRowScratch::unsuperseded", scratch, &Scratch::unsuperseded);
    print_offset("RetiredRowScratch::reissue_high_water", scratch, &Scratch::reissue_high_water);
    print_offset("RetiredRowScratch::reissue_cycle", scratch, &Scratch::reissue_cycle);
    print_offset("RetiredRowScratch::reissues", scratch, &Scratch::reissues);
    print_offset("RetiredRowScratch::next_reissues", scratch, &Scratch::next_reissues);
    print_offset("RetiredRowScratch::new_candidates", scratch, &Scratch::new_candidates);
    print_offset("RetiredRowScratch::known_candidates", scratch, &Scratch::known_candidates);
    print_offset("RetiredRowScratch::unresolved_candidates", scratch, &Scratch::unresolved_candidates);
    print_offset("RetiredRowScratch::adopted_reissues", scratch, &Scratch::adopted_reissues);
    print_offset("RetiredRowScratch::working_roots", scratch, &Scratch::working_roots);
    if constexpr (HasRosterRoots<Scratch>::value)
        print_offset("RetiredRowScratch::roster_roots", scratch, &Scratch::roster_roots);
    print_offset("RetiredRowScratch::lot_openings", scratch, &Scratch::lot_openings);
    print_offset("RetiredRowScratch::successor_lots", scratch, &Scratch::successor_lots);
    print_offset("RetiredRowScratch::released_origins", scratch, &Scratch::released_origins);
    print_offset("RetiredRowScratch::lot_probe", scratch, &Scratch::lot_probe);
    print_offset("RetiredRowScratch::roster_pass_due", scratch, &Scratch::roster_pass_due);
    print_offset("RetiredRowScratch::live_families", scratch, &Scratch::live_families);
    print_offset("RetiredRowScratch::origin_fronts_pinned", scratch, &Scratch::origin_fronts_pinned);
}
} // namespace abi_layout_probe

int main() {
    using namespace abi_layout_probe;
    ProbeHost storage;
    Host& host = storage;
    auto& adapter = host.*access(HostAdapter{});
    print_scratch(adapter.*access(RetiredScratch{}));
    std::printf("abi sizeof PineExecutionAdapter=%zu\n", sizeof(Adapter));
    print_offset("PineExecutionAdapter::pine_view_", adapter, access(BeforeScratch{}));
    print_offset("PineExecutionAdapter::retired_row_scratch_", adapter, access(RetiredScratch{}));
    print_offset("PineExecutionAdapter::staged_scopes_", adapter, access(AfterScratch{}));
    std::printf("abi sizeof PineStrategyHost=%zu\n", sizeof(Host));
    print_offset("PineStrategyHost::adapter_", host, access(HostAdapter{}));
    return 0;
}
