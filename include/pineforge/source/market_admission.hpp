#pragma once
// The TradingView market-admission observation journal: every field of
// `Configuration` is a strategy() declaration parameter and every event is a
// TradingView admission review, so this is source-layer state (R5 lane N14:
// moved from include/pineforge/market_admission.hpp, where no kernel
// translation unit consumed it). It is hashed through the source host's
// extension fold (src/source/pine_state_hash.cpp), never by the kernel.
#include <pineforge/order_birth.hpp>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace pineforge::admission {
inline namespace market_admission_v2 {
constexpr double absent = std::numeric_limits<double>::quiet_NaN();
enum class CommandKind : int64_t { Entry, Raw, Cancel, CancelAll };
enum class Outcome : int64_t {
    Admitted, NoAdmission, IgnoredTradingWindow, IgnoredIntradayLoss,
    RejectedIntradayCap, RejectedFrozenMarketCap, RejectedAffordability,
    RejectedPricedCap, OpeningRejectedReductionAdmitted, CancelCompleted
};
enum class Checkpoint : int64_t { DefaultGross, ExplicitPair, TerminalGross };

// Immutable observed configuration, not selectable compatibility profiles.
struct Configuration {
    bool process_on_close = false;
    bool calc_on_fills = false;
    bool magnifier = false;
    bool fill_recalculation = false;
    bool scheduler = false;
    int slippage = 0;
    int pyramiding = 0;
    int default_quantity_type = 0;
    double default_quantity_value = 0;
    double long_margin = 0;
    double short_margin = 0;
    double commission_value = 0;
    int commission_type = 0;
    double pointvalue = 1;
    double fx = 1;
    double quantity_step = 0;
    double mintick = 0;
    int risk_direction = 0;
    int loss_days_limit = 0;
    double drawdown_limit = 0;
    double intraday_loss_limit = 0;
    double position_limit = 0;
    bool fill_cap_active = false;
    bool risk_halted = false;
};
struct PriceRequest {
    double limit = absent;
    double stop = absent;
};
struct CurrentPrices {
    double limit = absent;
    double stop = absent;
    double trail_points = absent;
    double trail_price = absent;
    double trail_offset = absent;
};
struct SizingObservation {
    double quantity = absent;
    double equity = absent;
    double price = absent;
    double mark = absent;
    double fx = absent;
};
struct CommandObservation {
    uint64_t command = 0;
    CommandKind kind = CommandKind::Entry;
    OrderBirth birth;
    std::string id;
    double requested_quantity = absent;
    int quantity_type = -1;
    bool buy = false;
    PriceRequest prices;
    std::string oca_name;
    int oca_type = 0;
    Configuration configuration;
    int bar = -1;
    int placement_side = 0;
    int64_t placement_cycle = 0;
    double prior_close_quantity = 0;
    double held_quantity = 0;
    int held_entries = 0;
    double realized_equity = 0;
    double placement_equity = 0;
    double signal_close = absent;
    double quantized_fixed_quantity = absent;
    std::optional<SizingObservation> original_sizing;
    double explicit_equity = absent;
    double explicit_price = absent;
};
struct ReviewReceipt {
    uint64_t sequence = 0;
    Checkpoint checkpoint = Checkpoint::DefaultGross;
    int bar = -1;
    // Zero only for the batch review; per-order receipts name their origin.
    uint64_t target_command = 0;
};
struct SizingRevision {
    uint64_t sequence = 0;
    uint64_t cause_fill = 0;
    int bar = -1;
    uint64_t target_command = 0;
};

class Draft {
public:
    void bind(std::shared_ptr<const CommandObservation> observation);
    void reviewed(ReviewReceipt receipt);
    void sizing_revised(SizingRevision receipt);
    const std::shared_ptr<const CommandObservation>& observation() const { return observation_; }
    const std::optional<ReviewReceipt>& review() const { return review_; }
    const std::optional<SizingRevision>& sizing_revision() const { return sizing_revision_; }
private:
    std::shared_ptr<const CommandObservation> observation_;
    std::optional<ReviewReceipt> review_;
    std::optional<SizingRevision> sizing_revision_;
};
struct BookObservation {
    uint64_t incarnation = 0;
    int64_t priority = 0;
    int bar = -1;
    int type = 0;
    int placement_side = 0;
    // Raw requested direction captured with the physical book row. This is
    // needed to reconstruct a flat-born MARKET peer; placement_side is the
    // held position side and is FLAT for that peer.
    bool buy = false;
    std::string id;
    std::string oca_name;
    int oca_type = 0;
    OrderBirth birth;
    CurrentPrices prices;
    Draft draft;
};
struct CommandEvent {
    std::shared_ptr<const CommandObservation> observation;
    Outcome outcome = Outcome::NoAdmission;
    uint64_t admitted_incarnation = 0;
    std::vector<BookObservation> before;
    std::vector<uint64_t> removed;
};
// Actual resolution output kinds. No selector/member bit is retained.
enum class ResolutionKind : int64_t { Original, Rejected, PairedTransaction };
struct InstructionResolution {
    uint64_t incarnation = 0;
    ResolutionKind kind = ResolutionKind::Original;
    uint64_t peer_incarnation = 0;
    int64_t own_priority = 0;
    int64_t peer_priority = 0;
    double transaction_quantity = absent;
};
struct ReviewEvent {
    ReviewReceipt receipt;
    Configuration configuration;
    double open_price = absent;
    int position_side = 0;
    int64_t position_cycle = 0;
    std::vector<BookObservation> book;
    std::vector<BookObservation> reviewed;
    std::vector<InstructionResolution> resolutions;
    std::vector<uint64_t> causes;
};
struct SizingEvent {
    SizingRevision receipt;
    uint64_t incarnation = 0;
    SizingObservation before;
    SizingObservation after;
    double affordability_equity_before = absent;
    double affordability_equity_after = absent;
};
using Event = std::variant<CommandEvent, ReviewEvent, SizingEvent>;
using FieldValue = std::variant<uint64_t, int64_t, double, std::string>;
struct Field { std::string path; FieldValue value; };
using FieldVisitor = std::function<void(const Field&)>;
void reflect(const Draft& value, const std::string& path, const FieldVisitor& visit);
void reflect(const Event& value, const std::string& path, const FieldVisitor& visit);

class Allocation;
class Journal {
public:
    Journal() = default;
    Journal(const Journal& other);
    Journal& operator=(const Journal& other);
    Journal(Journal&& other);
    Journal& operator=(Journal&& other);
    // Raw allocations must be appended or abandoned. Production producers use
    // reserve() so failed construction/unwinding abandons the unfinished event.
    uint64_t next_sequence();
    Allocation reserve();
    void abandon(uint64_t sequence) noexcept;
    void append(Event event);
    const std::vector<Event>& events() const { return events_; }
    uint64_t sequence_frontier() const { return next_sequence_; }
    void reset();
    // Keep actual causes still referenced by history, instructions or settlement.
    void retain(const std::vector<uint64_t>& sequences);
    void reflect(const std::string& path, const FieldVisitor& visit) const;
private:
    friend class Allocation;
    void release_allocation(uint64_t sequence) noexcept;
    uint64_t next_sequence_ = 1;
    uint64_t active_allocations_ = 0;
    std::vector<uint64_t> outstanding_sequences_;
    std::vector<Event> events_;
};
// Ephemeral ownership of an unfinished event. Completed/reclaimed IDs cannot
// be re-created by keeping this handle: append consumes the journal allocation.
class Allocation {
public:
    Allocation(const Allocation&) = delete;
    Allocation& operator=(const Allocation&) = delete;
    Allocation(Allocation&& other) noexcept;
    Allocation& operator=(Allocation&&) = delete;
    ~Allocation() noexcept;
    uint64_t sequence() const { return sequence_; }
private:
    friend class Journal;
    Allocation(Journal& journal, uint64_t sequence) noexcept
        : journal_(&journal), sequence_(sequence) {}
    Journal* journal_;
    uint64_t sequence_;
};
// Ephemeral call frame; only its completed immutable record enters the journal.
class CommandCapture {
public:
    CommandCapture(Allocation allocation, CommandObservation input, std::vector<BookObservation> before,
                   std::function<void(CommandEvent)> complete);
    CommandCapture(const CommandCapture&) = delete;
    CommandCapture& operator=(const CommandCapture&) = delete;
    ~CommandCapture() noexcept(false);
    const CommandObservation& input() const { return input_; }
    void outcome(Outcome outcome) { outcome_ = outcome; }
    void bind(Draft& draft, std::optional<SizingObservation> sizing,
              double explicit_equity, double explicit_price);
private:
    Allocation allocation_;
    CommandObservation input_;
    std::vector<BookObservation> before_;
    Outcome outcome_ = Outcome::NoAdmission;
    std::function<void(CommandEvent)> complete_;
};
class ReviewCapture {
public:
    ReviewCapture(Allocation allocation, ReviewEvent event,
                  std::function<void(ReviewEvent)> complete);
    ReviewCapture(const ReviewCapture&) = delete;
    ReviewCapture& operator=(const ReviewCapture&) = delete;
    ~ReviewCapture() noexcept(false);
    ReviewReceipt receipt_for(const Draft& draft) const;
    void reject(uint64_t incarnation) {
        InstructionResolution result;result.incarnation=incarnation;result.kind=ResolutionKind::Rejected;
        event_.resolutions.push_back(result);
    }
    InstructionResolution transaction(uint64_t incarnation,uint64_t peer,int64_t priority,
                                      int64_t peer_priority,double quantity) {
        InstructionResolution result{incarnation,ResolutionKind::PairedTransaction,peer,priority,peer_priority,quantity};
        event_.resolutions.push_back(result);return result;
    }
private:
    Allocation allocation_;
    ReviewEvent event_;
    std::function<void(ReviewEvent)> complete_;
};
std::vector<Field> fields(const Draft& draft);
uint64_t read_unsigned(const std::vector<Field>& fields, const std::string& path);
int64_t read_integer(const std::vector<Field>& fields, const std::string& path);
double read_double(const std::vector<Field>& fields, const std::string& path);
std::string read_string(const std::vector<Field>& fields, const std::string& path);
uint64_t sequence(const Event& event);
} // inline namespace market_admission_v2
} // namespace pineforge::admission
namespace pineforge { using MarketAdmissionDraft = admission::Draft; using MarketAdmissionJournal = admission::Journal; }
