#include "admission_journal_codec.hpp"
#include <pineforge/detail/journal_codec.hpp>
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
        if (value.size() > bytes.size() - length) throw std::length_error("admission field path");
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
    bool good() const { return out.status == Status::ok; }
    template<class T> void field(const Path& path, const char* name, T& value) {
        ++rows;
        if (counting || !good()) return;
        const auto key = path.child(name);
        out.word(key.length); out.bytes(key.bytes.data(), key.length);
        out.word(tag<T>());
        if constexpr (std::is_same_v<T, std::string>) out.text(value);
        else if constexpr (std::is_same_v<T, double>) {
            std::uint64_t bits = 0; std::memcpy(&bits, &value, sizeof bits); out.word(bits);
        } else out.word(static_cast<std::uint64_t>(value));
    }
};

template<bool Owning> struct Load {
    static constexpr bool loading = true;
    static constexpr bool owning = Owning;
    jc::Reader& in;
    std::uint64_t rows = 0, expected_rows = 0;
    JournalRecordSummary summary;
    Outcome outcome = Outcome::NoAdmission;
    int bar = -1;
    bool good() const { return in.status == Status::ok; }
    template<class T> void field(const Path& path, const char* name, T& value) {
        if (!good()) return;
        if (++rows > expected_rows) { in.status = Status::corrupt; return; }
        const auto key = path.child(name);
        const auto length = in.word();
        if (!good()) return;
        if (length != key.length) { in.status = Status::corrupt; return; }
        std::array<char, 256> bytes{};
        in.bytes(bytes.data(), key.length);
        if (!good()) return;
        if (std::memcmp(bytes.data(), key.bytes.data(), key.length) || in.word() != tag<T>()) {
            in.status = Status::corrupt; return;
        }
        if constexpr (std::is_same_v<T, std::string>) in.text(value, Owning);
        else if constexpr (std::is_same_v<T, double>) in.real(value);
        else if constexpr (std::is_same_v<T, bool>) {
            const auto raw = in.word();
            if (raw > 1) in.status = Status::corrupt;
            value = raw == 1;
        } else if constexpr (std::is_enum_v<T>) {
            std::underlying_type_t<T> raw{}; in.integer(raw); value = static_cast<T>(raw);
        } else in.integer(value);
        if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
            if (key.is("event.observation.command") || key.is("event.receipt.sequence"))
                summary.sequence = static_cast<std::uint64_t>(value);
            if (key.is("event.outcome")) outcome = static_cast<Outcome>(value);
            if (key.is("event.observation.bar")) bar = static_cast<int>(value);
        }
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
        if constexpr (A::loading && A::owning) {
            if (!a.good()) return;
            switch (cause) {
            case OrderBirthCause::Unattributed: value = OrderBirth{}; break;
            case OrderBirthCause::DirectCommand: value = OrderBirth::direct_command(bar,timestamp); break;
            case OrderBirthCause::ChartEvaluation: value = OrderBirth::chart_evaluation(bar,timestamp); break;
            case OrderBirthCause::FillEvaluation: {
                const auto cursor = position == BirthCursorPosition::Point
                    ? BirthCursor::point(domain,index,count) : BirthCursor::segment(domain,index,count);
                value = OrderBirth::fill_evaluation(bar,timestamp,cursor,price,first,last,ordinal);
                break;
            }
            default: throw std::invalid_argument("invalid admission birth");
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
        for (std::uint64_t i = 0; i != count && a.good(); ++i) {
            if constexpr (A::loading && !A::owning) { T row{}; emit(p.element(i),row); }
            else emit(p.element(i),rows[static_cast<std::size_t>(i)]);
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

bool rejection(Outcome outcome) {
    switch (outcome) {
    case Outcome::RejectedIntradayCap: case Outcome::RejectedFrozenMarketCap:
    case Outcome::RejectedAffordability: case Outcome::RejectedPricedCap:
    case Outcome::OpeningRejectedReductionAdmitted: return true;
    default: return false;
    }
}
template<bool Owning> Status decode(const detail::ScratchByteSource& source,
    detail::ScratchBuffer page, Event& event, JournalRecordSummary& summary) noexcept {
    if (!page.data || !page.size) return Status::programming_error;
    try {
        jc::Reader in{source,page};
        Load<Owning> load{in};
        load.expected_rows = in.word();
        if (load.expected_rows > in.remaining() / 24) return Status::corrupt;
        Schema<Load<Owning>>{load}.event(event);
        if (load.rows != load.expected_rows && in.status == Status::ok) in.status = Status::corrupt;
        const auto status = in.finish();
        if (status == Status::ok) {
            summary = load.summary;
            summary.rejected = rejection(load.outcome);
            summary.rejected_bar = load.bar;
        }
        return status;
    } catch (const std::bad_alloc&) { return Status::programming_error; }
    catch (...) { return Status::corrupt; }
}
} // namespace

JournalRecordSource::JournalRecordSource(const Event& event) noexcept : event_(&event) {
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
        status_ = out.status;
    } catch (...) { status_ = Status::programming_error; }
}
detail::ScratchStatus JournalRecordSource::read(std::uint64_t offset,
                                               detail::ScratchBuffer buffer) const noexcept {
    if (status_ != Status::ok) return status_;
    if (offset > length_ || buffer.size > length_ - offset || (buffer.size && !buffer.data))
        return Status::programming_error;
    try {
        jc::Writer out{0,offset,buffer};
        out.word(rows_);
        Save save{out};
        Schema<Save>{save}.event(const_cast<Event&>(*event_));
        return out.status;
    } catch (...) { return Status::programming_error; }
}
JournalRecordSource encode_journal_record(const Event& event) noexcept { return JournalRecordSource(event); }
detail::ScratchStatus decode_journal_record(const detail::ScratchByteSource& source,
    detail::ScratchBuffer page, Event& event) noexcept {
    Event result;
    JournalRecordSummary summary;
    const auto status = decode<true>(source,page,result,summary);
    if (status == Status::ok) event = std::move(result);
    return status;
}
detail::ScratchStatus scan_journal_record(const detail::ScratchByteSource& source,
    detail::ScratchBuffer page, JournalRecordSummary& summary) noexcept {
    Event scratch;
    return decode<false>(source,page,scratch,summary);
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
