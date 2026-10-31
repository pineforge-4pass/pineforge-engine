#include "admission_journal_codec.hpp"
#include <pineforge/detail/journal_codec.hpp>
#include <pineforge/run_failure.hpp>
#include <string_view>
#include <new>

namespace pineforge::admission {
inline namespace market_admission_v2 {
namespace {
namespace jc = detail::journal_codec;
using Status = detail::ScratchStatus;

// Path depth is fixed by the schema. Decimal array subscripts have at most
// twenty digits; neither text values nor array populations enter this buffer.
struct Path {
    std::array<char, 256> bytes{};
    std::size_t length = 0;
    Path() { append("event"); }
    void append(std::string_view value) {
        if (value.size() > bytes.size() - length)
            throw coded<std::length_error>(RunFailureCode::engine_invariant, {},
                                           "admission field path");
        std::memcpy(bytes.data() + length, value.data(), value.size());
        length += value.size();
    }
    Path child(const char* name) const {
        Path out = *this;
        out.append("."); out.append(name);
        return out;
    }
    Path element(std::uint64_t index) const {
        Path out = *this;
        out.append("[");
        std::array<char, 20> digits{};
        std::size_t count = 0;
        do { digits[count++] = char('0' + index % 10); index /= 10; } while (index);
        while (count) out.append(std::string_view(&digits[--count], 1));
        out.append("]");
        return out;
    }
    bool is(std::string_view value) const noexcept {
        return value == std::string_view(bytes.data(), length);
    }
};
template<class T> constexpr std::uint64_t tag() {
    if constexpr (std::is_same_v<T, std::string>) return 3;
    else if constexpr (std::is_same_v<T, double>) return 2;
    else if constexpr (std::is_same_v<T, bool> || std::is_unsigned_v<T>) return 0;
    else return 1;
}

struct Save {
    static constexpr bool loading = false;
    static constexpr bool owning = false;
    jc::Writer& out;
    bool counting = false;
    std::uint64_t rows = 0;
    std::uint64_t* next_offset = nullptr;
    std::uint64_t* next_row = nullptr;
    std::array<std::uint64_t, 4>* array_elements = nullptr;
    std::array<std::uint64_t, 4>* array_rows = nullptr;
    std::size_t arrays = 0;
    bool stopped = false;
    bool good() const { return out.checked_status() == Status::ok && !stopped; }
    template<class T> void field(const Path& path, const char* name, T& value) {
        if (!good()) return;
        const auto row = rows++;
        if (counting || (next_row && row < *next_row)) return;
        if (next_offset && out.position >= out.offset + out.destination.size) {
            stopped = true;
            return;
        }
        const auto key = path.child(name);
        out.word(key.length); out.bytes(key.bytes.data(), key.length);
        out.word(tag<T>());
        if constexpr (std::is_same_v<T, std::string>) out.text(value);
        else if constexpr (std::is_same_v<T, double>) {
            std::uint64_t bits = 0; std::memcpy(&bits, &value, sizeof bits); out.word(bits);
        } else out.word(static_cast<std::uint64_t>(value));
        if (next_offset && out.checked_status() == Status::ok) {
            if (out.position <= out.offset + out.destination.size) {
                *next_offset = out.position;
                *next_row = rows;
            } else stopped = true; // Retry only this field at the next page.
        }
    }
    std::uint64_t begin_array(std::size_t slot) {
        if (!array_elements || !good()) return 0;
        // Restore the field ordinal at the first unfinished element without
        // traversing any completed element, including its optional fields.
        rows = std::max(rows, (*array_rows)[slot]);
        return (*array_elements)[slot];
    }
    void end_element(std::size_t slot, std::uint64_t next) {
        if (!array_elements || !good()) return;
        (*array_elements)[slot] = next;
        (*array_rows)[slot] = rows;
    }
};

class FoldSource final : public detail::ScratchByteSource {
    const detail::ScratchByteSource& source_;
public:
    mutable Status failure = Status::ok;
    mutable std::optional<detail::ReportScratchIoError> carried;
    explicit FoldSource(const detail::ScratchByteSource& source) : source_(source) {}
    std::uint64_t size() const noexcept override { return source_.size(); }
    Status read(std::uint64_t offset, detail::ScratchBuffer out) const noexcept override {
        const auto status = source_.read(offset,out);
        if (status != Status::ok && failure == Status::ok) {
            failure = status;
            if (status == Status::io_error)
                if (const auto* error = source_.carried_failure()) carried = *error;
        }
        return status;
    }
};
struct CanonicalFold {
    std::uint64_t digest;
    std::uint64_t after_sequence;
    bool active = true;
    bool validation_skipped = false;
    void bytes(const char* data, std::size_t size) noexcept {
        for (std::size_t i = 0; i != size; ++i)
            digest = (digest ^ static_cast<unsigned char>(data[i])) * 1099511628211ULL;
    }
    void word(std::uint64_t value) noexcept {
        for (unsigned i = 0; i != 8; ++i)
            digest = (digest ^ ((value >> (8 * i)) & 255)) * 1099511628211ULL;
    }
    template<class T> void scalar(const T& value) noexcept {
        if constexpr (std::is_same_v<T, double>) {
            std::uint64_t bits = 0;
            std::memcpy(&bits, &value, sizeof bits);
            if ((bits & 0x7fffffffffffffffULL) == 0) bits = 0;
            else if ((bits & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL
                     && (bits & 0x000fffffffffffffULL)) bits = 0x7ff8000000000000ULL;
            word(bits);
        } else word(static_cast<std::uint64_t>(value));
    }
};
constexpr std::size_t kJournalFoldWorkspaceLimit = 1024;
constexpr std::size_t kJournalFoldFieldBytes = 256;
// Fixed candidate/read-failure state, two diagnostic transport slots, two
// summaries and eight words for scalar/pointer controls, plus the existing
// comparison buffer reused for text. Nothing is retained between calls.
// ReportScratchIoError copies share their prepared message/argument payload.
// The Reader page and non-owning schema frame remain the existing workspace.
constexpr std::size_t kJournalFoldWorkspaceBytes = sizeof(CanonicalFold) + sizeof(FoldSource)
    + 2 * sizeof(std::optional<detail::ReportScratchIoError>) + 2 * sizeof(JournalRecordSummary)
    + 8 * sizeof(std::uint64_t) + kJournalFoldFieldBytes;
static_assert(kJournalFoldWorkspaceBytes <= kJournalFoldWorkspaceLimit,
              "fixed admission fold workspace");
static_assert(std::is_nothrow_copy_constructible_v<detail::ReportScratchIoError>
              && std::is_nothrow_copy_assignable_v<detail::ReportScratchIoError>,
              "diagnostic transport must be nonthrowing");

template<bool Owning, bool Folding = false> struct Load {
    static constexpr bool loading = true;
    static constexpr bool owning = Owning;
    jc::Reader& in;
    std::uint64_t rows = 0, expected_rows = 0;
    JournalRecordSummary summary;
    Outcome outcome = Outcome::NoAdmission;
    int bar = -1;
    CanonicalFold* fold = nullptr;
    bool good() const { return in.status == Status::ok; }
    template<class T> void field(const Path& path, const char* name, T& value) {
        if (!good()) return;
        if (++rows > expected_rows) { in.status = Status::corrupt; return; }
        const auto key = path.child(name);
        const auto length = in.word();
        if (!good()) return;
        if (length != key.length) { in.status = Status::corrupt; return; }
        std::array<char, kJournalFoldFieldBytes> bytes{};
        in.bytes(bytes.data(), key.length);
        if (!good()) return;
        if (std::memcmp(bytes.data(), key.bytes.data(), key.length) || in.word() != tag<T>()) {
            in.status = Status::corrupt; return;
        }
        if constexpr (Folding) {
            if (fold->active) {
                fold->word(length); fold->bytes(bytes.data(), key.length); fold->word(tag<T>());
            }
        }
        if constexpr (std::is_same_v<T, std::string>) {
            if constexpr (!Folding) in.text(value, Owning);
            else {
                const auto size = in.word();
                if (!good()) return;
                if (size > in.remaining() || size > value.max_size()) {
                    in.status = Status::corrupt; return;
                }
                if (!fold->active) in.skip(size);
                else {
                    fold->word(size);
                    const auto end = in.position + size;
                    if (end > in.cached_at + in.cached_size) fold->validation_skipped = true;
                    while (in.position != end && good()) {
                        const auto count = static_cast<std::size_t>(
                            std::min<std::uint64_t>(bytes.size(), end - in.position));
                        in.bytes(bytes.data(), count);
                        if (good()) fold->bytes(bytes.data(), count);
                    }
                }
            }
        }
        else if constexpr (std::is_same_v<T, double>) in.real(value);
        else if constexpr (std::is_same_v<T, bool>) {
            const auto raw = in.word();
            if (raw > 1) in.status = Status::corrupt;
            value = raw == 1;
        } else if constexpr (std::is_enum_v<T>) {
            std::underlying_type_t<T> raw{}; in.integer(raw); value = static_cast<T>(raw);
        } else in.integer(value);
        if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
            if (key.is("event.observation.command") || key.is("event.receipt.sequence")) {
                summary.sequence = static_cast<std::uint64_t>(value);
                if constexpr (Folding)
                    if (summary.sequence <= fold->after_sequence) fold->active = false;
            }
            if (key.is("event.outcome")) outcome = static_cast<Outcome>(value);
            if (key.is("event.observation.bar")) bar = static_cast<int>(value);
        }
        if constexpr (Folding && !std::is_same_v<T, std::string>)
            if (good() && fold->active) fold->scalar(value);
    }
};

template<class A> struct Schema {
    A& a;
    template<class T> void field(const Path& p, const char* name, T& value) {
        a.field(p, name, value);
    }
    void birth(const Path& p, OrderBirth& value) {
        auto cause = value.cause(); auto bar = value.bar(); auto timestamp = value.timestamp();
        auto domain = value.cursor().domain(); auto position = value.cursor().position();
        auto index = value.cursor().index(); auto count = value.cursor().count();
        auto price = value.cursor_price();
        auto first = value.first_fill(); auto last = value.last_fill();
        auto ordinal = value.evaluation_ordinal();
        field(p,"cause",cause); field(p,"bar",bar); field(p,"timestamp",timestamp);
        field(p,"cursor_domain",domain); field(p,"cursor_position",position);
        field(p,"cursor_index",index); field(p,"cursor_count",count);
        field(p,"cursor_price",price); field(p,"first_fill",first);
        field(p,"last_fill",last); field(p,"evaluation_ordinal",ordinal);
        if constexpr (A::loading) {
            if constexpr (A::owning) {
                if (!a.good()) return;
            }
            switch (cause) {
            case OrderBirthCause::Unattributed:
                if constexpr (A::owning) value = OrderBirth{};
                break;
            case OrderBirthCause::DirectCommand:
                if constexpr (A::owning) value = OrderBirth::direct_command(bar,timestamp);
                break;
            case OrderBirthCause::ChartEvaluation:
                if constexpr (A::owning) value = OrderBirth::chart_evaluation(bar,timestamp);
                break;
            case OrderBirthCause::FillEvaluation: {
                if constexpr (A::owning) {
                    const auto cursor = position == BirthCursorPosition::Point
                        ? BirthCursor::point(domain,index,count) : BirthCursor::segment(domain,index,count);
                    value = OrderBirth::fill_evaluation(bar,timestamp,cursor,price,first,last,ordinal);
                }
                break;
            }
            default:
                if constexpr (A::owning)
                    throw coded<std::invalid_argument>(RunFailureCode::engine_invariant, {},
                                                       "invalid admission birth");
                else a.in.status = Status::corrupt;
            }
        }
    }
    void config(const Path& p, Configuration& x) {
#define F(name) field(p,#name,x.name)
        F(process_on_close); F(calc_on_fills); F(magnifier); F(fill_recalculation); F(scheduler);
        F(slippage); F(pyramiding); F(default_quantity_type); F(default_quantity_value);
        F(long_margin); F(short_margin); F(commission_value); F(commission_type); F(pointvalue);
        F(fx); F(quantity_step); F(mintick); F(risk_direction); F(loss_days_limit);
        F(drawdown_limit); F(intraday_loss_limit); F(position_limit); F(fill_cap_active); F(risk_halted);
#undef F
    }
    void sizing(const Path& p, SizingObservation& x) {
        field(p,"quantity",x.quantity); field(p,"equity",x.equity); field(p,"price",x.price);
        field(p,"mark",x.mark); field(p,"fx",x.fx);
    }
    void observation(const Path& p, CommandObservation& x) {
#define F(name) field(p,#name,x.name)
        F(command); F(kind); birth(p.child("birth"),x.birth); F(id); F(requested_quantity);
        F(quantity_type); F(buy); field(p.child("prices"),"limit",x.prices.limit);
        field(p.child("prices"),"stop",x.prices.stop); F(oca_name); F(oca_type);
        config(p.child("configuration"),x.configuration); F(bar); F(placement_side);
        F(placement_cycle); F(prior_close_quantity); F(held_quantity); F(held_entries);
        F(realized_equity); F(placement_equity); F(signal_close); F(quantized_fixed_quantity);
        bool present = x.original_sizing.has_value();
        field(p,"original_sizing_present",present);
        if constexpr (A::loading) {
            if (present) x.original_sizing.emplace(); else x.original_sizing.reset();
        }
        if (present) sizing(p.child("original_sizing"),*x.original_sizing);
        F(explicit_equity); F(explicit_price);
#undef F
    }
    void receipt(const Path& p, ReviewReceipt& x) {
        field(p,"sequence",x.sequence); field(p,"checkpoint",x.checkpoint);
        field(p,"bar",x.bar); field(p,"target_command",x.target_command);
    }
    void receipt(const Path& p, SizingRevision& x) {
        field(p,"sequence",x.sequence); field(p,"cause_fill",x.cause_fill);
        field(p,"bar",x.bar); field(p,"target_command",x.target_command);
    }
    void draft(const Path& p, Draft& x) {
        bool present = bool(x.observation());
        field(p,"observation_present",present);
        if (present) {
            if constexpr (A::loading) {
                CommandObservation value;
                observation(p.child("observation"),value);
                if constexpr (A::owning)
                    if (a.good()) x.bind(std::make_shared<const CommandObservation>(std::move(value)));
            } else observation(p.child("observation"),const_cast<CommandObservation&>(*x.observation()));
        }
        present = x.review().has_value(); field(p,"review_present",present);
        if (present) {
            ReviewReceipt value;
            if constexpr (!A::loading) value = *x.review();
            receipt(p.child("review"),value);
            if constexpr (A::loading && A::owning) if (a.good()) x.reviewed(value);
        }
        present = x.sizing_revision().has_value(); field(p,"sizing_revision_present",present);
        if (present) {
            SizingRevision value;
            if constexpr (!A::loading) value = *x.sizing_revision();
            receipt(p.child("sizing_revision"),value);
            if constexpr (A::loading && A::owning) if (a.good()) x.sizing_revised(value);
        }
    }
    void book(const Path& p, BookObservation& x) {
#define F(name) field(p,#name,x.name)
        F(incarnation); F(priority); F(bar); F(type); F(placement_side); F(buy);
        F(id); F(oca_name); F(oca_type); birth(p.child("birth"),x.birth);
        field(p.child("prices"),"limit",x.prices.limit); field(p.child("prices"),"stop",x.prices.stop);
        field(p.child("prices"),"trail_points",x.prices.trail_points);
        field(p.child("prices"),"trail_price",x.prices.trail_price);
        field(p.child("prices"),"trail_offset",x.prices.trail_offset); draft(p.child("draft"),x.draft);
#undef F
    }
    template<class T, class Fn> void array(const Path& p, std::vector<T>& rows, Fn emit) {
        auto count = static_cast<std::uint64_t>(rows.size());
        field(p,"size",count);
        if constexpr (A::loading) {
            if (count > a.expected_rows - std::min(a.rows,a.expected_rows)
                || count > rows.max_size()) { a.in.status = Status::corrupt; return; }
            if constexpr (A::owning) rows.resize(static_cast<std::size_t>(count));
        }
        std::uint64_t first = 0;
        std::size_t slot = 0;
        if constexpr (!A::loading) {
            slot = a.arrays++;
            first = a.begin_array(slot);
        }
        for (std::uint64_t i = first; i != count && a.good(); ++i) {
            if constexpr (A::loading && !A::owning) { T row{}; emit(p.element(i),row); }
            else emit(p.element(i),rows[static_cast<std::size_t>(i)]);
            if constexpr (!A::loading) a.end_element(slot,i+1);
        }
    }
    void event(Event& value) {
        const Path p;
        auto kind = static_cast<std::uint64_t>(value.index()); field(p,"kind",kind);
        if constexpr (A::loading) {
            switch (kind) {
            case 0: value.template emplace<0>(); break;
            case 1: value.template emplace<1>(); break;
            case 2: value.template emplace<2>(); break;
            default: a.in.status = Status::corrupt; return;
            }
        }
        std::visit([&](auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T,CommandEvent>) {
                if constexpr (A::loading) {
                    CommandObservation input;
                    observation(p.child("observation"),input);
                    if constexpr (A::owning)
                        if (a.good()) x.observation =
                            std::make_shared<const CommandObservation>(std::move(input));
                } else {
                    if (!x.observation) { a.out.status = Status::programming_error; return; }
                    observation(p.child("observation"),const_cast<CommandObservation&>(*x.observation));
                }
                field(p,"outcome",x.outcome); field(p,"admitted_incarnation",x.admitted_incarnation);
                array(p.child("before"),x.before,[&](const auto& q,auto& row){book(q,row);});
                array(p.child("removed"),x.removed,[&](const auto& q,auto& id){field(q,"incarnation",id);});
            } else if constexpr (std::is_same_v<T,ReviewEvent>) {
                receipt(p.child("receipt"),x.receipt); config(p.child("configuration"),x.configuration);
                field(p,"open_price",x.open_price); field(p,"position_side",x.position_side);
                field(p,"position_cycle",x.position_cycle);
                array(p.child("book"),x.book,[&](const auto& q,auto& row){book(q,row);});
                array(p.child("reviewed"),x.reviewed,[&](const auto& q,auto& row){book(q,row);});
                array(p.child("resolutions"),x.resolutions,[&](const auto& q,auto& row){
                    field(q,"incarnation",row.incarnation); field(q,"kind",row.kind);
                    field(q,"peer_incarnation",row.peer_incarnation); field(q,"own_priority",row.own_priority);
                    field(q,"peer_priority",row.peer_priority); field(q,"transaction_quantity",row.transaction_quantity);
                });
                array(p.child("causes"),x.causes,[&](const auto& q,auto& id){field(q,"sequence",id);});
            } else {
                receipt(p.child("receipt"),x.receipt); field(p,"incarnation",x.incarnation);
                sizing(p.child("before"),x.before); sizing(p.child("after"),x.after);
                field(p,"affordability_equity_before",x.affordability_equity_before);
                field(p,"affordability_equity_after",x.affordability_equity_after);
            }
        },value);
    }
};

template<bool Owning, bool Folding = false> Status decode(const detail::ScratchByteSource& source,
    detail::ScratchBuffer page, Event& event, JournalRecordSummary& summary,
    const detail::ReportScratchIoError*& failure, CanonicalFold* fold = nullptr) noexcept {
    failure = nullptr;
    if (!page.data || !page.size) return Status::programming_error;
    try {
        jc::Reader in{source,page};
        Load<Owning,Folding> load{in};
        load.fold = fold;
        load.expected_rows = in.word();
        if (load.expected_rows > in.remaining() / 24) return Status::corrupt;
        Schema<Load<Owning,Folding>>{load}.event(event);
        if (load.rows != load.expected_rows && in.status == Status::ok) in.status = Status::corrupt;
        const auto status = in.finish();
        if (status == Status::ok) {
            summary = load.summary;
            summary.rejected = rejected_command(load.outcome);
            summary.rejected_bar = load.bar;
        }
        return status;
    } catch (const std::bad_alloc&) {
        if constexpr (Owning) failure = &detail::scratch_out_of_memory_failure();
        return Status::programming_error;
    }
    catch (...) { return Status::corrupt; }
}
} // namespace

JournalRecordSource::JournalRecordSource(const Event& event) noexcept : event_(&event) {
    jc::observe_work(jc::WorkOperation::payload_source);
    try {
        jc::Writer counter;
        Save count{counter,true};
        Schema<Save>{count}.event(const_cast<Event&>(event));
        rows_ = count.rows;
        jc::Writer out;
        out.word(rows_);
        Save save{out};
        Schema<Save>{save}.event(const_cast<Event&>(event));
        length_ = out.position;
        status_ = out.checked_status();
    } catch (...) { status_ = Status::programming_error; }
}
detail::ScratchStatus JournalRecordSource::read(std::uint64_t offset,
                                               detail::ScratchBuffer buffer) const noexcept {
    if (status_ != Status::ok) return status_;
    if (offset > length_ || buffer.size > length_ - offset || (buffer.size && !buffer.data))
        return Status::programming_error;
    if (!buffer.size) return Status::ok;
    jc::observe_work(jc::WorkOperation::payload_read, buffer.size);
    try {
        // Forward pages resume within the current array element. A seek before
        // the saved field boundary (including the second pass) starts anew.
        if (offset < next_offset_) {
            next_offset_ = next_row_ = 0;
            array_elements_.fill(0);
            array_rows_.fill(0);
        }
        jc::Writer out{next_offset_,offset,buffer};
        if (!next_offset_) out.word(rows_);
        Save save{out,false,0,&next_offset_,&next_row_,&array_elements_,&array_rows_};
        Schema<Save>{save}.event(const_cast<Event&>(*event_));
        return out.checked_status();
    } catch (...) { return Status::programming_error; }
}
JournalRecordSource encode_journal_record(const Event& event) noexcept { return JournalRecordSource(event); }
detail::ScratchStatus decode_journal_record(const detail::ScratchByteSource& source,
    detail::ScratchBuffer page, Event& event) {
    try {
        Event result;
        JournalRecordSummary summary;
        const detail::ReportScratchIoError* failure = nullptr;
        const auto status = decode<true>(source,page,result,summary,failure);
        detail::rethrow_carried_scratch_failure(failure);
        if (status == Status::ok) event = std::move(result);
        return status;
    } catch (const std::bad_alloc&) {
        detail::rethrow_carried_scratch_failure(&detail::scratch_out_of_memory_failure());
        return Status::programming_error; // unreachable: the helper always throws a non-null failure
    }
}
detail::ScratchStatus scan_journal_record(const detail::ScratchByteSource& source,
    detail::ScratchBuffer page, JournalRecordSummary& summary) noexcept {
    Event scratch;
    const detail::ReportScratchIoError* failure = nullptr;
    return decode<false>(source,page,scratch,summary,failure);
}
detail::ScratchStatus fold_validated_journal_record(const detail::ScratchByteSource& source,
    detail::ScratchBuffer page, JournalRecordSummary& summary, std::uint64_t& digest,
    std::uint64_t after_sequence, std::optional<detail::ReportScratchIoError>* failure) noexcept {
    if (failure) failure->reset();
    Event scratch;
    FoldSource input(source);
    CanonicalFold fold{digest,after_sequence};
    JournalRecordSummary candidate;
    const detail::ReportScratchIoError* unused = nullptr;
    const auto status = decode<false,true>(input,page,scratch,candidate,unused,&fold);
    if (status != Status::ok && fold.validation_skipped) {
        // A skipped text region changes the old scan's refill boundaries.
        // On any fused failure, replay that scan to retain its validation
        // priority: its next page can fail before the fused schema error.
        // Keep its tag-read error handling too. Never retry the hash or turn
        // the original failure into a successful query.
        JournalRecordSummary checked;
        const auto validation = scan_journal_record(source,page,checked);
        if (validation != Status::ok) {
            if (failure && validation == Status::io_error)
                if (const auto* error = source.carried_failure()) *failure = *error;
            return validation;
        }
        if (failure) *failure = input.carried;
        return input.failure != Status::ok ? input.failure : status;
    }
    if (failure && status == Status::io_error) *failure = input.carried;
    if (status == Status::ok) {
        summary = candidate;
        if (summary.sequence > after_sequence) digest = fold.digest;
    }
    return status;
}
detail::ScratchStatus fold_journal_record(const detail::ScratchByteSource& source,
    detail::ScratchBuffer page, std::uint64_t& digest) noexcept {
    if (!page.data || !page.size) return Status::programming_error;
    jc::Reader in{source,page};
    const auto rows = in.word();
    if (rows > in.remaining() / 24) return Status::corrupt;
    auto h = digest;
    const auto byte = [&](unsigned value) { h = (h ^ value) * 1099511628211ULL; };
    const auto word = [&](std::uint64_t value) {
        for (unsigned i = 0; i != 8; ++i) byte((value >> (8 * i)) & 255);
    };
    const auto text = [&] {
        const auto length = in.word();
        if (length > in.remaining()) { in.status = Status::corrupt; return; }
        word(length);
        std::array<std::byte, 97> bytes{};
        for (std::uint64_t at = 0; at < length && in.status == Status::ok;) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(),length-at));
            in.bytes(bytes.data(),count);
            for (std::size_t i = 0; i != count; ++i) byte(std::to_integer<unsigned>(bytes[i]));
            at += count;
        }
    };
    for (std::uint64_t i = 0; i != rows && in.status == Status::ok; ++i) {
        text();
        const auto kind = in.word(); word(kind);
        if (kind == 3) text();
        else if (kind < 3) {
            auto value = in.word();
            if (kind == 2) {
                if ((value & 0x7fffffffffffffffULL) == 0) value = 0;
                else if ((value & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL
                         && (value & 0x000fffffffffffffULL)) value = 0x7ff8000000000000ULL;
            }
            word(value);
        } else in.status = Status::corrupt;
    }
    const auto status = in.finish();
    if (status == Status::ok) digest = h;
    return status;
}
} // inline namespace market_admission_v2
} // namespace pineforge::admission
