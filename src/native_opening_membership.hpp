#pragma once

#include <pineforge/native_order.hpp>
#include <exception>
#include <type_traits>

namespace pineforge::native_order {
inline namespace native_order_v8 {

// The same canonical sequence in a live definition and its authority. Borrow
// the inline vector or the frozen readback; never materialize the whole list.
class OpeningMembership {
public:
    template<class Scope>
    explicit OpeningMembership(const Scope& scope) noexcept
        : owned_(scope.openings), external_(scope.external_openings.get()) {}

    bool is_inline() const noexcept { return !external_; }
    std::uint64_t size() const noexcept { return opening_count(owned_, external_); }
    RequestHandle operator[](std::uint64_t i) const {
        return external_ ? read_external(*external_, i) : owned_[static_cast<std::size_t>(i)];
    }

    // A noexcept fold can consume each handle as it is read. Return the
    // first failure to the book walker; never hash a failed read's output.
    template<class Visitor>
    static std::exception_ptr visit_external(const OpeningReadback& external,
                                             std::uint64_t count, Visitor visit) noexcept {
        static_assert(std::is_nothrow_invocable_v<Visitor&, const RequestHandle&>);
        try {
            for (std::uint64_t i = 0; i < count; ++i) visit(read_external(external, i));
            return {};
        } catch (...) {
            return std::current_exception();
        }
    }

private:
    static RequestHandle read_external(const OpeningReadback&, std::uint64_t);
    const std::vector<RequestHandle>& owned_;
    const OpeningReadback* external_;
};

} // inline namespace native_order_v8
} // namespace pineforge::native_order

namespace pineforge {
inline namespace engine_script_run_v20 {

// Only the live table needs a fallible read. Other continuations and receipt
// digests keep their ordinary word sink; the exception is never a hash word.
template<class Fold>
struct OpeningMembershipFold : Fold {
    std::exception_ptr read_failure;

    template<class HashHandle>
    void hash_external(const native_order::OpeningReadback& openings, HashHandle hash) noexcept {
        const auto count = openings.count();
        this->u(count);
        read_failure = native_order::OpeningMembership::visit_external(openings, count, hash);
    }
};

} // inline namespace engine_script_run_v20
} // namespace pineforge
