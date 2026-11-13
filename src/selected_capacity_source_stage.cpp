#include "selected_capacity_source_stage.hpp"
#include "selected_capacity_csv_reader.hpp"
#include "../tools/selected_admit/record_tracker.hpp"

#include <pineforge/run_failure.hpp>

#include <exception>
#include <memory>
#include <streambuf>

namespace pineforge::detail::selected_capacity {
namespace {
void add_count(std::uint64_t& count, std::uint64_t amount) {
    if (amount > std::numeric_limits<std::uint64_t>::max() - count)
        throw coded<std::runtime_error>(RunFailureCode::engine_invariant, {},
                                         "Pine native run counter exhausted");
    count += amount;
}

class SourceRecordBuffer final : public std::streambuf {
public:
    SourceRecordBuffer(const SourceStageInput& input, const SourceStageOptions& options,
                       std::size_t index)
        : input_(input), options_(options), index_(index), tracker_(options.max_record_bytes) {
        setg(nullptr, nullptr, nullptr);
        window_.reserve(options.max_record_bytes);
    }
    void begin_record() { window_.clear(); }
    const std::string& window() const noexcept { return window_; }
    bool too_long() const noexcept { return too_long_; }
    void rethrow_internal() const { if (internal_) std::rethrow_exception(internal_); }

protected:
    int_type underflow() override {
        if (position_ < size_) return traits_type::to_int_type(chunk_[position_]);
        if (ended_) return traits_type::eof();
        try {
            if (!chunk_) chunk_ = std::make_unique<char[]>(options_.chunk_bytes);
            const auto count = input_.read(input_.read_ctx, chunk_.get(), options_.chunk_bytes);
            if (count < 0 || static_cast<std::size_t>(count) > options_.chunk_bytes)
                throw std::ios_base::failure("input_read_failure");
            if (count == 0) {
                ended_ = true;
                chunk_.reset();
                if (auto* progress = options_.progress) {
                    if (index_ < progress->slots && progress->complete)
                        progress->complete[index_] = true;
                }
                return traits_type::eof();
            }
            position_ = 0;
            size_ = static_cast<std::size_t>(count);
            return traits_type::to_int_type(chunk_[0]);
        } catch (const std::bad_alloc&) {
            internal_ = std::current_exception();
            throw;
        }
    }
    int_type uflow() override {
        tracker_.begin_get();
        const auto value = underflow();
        if (traits_type::eq_int_type(value, traits_type::eof())) return value;
        if (!tracker_.consume(static_cast<unsigned char>(traits_type::to_char_type(value)))) {
            too_long_ = true;
            throw std::ios_base::failure("record_too_long");
        }
        try {
            window_.push_back(traits_type::to_char_type(value));
            ++position_;
            if (auto* progress = options_.progress) {
                progress->row_index = tracker_.row_index();
                if (index_ < progress->slots && progress->bytes_read)
                    add_count(progress->bytes_read[index_], 1);
            }
        } catch (...) {
            internal_ = std::current_exception();
            throw;
        }
        return value;
    }

private:
    const SourceStageInput& input_;
    const SourceStageOptions& options_;
    std::size_t index_, position_ = 0, size_ = 0;
    RecordTracker tracker_;
    std::unique_ptr<char[]> chunk_;
    std::string window_;
    std::exception_ptr internal_;
    bool ended_ = false, too_long_ = false;
};

bool read_valid(SourceStageResult& result, const CsvRead& record, SourceRecordBuffer& buffer) {
    buffer.rethrow_internal();
    if (record.csv_bytes != buffer.window().size()) {
        result.status = DriverStatus::NotAdmitted;
        result.reason = SourceStageReason::InternalError;
        result.message = "Source record window disagrees with reader byte count";
        return false;
    }
    if (record.status == CsvStatus::Ok || record.status == CsvStatus::End) return true;
    result.message = record.message;
    if (record.status == CsvStatus::IoError) {
        result.status = buffer.too_long() ? DriverStatus::InputError : DriverStatus::IoError;
        result.reason = buffer.too_long() ? SourceStageReason::RecordTooLong
                                         : SourceStageReason::InputReadFailure;
        return false;
    }
    result.status = DriverStatus::FormatError;
    switch (record.reason) {
    case CsvReason::Columns: result.reason = SourceStageReason::CsvColumns; break;
    case CsvReason::Value: result.reason = SourceStageReason::CsvValue; break;
    case CsvReason::Encoding: result.reason = SourceStageReason::CsvUtf8; break;
    case CsvReason::Syntax: result.reason = SourceStageReason::CsvSyntax; break;
    case CsvReason::None:
        result.status = DriverStatus::NotAdmitted;
        result.reason = SourceStageReason::InternalError;
        break;
    }
    return false;
}
}  // namespace

SourceStageResult admit_source_stage(const SourceStageInput* inputs, std::size_t count,
    const Limits& limits, const SourceStageOptions& options, ByteForward forward, void* context) {
    SourceStageResult result;
    if (!limits.source_rows.present && !limits.bytes.present) {
        result.reason = SourceStageReason::NoStageFields;
        return result;
    }
    if (!inputs || count == 0) {
        result.reason = SourceStageReason::MissingInput;
        return result;
    }
    if (!forward || options.max_record_bytes == 0 || options.max_record_bytes > 1048576
        || options.chunk_bytes == 0 || options.chunk_bytes > 65536) {
        result.reason = SourceStageReason::InternalError;
        return result;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (!inputs[i].read) {
            result.input_index = i;
            result.reason = SourceStageReason::MissingInput;
            return result;
        }
    }
    const auto observers = make_product_observer_table();
    for (std::size_t i = 0; i < count; ++i) {
        result.input_index = i;
        result.row_index = 0;
        if (auto* progress = options.progress) {
            progress->input_index = i;
            progress->row_index = 0;
        }
        SourceRecordBuffer buffer(inputs[i], options, i);
        std::istream stream(&buffer);
        CsvReader reader(stream);
        buffer.begin_record();
        const auto header = reader.read_header();
        if (!read_valid(result, header, buffer)) return result;
        {
            auto tentative = result.state;
            if (limits.bytes.present) observers.bytes(tentative, limits, header.csv_bytes);
            if (!forward(context, buffer.window().data(), buffer.window().size())) {
                result.status = DriverStatus::IoError;
                result.reason = SourceStageReason::ForwardFailure;
                return result;
            }
            result.state = tentative;
            add_count(result.coverage.source_bytes, header.csv_bytes);
            add_count(result.forwarded_bytes, header.csv_bytes);
        }
        bool have_previous = false;
        std::int64_t previous = 0;
        std::uint64_t rows = 0;
        for (;;) {
            buffer.begin_record();
            result.row_index = rows;
            add_count(result.row_index, 1);
            if (options.progress) options.progress->row_index = result.row_index;
            const auto record = reader.next();
            if (!read_valid(result, record, buffer)) return result;
            if (record.status == CsvStatus::End) break;
            if (have_previous && record.bar.timestamp <= previous) {
                result.status = DriverStatus::InputError;
                result.reason = SourceStageReason::RowsUnordered;
                result.message = "Source CSV timestamps must increase strictly";
                return result;
            }
            auto tentative = result.state;
            if (limits.bytes.present) observers.bytes(tentative, limits, record.csv_bytes);
            RecordEvent event;
            event.role = Role::Source;
            event.timestamp_ms = record.bar.timestamp;
            event.bar = &record.bar;
            if (limits.source_rows.present) observers.source_rows(tentative, limits, event);
            if (!forward(context, buffer.window().data(), buffer.window().size())) {
                result.status = DriverStatus::IoError;
                result.reason = SourceStageReason::ForwardFailure;
                return result;
            }
            result.state = tentative;
            add_count(result.coverage.source_rows, 1);
            add_count(result.coverage.source_bytes, record.csv_bytes);
            add_count(result.forwarded_bytes, record.csv_bytes);
            previous = record.bar.timestamp;
            have_previous = true;
            rows = result.row_index;
        }
        add_count(result.coverage.source_streams, 1);
    }
    result.coverage.source_complete = true;
    result.status = DriverStatus::Completed;
    result.reason = SourceStageReason::None;
    result.input_index = std::numeric_limits<std::size_t>::max();
    result.row_index = 0;
    return result;
}
}  // namespace pineforge::detail::selected_capacity
