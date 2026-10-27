#pragma once

#include <pineforge/source/detail/live_handle_sequence.hpp>
#include <pineforge/source/live_source_identity.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pineforge::source {
class PlacementTable;
class OriginRoster;
struct PlacementSnapshot;

namespace detail {

enum class LiveClass : std::uint8_t {
    Placed, Opening, EntryLikeOpening,
    EntryOpening, OrderOpening, CloseOpening, CloseAllOpening,
    ExitLimitOpening, ExitStopOpening, ExitTrailOpening, MarginOpening, RiskOpening,
    Entry, CloseAll, NonImmediateClose, DynamicGlobalExit, ReservationCapture,
    ReservationEligible, ReservationExit, ReservationClose, WaypointMargin, Trail, RestedStopForced,
    EntryOpeningU2, DefaultEntryOpening, ReservationParent, UnpricedEntry,
    UnpricedClose, ReverseOpeningEntry, SamePointEligible, SamePointMarket, SamePointStop, SamePointLimit,
    PureStopEntry, PureBuyLimitOpening, TouchedStopEntry, DropLimit, DropStop,
    NonPureLimitOpening, ExitStop, ExitLimit, PureMarketAddOpening,
    DefaultEntryOpeningU2, TerminalExplicitMarketEntry, Count
};
enum class LivePrice : std::uint8_t { Stop, Limit };

// Empty identity strings are ordinary exact keys. All is the only wildcard.
struct LiveKey {
    enum class Kind : std::uint8_t {
        All, Source, FromEntry, Pair, ScriptTime, CreatedBar, SourceTime,
        BracketOrigin, PerEntryOrigin
    };
    Kind kind = Kind::All;
    std::string first, second;
    std::int64_t number = 0;
    native_order::RequestHandle origin{};
    std::uint64_t incarnation = 0;
    static LiveKey source(const std::string&);
    static LiveKey from_entry(const std::string&);
    static LiveKey pair(const std::string&, const std::string&);
    static LiveKey script_time(std::int64_t);
    static LiveKey created_bar(std::int32_t);
    static LiveKey source_time(const std::string&, std::int64_t);
    static LiveKey bracket_origin(native_order::RequestHandle);
    static LiveKey per_entry_origin(std::uint64_t);
    bool operator<(const LiveKey&) const;
    bool operator==(const LiveKey&) const;
};

// Run-owned, derived directory. Small books read the canonical tables without
// allocating directory storage; binding never changes canonical row values.
class LiveRowIndex {
    using Handle = native_order::RequestHandle;
    using Ordered = std::map<std::uint64_t, Handle>;
    static constexpr std::size_t kClasses = static_cast<std::size_t>(LiveClass::Count);
    struct Record {
        std::uint64_t signature = 0, command_sequence = 0;
        std::vector<std::uint64_t> orders;
        std::int64_t cycle = 0;
        bool is_long = false;
        std::array<LiveKey, 9> keys;
        std::array<double, 2> prices{};
    };
    struct Bucket {
        Ordered rows;
        std::array<Ordered, 2> sides;
        // field -> side -> positive finite price -> live-ordered members.
        std::array<std::array<std::map<double, Ordered>, 2>, 2> prices;
        std::map<std::uint64_t, Ordered> commands;
        std::map<std::int64_t, Ordered> cycles;
    };
public:
    static constexpr std::size_t kActivationThreshold = 64;
    // Non-owning, allocation-free binding. Activation asks the owner to
    // re-register its CURRENT canonical rosters, including inactive appends.
    using OriginRebuilder = void (*)(const void*, LiveRowIndex&);
    void bind_origin_rebuilder(const void*, OriginRebuilder) noexcept;
    bool bound_to(const PlacementTable&, const LiveHandleSequence&) const noexcept;
    void rebuild_from(const PlacementTable&, const LiveHandleSequence&);
    void clear();
    void insert(Handle); // Canonical sequence/row must already be published.
    void refresh(Handle); // Refresh all full live handles of this incarnation.
    void refresh_incarnation(std::uint64_t);
    void erase(Handle); // Canonical sequence must already exclude the handle.
    using SourceMap = std::unordered_map<std::uint64_t, LiveSourceHandle>;
    void bind_source_keys(const SourceMap&);
    void refresh_source_key(std::uint64_t);
    std::vector<std::uint64_t> source_keys(Handle) const;
    void clear_origins();
    void rebuild_origins(std::uint64_t cohort, const OriginRoster&);
    void append_origin(std::uint64_t cohort, std::size_t global_position, Handle);
    void retire_origins_before(std::uint64_t cohort, std::size_t exclusive);
    std::vector<std::size_t> origin_positions(std::uint64_t cohort, const OriginRoster&) const;
    bool contains(Handle) const;
    std::size_t size() const;
    std::size_t missing_rows() const;
    std::vector<Handle> members() const;
    std::size_t count(LiveClass, const LiveKey& = {}, int side = -1) const;
    std::vector<Handle> rows(LiveClass, const LiveKey& = {}, int side = -1) const;
    std::optional<double> min_price(LiveClass, LivePrice, const LiveKey& = {}, int side = -1) const;
    std::optional<double> max_price(LiveClass, LivePrice, const LiveKey& = {}, int side = -1) const;
    std::size_t count_in_range(LiveClass, LivePrice, double low, double high,
                               const LiveKey& = {}, int side = -1) const;
    std::vector<Handle> rows_in_range(LiveClass, LivePrice, double low, double high,
                                      const LiveKey& = {}, int side = -1) const;
    std::vector<Handle> before_command(LiveClass, std::uint64_t exclusive,
                                      const LiveKey& = {}, int side = -1) const;
    std::size_t count_before_command(LiveClass, std::uint64_t exclusive,
                                      const LiveKey& = {}, int side = -1) const;
    // Preserve a caller's original loop as the small-book path and the Debug
    // oracle. Only the production fallback contributes physical visit work.
    template<class Indexed, class Scan>
    auto query_or_scan(const char* component, Indexed&& indexed, Scan&& scan) const {
        if (!active_) return scan(true);
        const auto answer = indexed();
        if (oracle_enabled()) check_answer(answer == scan(false), component);
        return answer;
    }
    // Observers retain their original row predicate. Build identity keys and
    // owning selections only in active mode.
    template<class MakeKey, class Observe>
    void visit_candidates(LiveClass c, MakeKey&& make_key, Observe&& observe) const {
        if (!active_) {
            if (sequence_) for (const auto& h : *sequence_) observe(h);
        } else {
            for (const auto& h : rows(c, make_key())) observe(h);
        }
    }
    std::vector<Handle> dropped_exit_rows(const std::string& source, const std::string& from,
                                         bool drop_limit, bool drop_stop,
                                         std::uint64_t* visits = nullptr) const;
    std::vector<Handle> reservation_rows(const std::string& exact_from,
                                         std::int64_t current_cycle) const;
    // The observer retains the original reservation predicate. Small books
    // use its old loop without first allocating an owning handle vector;
    // active books supply the oracle-checked ordered reservation selection.
    template<class Observe>
    void visit_reservation_candidates(const std::string& exact_from,
                                      std::int64_t current_cycle, Observe&& observe) const {
        if (!active_) {
            if (sequence_) for (const auto& h : *sequence_) observe(h);
        } else {
            for (const auto& h : reservation_rows(exact_from, current_cycle)) observe(h);
        }
    }
    std::size_t reservation_count(const std::string& exact_from, std::int64_t current_cycle) const;
    static const char* class_name(LiveClass);
    static bool oracle_enabled() noexcept;
    // Assertion builds only: independent whole-book comparison, never repair.
    void verify() const;
private:
    static void check_answer(bool matches, const char* component);
    bool update_activation();
    void release_storage();
    Record project(Handle) const;
    void publish(Handle);
    void remove(Handle);
    void add_buckets(Handle, const Record&, std::uint64_t mask = ~std::uint64_t{0});
    void remove_buckets(const Record&, std::uint64_t mask = ~std::uint64_t{0});
    const Bucket* bucket(LiveClass, const LiveKey&) const;
    void check_query(LiveClass, const LiveKey&, int, const std::vector<Handle>&) const;
    void set_origin_live(Handle, bool);
    const PlacementTable* table_ = nullptr;
    const LiveHandleSequence* sequence_ = nullptr;
    const SourceMap* sources_ = nullptr;
    const void* origin_owner_ = nullptr;
    OriginRebuilder origin_rebuilder_ = nullptr;
    bool active_ = false;
    std::unordered_map<std::uint64_t, Handle> source_owners_;
    std::unordered_map<Handle, std::unordered_set<std::uint64_t>, LiveHandleHash> keys_by_handle_;
    std::unordered_map<std::uint64_t, std::map<std::size_t, Handle>> origins_, active_origins_;
    std::unordered_set<std::uint64_t> built_origin_rosters_;
    std::unordered_map<Handle, std::set<std::pair<std::uint64_t, std::size_t>>, LiveHandleHash> origin_incidence_;
    std::unordered_map<Handle, Record, LiveHandleHash> records_;
    Ordered ordered_members_;
    std::size_t occurrences_ = 0;
    std::unordered_map<std::uint64_t,
        std::unordered_set<Handle, LiveHandleHash>> incarnations_;
    std::array<std::map<LiveKey, Bucket>, kClasses> buckets_;
};

// The adapter keeps its original predicate at the gate. ADL selects this
// overload when the index and its scalar query are supplied after it.
template<class Handles, class Placements, class Row, class Indexed>
bool any_live_row(const Handles& live, const Placements& placement, Row&& row,
                  const LiveRowIndex& index, Indexed&& indexed) {
    return index.query_or_scan("adapter live-row predicate", indexed, [&](bool) {
        for (const auto& handle : live) {
            const auto found = placement.find(handle.incarnation);
            if (found != placement.end() && row(found->second)) return true;
        }
        return false;
    });
}

} // namespace detail
} // namespace pineforge::source
