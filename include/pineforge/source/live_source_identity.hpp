#pragma once

// A lossless descriptor for represented adapter replacement keys; Unverified
// retains hash-only behavior. Text already belongs to the placement row; only
// the shape and frozen suffix live here. No owning string, pointer or id limit.
#include <pineforge/native_order_identity.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <type_traits>

namespace pineforge::source::detail {

struct LiveSourceIdentity {
    // A new production replacement-key spelling must add a Shape (or fixed
    // Spelling) in the same commit and a public-run test walking EVERY spelling
    // it introduces, including delayed admission and origin rebinding. Keep
    // capture/matches lossless. Only the test adapter rejects unknown spellings;
    // production leaves them unprotected, never broken by an incomplete table.
    enum class Shape : std::uint8_t {
        Source, PairFamily, Bracket, CloseId, ShortHold, CurrentExit,
        MarginRevival, Cover, Spelling, SeparatedFamily, Unverified
    };
    struct Spelling {
        std::string_view prefix;
        bool numbered;
    };
    inline static constexpr std::array<Spelling, 11> spellings{{
        {"__margin_call__", false},
        {"__pine_close_all", false},
        {"__tv_money_margin_path__", false},
        {"__margin_follow_up__", false},
        {"__margin_priced_opening__", false},
        {"__margin_waypoint__", false},
        {"__intraday_loss_close__", false},
        {"__intraday_cap_close__", false},
        {"__pine_close_site__", true},
        {"__pine_close_flush__", true},
        {"__margin_revival__", true},
    }};

    std::uint64_t number = 0;
    Shape shape = Shape::Unverified;
    std::uint8_t family = 0;
    std::uint8_t spelling = 0;

    static bool consume(std::string_view& key, std::string_view part) noexcept {
        if (key.size() < part.size() || key.substr(0, part.size()) != part) return false;
        key.remove_prefix(part.size());
        return true;
    }
    static bool consume_number(std::string_view& key, std::uint64_t value) noexcept {
        // Every uint64_t has at most digits10 + 1 decimal digits.
        char bytes[std::numeric_limits<std::uint64_t>::digits10 + 1];
        const auto result = std::to_chars(bytes, bytes + sizeof(bytes), value);
        return result.ec == std::errc{}
            && consume(key, {bytes, static_cast<std::size_t>(result.ptr - bytes)});
    }
    static bool read_number(std::string_view key, std::uint64_t& value) noexcept {
        if (key.empty()) return false;
        const auto result = std::from_chars(key.data(), key.data() + key.size(), value);
        return result.ec == std::errc{} && result.ptr == key.data() + key.size();
    }
    static bool consume_pair(std::string_view& key, std::string_view source,
                             std::string_view from) noexcept {
        return consume(key, source) && consume(key, "\x1f") && consume(key, from);
    }

    bool matches(std::string_view key, std::string_view source,
                 std::string_view from) const noexcept {
        bool matched = false;
        switch (shape) {
        case Shape::Unverified:
            // No identity was captured: impose no constraint on a hash-only access.
            return true;
        case Shape::Source:
            matched = consume(key, source);
            break;
        case Shape::PairFamily:
            matched = consume_pair(key, source, from) && consume_number(key, family);
            break;
        case Shape::SeparatedFamily:
            matched = consume_pair(key, source, from) && consume(key, "\x1f")
                && consume_number(key, family);
            break;
        case Shape::Bracket:
            matched = consume_pair(key, source, from) && consume(key, "\x1f")
                && consume_number(key, family) && consume(key, "\x1f")
                && consume_number(key, number);
            break;
        case Shape::CloseId:
            matched = consume(key, source) && consume(key, "#close#")
                && consume_number(key, number);
            break;
        case Shape::ShortHold:
            matched = consume(key, "__short_seed_partial_hold__") && consume(key, source);
            break;
        case Shape::CurrentExit:
            matched = consume(key, "__pooc_current_exit__") && consume_pair(key, source, from);
            break;
        case Shape::MarginRevival:
            matched = consume_pair(key, source, from) && consume(key, "\x1fmargin-revival");
            break;
        case Shape::Cover:
            matched = consume_pair(key, source, from) && consume(key, "\x1f" "cover");
            break;
        case Shape::Spelling:
            if (spelling < spellings.size()) {
                const auto& text = spellings[spelling];
                matched = consume(key, text.prefix)
                    && (!text.numbered || consume_number(key, number));
            }
            break;
        }
        return matched && key.empty();
    }

    static LiveSourceIdentity capture(std::string_view key, std::string_view source,
                                      std::string_view from) {
        LiveSourceIdentity identity;
        for (const auto shape : {Shape::Source, Shape::ShortHold, Shape::CurrentExit,
                                 Shape::MarginRevival, Shape::Cover}) {
            identity.shape = shape;
            if (identity.matches(key, source, from)) return identity;
        }
        auto suffix = key;
        if (consume_pair(suffix, source, from)) {
            std::uint64_t family = 0;
            if (read_number(suffix, family) && family <= std::numeric_limits<std::uint8_t>::max()) {
                identity.shape = Shape::PairFamily;
                identity.family = static_cast<std::uint8_t>(family);
                if (identity.matches(key, source, from)) return identity;
            }
            if (consume(suffix, "\x1f")) {
                if (read_number(suffix, family)
                    && family <= std::numeric_limits<std::uint8_t>::max()) {
                    identity.shape = Shape::SeparatedFamily;
                    identity.family = static_cast<std::uint8_t>(family);
                    if (identity.matches(key, source, from)) return identity;
                }
                const auto separator = suffix.find('\x1f');
                if (separator != std::string_view::npos
                    && read_number(suffix.substr(0, separator), family)
                    && family <= std::numeric_limits<std::uint8_t>::max()
                    && read_number(suffix.substr(separator + 1), identity.number)) {
                    identity.shape = Shape::Bracket;
                    identity.family = static_cast<std::uint8_t>(family);
                    if (identity.matches(key, source, from)) return identity;
                }
            }
        }
        suffix = key;
        if (consume(suffix, source) && consume(suffix, "#close#")
            && read_number(suffix, identity.number)) {
            identity.shape = Shape::CloseId;
            if (identity.matches(key, source, from)) return identity;
        }
        identity.shape = Shape::Spelling;
        for (std::size_t index = 0; index < spellings.size(); ++index) {
            suffix = key;
            const auto& text = spellings[index];
            if (!consume(suffix, text.prefix)
                || (text.numbered && !read_number(suffix, identity.number))) continue;
            identity.spelling = static_cast<std::uint8_t>(index);
            if (identity.matches(key, source, from)) return identity;
        }
        // Missing grammar coverage must not reject a valid production run.
        // live_source_value enforces coverage only in the compile-time test seam.
        return {};
    }
};

// The native handle stays a base subobject so the existing state hash folds
// exactly its previous bytes. Identity is verification metadata only.
struct LiveSourceHandle : native_order::RequestHandle {
    LiveSourceIdentity identity{};
    LiveSourceHandle() = default;
    LiveSourceHandle(const native_order::RequestHandle& handle, std::string_view key,
                     std::string_view source, std::string_view from)
        : native_order::RequestHandle(handle),
          identity(LiveSourceIdentity::capture(key, source, from)) {}
};

// Resident metadata remains scalar and independent of source-id length.
// Assert the ownership property without imposing a platform's padding layout.
static_assert(std::is_trivially_copyable<LiveSourceIdentity>::value,
              "live source identity must remain non-owning verification data");

} // namespace pineforge::source::detail
