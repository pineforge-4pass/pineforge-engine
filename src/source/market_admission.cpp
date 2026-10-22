#include <pineforge/source/market_admission.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/detail/journal_codec.hpp>
#include <pineforge/detail/scratch_errors.hpp>
#include "admission_journal_codec.hpp"
#include <algorithm>
#include <cerrno>
#include <exception>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace pineforge::admission {
inline namespace market_admission_v2 {
namespace {
bool same(const ReviewReceipt& a, const ReviewReceipt& b) {
    return a.sequence == b.sequence && a.checkpoint == b.checkpoint &&
        a.bar == b.bar && a.target_command == b.target_command;
}
bool same(const SizingRevision& a, const SizingRevision& b) {
    return a.sequence == b.sequence && a.cause_fill == b.cause_fill &&
        a.bar == b.bar && a.target_command == b.target_command;
}
bool known_checkpoint(Checkpoint checkpoint) {
    return checkpoint == Checkpoint::DefaultGross ||
        checkpoint == Checkpoint::ExplicitPair || checkpoint == Checkpoint::TerminalGross;
}
void require_origin(const std::shared_ptr<const CommandObservation>& origin,
                    uint64_t target, uint64_t event, int bar) {
    if (!origin || target != origin->command || event <= origin->command ||
        bar < origin->bar)
        throw coded<std::invalid_argument>(RunFailureCode::engine_invariant, {},
                                           "admission receipt requires its exact earlier command");
}
}
void Draft::bind(std::shared_ptr<const CommandObservation> observation) {
    if(observation_ || !observation || observation->command==0)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                      "market draft requires one original command observation");
    observation_=std::move(observation);
}
void Draft::reviewed(ReviewReceipt receipt) {
    require_origin(observation_, receipt.target_command, receipt.sequence, receipt.bar);
    if (!known_checkpoint(receipt.checkpoint))
        throw coded<std::invalid_argument>(RunFailureCode::engine_invariant, {},
                                           "review requires a known checkpoint");
    if (review_) {
        if (same(*review_, receipt)) return;
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                      "first admission review cannot be replaced");
    }
    review_ = receipt;
}
void Draft::sizing_revised(SizingRevision receipt) {
    require_origin(observation_, receipt.target_command, receipt.sequence, receipt.bar);
    if (receipt.cause_fill == 0)
        throw coded<std::invalid_argument>(RunFailureCode::engine_invariant, {},
                                           "sizing revision requires committed cause");
    if (sizing_revision_) {
        if (same(*sizing_revision_, receipt)) return;
        if (receipt.sequence <= sizing_revision_->sequence ||
            receipt.bar < sizing_revision_->bar || receipt.cause_fill < sizing_revision_->cause_fill)
            throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                "sizing revision cannot rewrite or precede its latest cause");
    }
    sizing_revision_ = receipt;
}
uint64_t sequence(const Event& event) {
    return std::visit([](const auto& value)->uint64_t {
        using T=std::decay_t<decltype(value)>;
        if constexpr(std::is_same_v<T,CommandEvent>) {
            if(!value.observation) {
                throw coded<std::invalid_argument>(RunFailureCode::engine_invariant, {},
                                                   "command event requires an observation");
            }
            return value.observation->command;
        }
        else return value.receipt.sequence;
    },event);
}
struct Journal::CodecState final : detail::ScratchReadback {
    detail::ScratchStore& store;
    mutable std::vector<std::byte> page;
    std::uint64_t count = 0;
    // One write-behind abandonment slot. It joins the next successful record
    // transaction, so abandoning a failed append cannot modify that append's
    // previously committed relation.
    std::uint64_t abandoned = 0;
    std::uint64_t failed_sequence = 0;
    mutable bool snapshot_valid = false;
    detail::ScratchStatus failure = detail::ScratchStatus::ok;

    explicit CodecState(detail::ScratchStore& source) : store(source), page(source.page_size()) {}
    static void check(detail::ScratchStatus status) {
        if (status == detail::ScratchStatus::ok) return;
        if (status == detail::ScratchStatus::io_error)
            detail::raise_scratch_io_error(EIO, "admission journal codec I/O");
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                      "admission journal codec invalid storage");
    }
    detail::ScratchBuffer buffer() const { return {page.data(),page.size()}; }
    void retire_through(std::uint64_t sequence) {
        auto status = store.begin();
        check(status);
        detail::ScratchHandle handle = detail::scratch_no_handle;
        while ((status = store.first(handle)) == detail::ScratchStatus::ok) {
            std::array<std::byte,8> key{};
            status = store.read(handle,detail::ScratchField::key,0,{key.data(),key.size()});
            if (status != detail::ScratchStatus::ok) break;
            std::uint64_t id = 0;
            for (unsigned i=0;i!=8;++i)
                id |= std::uint64_t(std::to_integer<unsigned>(key[i])) << (8*i);
            if (id > sequence) break; // trailing abandoned IDs remain consumed
            status = store.erase(handle);
            if (status != detail::ScratchStatus::ok) break;
        }
        if (status == detail::ScratchStatus::end) status = detail::ScratchStatus::ok;
        check(detail::journal_codec::finish(store,status));
    }
    std::size_t page_size() const noexcept override { return store.page_size(); }
    std::uint64_t record_count() const noexcept override { return count; }
    detail::ScratchStatus point(detail::ScratchHandle handle,
                                detail::ScratchRecordInfo& info) const noexcept override {
        detail::ScratchRecordInfo actual;
        const auto status = store.point(handle,actual);
        if (status != detail::ScratchStatus::ok) return status;
        if (!actual.value_bytes) return detail::ScratchStatus::not_found;
        info = actual;
        return status;
    }
    detail::ScratchStatus read(detail::ScratchHandle handle, detail::ScratchField field,
        std::uint64_t offset, detail::ScratchBuffer out) const noexcept override {
        detail::ScratchRecordInfo info;
        const auto status = point(handle,info);
        return status == detail::ScratchStatus::ok ? store.read(handle,field,offset,out) : status;
    }
    detail::ScratchStatus visit(detail::ScratchHandle handle, detail::ScratchBuffer buffer,
        detail::ScratchChunkVisitor visitor, void* context) const noexcept override {
        if (!buffer.data || buffer.size != page_size() || !visitor)
            return detail::ScratchStatus::programming_error;
        detail::ScratchRecordInfo info;
        const auto status = point(handle,info);
        return status == detail::ScratchStatus::ok ? store.visit(handle,buffer,visitor,context) : status;
    }
    detail::ScratchStatus for_each(detail::ScratchBuffer buffer,
        detail::ScratchRecordVisitor visitor, void* context) const noexcept override {
        if (!visitor) return detail::ScratchStatus::programming_error;
        struct Walk {
            detail::ScratchRecordVisitor visitor;
            void* context;
            static bool row(void* self,const detail::ScratchRecordView& value) noexcept {
                auto& walk = *static_cast<Walk*>(self);
                return !value.info.value_bytes || walk.visitor(walk.context,value);
            }
        } walk{visitor,context};
        return store.for_each(buffer,&Walk::row,&walk);
    }
};

Journal::Journal() noexcept = default;
Journal::~Journal() = default;

void bind_journal_codec(Journal& journal, detail::ScratchStore& store) {
    if (journal.next_sequence_ != 1 || journal.event_count() || journal.has_unfinished_events()
        || journal.codec_ || store.status() != detail::ScratchStatus::ok
        || store.record_count() || store.transaction_active())
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                      "journal codec requires an empty committed store");
    journal.codec_ = std::make_unique<Journal::CodecState>(store);
}

const detail::ScratchReadback& journal_readback(const Journal& journal) {
    if (!journal.codec_)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                      "journal codec is not bound");
    return *journal.codec_;
}

detail::ScratchStatus append_journal_record(Journal& journal,
    const detail::ScratchByteSource& source) noexcept {
    using Status = detail::ScratchStatus;
    namespace jc = detail::journal_codec;
    if (!journal.codec_) return Status::programming_error;
    auto& codec = *journal.codec_;
    if (codec.failure != Status::ok) return codec.failure;
    JournalRecordSummary summary;
    auto status = scan_journal_record(source,codec.buffer(),summary);
    if (status != Status::ok) return status;
    if (!summary.sequence || summary.sequence >= journal.next_sequence_
        || summary.sequence <= journal.retired_prefix_.last_sequence
        || summary.sequence == codec.abandoned)
        return Status::programming_error;
    detail::ScratchRecordInfo existing;
    status = jc::find(codec.store,summary.sequence,existing);
    if (status == Status::ok) return Status::programming_error; // completed or abandoned
    if (status != Status::not_found) return status;
    status = codec.store.begin();
    if (status != Status::ok) return status;
    detail::ScratchHandle handle = detail::scratch_no_handle;
    if (codec.abandoned && codec.abandoned > journal.retired_prefix_.last_sequence) {
        const jc::WordSource abandoned(codec.abandoned);
        const detail::ScratchSpanSource empty({nullptr,0});
        status = codec.store.put(abandoned,empty,handle);
    }
    if (status == Status::ok) {
        const jc::WordSource key(summary.sequence);
        status = codec.store.put(key,source,handle);
    }
    status = jc::finish(codec.store,status);
    if (status == Status::ok) {
        codec.abandoned = 0;
        codec.failed_sequence = 0;
        ++codec.count;
        --journal.outstanding_count_;
        codec.snapshot_valid = false;
        std::vector<Event>().swap(journal.events_);
        if (summary.sequence <= journal.folded_window_.last_sequence) journal.fold_valid_ = false;
        if (summary.rejected && journal.rejection_gate_.valid
            && summary.sequence > journal.rejection_gate_.sequence)
            journal.rejection_gate_ = {summary.sequence,summary.rejected_bar,true};
    } else codec.failed_sequence = summary.sequence;
    return status;
}

std::size_t Journal::event_count() const {
    return retired_prefix_.count + (codec_ ? static_cast<std::size_t>(codec_->count) : events_.size());
}

const std::vector<Event>& Journal::events() const {
    if (!codec_ || codec_->snapshot_valid) return events_;
    std::vector<Event> result;
    detail::ScratchHandle handle = detail::scratch_no_handle;
    auto status = codec_->store.first(handle);
    while (status == detail::ScratchStatus::ok) {
        detail::ScratchRecordInfo info;
        CodecState::check(codec_->store.point(handle,info));
        if (info.value_bytes) {
            detail::journal_codec::StoreSource source(codec_->store,handle,info.value_bytes);
            Event event;
            CodecState::check(decode_journal_record(source,codec_->buffer(),event));
            result.push_back(std::move(event));
        }
        status = codec_->store.next(handle,handle);
    }
    if (status != detail::ScratchStatus::end) CodecState::check(status);
    events_.swap(result);
    codec_->snapshot_valid = true;
    return events_;
}

Journal::Prefix Journal::folded_window() const {
    // A destructive filter supplies no prefix proof of its own. A validated
    // nonzero retired prefix may seed its surviving tail, but never a zero
    // restart or a rewrite of the command gate's last rejected bar.
    if (filtered_ && retired_prefix_.count == 0)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                      "admission filter cannot rewrite folded lifetime history");
    if (codec_) CodecState::check(codec_->failure);
    if (fold_valid_ && !filtered_ && folded_window_.count == event_count())
        return folded_window_;
    Prefix result = fold_valid_ && !filtered_ ? folded_window_ : retired_prefix_;
    auto gate = rejection_gate_.valid ? rejection_gate_
        : RejectionGate{retired_prefix_.last_sequence,retired_prefix_.last_rejected_command_bar,true};
    auto retained_rejected_bar = retired_prefix_.last_rejected_command_bar;
    const auto append = [&](const detail::ScratchByteSource& source, detail::ScratchBuffer page) {
        JournalRecordSummary summary;
        CodecState::check(scan_journal_record(source,page,summary));
        if (summary.sequence <= result.last_sequence) return;
        CodecState::check(fold_journal_record(source,page,result.digest));
        if (result.count == std::numeric_limits<std::size_t>::max())
            throw coded<std::overflow_error>(RunFailureCode::engine_invariant, {},
                                              "admission event count exhausted");
        ++result.count;
        result.last_sequence = summary.sequence;
        if (summary.rejected) {
            result.last_rejected_command_bar = summary.rejected_bar;
            if (summary.sequence > gate.sequence)
                gate = {summary.sequence,summary.rejected_bar,true};
            // Prove the old gate at its folded frontier independently of any
            // later appends; a new rejection cannot repair a discarded cause.
            if (summary.sequence <= folded_window_.last_sequence)
                retained_rejected_bar = summary.rejected_bar;
        }
    };
    if (codec_) {
        detail::ScratchHandle handle = detail::scratch_no_handle;
        // Inclusive seek permits one frontier key check, never a payload scan
        // of the prefix. Keys are the sequences validated at publication.
        const detail::journal_codec::WordSource frontier(result.last_sequence);
        auto status = codec_->store.lower_bound(frontier,handle);
        while (status == detail::ScratchStatus::ok) {
            detail::ScratchRecordInfo info;
            CodecState::check(codec_->store.point(handle,info));
            std::array<std::byte,8> key{};
            CodecState::check(codec_->store.read(handle,detail::ScratchField::key,0,
                                                 {key.data(),key.size()}));
            std::uint64_t id = 0;
            for (unsigned i=0;i!=8;++i)
                id |= std::uint64_t(std::to_integer<unsigned>(key[i])) << (8*i);
            if (id > result.last_sequence && info.value_bytes) {
                detail::journal_codec::StoreSource source(codec_->store,handle,info.value_bytes);
                append(source,codec_->buffer());
            }
            status = codec_->store.next(handle,handle);
        }
        if (status != detail::ScratchStatus::end) CodecState::check(status);
    } else {
        std::array<std::byte,512> page{};
        const auto first = std::upper_bound(events_.begin(),events_.end(),result.last_sequence,
            [](uint64_t id,const auto& event){return id < sequence(event);});
        for (auto at = first; at != events_.end(); ++at) {
            const auto source = encode_journal_record(*at);
            append(source,{page.data(),page.size()});
        }
    }
    if (filtered_ && retained_rejected_bar != folded_window_.last_rejected_command_bar)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                      "admission filter cannot rewrite the rejection gate");
    folded_window_ = result;
    fold_valid_ = true;
    filtered_ = false;
    rejection_gate_ = gate;
    return result;
}

int Journal::last_rejected_command_bar() const {
    if (filtered_ && fold_valid_) return folded_window_.last_rejected_command_bar;
    if (codec_) CodecState::check(codec_->failure);
    if (!rejection_gate_.valid) return folded_window().last_rejected_command_bar;
    return rejection_gate_.bar;
}

Journal::Journal(const Journal& other) {
    if (other.active_allocations_ != 0)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                      "cannot copy admission journal with active captures");
    next_sequence_ = other.next_sequence_;
    outstanding_sequences_ = other.outstanding_sequences_;
    if (other.codec_)
        for (auto id = other.retired_prefix_.last_sequence + 1; id < other.next_sequence_; ++id) {
            detail::ScratchRecordInfo info;
            const auto status = detail::journal_codec::find(other.codec_->store,id,info);
            if (status == detail::ScratchStatus::not_found) {
                if (id != other.codec_->abandoned) outstanding_sequences_.push_back(id);
            }
            else CodecState::check(status);
        }
    events_ = other.events();
    outstanding_count_ = other.outstanding_count_;
    retired_prefix_ = other.retired_prefix_;
    folded_window_ = other.folded_window_;
    fold_valid_ = other.fold_valid_;
    filtered_ = other.filtered_;
    rejection_gate_ = other.rejection_gate_;
    retain_full_observation_history_ = other.retain_full_observation_history_;
}
Journal& Journal::operator=(const Journal& other) {
    if (this == &other) return *this;
    if (active_allocations_ != 0)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                      "cannot replace admission journal with active captures");
    Journal copy(other);
    std::swap(next_sequence_, copy.next_sequence_);
    codec_.swap(copy.codec_);
    std::swap(outstanding_count_, copy.outstanding_count_);
    outstanding_sequences_.swap(copy.outstanding_sequences_);
    events_.swap(copy.events_);
    std::swap(retired_prefix_, copy.retired_prefix_);
    std::swap(folded_window_, copy.folded_window_);
    std::swap(fold_valid_, copy.fold_valid_);
    std::swap(filtered_, copy.filtered_);
    std::swap(rejection_gate_, copy.rejection_gate_);
    std::swap(retain_full_observation_history_, copy.retain_full_observation_history_);
    return *this;
}
Journal::Journal(Journal&& other) {
    if (other.active_allocations_ != 0)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                      "cannot move admission journal with active captures");
    next_sequence_ = other.next_sequence_;
    codec_ = std::move(other.codec_);
    outstanding_count_ = std::exchange(other.outstanding_count_,0);
    outstanding_sequences_ = std::move(other.outstanding_sequences_);
    events_ = std::move(other.events_);
    retired_prefix_ = other.retired_prefix_;
    folded_window_ = other.folded_window_;
    fold_valid_ = other.fold_valid_;
    filtered_ = other.filtered_;
    rejection_gate_ = other.rejection_gate_;
    retain_full_observation_history_ = other.retain_full_observation_history_;
    other.outstanding_sequences_.clear();
    other.events_.clear();
    other.next_sequence_ = 1;
    other.retired_prefix_ = {};
    other.folded_window_ = {};
    other.fold_valid_ = false;
    other.filtered_ = false;
    other.rejection_gate_ = {};
    other.retain_full_observation_history_ = false;
}
Journal& Journal::operator=(Journal&& other) {
    if (this == &other) return *this;
    if (active_allocations_ != 0)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                      "cannot replace admission journal with active captures");
    Journal moved(std::move(other));
    std::swap(next_sequence_, moved.next_sequence_);
    codec_.swap(moved.codec_);
    std::swap(outstanding_count_, moved.outstanding_count_);
    outstanding_sequences_.swap(moved.outstanding_sequences_);
    events_.swap(moved.events_);
    std::swap(retired_prefix_, moved.retired_prefix_);
    std::swap(folded_window_, moved.folded_window_);
    std::swap(fold_valid_, moved.fold_valid_);
    std::swap(filtered_, moved.filtered_);
    std::swap(rejection_gate_, moved.rejection_gate_);
    std::swap(retain_full_observation_history_, moved.retain_full_observation_history_);
    return *this;
}
uint64_t Journal::next_sequence() {
    if(next_sequence_==std::numeric_limits<uint64_t>::max()) {
        throw coded<std::overflow_error>(RunFailureCode::engine_invariant, {},
                                         "admission event sequence exhausted");
    }
    if (codec_) CodecState::check(codec_->failure);
    else outstanding_sequences_.push_back(next_sequence_);
    ++outstanding_count_;
    return next_sequence_++;
}
Allocation Journal::reserve() {
    const auto id = next_sequence();
    ++active_allocations_;
    return Allocation(*this, id);
}
void Journal::abandon(uint64_t id) noexcept {
    if (codec_) {
        if (!id || id >= next_sequence_ || id <= retired_prefix_.last_sequence) return;
        if (id == codec_->abandoned) return;
        detail::ScratchRecordInfo info;
        auto status = id == codec_->failed_sequence ? detail::ScratchStatus::not_found
            : detail::journal_codec::find(codec_->store,id,info);
        if (status == detail::ScratchStatus::ok) return;
        if (status == detail::ScratchStatus::not_found) {
            if (codec_->abandoned && codec_->abandoned > retired_prefix_.last_sequence) {
                const detail::ScratchSpanSource empty({nullptr,0});
                status = detail::journal_codec::put(codec_->store,codec_->abandoned,empty);
                if (status != detail::ScratchStatus::ok) { codec_->failure = status; return; }
            }
            codec_->abandoned = id;
            codec_->failed_sequence = 0;
            --outstanding_count_;
            return;
        }
        codec_->failure = status; // destructor cannot throw; next boundary reports it
        return;
    }
    const auto at = std::find(outstanding_sequences_.begin(), outstanding_sequences_.end(), id);
    if (at != outstanding_sequences_.end()) {
        outstanding_sequences_.erase(at);
        --outstanding_count_;
    }
}
Allocation::Allocation(Allocation&& other) noexcept
    : journal_(other.journal_), sequence_(other.sequence_) {
    other.journal_ = nullptr;
    other.sequence_ = 0;
}
void Journal::release_allocation(uint64_t sequence) noexcept {
    abandon(sequence);
    --active_allocations_;
}
Allocation::~Allocation() noexcept { if (journal_) journal_->release_allocation(sequence_); }
void Journal::append(Event event) {
    const auto id=sequence(event);
    if (codec_) {
        if (!id || id >= next_sequence_ || id <= retired_prefix_.last_sequence)
            throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                          "admission event has no outstanding allocation");
        if (id == codec_->abandoned)
            throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                          "admission event has no outstanding allocation");
        detail::ScratchRecordInfo info;
        const auto found = detail::journal_codec::find(codec_->store,id,info);
        if (found == detail::ScratchStatus::ok)
            throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                          "admission event has no outstanding allocation");
        if (found != detail::ScratchStatus::not_found) CodecState::check(found);
        const auto source = encode_journal_record(event);
        CodecState::check(append_journal_record(*this,source));
        return;
    }
    const auto pending = std::find(outstanding_sequences_.begin(), outstanding_sequences_.end(), id);
    if (pending == outstanding_sequences_.end())
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                      "admission event has no outstanding allocation");
    const auto* command = std::get_if<CommandEvent>(&event);
    const bool rejected = command && rejected_command(command->outcome);
    const int rejected_bar = rejected ? command->observation->bar : -1;
    auto at=std::lower_bound(events_.begin(),events_.end(),id,[](const auto& e,uint64_t n){return sequence(e)<n;});
    events_.insert(at,std::move(event));
    // Consume only after insertion succeeds. Allocation failure leaves a caller
    // free to retry this still-outstanding event; retain never restores an ID.
    outstanding_sequences_.erase(pending);
    --outstanding_count_;
    if (id <= folded_window_.last_sequence) fold_valid_ = false;
    if (rejected && rejection_gate_.valid && id > rejection_gate_.sequence)
        rejection_gate_ = {id,rejected_bar,true};
}
void Journal::retain(const std::vector<uint64_t>& retained) {
    if (codec_) {
        auto& store = codec_->store;
        auto status = store.begin();
        CodecState::check(status);
        auto removed = std::uint64_t{0};
        detail::ScratchHandle handle = detail::scratch_no_handle;
        status = store.first(handle);
        while (status == detail::ScratchStatus::ok) {
            detail::ScratchHandle next = detail::scratch_no_handle;
            const auto following = store.next(handle,next);
            if (following != detail::ScratchStatus::ok && following != detail::ScratchStatus::end) {
                status = following; break;
            }
            detail::ScratchRecordInfo info;
            status = store.point(handle,info);
            if (status != detail::ScratchStatus::ok) break;
            if (info.value_bytes) {
                std::array<std::byte,8> key{};
                status = store.read(handle,detail::ScratchField::key,0,{key.data(),key.size()});
                if (status != detail::ScratchStatus::ok) break;
                std::uint64_t id = 0;
                for (unsigned i=0;i!=8;++i)
                    id |= std::uint64_t(std::to_integer<unsigned>(key[i])) << (8*i);
                if (std::find(retained.begin(),retained.end(),id)==retained.end()) {
                    const detail::journal_codec::WordSource probe(id);
                    const detail::ScratchSpanSource empty({nullptr,0});
                    detail::ScratchHandle same = detail::scratch_no_handle;
                    status = store.put(probe,empty,same);
                    if (status != detail::ScratchStatus::ok) break;
                    ++removed;
                }
            }
            handle = next; status = following;
        }
        if (status == detail::ScratchStatus::end) status = detail::ScratchStatus::ok;
        CodecState::check(detail::journal_codec::finish(store,status));
        if (removed) {
            codec_->count -= removed;
            codec_->snapshot_valid = false;
            std::vector<Event>().swap(events_);
            filtered_ = filtered_ || fold_valid_;
            rejection_gate_.valid = false;
        }
        return;
    }
    const auto before = events_.size();
    events_.erase(std::remove_if(events_.begin(),events_.end(),[&](const auto& e){
        return std::find(retained.begin(),retained.end(),sequence(e))==retained.end();
    }),events_.end());
    if (events_.size() != before) {
        if (fold_valid_) filtered_ = true;
        rejection_gate_.valid = false;
    }
}
void Journal::reset() {
    if (has_unfinished_events())
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                      "cannot reset admission journal with unfinished events");
    if (codec_) {
        CodecState::check(codec_->failure);
        CodecState::check(detail::journal_codec::clear(codec_->store));
        codec_->count = 0; codec_->snapshot_valid = false;
        codec_->abandoned = codec_->failed_sequence = 0;
    }
    std::vector<Event>().swap(events_);
    std::vector<uint64_t>().swap(outstanding_sequences_);
    next_sequence_=1;retired_prefix_={};folded_window_={};fold_valid_=false;filtered_=false;
    rejection_gate_ = {};
}
void Journal::retain_full_observation_history() {
    if (next_sequence_ != 1 || has_unfinished_events() || !events_.empty()
        || retired_prefix_.count != 0)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                      "full admission observation requires a fresh journal");
    retain_full_observation_history_ = true;
}
void Journal::retire_prefix(std::size_t count, uint64_t digest, uint64_t last_sequence) {
    if (has_unfinished_events())
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                      "cannot retire admission journal with unfinished events");
    const auto expected = folded_window();
    if (count != expected.count || last_sequence != expected.last_sequence
        || digest != expected.digest || count != event_count())
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                      "admission retirement requires the exact folded window");
    if (retain_full_observation_history_) return;
    if (codec_) {
        codec_->retire_through(expected.last_sequence);
        codec_->count = 0; codec_->snapshot_valid = false;
        if (codec_->abandoned <= expected.last_sequence) codec_->abandoned = 0;
    }
    retired_prefix_ = expected;
    // Clear destroys all variant payloads, including before/book/review vectors
    // and shared observations. Capacity is bounded by the largest live bar.
    std::vector<Event>().swap(events_);
}
CommandCapture::CommandCapture(Allocation allocation,CommandObservation input,std::vector<BookObservation> before,
        std::function<void(CommandEvent)> complete):allocation_(std::move(allocation)),input_(std::move(input)),before_(std::move(before)),complete_(std::move(complete)) {
    if (input_.command != allocation_.sequence())
        throw coded<std::invalid_argument>(RunFailureCode::engine_invariant, {},
                                           "command capture requires its allocated event");
}
CommandCapture::~CommandCapture() noexcept(false) {
    // Never run a potentially throwing completion while another exception is
    // unwinding. The Allocation member abandons this unfinished event instead.
    if (std::uncaught_exceptions() != 0) return;
    complete_({std::make_shared<const CommandObservation>(input_),outcome_,0,std::move(before_),{}});
}
void CommandCapture::bind(Draft& draft,std::optional<SizingObservation> sizing,double equity,double price) {
    input_.original_sizing=std::move(sizing);input_.explicit_equity=equity;input_.explicit_price=price;
    draft.bind(std::make_shared<const CommandObservation>(input_));
    if(outcome_!=Outcome::OpeningRejectedReductionAdmitted)outcome_=Outcome::Admitted;
}
ReviewCapture::ReviewCapture(Allocation allocation,ReviewEvent event,
        std::function<void(ReviewEvent)> complete)
    : allocation_(std::move(allocation)),event_(std::move(event)),complete_(std::move(complete)) {
    if (event_.receipt.sequence != allocation_.sequence() || event_.receipt.target_command != 0 ||
        !known_checkpoint(event_.receipt.checkpoint))
        throw coded<std::invalid_argument>(RunFailureCode::engine_invariant, {},
                                           "review capture requires an allocated batch checkpoint");
}
ReviewCapture::~ReviewCapture() noexcept(false) {
    if (std::uncaught_exceptions() != 0) return;
    complete_(std::move(event_));
}
ReviewReceipt ReviewCapture::receipt_for(const Draft& draft) const {
    const auto& origin = draft.observation();
    if (!origin || std::none_of(event_.reviewed.begin(), event_.reviewed.end(), [&](const auto& order) {
            return order.draft.observation() && order.draft.observation()->command == origin->command;
        }))
        throw coded<std::invalid_argument>(RunFailureCode::engine_invariant, {},
                                           "draft does not belong to this admission review");
    auto receipt = event_.receipt;
    receipt.target_command = origin->command;
    require_origin(origin, receipt.target_command, receipt.sequence, receipt.bar);
    return receipt;
}

namespace {
struct Reflect {
    const FieldVisitor& visit;
    template<class T>void field(const std::string& path,const char* name,const T& value) const {
        if constexpr(std::is_same_v<T,bool>)visit({path+"."+name,uint64_t(value?1:0)});
        else if constexpr(std::is_enum_v<T>||std::is_same_v<T,int>)visit({path+"."+name,int64_t(value)});
        else visit({path+"."+name,value});
    }
    void birth(const OrderBirth& o,const std::string& p)const {
        field(p,"cause",o.cause());field(p,"bar",o.bar());field(p,"timestamp",o.timestamp());
        field(p,"cursor_domain",o.cursor().domain());field(p,"cursor_position",o.cursor().position());
        field(p,"cursor_index",o.cursor().index());field(p,"cursor_count",o.cursor().count());
        field(p,"cursor_price",o.cursor_price());field(p,"first_fill",o.first_fill());
        field(p,"last_fill",o.last_fill());field(p,"evaluation_ordinal",o.evaluation_ordinal());
    }
    void config(const Configuration& o,const std::string& p)const {
#define F(name) field(p,#name,o.name)
        F(process_on_close);F(calc_on_fills);F(magnifier);F(fill_recalculation);F(scheduler);
        F(slippage);F(pyramiding);F(default_quantity_type);F(default_quantity_value);
        F(long_margin);F(short_margin);F(commission_value);F(commission_type);F(pointvalue);F(fx);F(quantity_step);F(mintick);F(risk_direction);F(loss_days_limit);
        F(drawdown_limit);F(intraday_loss_limit);F(position_limit);F(fill_cap_active);F(risk_halted);
#undef F
    }
    void sizing(const SizingObservation& o,const std::string& p)const {
#define F(name) field(p,#name,o.name)
        F(quantity);F(equity);F(price);F(mark);F(fx);
#undef F
    }
    void command(const CommandObservation& o,const std::string& p)const {
#define F(name) field(p,#name,o.name)
        F(command);F(kind);birth(o.birth,p+".birth");F(id);F(requested_quantity);F(quantity_type);F(buy);
        field(p+".prices","limit",o.prices.limit);field(p+".prices","stop",o.prices.stop);
        F(oca_name);F(oca_type);config(o.configuration,p+".configuration");F(bar);F(placement_side);F(placement_cycle);
        F(prior_close_quantity);F(held_quantity);F(held_entries);F(realized_equity);F(placement_equity);F(signal_close);F(quantized_fixed_quantity);
        field(p,"original_sizing_present",o.original_sizing.has_value());
        if(o.original_sizing)sizing(*o.original_sizing,p+".original_sizing");
        F(explicit_equity);F(explicit_price);
#undef F
    }
    void review(const ReviewReceipt& o,const std::string& p)const {
        field(p,"sequence",o.sequence);field(p,"checkpoint",o.checkpoint);field(p,"bar",o.bar);
        field(p,"target_command",o.target_command);
    }
    void revision(const SizingRevision& o,const std::string& p)const {
        field(p,"sequence",o.sequence);field(p,"cause_fill",o.cause_fill);field(p,"bar",o.bar);
        field(p,"target_command",o.target_command);
    }
    void draft(const Draft& o,const std::string& p)const {
        field(p,"observation_present",bool(o.observation()));
        if(o.observation())command(*o.observation(),p+".observation");
        field(p,"review_present",o.review().has_value());if(o.review())review(*o.review(),p+".review");
        field(p,"sizing_revision_present",o.sizing_revision().has_value());if(o.sizing_revision())revision(*o.sizing_revision(),p+".sizing_revision");
    }
    void book(const BookObservation& o,const std::string& p)const {
        field(p,"incarnation",o.incarnation);field(p,"priority",o.priority);field(p,"bar",o.bar);
        field(p,"type",o.type);field(p,"placement_side",o.placement_side);field(p,"buy",o.buy);field(p,"id",o.id);
        field(p,"oca_name",o.oca_name);field(p,"oca_type",o.oca_type);birth(o.birth,p+".birth");
        field(p+".prices","limit",o.prices.limit);field(p+".prices","stop",o.prices.stop);
        field(p+".prices","trail_points",o.prices.trail_points);field(p+".prices","trail_price",o.prices.trail_price);
        field(p+".prices","trail_offset",o.prices.trail_offset);draft(o.draft,p+".draft");
    }
    void resolution(const InstructionResolution& o,const std::string& p)const {
        field(p,"incarnation",o.incarnation);field(p,"kind",o.kind);field(p,"peer_incarnation",o.peer_incarnation);
        field(p,"own_priority",o.own_priority);field(p,"peer_priority",o.peer_priority);field(p,"transaction_quantity",o.transaction_quantity);
    }
    template<class T,class Fn>void array(const std::vector<T>& values,const std::string& p,Fn fn)const {
        field(p,"size",uint64_t(values.size()));for(size_t i=0;i<values.size();++i)fn(values[i],p+"["+std::to_string(i)+"]");
    }
    void event(const Event& value,const std::string& p)const {
        field(p,"kind",uint64_t(value.index()));
        std::visit([&](const auto& o){using T=std::decay_t<decltype(o)>;
            if constexpr(std::is_same_v<T,CommandEvent>){
                command(*o.observation,p+".observation");field(p,"outcome",o.outcome);field(p,"admitted_incarnation",o.admitted_incarnation);
                array(o.before,p+".before",[&](const auto& x,const auto& q){book(x,q);});
                array(o.removed,p+".removed",[&](auto x,const auto& q){field(q,"incarnation",x);});
            }else if constexpr(std::is_same_v<T,ReviewEvent>){
                review(o.receipt,p+".receipt");config(o.configuration,p+".configuration");field(p,"open_price",o.open_price);field(p,"position_side",o.position_side);field(p,"position_cycle",o.position_cycle);array(o.book,p+".book",[&](const auto& x,const auto& q){book(x,q);});array(o.reviewed,p+".reviewed",[&](const auto& x,const auto& q){book(x,q);});
                array(o.resolutions,p+".resolutions",[&](const auto& x,const auto& q){resolution(x,q);});
                array(o.causes,p+".causes",[&](auto x,const auto& q){field(q,"sequence",x);});
            }else{
                revision(o.receipt,p+".receipt");field(p,"incarnation",o.incarnation);
                sizing(o.before,p+".before");sizing(o.after,p+".after");
                field(p,"affordability_equity_before",o.affordability_equity_before);field(p,"affordability_equity_after",o.affordability_equity_after);
            }
        },value);
    }
};
}
void reflect(const Draft& value,const std::string& path,const FieldVisitor& visit){Reflect{visit}.draft(value,path);}
void reflect(const Event& value,const std::string& path,const FieldVisitor& visit){Reflect{visit}.event(value,path);}
void Journal::reflect(const std::string& path,const FieldVisitor& visit)const {
    Reflect r{visit};r.field(path,"next_sequence",next_sequence_);
    r.field(path,"active_allocations",active_allocations_);
    if (codec_) {
        r.field(path+".outstanding_sequences","size",outstanding_count_);
        std::size_t rank = 0;
        for (auto id = retired_prefix_.last_sequence + 1; id < next_sequence_; ++id) {
            if (id == codec_->abandoned) continue;
            detail::ScratchRecordInfo info;
            const auto status = detail::journal_codec::find(codec_->store,id,info);
            if (status == detail::ScratchStatus::not_found)
                r.field(path+".outstanding_sequences["+std::to_string(rank++)+"]","sequence",id);
            else CodecState::check(status);
        }
    } else r.array(outstanding_sequences_,path+".outstanding_sequences",[&](auto sequence,const auto& p){r.field(p,"sequence",sequence);});
    r.array(events(),path+".events",[&](const auto& event,const auto& p){r.event(event,p);});
    // These are the original event fold and its derived lookup index, not new
    // broker hash bytes. The v4 count/digest already covers their observations.
    if (retired_prefix_.count != 0) {
        r.field(path+".base","count",uint64_t(retired_prefix_.count));
        r.field(path+".base","digest",retired_prefix_.digest);
        r.field(path+".base","last_sequence",retired_prefix_.last_sequence);
        r.field(path+".base","last_rejected_command_bar",retired_prefix_.last_rejected_command_bar);
    }
}
std::vector<Field> fields(const Draft& draft) {
    std::vector<Field> result;reflect(draft,"draft",[&](const auto& field){result.push_back(field);});return result;
}
namespace {
template<class T>T read_field(const std::vector<Field>& fields,const std::string& path) {
    for(const auto& field:fields)if(field.path==path)return std::get<T>(field.value);
    return T{}; // absent optional payload; the presence leaf is always reflected
}
}
uint64_t read_unsigned(const std::vector<Field>& f,const std::string& p){return read_field<uint64_t>(f,p);}
int64_t read_integer(const std::vector<Field>& f,const std::string& p){return read_field<int64_t>(f,p);}
double read_double(const std::vector<Field>& f,const std::string& p){return read_field<double>(f,p);}
std::string read_string(const std::vector<Field>& f,const std::string& p){return read_field<std::string>(f,p);}
} // inline namespace market_admission_v2
} // namespace pineforge::admission
