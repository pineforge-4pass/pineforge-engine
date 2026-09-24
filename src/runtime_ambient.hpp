#pragma once
/*
 * runtime_ambient.hpp -- RUNTIME-PRIVATE header (src/, never installed).
 *
 * The library's per-thread runtime state, kept in one block (R5 lane D2-C):
 * the TA bar context (ta::bar_context), the EMA seeding default
 * (ta::ema_na_warmup_flag) and the chart's native day partition
 * (active_native_day_partition). Their accessors keep their signatures and
 * their answers; what moved is where the state lives while a run pumps.
 *
 * A host sets these around each calculation it runs -- the Pine host around
 * every script bar it publishes -- and each of those writes used to be a
 * thread-local access, which a strategy module loaded with dlopen pays as a
 * TLS-descriptor call: nine a bar for the three scopes the Pine host opens.
 * Now the thread holds one pointer to the block in force: its own, or the
 * block of the consumer whose pump is running on it
 * (NativeExecutionConsumer::pump_ambient). A host that holds that block
 * writes it as plain memory. Every reader still finds the block through the
 * thread's pointer, so it reads exactly the values it read before, and each
 * thread keeps its own state: one handle per thread, and any number of
 * handles taking turns on one thread.
 *
 * An installed block is a window onto the thread's state. Installing it
 * copies the values in force into it; removing it copies what it holds back
 * into the block it covered and makes that block current again. So code that
 * writes through the accessors inside a pump leaves the thread exactly as it
 * would have without the window, and pumps nest (another engine's, run from a
 * callback, covers this one until it returns).
 */

#include <pineforge/ta.hpp>
#include <pineforge/timeframe.hpp>

namespace pineforge {
namespace internal {

struct RuntimeAmbient {
    const NativeDayPartition* day_partition = nullptr;
    bool ema_na_warmup = false;
    ta::BarContext bar_context{};
};

// The calling thread's state: the block in force, and the thread's own.
// Constant-initialized (no TLS initializer runs), defined in
// ta_extremes_volume.cpp beside ta::bar_context, its most frequent reader.
struct ThreadRuntimeAmbient {
    RuntimeAmbient* installed = nullptr;  // a pump's block, or null: `own`
    RuntimeAmbient own{};
};
extern thread_local ThreadRuntimeAmbient tl_runtime_ambient;

// The block in force on the calling thread.
inline RuntimeAmbient& runtime_ambient() noexcept {
    ThreadRuntimeAmbient& thread = tl_runtime_ambient;
    return thread.installed ? *thread.installed : thread.own;
}

// Makes `block` the calling thread's block in force, holding the values that
// were in force; answers the block it now covers (null: the thread's own).
RuntimeAmbient* install_runtime_ambient(RuntimeAmbient& block) noexcept;
// Copies what `block` holds into the block it covered and makes that one
// current again. `covered` is install_runtime_ambient's answer.
void uninstall_runtime_ambient(RuntimeAmbient& block, RuntimeAmbient* covered) noexcept;

// The partition active_native_day_partition answers once `partition` is set:
// set_active_native_day_partition keeps no empty one.
inline const NativeDayPartition* active_day_partition_value(
        const NativeDayPartition* partition) noexcept {
    return partition != nullptr && !partition->stamps.empty() ? partition : nullptr;
}

// The three scopes a host opens around a calculation, written on a pump's
// block when it has one (NativeExecutionConsumer::pump_ambient) and through
// the accessors otherwise -- the second path is NativeDayPartitionScope, the
// raised ta::ema_na_warmup_flag() and ta::BarContextScope exactly, access for
// access. Either way the field holds the scope's value until the scope ends
// and then the value it held before.
class AmbientDayPartitionScope {
public:
    AmbientDayPartitionScope(RuntimeAmbient* block, const NativeDayPartition* partition) noexcept
        : block_(block),
          prior_(block ? block->day_partition : set_active_native_day_partition(partition)) {
        if (block_) block_->day_partition = active_day_partition_value(partition);
    }
    ~AmbientDayPartitionScope() {
        if (block_) block_->day_partition = prior_;
        else set_active_native_day_partition(prior_);
    }
    AmbientDayPartitionScope(const AmbientDayPartitionScope&) = delete;
    AmbientDayPartitionScope& operator=(const AmbientDayPartitionScope&) = delete;

private:
    RuntimeAmbient* block_;
    const NativeDayPartition* prior_;
};

class AmbientEmaSeedingScope {
public:
    AmbientEmaSeedingScope(RuntimeAmbient* block, bool na_warmup) noexcept
        : block_(block), prior_(block ? block->ema_na_warmup : ta::ema_na_warmup_flag()) {
        (block_ ? block_->ema_na_warmup : ta::ema_na_warmup_flag()) = na_warmup;
    }
    ~AmbientEmaSeedingScope() {
        (block_ ? block_->ema_na_warmup : ta::ema_na_warmup_flag()) = prior_;
    }
    AmbientEmaSeedingScope(const AmbientEmaSeedingScope&) = delete;
    AmbientEmaSeedingScope& operator=(const AmbientEmaSeedingScope&) = delete;

private:
    RuntimeAmbient* block_;
    bool prior_;
};

class AmbientBarContextScope {
public:
    AmbientBarContextScope(RuntimeAmbient* block, long long bar_index, long long origin) noexcept
        : block_(block), prior_(block ? block->bar_context : ta::bar_context()) {
        ta::BarContext& context = block_ ? block_->bar_context : ta::bar_context();
        context.installed = true;
        context.bar_index = bar_index;
        context.origin = origin;
    }
    ~AmbientBarContextScope() {
        (block_ ? block_->bar_context : ta::bar_context()) = prior_;
    }
    AmbientBarContextScope(const AmbientBarContextScope&) = delete;
    AmbientBarContextScope& operator=(const AmbientBarContextScope&) = delete;

private:
    RuntimeAmbient* block_;
    ta::BarContext prior_;
};

}  // namespace internal
}  // namespace pineforge
