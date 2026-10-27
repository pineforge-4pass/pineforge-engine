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
    PureStopEntry, PureBuyLimitOpening, TouchedStopEntry, DropLimit, DropStop, Count
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

// Run-owned, derived directory. The adapter's consumer cache owns it; binding
// never publishes a second executable order or changes canonical row values.
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
    std::vector<Handle> reservation_rows(const std::string& exact_from,
                                         std::int64_t current_cycle) const;
    std::size_t reservation_count(const std::string& exact_from, std::int64_t current_cycle) const;
    static const char* class_name(LiveClass);
    static bool oracle_enabled() noexcept;
    // Assertion builds only: independent whole-book comparison, never repair.
    void verify() const;
private:
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
    std::unordered_map<std::uint64_t, Handle> source_owners_;
    std::unordered_map<Handle, std::unordered_set<std::uint64_t>, LiveHandleHash> keys_by_handle_;
    std::unordered_map<std::uint64_t, std::map<std::size_t, Handle>> origins_, active_origins_;
    std::unordered_map<Handle, std::set<std::pair<std::uint64_t, std::size_t>>, LiveHandleHash> origin_incidence_;
    std::unordered_map<Handle, Record, LiveHandleHash> records_;
    Ordered ordered_members_;
    std::size_t occurrences_ = 0;
    std::unordered_map<std::uint64_t,
        std::unordered_set<Handle, LiveHandleHash>> incarnations_;
    std::array<std::map<LiveKey, Bucket>, kClasses> buckets_;
};

} // namespace detail
} // namespace pineforge::source
