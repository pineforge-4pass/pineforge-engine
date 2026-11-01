#include <pineforge/source/pine_strategy_host.hpp>

#include <cstddef>
#include <cstdio>
#include <memory>
#include <type_traits>
#include <utility>

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
std::size_t member_offset(const Object& object, Member Object::*member) {
    // Use actual subobject addresses so non-standard-layout hosts need no offsetof.
    const auto* base = reinterpret_cast<const unsigned char*>(std::addressof(object));
    const auto* field = reinterpret_cast<const unsigned char*>(std::addressof(object.*member));
    return static_cast<std::size_t>(field - base);
}

template<class Object, class Member>
void print_offset(const char* name, const Object& object, Member Object::*member) {
    std::printf("abi offset %s=%zu\n", name, member_offset(object, member));
}

// The same probe also reads headers that still expose the roster workspace.
template<class Scratch, class = void>
struct HasRosterRoots : std::false_type {};
template<class Scratch>
struct HasRosterRoots<Scratch, std::void_t<decltype(&Scratch::roster_roots)>>
    : std::true_type {};

using Scratch = std::remove_reference_t<
    decltype(std::declval<Adapter&>().*access(RetiredScratch{}))>;

#if defined(__linux__) && defined(__x86_64__) && defined(__GLIBCXX__)
// The ABI pins apply to Linux x86_64 builds that use GNU libstdc++.
static_assert(sizeof(Scratch) == 768, "RetiredRowScratch size changed");
static_assert(sizeof(Adapter) == 89584, "PineExecutionAdapter size changed");
static_assert(sizeof(Host) == 108040, "PineStrategyHost size changed");
static_assert(!HasRosterRoots<Scratch>::value,
              "RetiredRowScratch::roster_roots must be absent");
#endif

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

#if defined(__linux__) && defined(__x86_64__) && defined(__GLIBCXX__)
bool pin(const char* name, std::size_t actual, std::size_t expected) {
    if (actual == expected)
        return true;
    std::printf("abi PIN FAILED %s expected=%zu actual=%zu\n", name, expected, actual);
    return false;
}
#endif
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
#if defined(__linux__) && defined(__x86_64__) && defined(__GLIBCXX__)
    bool pins_hold = true;
    pins_hold &= pin("sizeof RetiredRowScratch", sizeof(Scratch), 672);
    pins_hold &= pin("sizeof PineExecutionAdapter", sizeof(Adapter), 89488);
    pins_hold &= pin("sizeof PineStrategyHost", sizeof(Host), 107944);
    pins_hold &= pin("RetiredRowScratch::roots",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::roots), 0);
    pins_hold &= pin("RetiredRowScratch::askable_origins",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::askable_origins), 24);
    pins_hold &= pin("RetiredRowScratch::live_groups",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::live_groups), 48);
    pins_hold &= pin("RetiredRowScratch::source_bars",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::source_bars), 72);
    pins_hold &= pin("RetiredRowScratch::candidates",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::candidates), 96);
    pins_hold &= pin("RetiredRowScratch::candidate_names",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::candidate_names), 120);
    pins_hold &= pin("RetiredRowScratch::first_reissues",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::first_reissues), 144);
    pins_hold &= pin("RetiredRowScratch::origin_pins",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::origin_pins), 168);
    pins_hold &= pin("RetiredRowScratch::doomed",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::doomed), 192);
    pins_hold &= pin("RetiredRowScratch::held",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::held), 216);
    pins_hold &= pin("RetiredRowScratch::released",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::released), 240);
    pins_hold &= pin("RetiredRowScratch::unsuperseded",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::unsuperseded), 264);
    pins_hold &= pin("RetiredRowScratch::reissue_high_water",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::reissue_high_water), 288);
    pins_hold &= pin("RetiredRowScratch::reissue_cycle",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::reissue_cycle), 296);
    pins_hold &= pin("RetiredRowScratch::reissues",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::reissues), 304);
    pins_hold &= pin("RetiredRowScratch::next_reissues",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::next_reissues), 328);
    pins_hold &= pin("RetiredRowScratch::new_candidates",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::new_candidates), 352);
    pins_hold &= pin("RetiredRowScratch::known_candidates",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::known_candidates), 376);
    pins_hold &= pin("RetiredRowScratch::unresolved_candidates",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::unresolved_candidates), 400);
    pins_hold &= pin("RetiredRowScratch::adopted_reissues",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::adopted_reissues), 424);
    pins_hold &= pin("RetiredRowScratch::working_roots",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::working_roots), 448);
    pins_hold &= pin("RetiredRowScratch::lot_openings",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::lot_openings), 472);
    pins_hold &= pin("RetiredRowScratch::successor_lots",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::successor_lots), 496);
    pins_hold &= pin("RetiredRowScratch::released_origins",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::released_origins), 520);
    pins_hold &= pin("RetiredRowScratch::lot_probe",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::lot_probe), 544);
    pins_hold &= pin("RetiredRowScratch::roster_pass_due",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::roster_pass_due), 568);
    pins_hold &= pin("RetiredRowScratch::live_families",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::live_families), 576);
    pins_hold &= pin("RetiredRowScratch::origin_fronts_pinned",
                     member_offset(adapter.*access(RetiredScratch{}), &Scratch::origin_fronts_pinned), 664);
    pins_hold &= pin("PineExecutionAdapter::pine_view_",
                     member_offset(adapter, access(BeforeScratch{})), 88208);
    pins_hold &= pin("PineExecutionAdapter::retired_row_scratch_",
                     member_offset(adapter, access(RetiredScratch{})), 88216);
    pins_hold &= pin("PineExecutionAdapter::staged_scopes_",
                     member_offset(adapter, access(AfterScratch{})), 88888);
    pins_hold &= pin("PineStrategyHost::adapter_",
                     member_offset(host, access(HostAdapter{})), 6424);
    return pins_hold ? 0 : 1;
#else
    std::printf("abi pins skipped: not linux x86_64 libstdc++\n");
    return 0;
#endif
}
