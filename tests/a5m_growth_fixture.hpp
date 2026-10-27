#pragma once

#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "a5m_store_fixture.hpp"

#include <pineforge/generic_matrix.hpp>
#include <pineforge/matrix.hpp>
#include <pineforge/native_host.hpp>

#if __has_include(<pineforge/detail/matrix_store.hpp>)
#include <pineforge/detail/matrix_store.hpp>
#define PF_A5M_EXTERNAL 1
#else
#define PF_A5M_EXTERNAL 0
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <vector>
#if defined(__GLIBC__)
#include <malloc.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#endif

namespace a5m_growth {

inline std::size_t* requested_high_water = nullptr;

inline void record_requested_peak(std::size_t) {
    if (requested_high_water)
        *requested_high_water = std::max(*requested_high_water, global_allocation::live_bytes);
}

// Observe the allocator at EACH allocation, including workspaces freed before
// the operation returns. The allocation replacement owns the size bookkeeping.
class OperationWindow {
public:
    explicit OperationWindow(std::size_t& peak)
        : previous_recording_(global_allocation::recording),
          previous_observer_(global_allocation::on_recorded),
          previous_peak_(requested_high_water) {
        requested_high_water = &peak;
        global_allocation::on_recorded = record_requested_peak;
        global_allocation::recording = true;
        record_requested_peak(0);
    }
    ~OperationWindow() {
        global_allocation::recording = previous_recording_;
        global_allocation::on_recorded = previous_observer_;
        requested_high_water = previous_peak_;
    }
private:
    bool previous_recording_;
    void (*previous_observer_)(std::size_t);
    std::size_t* previous_peak_;
};

// Eigen allocates directly with malloc. Count libc's retained bytes as well as
// global-new blocks; store stats alone could not detect a native cell mirror.
inline std::int64_t heap_bytes() {
#if defined(__GLIBC__)
    const auto info = ::mallinfo2();
    return static_cast<std::int64_t>(info.uordblks) +
           static_cast<std::int64_t>(info.hblkhd);
#elif defined(__APPLE__)
    malloc_statistics_t info{};
    ::malloc_zone_statistics(nullptr, &info);
    return static_cast<std::int64_t>(info.size_in_use);
#else
    return static_cast<std::int64_t>(global_allocation::live_bytes);
#endif
}

inline double value_at(int index, int column) {
    return 100.0 + static_cast<double>(index) * 2.0 +
           static_cast<double>(column + (index * 37) % 8) * 0.125;
}

inline bool boolean_at(int index, int column) {
    // A short periodic mask could hide a stale page or a row permutation.
    auto bits = static_cast<std::uint64_t>(index) +
                UINT64_C(0x9e3779b97f4a7c15) * static_cast<std::uint64_t>(column + 1);
    bits = (bits ^ (bits >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    bits = (bits ^ (bits >> 27)) * UINT64_C(0x94d049bb133111eb);
    return ((bits ^ (bits >> 31)) >> 63) != 0;
}

struct Numeric {
    pineforge::NumericMatrix matrix = pineforge::NumericMatrix::new_(0, 2);
    std::vector<double> values{0.0, 0.0};
    void append(int bar, int) {
        for (int c = 0; c < 2; ++c) values[static_cast<std::size_t>(c)] = value_at(bar, c);
        matrix.add_row(matrix.rows(), values);
    }
    void verify(int count) const {
        a5m_test::require(matrix.rows() == count && matrix.columns() == 2,
                          "numeric population was refused or truncated");
        for (int r = 0; r < count; ++r)
            for (int c = 0; c < 2; ++c)
                a5m_test::require(matrix.get(r, c) == value_at(r, c),
                                  "late numeric read changed");
    }
};

// The generic matrix's primary template: one int row appended per bar.
struct Integer {
    pineforge::GenericMatrix<int> matrix = pineforge::GenericMatrix<int>::new_(0, 0, 0);
    std::vector<int> values{0, 0};
    // value_at() steps by eighths, so eight times it is an exact integer.
    static int at(int bar, int column) { return static_cast<int>(value_at(bar, column) * 8.0); }
    void append(int bar, int) {
        for (int c = 0; c < 2; ++c) values[static_cast<std::size_t>(c)] = at(bar, c);
        matrix.add_row(matrix.rows(), values);
    }
    void verify(int count) const {
        a5m_test::require(matrix.rows() == count && matrix.columns() == 2,
                          "integer population was refused or truncated");
        for (int r = 0; r < count; ++r)
            for (int c = 0; c < 2; ++c)
                a5m_test::require(matrix.get(r, c) == at(r, c), "late integer read changed");
    }
};

struct Boolean {
    pineforge::GenericMatrix<bool> matrix;
    void append(int bar, int count) {
        // A script with proved dimensions can construct its mask on bar zero.
        // Measuring construction inside the callback also avoids the baseline's
        // quadratic outer-vector reserve idiom hiding a memory test in run time.
        if (bar == 0) matrix = pineforge::GenericMatrix<bool>::new_(count, 4, false);
        for (int c = 0; c < 4; ++c) matrix.set(bar, c, boolean_at(bar, c));
    }
    void verify(int count) const {
        a5m_test::require(matrix.rows() == count && matrix.columns() == 4,
                          "boolean population was refused or truncated");
        for (int r = 0; r < count; ++r)
            for (int c = 0; c < 4; ++c)
                a5m_test::require(matrix.get(r, c) == boolean_at(r, c),
                                  "late boolean read changed");
    }
};

template<class Family>
class Host final : public pineforge::NativeStrategyHost {
public:
    Family family;
    int count;
    int callbacks = 0;
    std::int64_t retained = 0, peak = 0;
    std::size_t requested_peak = 0;

    explicit Host(int population) : count(population) {
        pineforge::NativeRunSpec spec;
        spec.identity = {"a5m-growth", 1};
        spec.input_tf = "1";
        spec.script_tf = "1";
        spec.ticker = "MATRIX";
        spec.tickerid = "TEST:MATRIX";
        spec.type = "crypto";
        spec.currency = "USDT";
        spec.basecurrency = "ETH";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.slot_label_policy = pineforge::NativeSlotLabelPolicy::FeedTolerant;
        spec.initial_capital = 1000000.0;
        spec.point_value = spec.account_fx = 1.0;
        spec.price_tick = 0.125;
        spec.event_retention = pineforge::NativeEventRetention::Window;
        a5m_test::require(configure_native(spec).status == pineforge::NativeSetupStatus::Applied,
                          "configure matrix growth host");
    }

    void on_native_bar(const pineforge::Bar& bar,
                       const pineforge::NativeDecisionContext&) override {
        a5m_test::require(bar.close == value_at(callbacks, 0), "synthetic bar sequence changed");
        const auto before = heap_bytes();
        {
            OperationWindow window(requested_peak);
            family.append(callbacks, count);
        }
        retained += heap_bytes() - before;
        peak = std::max(peak, retained);
        ++callbacks;
    }
};

template<class Family>
inline void drive(int count, bool native, const char* kind) {
    std::vector<pineforge::Bar> bars(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const double close = value_at(i, 0);
        bars[static_cast<std::size_t>(i)] =
            {close, close + 1.0, close - 1.0, close, 1000.0,
             INT64_C(1736121600000) + static_cast<std::int64_t>(i) * 60000};
    }
    a5m_test::StoreFixture fixture;
#if PF_A5M_EXTERNAL
    pineforge::detail::matrix_store::Runtime runtime(*fixture.store);
    // A disabled scope retains the real baseline path for RED on the final tree.
    pineforge::detail::matrix_store::ScopedRuntime scope(native ? nullptr : &runtime);
#endif
    Host<Family> host(count);
    host.run(bars.data(), count, "1", "1");
    a5m_test::require(host.last_error().empty(), host.last_error().c_str());
    a5m_test::require(host.callbacks == count, "host did not reach every bar");
    {
        OperationWindow window(host.requested_peak);
        host.family.verify(count); // Read every old cell after the final callback.
    }
    const auto stats = fixture.store->stats();
    const auto bound = a5m_test::kStoreBound + a5m_test::kOperationBound;
    std::printf("A5M %s bars=%d retained=%lld requested=%zu bound=%llu live=%llu backing=%llu mode=%s\n",
                kind, count, static_cast<long long>(host.peak), host.requested_peak,
                static_cast<unsigned long long>(bound),
                static_cast<unsigned long long>(stats.bytes_live),
                static_cast<unsigned long long>(stats.bytes_backing), native ? "native-RED" : "store");
    // The native population crosses the same design-derived fixed bound.
    a5m_test::require(static_cast<std::uint64_t>(count) * 4 > bound,
                      "growth population cannot exceed the claimed bound");
    a5m_test::require(host.peak <= static_cast<std::int64_t>(bound) &&
                      host.requested_peak <= bound,
                      "matrix retained heap grows with the cell population");
#if PF_A5M_EXTERNAL
    if (!native) {
        a5m_test::require(stats.bytes_resident <= a5m_test::kStoreBound,
                          "matrix store resident workspace exceeded its shape");
        a5m_test::require(stats.bytes_live > bound && stats.bytes_backing > bound,
                          "matrix cells never grew beyond the resident workspace");
        a5m_test::require(host.family.matrix.is_external_storage(),
                          "matrix silently escaped to native storage");
    }
#endif
}

inline int run(int count, int argc, char** argv) {
    bool numeric_ok = true, integer_ok = true, boolean_ok = true;
    try {
        a5m_test::require(argc == 1 || (argc == 2 && std::strcmp(argv[1], "--native") == 0),
                          "usage: test_a5m_growth [--native]");
        const bool native = !PF_A5M_EXTERNAL || argc == 2;
        // Execute every control even when an earlier family's bound fails.
        try { drive<Numeric>(count, native, "numeric"); }
        catch (const std::exception& error) {
            global_allocation::recording = false;
            numeric_ok = false;
            std::fprintf(stderr, "A5M numeric growth: %s\n", error.what());
        }
        global_allocation::live_bytes = 0; // The previous host has been destroyed.
        try { drive<Integer>(count, native, "int"); }
        catch (const std::exception& error) {
            global_allocation::recording = false;
            integer_ok = false;
            std::fprintf(stderr, "A5M integer growth: %s\n", error.what());
        }
        global_allocation::live_bytes = 0;
        try { drive<Boolean>(count, native, "bool"); }
        catch (const std::exception& error) {
            global_allocation::recording = false;
            boolean_ok = false;
            std::fprintf(stderr, "A5M boolean growth: %s\n", error.what());
        }
        return numeric_ok && integer_ok && boolean_ok ? 0 : 1;
    } catch (const std::exception& error) {
        global_allocation::recording = false;
        std::fprintf(stderr, "A5M growth: %s\n", error.what());
        return 1;
    }
}

template<class Matrix, class Value>
struct Objects {
#if PF_A5M_EXTERNAL
    pineforge::detail::matrix_store::Runtime* runtime =
        pineforge::detail::matrix_store::current_runtime();
    pineforge::detail::o1::Id roots = 0;
#endif
    std::vector<Matrix> native_roots;
    int population = 0;

    static Value element_at(int bar, int column) {
        if constexpr (std::is_same_v<Value, bool>)
            return boolean_at(bar, column);
        else
            return static_cast<Value>(value_at(bar, column));
    }

    void append(int bar, int count) {
        population = count;
#if PF_A5M_EXTERNAL
        if (runtime) {
            auto& store = runtime->store();
            auto transaction = store.begin();
            if (bar == 0) roots = store.create(static_cast<std::uint64_t>(count) * sizeof(roots),
                                                alignof(decltype(roots)));
            auto matrix = Matrix::new_(1, 2, Value{});
            for (int c = 0; c < 2; ++c)
                matrix.set(0, c, element_at(bar, c));
            const auto id = matrix.detach_external(); // Generated root takes ownership.
            a5m_test::require(store.write(roots, static_cast<std::uint64_t>(bar) * sizeof(id),
                                          &id, sizeof(id)) == sizeof(id), "matrix root write");
            transaction->commit();
            return;
        }
#endif
        auto matrix = Matrix::new_(1, 2, Value{});
        for (int c = 0; c < 2; ++c) matrix.set(0, c, element_at(bar, c));
        native_roots.push_back(matrix); // The generated-container RED idiom.
    }

    void verify(int count) const {
#if PF_A5M_EXTERNAL
        if (runtime) {
            auto& store = runtime->store();
            pineforge::detail::o1::Id previous_id = 0;
            for (int r = 0; r < count; ++r) {
                pineforge::detail::o1::Id id = 0;
                a5m_test::require(store.read(roots, static_cast<std::uint64_t>(r) * sizeof(id),
                                             &id, sizeof(id)) == sizeof(id), "matrix root read");
                a5m_test::require(id > previous_id, "created matrix roots reused a reachable id");
                previous_id = id;
                auto matrix = Matrix::borrow_external(*runtime, id);
                for (int c = 0; c < 2; ++c)
                    a5m_test::require(matrix.get(0, c) == element_at(r, c),
                                      "old matrix identity or cell changed");
            }
            return;
        }
#endif
        a5m_test::require(native_roots.size() == static_cast<std::size_t>(count),
                          "matrix creation population truncated");
        for (int r = 0; r < count; ++r)
            for (int c = 0; c < 2; ++c)
                a5m_test::require(native_roots[static_cast<std::size_t>(r)].get(0, c) == element_at(r, c),
                                  "old native matrix changed");
    }

    void close() {
#if PF_A5M_EXTERNAL
        if (runtime && roots) {
            auto& store = runtime->store();
            for (int r = 0; r < population; ++r) {
                pineforge::detail::o1::Id id = 0;
                a5m_test::require(store.read(roots, static_cast<std::uint64_t>(r) * sizeof(id),
                                             &id, sizeof(id)) == sizeof(id), "matrix release root read");
                store.release(id);
            }
            store.release(roots);
            roots = 0;
        }
#endif
    }
};

template<class Matrix, class Value>
inline void drive_objects(int count, bool native, const char* kind) {
        std::vector<pineforge::Bar> bars(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            const double close = value_at(i, 0);
            bars[static_cast<std::size_t>(i)] =
                {close, close + 1.0, close - 1.0, close, 1000.0,
                 INT64_C(1736121600000) + static_cast<std::int64_t>(i) * 60000};
        }
        a5m_test::StoreFixture fixture;
#if PF_A5M_EXTERNAL
        pineforge::detail::matrix_store::Runtime runtime(*fixture.store);
        pineforge::detail::matrix_store::ScopedRuntime scope(native ? nullptr : &runtime);
#endif
        Host<Objects<Matrix, Value>> host(count);
        host.run(bars.data(), count, "1", "1");
        a5m_test::require(host.last_error().empty(), host.last_error().c_str());
        a5m_test::require(host.callbacks == count, "matrix creation host did not reach every bar");
        {
            OperationWindow window(host.requested_peak);
            host.family.verify(count);
        }
        const auto stats = fixture.store->stats();
        const auto bound = a5m_test::kStoreBound + a5m_test::kOperationBound;
        std::printf("A5M %s objects bars=%d retained=%lld requested=%zu bound=%llu live=%llu backing=%llu\n",
                    kind, count, static_cast<long long>(host.peak), host.requested_peak,
                    static_cast<unsigned long long>(bound),
                    static_cast<unsigned long long>(stats.bytes_live),
                    static_cast<unsigned long long>(stats.bytes_backing));
        host.family.close();
        a5m_test::require(host.peak <= static_cast<std::int64_t>(bound) &&
                          host.requested_peak <= bound, "native matrix directory grows with objects");
        if (!native) {
            a5m_test::require(stats.bytes_live > bound && stats.bytes_backing > bound &&
                              stats.bytes_resident <= a5m_test::kStoreBound,
                              "created matrix population did not spill within the store shape");
            a5m_test::require(fixture.store->stats().bytes_live == 0,
                              "matrix graph owner did not release all matrix objects");
        }
}

inline int run_objects(int count, int argc, char** argv) {
    try {
        a5m_test::require(argc == 1 || (argc == 2 && std::strcmp(argv[1], "--native") == 0),
                          "usage: test_a5m_objects [--native]");
        const bool native = !PF_A5M_EXTERNAL || argc == 2;
        const auto attempt = [](const char* kind, auto run_family) {
            // The previous family and all its marked blocks were destroyed.
            global_allocation::live_bytes = 0;
            try { run_family(); return true; }
            catch (const std::exception& error) {
                std::fprintf(stderr, "A5M %s object growth: %s\n", kind, error.what());
                return false;
            }
        };
        const bool numeric = attempt("numeric", [&] {
            drive_objects<pineforge::NumericMatrix, double>(count, native, "numeric");
        });
        const bool integer = attempt("int", [&] {
            drive_objects<pineforge::GenericMatrix<int>, int>(count, native, "int");
        });
        const bool boolean = attempt("bool", [&] {
            drive_objects<pineforge::GenericMatrix<bool>, bool>(count, native, "bool");
        });
        return numeric && integer && boolean ? 0 : 1;
    } catch (const std::exception& error) {
        global_allocation::recording = false;
        std::fprintf(stderr, "A5M object growth: %s\n", error.what());
        return 1;
    }
}

} // namespace a5m_growth
