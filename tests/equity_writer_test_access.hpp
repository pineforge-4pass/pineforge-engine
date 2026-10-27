#pragma once

#include <pineforge/engine.hpp>

namespace equity_writer_test {

// Test-only access, following reservation_expansion_test_access.hpp and
// test_k1_global_origin_margin_revival.cpp. Explicit instantiation may name a
// private member (C++ [temp.explicit]); no product friend or export is added.
template<class Tag, typename Tag::type Member>
struct PrivateAccess {
    friend typename Tag::type access(Tag) { return Member; }
};

struct LastPoint {
    using type = pf_equity_point_t* (pineforge::BacktestEngine::*)() noexcept;
    friend type access(LastPoint);
};
template struct PrivateAccess<LastPoint, &pineforge::BacktestEngine::last_equity_point>;

struct ConstLastPoint {
    using type = const pf_equity_point_t* (pineforge::BacktestEngine::*)() const noexcept;
    friend type access(ConstLastPoint);
};
template struct PrivateAccess<ConstLastPoint, &pineforge::BacktestEngine::last_equity_point>;

struct RefoldTail {
    using type = void (pineforge::BacktestEngine::*)();
    friend type access(RefoldTail);
};
template struct PrivateAccess<RefoldTail, &pineforge::BacktestEngine::refold_equity_tail>;

inline pf_equity_point_t* last(pineforge::BacktestEngine& engine) noexcept {
    return (engine.*access(LastPoint{}))();
}

inline const pf_equity_point_t* last(const pineforge::BacktestEngine& engine) noexcept {
    return (engine.*access(ConstLastPoint{}))();
}

inline void refold(pineforge::BacktestEngine& engine) {
    (engine.*access(RefoldTail{}))();
}

} // namespace equity_writer_test
