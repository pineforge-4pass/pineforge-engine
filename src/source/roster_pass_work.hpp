#ifndef PINEFORGE_SOURCE_ROSTER_PASS_WORK_HPP
#define PINEFORGE_SOURCE_ROSTER_PASS_WORK_HPP

#if defined(PINEFORGE_ROSTER_PASS_OBSERVER)
#include <cstdint>

// Test-build observation only. No decision, receipt or hash reads this
// overlay, and ordinary builds define neither storage nor observer code.
namespace pineforge::source::roster_pass_work {
struct Counts {
    std::uint64_t roster_cohort_visits = 0;
    std::uint64_t due_passes = 0;
};
inline thread_local Counts* observer = nullptr;

inline void note(std::uint64_t Counts::*field) noexcept {
    if (observer) ++(observer->*field);
}

class Scope {
public:
    explicit Scope(Counts& counts) noexcept : previous_(observer) { observer = &counts; }
    ~Scope() { observer = previous_; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Counts* previous_;
};
} // namespace pineforge::source::roster_pass_work

#define PF_ROSTER_PASS_WORK(field) \
    ::pineforge::source::roster_pass_work::note( \
        &::pineforge::source::roster_pass_work::Counts::field)
#else
#define PF_ROSTER_PASS_WORK(field) ((void)0)
#endif

#endif // PINEFORGE_SOURCE_ROSTER_PASS_WORK_HPP
