#include <pineforge/detail/scratch_trial.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace pineforge::detail {
namespace {

struct MalformedReceipt {};

struct ReceiptFields {
    bool present = false;
    std::uint32_t version = 0;
    std::string classification;
    std::string certificate;
};

// A setup-only reader, with no byte-count policy cap and no recursive walk.
// Unknown additive fields are syntactically checked and skipped, not retained.
// Only the four scratch_facts fields can authorize a certificate. Duplicate
// keys in that object or the document root cannot shadow an OUT/missing field.
class ReceiptJson {
public:
    explicit ReceiptJson(std::string_view text) : text_(text) {}

    ReceiptFields read() {
        ReceiptFields fields;
        whitespace();
        if (position_ == text_.size()) return fields;
        expect('{');
        std::unordered_set<std::string> keys;
        if (!take('}')) {
            do {
                const auto key = string();
                if (!keys.insert(key).second) fail();
                expect(':');
                if (key == "scratch_facts") fields = scratch_object();
                else skip_value();
            } while (take(','));
            expect('}');
        }
        whitespace();
        if (position_ != text_.size()) fail();
        return fields;
    }

private:
    std::string_view text_;
    std::size_t position_ = 0;

    [[noreturn]] static void fail() { throw MalformedReceipt{}; }

    void whitespace() noexcept {
        while (position_ < text_.size()) {
            const char ch = text_[position_];
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
            ++position_;
        }
    }
    bool take(char ch) noexcept {
        whitespace();
        if (position_ == text_.size() || text_[position_] != ch) return false;
        ++position_;
        return true;
    }
    void expect(char ch) { if (!take(ch)) fail(); }

    unsigned hex_word() {
        unsigned result = 0;
        for (unsigned index = 0; index != 4; ++index) {
            if (position_ == text_.size()) fail();
            const char ch = text_[position_++];
            result <<= 4;
            if (ch >= '0' && ch <= '9') result += static_cast<unsigned>(ch - '0');
            else if (ch >= 'a' && ch <= 'f') result += static_cast<unsigned>(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') result += static_cast<unsigned>(ch - 'A' + 10);
            else fail();
        }
        return result;
    }
    static void utf8(std::string& out, unsigned point) {
        if (point < 0x80) out.push_back(static_cast<char>(point));
        else if (point < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (point >> 6)));
            out.push_back(static_cast<char>(0x80 | (point & 63)));
        } else if (point < 0x10000) {
            out.push_back(static_cast<char>(0xe0 | (point >> 12)));
            out.push_back(static_cast<char>(0x80 | ((point >> 6) & 63)));
            out.push_back(static_cast<char>(0x80 | (point & 63)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (point >> 18)));
            out.push_back(static_cast<char>(0x80 | ((point >> 12) & 63)));
            out.push_back(static_cast<char>(0x80 | ((point >> 6) & 63)));
            out.push_back(static_cast<char>(0x80 | (point & 63)));
        }
    }
    std::string string() {
        expect('"');
        std::string out;
        while (position_ < text_.size()) {
            const auto ch = static_cast<unsigned char>(text_[position_++]);
            if (ch == '"') return out;
            if (ch < 32) fail();
            if (ch != '\\') {
                if (ch < 128) out.push_back(static_cast<char>(ch));
                else {
                    const unsigned tails = ch >= 0xf0 ? 3 : ch >= 0xe0 ? 2 : ch >= 0xc2 ? 1 : 0;
                    if (tails == 0 || ch > 0xf4) fail();
                    unsigned point = ch & ((1u << (6 - tails)) - 1);
                    for (unsigned index = 0; index != tails; ++index) {
                        if (position_ == text_.size()) fail();
                        const auto tail = static_cast<unsigned char>(text_[position_++]);
                        if ((tail & 0xc0) != 0x80) fail();
                        point = (point << 6) | (tail & 63);
                    }
                    const unsigned minimum = tails == 1 ? 0x80 : tails == 2 ? 0x800 : 0x10000;
                    if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff))
                        fail();
                    utf8(out, point);
                }
                continue;
            }
            if (position_ == text_.size()) fail();
            switch (text_[position_++]) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                unsigned point = hex_word();
                if (point >= 0xd800 && point <= 0xdbff) {
                    // No whitespace is allowed inside a surrogate pair.
                    if (text_.size() - position_ < 2
                        || text_[position_] != '\\' || text_[position_ + 1] != 'u') fail();
                    position_ += 2;
                    const unsigned low = hex_word();
                    if (low < 0xdc00 || low > 0xdfff) fail();
                    point = 0x10000 + ((point - 0xd800) << 10) + (low - 0xdc00);
                } else if (point >= 0xdc00 && point <= 0xdfff) fail();
                utf8(out, point);
                break;
            }
            default: fail();
            }
        }
        fail();
    }

    std::string_view number() {
        whitespace();
        const auto begin = position_;
        if (position_ < text_.size() && text_[position_] == '-') ++position_;
        const auto digit = [&]() {
            return position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9';
        };
        if (!digit()) fail();
        if (text_[position_] == '0') ++position_;
        else while (digit()) ++position_;
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            if (!digit()) fail();
            while (digit()) ++position_;
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-'))
                ++position_;
            if (!digit()) fail();
            while (digit()) ++position_;
        }
        return text_.substr(begin, position_ - begin);
    }

    void scalar() {
        whitespace();
        if (position_ == text_.size()) fail();
        if (text_[position_] == '"') { (void)string(); return; }
        for (const std::string_view literal : {"true", "false", "null"}) {
            if (text_.substr(position_, literal.size()) == literal) {
                position_ += literal.size();
                return;
            }
        }
        (void)number();
    }

    void skip_value() {
        enum class Frame { array_first, array_value, array_after, object_first, object_key, object_after };
        std::vector<Frame> stack;
        bool need_value = true;
        for (;;) {
            if (need_value) {
                if (take('{')) stack.push_back(Frame::object_first);
                else if (take('[')) stack.push_back(Frame::array_first);
                else scalar();
                need_value = false;
            }
            if (stack.empty()) return;
            switch (stack.back()) {
            case Frame::array_first:
                if (take(']')) { stack.pop_back(); break; }
                stack.back() = Frame::array_after;
                need_value = true;
                break;
            case Frame::array_value:
                stack.back() = Frame::array_after;
                need_value = true;
                break;
            case Frame::array_after:
                if (take(']')) { stack.pop_back(); break; }
                expect(',');
                stack.back() = Frame::array_value;
                break;
            case Frame::object_first:
                if (take('}')) { stack.pop_back(); break; }
                (void)string();
                expect(':');
                stack.back() = Frame::object_after;
                need_value = true;
                break;
            case Frame::object_key:
                (void)string();
                expect(':');
                stack.back() = Frame::object_after;
                need_value = true;
                break;
            case Frame::object_after:
                if (take('}')) { stack.pop_back(); break; }
                expect(',');
                stack.back() = Frame::object_key;
                break;
            }
        }
    }

    ReceiptFields scratch_object() {
        ReceiptFields fields;
        fields.present = true;
        bool version = false, classification = false, certificate = false, reasons = false;
        std::unordered_set<std::string> keys;
        expect('{');
        if (!take('}')) {
            do {
                const auto key = string();
                if (!keys.insert(key).second) fail();
                expect(':');
                if (key == "version") {
                    const auto token = number();
                    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), fields.version);
                    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) fail();
                    version = true;
                } else if (key == "classification") {
                    fields.classification = string();
                    classification = true;
                } else if (key == "certificate") {
                    fields.certificate = string();
                    certificate = true;
                } else if (key == "reasons") {
                    expect('[');
                    if (!take(']')) {
                        do { (void)string(); } while (take(','));
                        expect(']');
                    }
                    reasons = true;
                } else skip_value();
            } while (take(','));
            expect('}');
        }
        if (!version || !classification || !certificate || !reasons) fail();
        return fields;
    }
};

ScratchTrialBinding external(ScratchTrialReason reason, ScratchFactsError error = ScratchFactsError::none) {
    return {ScratchTrialClassification::needs_file_capable_worker, reason, error, std::nullopt, false};
}

ScratchTrialBinding facts_failure(ScratchTrialReason reason, ScratchFactsError error) {
    if (error == ScratchFactsError::allocation_failure) throw std::bad_alloc{};
    return external(reason, error);
}

} // namespace

ScratchTrialBinding plan_scratch_trial(
        const ScratchTrialConfig& config,
        const std::unordered_map<std::string, std::string>& effective_inputs,
        bool has_run_overrides) {
    try {
        const auto receipt = ReceiptJson(config.order_shapes_receipt).read();
        if (!receipt.present) return external(ScratchTrialReason::receipt_absent);
        const auto facts = scratch_facts_from_receipt(
            receipt.version, receipt.classification, receipt.certificate);
        if (!facts) return facts_failure(ScratchTrialReason::certificate_invalid, facts.error);
        // V deliberately accepts OUT over complete facts. Never upgrade that
        // explicit producer verdict by looking only at facts.complete().
        if (receipt.classification == "out_of_subset")
            return external(ScratchTrialReason::producer_out_of_subset);
        for (const auto& input : effective_inputs) {
            const auto declared = config.declared_input_defaults.find(input.first);
            if (declared == config.declared_input_defaults.end() || declared->second != input.second)
                return external(ScratchTrialReason::effective_inputs_changed);
        }
        // Opaque source overrides may invalidate the separate live proof.
        // They have no proof-bearing representation in this protocol yet.
        if (has_run_overrides) return external(ScratchTrialReason::effective_overrides_unproved);

        const auto decision = decide_scratch_backend(*facts.value, config.live_bounds);
        if (!decision) return facts_failure(ScratchTrialReason::plan_invalid, decision.error);
        if (decision.value->backend != ScratchBackendDecision::finite_certified || !decision.value->finite)
            return external(ScratchTrialReason::live_proof_missing);
        // This tree has only the historical-R root backend. F's live slots
        // cannot be installed there: R=0 would fail on the first replacement,
        // and translating slots to historical R would invent a bound. Lane R
        // owns the reusable root/alias binder. Until it lands, keep every
        // nonzero root plan on the external route BEFORE any run mutation.
        const auto& finite = *decision.value->finite;
        if (finite.root_slots != 0 || finite.alias_slots != 0)
            return external(ScratchTrialReason::root_backend_unavailable);
        return {ScratchTrialClassification::in_subset, ScratchTrialReason::none,
                ScratchFactsError::none, finite, false};
    } catch (const MalformedReceipt&) {
        return external(ScratchTrialReason::receipt_malformed);
    } catch (const std::length_error&) {
        return external(ScratchTrialReason::plan_invalid, ScratchFactsError::arithmetic_overflow);
    }
}

} // namespace pineforge::detail
