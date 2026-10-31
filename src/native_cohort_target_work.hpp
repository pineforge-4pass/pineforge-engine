#ifndef PINEFORGE_NATIVE_COHORT_TARGET_WORK_HPP
#define PINEFORGE_NATIVE_COHORT_TARGET_WORK_HPP

#include <cstddef>

// Optional test-build observer. No request, hash, decision or allocation reads
// these counters. A run and its readers belong to one thread; scoped observers
// restore the previous registration even when an authoritative read throws.
namespace pineforge::native_order::cohort_target_work {
enum class Event : std::size_t {
    Target, TargetLot, Refresh, RefreshLot, Canonical, Contains, FindLive,
    Issued, IssuedComparison, Successor, RootTailComparison, RootDiskRead,
    ResidentStep, HandleCompare, Count
};
struct Observer {
    void* context = nullptr;
    void (*charge)(void*, Event) noexcept = nullptr;
};
inline thread_local Observer observer{};
inline void note(Event event) noexcept {
    if (observer.charge) observer.charge(observer.context, event);
}
class Scope {
public:
    explicit Scope(Observer value) noexcept : previous_(observer) { observer = value; }
    ~Scope() { observer = previous_; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Observer previous_;
};
}  // namespace pineforge::native_order::cohort_target_work

#if defined(PINEFORGE_COHORT_TARGET_OBSERVER)
#define PF_COHORT_WORK(event) \
    ::pineforge::native_order::cohort_target_work::note( \
        ::pineforge::native_order::cohort_target_work::Event::event)
#else
#define PF_COHORT_WORK(event) ((void)0)
#endif

#endif  // PINEFORGE_NATIVE_COHORT_TARGET_WORK_HPP
