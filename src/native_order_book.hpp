#pragma once

// Included within pineforge::native_order::native_order_v7 by native_order.cpp,
// after its file-local archive helpers and WorkingRequestCore::JournalStore.
// The include context preserves the existing helpers' linkage and lookup.

namespace {

// Compare the definition prefix of a just-decoded book record by walking the
// very same field lists as SaveAr/LoadAr. No output buffer or hash is involved:
// strings, vector lengths, tags, optional flags and every leaf must match.
// Raw leaves include padding, so a padding difference conservatively misses.
struct DefinitionMatchAr {
    static constexpr bool saving = true;
    const unsigned char* at;
    std::size_t left;
    bool matched = true;

    void raw(const void* value, std::size_t length) noexcept {
        if (!matched) return;
        if (length > left || (length != 0 && std::memcmp(at, value, length) != 0)) {
            matched = false;
            return;
        }
        at += length;
        left -= length;
    }
    template <class T>
    void operator()(const T& item) {
        if (matched) archive_value(*this, const_cast<T&>(item));
    }
};

// One spilled book row, filed under its priority: where its serialized
// LiveRequest sits in the blob file. state 0: the row is spilled; 1: it came
// back (or ended) and the entry is dead weight until the next compaction.
struct BookSlot {
    uint64_t offset = 0;
    uint64_t length = 0;
    uint64_t incarnation = 0;
    uint64_t state = 0;
    uint64_t stamp = 0;
    uint64_t dormancy = 0;  // bit 0: dormant; bit 1: Other rather than Limit
};

// The priority of a spilled row whose handle was re-priced, filed under its
// incarnation (a row that was never re-priced stands at its incarnation).
struct IncarnationSlot {
    uint64_t priority = 0;
    uint64_t state = 0;
};

// Table keys are big endian, so that byte order is numeric order.
std::string book_key(uint64_t value) {
    std::string key(8, '\0');
    for (std::size_t i = 0; i != 8; ++i) {
        key[i] = static_cast<char>((value >> (56 - 8 * i)) & 0xffu);
    }
    return key;
}

uint64_t book_key_value(const std::string& key) {
    uint64_t value = 0;
    for (const char byte : key) value = (value << 8) | static_cast<unsigned char>(byte);
    return value;
}

}  // namespace

// The working book's spilled rows: an ordered table keyed by priority (the
// repository's DiskIdTable, an AVL tree in a scratch file read by position, no
// resident nodes) whose values say where each row's serialized LiveRequest
// sits in an append-only blob file, plus a second table that maps the
// incarnation of a re-priced row to the priority it stands at. Entries of rows
// that came back are marked dead and skipped; the tables and the blob file are
// rebuilt from the live entries once the dead outweigh them, so the files stay
// within a constant factor of the rows they hold. A small ring keeps copies of
// the last spilled rows a reader asked for.
struct WorkingRequestCore::BookSpill {
    static constexpr std::size_t kSide = 8;
    detail::DiskIdTable<BookSlot> rows;
    detail::DiskIdTable<IncarnationSlot> links;
    ExtFile blobs;
    std::string directory;
    uint64_t alive = 0;     // rows spilled and not yet back: the book's spilled count
    uint64_t gone = 0;      // dead entries in rows
    uint64_t garbage = 0;   // blob bytes no live entry reads
    // How many times compact rebuilt the tables: a table offset taken before
    // a rebuild names nothing after it (hold_scanned). Storage metadata, like
    // the offsets themselves, so nothing folds it.
    uint64_t generation = 0;
    mutable std::array<std::shared_ptr<LiveRequest>, kSide> side{};
    mutable std::size_t side_next = 0;
    // A read failure a reader that cannot throw handed over
    // (carry_book_read_failure); every later read of these rows throws it.
    std::exception_ptr failure;
    // book_lookup_reads: rows read back and table steps walked by lookups.
    mutable uint64_t lookup_rows = 0;
    mutable uint64_t lookup_steps = 0;

    // C2 supplies the one reach predicate and its delta. Its 2^-20 relative
    // ulp margin is a chosen safety margin, not a derived bound; T8 guards it.
    using Reach = bool (*)(bool, double, double, double, double) noexcept;
    struct ResidentMeta {
        uint64_t priority = 0;
        double initial = 0.0;
        unsigned char dormant = 0;
        unsigned char cls = 0;
    };
    struct DormantSpec {
        double level = 0.0;
        double initial = 0.0;
        unsigned char cls = 0;
        bool ge = false;
    };
    struct SweepRegister {
        uint64_t seq = 0;
        uint64_t cur = 0;
        uint64_t prev = 0;
        uint64_t W = 0;
        uint64_t max_acceptance = 0;
        int64_t max_floor = std::numeric_limits<int64_t>::lowest();
        uint64_t spilled_dormant = 0;
        bool in_progress = false;
        bool guarded = false;
    };
    // the core is move-only (copy deleted), so stamps and registers need no snapshot or rollback.
    // Together they define the exact decoded allowance;
    // neither they nor the derived level index add a hashed byte.
    std::vector<ResidentMeta> resident;
    std::array<SweepRegister, 2> reg{};
    detail::CommandScratchStore* level_index = nullptr;  // borrowed run workspace
    uint64_t active_spilled = 0;
    uint64_t spilled_book_closes = 0;
    uint64_t resident_dormant = 0;
    bool middle_spill = false;  // only in an explicit candidate-free consumer scope
    bool ever_spilled = false;
    bool poison = false;  // permanent; class/active-spill guards are per point
    uint64_t poison_events = 0;
    mutable uint64_t projected_reads = 0;
    mutable uint64_t class_guard_failures = 0;

    explicit BookSpill(const std::vector<LiveRequest>& live) {
        // A fixed resident book, not a key/population admission limit. Files
        // remain unopened until the existing spill path first writes them.
        resident.reserve(kBookMax + 1);
        for (const auto& row : live) resident.push_back(active_meta(row.priority()));
    }
    static ResidentMeta active_meta(uint64_t priority) noexcept {
        ResidentMeta meta;
        meta.priority = priority;
        return meta;
    }
    void assert_index_complete() const noexcept {
#ifndef NDEBUG
        if (poison || !level_index) return;
        // Put/erase verify exact keys/incarnations; this independent census
        // closes the membership induction and catches a tolerated stale key
        // after promotion as well as a missing or duplicate admission.
        assert(!level_index->transaction_active());
        assert(level_index->record_count()
               == resident_dormant + reg[0].spilled_dormant + reg[1].spilled_dormant);
#endif
    }
    void assert_aligned(const WorkingRequestCore& core) const noexcept {
        assert(resident.size() == core.live_.size());
#ifndef NDEBUG
        for (std::size_t i = 0; i != resident.size(); ++i)
            assert(resident[i].priority == core.live_[i].priority());
#endif
        assert_index_complete();
        (void)core;
    }
    static BookSpill& ensure(WorkingRequestCore& core) {
        if (!core.book_spill_) {
            auto created = std::make_shared<BookSpill>(core.live_);
            const auto& directory = core.scratch_directory();
            if (!directory.empty()) created->set_directory(directory);
            core.book_spill_ = std::move(created);
        }
        return *core.book_spill_;
    }

    static bool dormant_class(const WorkingRequestCore& core, const LiveRequest& row,
                              DormantSpec& spec) noexcept {
        if (!row.definition || !std::holds_alternative<BookTransaction>(row.authority)
            || !std::holds_alternative<Independent>(row.request().owner)
            || !std::holds_alternative<NoGroup>(row.request().group)
            || !std::holds_alternative<Absolute>(row.request().anchor)
            || !std::holds_alternative<PendingNone>(row.pending)) return false;
        const auto& request = row.request();
        const auto* sized = as_host_sized(request.intent);
        if (!as_transact(request.intent) && !as_reverse_to(request.intent)
            && !(sized && sized->kind == HostSizedKind::Open && sized->side)) return false;
        const auto* remaining = std::get_if<RemainingUnits>(&row.remaining);
        if (!remaining || !std::isfinite(remaining->q) || !(remaining->q > 0.0)
            || !std::holds_alternative<AllowanceUnits>(row.allowance)) return false;
        const bool buy = core.working_is_buy(row);
        spec.cls = 1;
        if (const auto* limit = std::get_if<Limit>(&request.trigger)) {
            if (!std::holds_alternative<LimitReady>(row.trigger_state)) return false;
            spec.cls = 0;
            spec.level = limit->price;
            spec.ge = buy;
        } else if (const auto* stop = std::get_if<Stop>(&request.trigger)) {
            if (!std::holds_alternative<StopIdle>(row.trigger_state)) return false;
            spec.level = stop->price;
            spec.ge = !buy;
        } else if (const auto* stop_limit = std::get_if<StopLimit>(&request.trigger)) {
            if (std::holds_alternative<StopLimitPending>(row.trigger_state)) {
                spec.level = stop_limit->stop;
                spec.ge = !buy;
            } else if (std::holds_alternative<StopLimitLive>(row.trigger_state)) {
                spec.level = stop_limit->limit;
                spec.ge = buy;
            } else return false;
        } else return false;
        if (!std::isfinite(spec.level)) return false;
        const Allowance eager = core.evaluated_allowance(row, 0);
        spec.initial = std::get<AllowanceUnits>(eager).initial;
        return true;
    }

    // A representative of each equivalence class calls the BASE permission
    // function. Limit is its own class; Stop and both StopLimit legs share
    // every non-market driver permission. CurrentExecution's shape gate also
    // excludes all of them when only the provenance says CurrentExecution.
    static bool class_permitted(const WorkingRequestCore& core,
                                const EvaluationContext& context, unsigned cls) noexcept {
        const Trigger trigger = cls == 0 ? Trigger{Limit{}} : Trigger{Stop{}};
        const TriggerState state = cls == 0 ? TriggerState{LimitReady{}}
                                            : TriggerState{StopIdle{}};
        bool permitted = core.trigger_permits_driver(trigger, state, context.driver_class,
                                                     context.existing_matching_bit);
        if (context.cursor.point.provenance == NativePriceProvenance::CurrentExecution)
            permitted = false;
#ifdef PINEFORGE_TEST_MUTATION_M11
        if (context.driver_class == DriverEligibilityClass::CarriedOpen && cls == 1)
            permitted = true;
#endif
#if defined(PINEFORGE_TEST_MUTATION_M11_OBSERVED_PRINT_LIMIT) || defined(PINEFORGE_TEST_MUTATION_DRIVER_OBSERVED_LIMIT)
        if (context.driver_class == DriverEligibilityClass::ObservedPrint && cls == 0) permitted = !permitted;
#endif
#if defined(PINEFORGE_TEST_MUTATION_M11_OBSERVED_PRINT_OTHER) || defined(PINEFORGE_TEST_MUTATION_DRIVER_OBSERVED_OTHER)
        if (context.driver_class == DriverEligibilityClass::ObservedPrint && cls == 1) permitted = !permitted;
#endif
#if defined(PINEFORGE_TEST_MUTATION_M11_CARRIED_OPEN_LIMIT) || defined(PINEFORGE_TEST_MUTATION_DRIVER_CARRIED_LIMIT)
        if (context.driver_class == DriverEligibilityClass::CarriedOpen && cls == 0) permitted = !permitted;
#endif
#if defined(PINEFORGE_TEST_MUTATION_M11_CARRIED_OPEN_OTHER) || defined(PINEFORGE_TEST_MUTATION_DRIVER_CARRIED_OTHER)
        if (context.driver_class == DriverEligibilityClass::CarriedOpen && cls == 1) permitted = !permitted;
#endif
#if defined(PINEFORGE_TEST_MUTATION_M11_TICK_AFTER_CALCULATION_LIMIT) || defined(PINEFORGE_TEST_MUTATION_DRIVER_TICK_AFTER_CALC_LIMIT)
        if (context.driver_class == DriverEligibilityClass::TickAfterCalculation && cls == 0) permitted = !permitted;
#endif
#if defined(PINEFORGE_TEST_MUTATION_M11_TICK_AFTER_CALCULATION_OTHER) || defined(PINEFORGE_TEST_MUTATION_DRIVER_TICK_AFTER_CALC_OTHER)
        if (context.driver_class == DriverEligibilityClass::TickAfterCalculation && cls == 1) permitted = !permitted;
#endif
#if defined(PINEFORGE_TEST_MUTATION_M11_CONFIRMED_OPEN_LIMIT) || defined(PINEFORGE_TEST_MUTATION_DRIVER_CONFIRMED_OPEN_LIMIT)
        if (context.driver_class == DriverEligibilityClass::ConfirmedOpen && cls == 0) permitted = !permitted;
#endif
#if defined(PINEFORGE_TEST_MUTATION_M11_CONFIRMED_OPEN_OTHER) || defined(PINEFORGE_TEST_MUTATION_DRIVER_CONFIRMED_OPEN_OTHER)
        if (context.driver_class == DriverEligibilityClass::ConfirmedOpen && cls == 1) permitted = !permitted;
#endif
#if defined(PINEFORGE_TEST_MUTATION_M11_CONFIRMED_EXCURSION_LIMIT) || defined(PINEFORGE_TEST_MUTATION_DRIVER_CONFIRMED_EXCURSION_LIMIT)
        if (context.driver_class == DriverEligibilityClass::ConfirmedExcursion && cls == 0) permitted = !permitted;
#endif
#if defined(PINEFORGE_TEST_MUTATION_M11_CONFIRMED_EXCURSION_OTHER) || defined(PINEFORGE_TEST_MUTATION_DRIVER_CONFIRMED_EXCURSION_OTHER)
        if (context.driver_class == DriverEligibilityClass::ConfirmedExcursion && cls == 1) permitted = !permitted;
#endif
#if defined(PINEFORGE_TEST_MUTATION_M11_CONFIRMED_AFTER_CALCULATION_CLOSE_LIMIT) || defined(PINEFORGE_TEST_MUTATION_DRIVER_CONFIRMED_AFTER_CALC_LIMIT)
        if (context.driver_class == DriverEligibilityClass::ConfirmedAfterCalculationClose && cls == 0) permitted = !permitted;
#endif
#if defined(PINEFORGE_TEST_MUTATION_M11_CONFIRMED_AFTER_CALCULATION_CLOSE_OTHER) || defined(PINEFORGE_TEST_MUTATION_DRIVER_CONFIRMED_AFTER_CALC_OTHER)
        if (context.driver_class == DriverEligibilityClass::ConfirmedAfterCalculationClose && cls == 1) permitted = !permitted;
#endif
#if defined(PINEFORGE_TEST_MUTATION_M11_CURRENT_EXECUTION_LIMIT) || defined(PINEFORGE_TEST_MUTATION_DRIVER_CURRENT_LIMIT)
        if (context.driver_class == DriverEligibilityClass::CurrentExecution && cls == 0) permitted = !permitted;
#endif
#if defined(PINEFORGE_TEST_MUTATION_M11_CURRENT_EXECUTION_OTHER) || defined(PINEFORGE_TEST_MUTATION_DRIVER_CURRENT_OTHER)
        if (context.driver_class == DriverEligibilityClass::CurrentExecution && cls == 1) permitted = !permitted;
#endif
        return permitted;
    }
    bool fast_path_ok(const WorkingRequestCore& core,
                      const EvaluationContext& context) const noexcept {
        assert_aligned(core);
        if (poison || !level_index || active_spilled != 0
            || resident.size() != core.live_.size()) return false;
#ifndef PINEFORGE_TEST_MUTATION_M15
        if (failure) return false;
#endif
        for (unsigned cls = 0; cls != reg.size(); ++cls) {
            const auto& r = reg[cls];
            if (r.guarded && class_permitted(core, context, cls)
                && (context.cursor.point.ordinal <= r.max_acceptance
                    || context.cursor.point.effective_time_ms < r.max_floor)) {
                ++class_guard_failures;
                return false;
            }
        }
        return true;
    }

    using LevelKey = std::array<std::byte, 17>;
    using IndexValue = std::array<std::byte, 8>;
    static void store_be(std::byte* out, uint64_t word) noexcept {
        for (unsigned i = 0; i != 8; ++i)
            out[i] = static_cast<std::byte>((word >> (56 - 8 * i)) & 0xffu);
    }
    static uint64_t load_be(const std::byte* in) noexcept {
        uint64_t word = 0;
        for (unsigned i = 0; i != 8; ++i)
            word = (word << 8) | std::to_integer<unsigned char>(in[i]);
        return word;
    }
    static LevelKey level_key(const DormantSpec& spec, uint64_t priority) noexcept {
        LevelKey key{};
        key[0] = static_cast<std::byte>(2 * spec.cls + (spec.ge ? 0 : 1));
        const double level = spec.level == 0.0 ? 0.0 : spec.level;
        uint64_t bits = 0;
        std::memcpy(&bits, &level, sizeof(bits));
        uint64_t ordered = bits & (uint64_t{1} << 63) ? ~bits : bits ^ (uint64_t{1} << 63);
        if (spec.ge) ordered = ~ordered;
        store_be(key.data() + 1, ordered);
        store_be(key.data() + 9, priority);
        return key;
    }
    static double key_level(const LevelKey& key) noexcept {
        uint64_t ordered = load_be(key.data() + 1);
        if ((std::to_integer<unsigned char>(key[0]) & 1u) == 0) ordered = ~ordered;
        const uint64_t bits = ordered & (uint64_t{1} << 63)
                ? ordered ^ (uint64_t{1} << 63) : ~ordered;
        double level = 0.0;
        std::memcpy(&level, &bits, sizeof(level));
        return level;
    }
    static detail::ScratchStatus compare_keys(void*, const detail::ScratchByteSource& a,
            const detail::ScratchByteSource& b, int& order) noexcept {
        LevelKey left{}, right{};
        if (a.size() > left.size() || b.size() > right.size())
            return detail::ScratchStatus::programming_error;
        auto status = a.read(0, {left.data(), static_cast<std::size_t>(a.size())});
        if (status != detail::ScratchStatus::ok) return status;
        status = b.read(0, {right.data(), static_cast<std::size_t>(b.size())});
        if (status != detail::ScratchStatus::ok) return status;
        order = std::memcmp(left.data(), right.data(),
                            static_cast<std::size_t>(std::min(a.size(), b.size())));
        if (order == 0) order = a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
        return detail::ScratchStatus::ok;
    }
    static detail::ScratchKeyOrder index_order() noexcept {
        return {UINT64_C(0x5046424f4f4b0001), nullptr, compare_keys};
    }
    void index_failed() noexcept {
#ifndef PINEFORGE_TEST_MUTATION_M16
        if (!poison) ++poison_events;
        poison = true;
#endif
        if (level_index && level_index->transaction_active()) (void)level_index->rollback();
    }
    bool index_ok(detail::ScratchStatus status) noexcept {
        if (status == detail::ScratchStatus::ok) return true;
        index_failed();
        return false;
    }
    bool index_put(const LevelKey& key, uint64_t incarnation) noexcept {
        if (poison || !level_index) return false;
        IndexValue value{};
        store_be(value.data(), incarnation);
        const detail::ScratchSpanSource key_source({key.data(), key.size()});
        const detail::ScratchSpanSource value_source({value.data(), value.size()});
        detail::ScratchHandle handle = detail::scratch_no_handle;
        return index_ok(level_index->begin())
            && index_ok(level_index->put(key_source, value_source, handle))
            && index_ok(level_index->commit(detail::ScratchDurability::buffered));
    }
    bool index_record(detail::ScratchHandle handle, LevelKey& key,
                      IndexValue& value) noexcept {
        detail::ScratchRecordInfo info;
        if (!index_ok(level_index->point(handle, info))) return false;
        if (info.key_bytes != key.size() || info.value_bytes != value.size()) {
            index_failed();
            return false;
        }
        return index_ok(level_index->read(handle, detail::ScratchField::key, 0,
                                          {key.data(), key.size()}))
            && index_ok(level_index->read(handle, detail::ScratchField::value, 0,
                                          {value.data(), value.size()}));
    }
    void erase_index(const WorkingRequestCore& core, const LiveRequest& row) noexcept {
        if (poison || !level_index) return;
        DormantSpec spec;
        if (!dormant_class(core, row, spec)) { index_failed(); return; }
        const LevelKey key = level_key(spec, row.priority());
        const detail::ScratchSpanSource source({key.data(), key.size()});
        detail::ScratchHandle handle = detail::scratch_no_handle;
        if (!index_ok(level_index->lower_bound(source, handle))) return;
        LevelKey actual{};
        IndexValue value{};
        if (handle == detail::scratch_no_handle) { index_failed(); return; }
        if (!index_record(handle, actual, value)) return;
        if (actual != key || load_be(value.data()) != row.handle().incarnation) {
            index_failed();
            return;
        }
        if (!index_ok(level_index->begin())) return;
        if (!index_ok(level_index->erase(handle))) return;
        (void)index_ok(level_index->commit(detail::ScratchDurability::buffered));
    }

    static bool join(WorkingRequestCore& core, const RequestHandle& handle,
                     const EvaluationContext& context,
                     double lo, double hi, double delta, Reach reachable) {
        if (!core.book_spill_ || !names_run(handle, core.identity_)) return false;
        auto& book = *core.book_spill_;
        if (!book.fast_path_ok(core, context) || !reachable) return false;
        const std::size_t at = core.repriced_.empty()
                ? book_position<false>(core.live_, handle.incarnation)
                : core.repriced_position(handle.incarnation);
        if (at == core.live_.size() || book.resident[at].dormant) return false;
        const auto& row = core.live_[at];
        DormantSpec spec;
        if (!dormant_class(core, row, spec) || !class_permitted(core, context, spec.cls)
            || !point_eligible(row.birth(), context.cursor.point.ordinal,
                               context.cursor.point.effective_time_ms)
            || !std::isfinite(lo) || !std::isfinite(hi) || lo > hi
            || !std::isfinite(delta) || delta < 0.0
            || reachable(spec.ge, spec.level, lo, hi, delta)) return false;
        const auto& allowance = std::get<AllowanceUnits>(row.allowance);
        if (allowance.point_ordinal != context.cursor.point.ordinal
            || allowance.initial != spec.initial || allowance.left != spec.initial) return false;
        // The first join can create the pimpl after the point's begin bridge.
        // No older dormant row exists then; subsequent begin calls are no-ops.
        book.begin(core, context);
        if (!book.index_put(level_key(spec, row.priority()), handle.incarnation)) return false;
        ResidentMeta meta;
        meta.priority = row.priority();
        meta.initial = spec.initial;
        meta.dormant = 1;
        meta.cls = spec.cls;
        book.resident[at] = meta;
        ++book.resident_dormant;
        auto& r = book.reg[spec.cls];
        r.max_acceptance = std::max(r.max_acceptance, row.birth().acceptance_ordinal);
        r.max_floor = std::max(r.max_floor, row.birth().decision_time_lower_bound);
        r.guarded = true;
        book.assert_aligned(core);
        return true;
    }
    void wake_resident(WorkingRequestCore& core, std::size_t at) noexcept {
        assert_aligned(core);
        if (!resident[at].dormant) return;
        erase_index(core, core.live_[at]);
        resident[at].dormant = 0;
        --resident_dormant;
        assert_index_complete();
    }
    void wake_on_classify(WorkingRequestCore& core, std::size_t at) noexcept {
#ifndef PINEFORGE_TEST_MUTATION_M12
        wake_resident(core, at);
#else
        (void)core; (void)at;
#endif
        assert(!resident[at].dormant);
    }
    uint64_t wake_by_price(WorkingRequestCore& core, const EvaluationContext& context,
                          double lo, double hi, double delta, Reach reachable) {
        if (!fast_path_ok(core, context) || !reachable) return 0;
        uint64_t woken = 0;
        for (unsigned list = 0; list != 4; ++list) {
            if (!class_permitted(core, context, list / 2)) continue;
            const std::byte prefix = static_cast<std::byte>(list);
            const detail::ScratchSpanSource probe({&prefix, 1});
            while (fast_path_ok(core, context)) {
                detail::ScratchHandle handle = detail::scratch_no_handle;
                const auto status = level_index->lower_bound(probe, handle);
                if (status == detail::ScratchStatus::end) break;
                if (!index_ok(status)) return woken;
                if (handle == detail::scratch_no_handle) break;
                LevelKey key{};
                IndexValue value{};
                if (!index_record(handle, key, value)) return woken;
                if (key[0] != prefix) break;
                if (!reachable((list & 1u) == 0, key_level(key), lo, hi, delta)) break;
                const uint64_t priority = load_be(key.data() + 9);
                const uint64_t incarnation = load_be(value.data());
                const std::size_t at = book_position<true>(core.live_, priority);
                if (at != core.live_.size()) {
                    if (core.live_[at].handle().incarnation != incarnation
                        || !resident[at].dormant) { index_failed(); return woken; }
                    wake_resident(core, at);
                } else {
                    std::size_t promoted = 0;
                    try {
                        if (core.promote_book_row(incarnation, &promoted)) {
                            ++woken;
                            continue;
                        }
                    } catch (const std::bad_alloc&) {
                        // Admission/waking is derived work. A failed decode
                        // or reservation leaves eager matching authoritative.
                        index_failed();
                        return woken;
                    }
                    index_failed();
                    return woken;
                }
                ++woken;
            }
        }
        return woken;
    }

    void begin(WorkingRequestCore& core, const EvaluationContext& context) {
        assert_aligned(core);
        for (unsigned cls = 0; cls != reg.size(); ++cls) {
            if (!class_permitted(core, context, cls)) continue;
            auto& r = reg[cls];
            const uint64_t point = context.cursor.point.ordinal;
            if (r.cur == point) continue;
            if (r.seq == std::numeric_limits<uint64_t>::max())
                throw coded<std::overflow_error>(RunFailureCode::engine_invariant, {},
                                                  "native dormant sweep sequence exhausted");
            if (r.spilled_dormant != 0
                && core.epoch_ > std::numeric_limits<uint64_t>::max() - 2)
                throw coded<std::overflow_error>(RunFailureCode::engine_invariant, {},
                                                  "native working-request epoch exhausted");
            ++r.seq;
            r.prev = r.cur;
            r.cur = point;
            r.W = 0;
            r.in_progress = true;
            if (r.spilled_dormant != 0) core.epoch_ += 2;
        }
        mutate_keepalive(core);
    }
    void sweep(WorkingRequestCore& core, uint64_t priority, bool finish) {
        assert_aligned(core);
        uint64_t first_priority = std::numeric_limits<uint64_t>::max();
        bool pending = false;
        for (const auto& r : reg) {
            if (!r.in_progress) continue;
            first_priority = std::min(first_priority, r.W);
            pending = true;
        }
        if (!pending) return;
        const auto first = std::lower_bound(resident.begin(), resident.end(), first_priority,
                [](const ResidentMeta& meta, uint64_t value) { return meta.priority < value; });
        // Only the not-yet-swept interval, in priority order across both
        // classes. Earlier winners' prefixes are never walked again.
        for (std::size_t i = static_cast<std::size_t>(first - resident.begin());
             i != resident.size() && (finish || resident[i].priority < priority); ++i) {
            const auto& meta = resident[i];
            if (!meta.dormant) continue;
            const auto& r = reg[meta.cls];
            if (!r.in_progress || meta.priority < r.W
                || (!finish && meta.priority >= priority)) continue;
            auto& allowance = core.live_[i].allowance;
            const auto* units = std::get_if<AllowanceUnits>(&allowance);
            if (units && units->point_ordinal == r.cur) continue;
            if (core.epoch_ > std::numeric_limits<uint64_t>::max() - 2)
                throw coded<std::overflow_error>(RunFailureCode::engine_invariant, {},
                                                  "native working-request epoch exhausted");
            allowance = AllowanceUnits{r.cur, meta.initial, meta.initial};
#ifdef PINEFORGE_TEST_MUTATION_M18
            ++core.epoch_;
#else
            core.epoch_ += 2;
#endif
        }
        for (auto& r : reg) {
            if (r.in_progress) r.W = finish ? std::numeric_limits<uint64_t>::max()
                                           : std::max(r.W, priority);
        }
        mutate_keepalive(core);
    }
    void advance(WorkingRequestCore& core, uint64_t priority) { sweep(core, priority, false); }
    void complete(WorkingRequestCore& core, bool finish_sweep) {
        // Failure/unwinding leaves the partially refreshed eager book intact,
        // including W and in_progress: readers can still observe that prefix.
        if (!finish_sweep) return;
        sweep(core, std::numeric_limits<uint64_t>::max(), true);
        for (auto& r : reg) r.in_progress = false;
        mutate_keepalive(core);
    }
    void project(const WorkingRequestCore& core, const BookSlot& slot,
                 LiveRequest& row) const noexcept {
        if ((slot.dormancy & 1u) == 0) return;
        const auto& r = reg[(slot.dormancy >> 1) & 1u];
        if (slot.stamp >= r.seq) return;
        uint64_t point = r.cur;
#ifndef PINEFORGE_TEST_MUTATION_M17
        if (r.in_progress && row.priority() >= r.W) {
            if (slot.stamp >= r.seq - 1) return;
            point = r.prev;
        }
#endif
        const Allowance eager = core.evaluated_allowance(row, point);
        const auto& expected = std::get<AllowanceUnits>(eager);
        const auto* before = std::get_if<AllowanceUnits>(&row.allowance);
        if (!before || before->point_ordinal != expected.point_ordinal
            || before->initial != expected.initial || before->left != expected.left)
            ++projected_reads;
        row.allowance = eager;
    }
    // Only deliberately bad mutation builds touch held copies. In production
    // this is empty: the side ring is a KEEPALIVE, never a lookup cache, and
    // a copy held across a watermark change stays frozen exactly as in base.
    void mutate_keepalive(const WorkingRequestCore& core) noexcept {
#ifdef PINEFORGE_TEST_MUTATION_M25
        for (auto& held : side) {
            DormantSpec spec;
            if (held && dormant_class(core, *held, spec))
                held->allowance = core.evaluated_allowance(*held, reg[spec.cls].cur);
        }
#endif
#ifdef PINEFORGE_TEST_MUTATION_M26
        side = {};
#endif
        (void)core;
    }

    std::size_t spill_index(const WorkingRequestCore& core) const noexcept {
        assert_aligned(core);
        // K1: retained winners/candidates keep the back-only contract. Middle
        // erasure requires a consumer scope with no retained resident alias;
        // completing an allowance sweep alone never grants that scope.
#ifdef PINEFORGE_TEST_MUTATION_M24
        const bool may_erase = true;
#else
        const bool may_erase = middle_spill;
#endif
        if (may_erase)
            for (std::size_t i = resident.size(); i != 0; --i)
                if (resident[i - 1].dormant) return i - 1;
        return core.live_.size() - 1;
    }
    void drop_spilled(WorkingRequestCore& core, std::size_t at) noexcept {
        const ResidentMeta meta = resident[at];
        if (meta.dormant) {
            --resident_dormant;
            ++reg[meta.cls].spilled_dormant;
        } else ++active_spilled;
        if (std::holds_alternative<BookClose>(core.live_[at].authority))
            ++spilled_book_closes;
#ifdef PINEFORGE_TEST_MUTATION_M24
        core.live_.erase(core.live_.begin() + static_cast<std::ptrdiff_t>(at));
        resident.erase(resident.begin() + static_cast<std::ptrdiff_t>(at));
#else
        if (at + 1 == core.live_.size()) {
            core.live_.pop_back();
            resident.pop_back();
        } else {
            assert(middle_spill);
            core.live_.erase(core.live_.begin() + static_cast<std::ptrdiff_t>(at));
            resident.erase(resident.begin() + static_cast<std::ptrdiff_t>(at));
        }
#endif
        ever_spilled = true;
        ++alive;
        assert_aligned(core);
    }
    void promoted(WorkingRequestCore& core, const BookSlot& slot, std::size_t at) noexcept {
        const auto& row = core.live_[at];
        if (std::holds_alternative<BookClose>(row.authority)) --spilled_book_closes;
        if ((slot.dormancy & 1u) != 0) {
#ifndef PINEFORGE_TEST_MUTATION_M3
            erase_index(core, row);
#endif
#ifndef PINEFORGE_TEST_MUTATION_M21
            --reg[(slot.dormancy >> 1) & 1u].spilled_dormant;
#endif
        } else --active_spilled;
#ifndef PINEFORGE_TEST_MUTATION_M4
        resident.insert(resident.begin() + static_cast<std::ptrdiff_t>(at),
                        active_meta(row.priority()));
#endif
        assert_aligned(core);
    }
    void pushed(const WorkingRequestCore& core) noexcept {
        resident.push_back(active_meta(core.live_.back().priority()));
        assert_aligned(core);
    }
    void erasing(std::size_t at) noexcept {
        assert(!resident[at].dormant);
#ifndef PINEFORGE_TEST_MUTATION_M5
        resident.erase(resident.begin() + static_cast<std::ptrdiff_t>(at));
#else
        (void)at;
#endif
    }
    void replacing(std::size_t at, uint64_t priority) noexcept {
        assert(!resident[at].dormant);
#ifndef PINEFORGE_TEST_MUTATION_M13
        std::rotate(resident.begin() + static_cast<std::ptrdiff_t>(at),
                    resident.begin() + static_cast<std::ptrdiff_t>(at + 1), resident.end());
        resident.back() = active_meta(priority);
#else
        (void)at; (void)priority;
#endif
    }
    bool scan_spilled_closes() const noexcept {
#ifdef PINEFORGE_TEST_MUTATION_M14
        return false;
#else
        return spilled_book_closes != 0;
#endif
    }
    bool carries_read_failure() const noexcept {
        return ever_spilled;
    }

    // Throws the carried failure, if any: an I/O failure as the typed
    // scratch failure (scratch_io_failed), anything else as it was thrown.
    void check() const {
        if (!failure) return;
        try {
            std::rethrow_exception(failure);
        } catch (const detail::AuxiliaryScratchIoError&) {
            throw;
        } catch (const std::system_error& error) {
            throw detail::AuxiliaryScratchIoError(error);
        }
    }

    void set_directory(const std::string& value) {
        directory = value;
        rows.set_directory(value);
        links.set_directory(value);
        blobs.set_directory(value);
    }
    // Writes `row` out and files it under its priority. The caller drops the
    // resident row once this has returned.
    void spill(const LiveRequest& row, const ResidentMeta& meta) {
        ByteWriter out;
        SaveAr ar{out};
        ar(row);
        const uint64_t priority = row.priority();
        const uint64_t incarnation = row.handle().incarnation;
        BookSlot slot;
        slot.offset = blobs.append(out.data(), out.size());
        slot.length = out.size();
        slot.incarnation = incarnation;
        if (meta.dormant) {
            const auto& r = reg[meta.cls];
            slot.dormancy = 1u | (uint64_t{meta.cls} << 1);
            slot.stamp = r.seq;
            if (r.in_progress && priority >= r.W && r.seq != 0) {
#ifndef PINEFORGE_TEST_MUTATION_M2
                slot.stamp = r.seq - 1;
#endif
            }
        }
#ifdef PINEFORGE_TEST_MUTATION_M1
        slot.stamp = slot.dormancy = 0;
#endif
        if (priority != incarnation) {
            IncarnationSlot link;
            link.priority = priority;
            const auto linked = links.insert(book_key(incarnation), link);
            if (!linked.second) links.set(linked.first, link);
        }
        const auto placed = rows.insert(book_key(priority), slot);
        if (!placed.second) {
            // The row was spilled before and came back: its entry is revived.
            const BookSlot old = rows.get(placed.first);
            rows.set(placed.first, slot);
            if (old.state != 0) --gone;
            else garbage += old.length;
        }
    }
    LiveRequest read(const WorkingRequestCore& core, const BookSlot& slot) const {
        std::vector<unsigned char> bytes(static_cast<std::size_t>(slot.length));
        blobs.read(slot.offset, bytes.data(), bytes.size());
        ByteReader in(bytes.data(), bytes.size());
        LoadAr ar{in};
        LiveRequest row;
        ar(row);
        project(core, slot, row);
        if (!row.definition || !core.journal_ || core.journal_->codec) return row;
        const JournalStore& journal = *core.journal_;
        // Restrict the existing ordinal bisection to resident events: no cold
        // journal reads or new index. One O(log kMaxResident) lookup per book
        // decode, then O(definition bytes) equality, never a per-child scan.
        const auto position = journal.find_ordinal(row.birth().acceptance_ordinal, journal.first);
        if (!position) return row;
        const CommandEvent& event = journal.resident_at(*position);
        const DefinitionRef* resident = nullptr;
        if (const auto* accepted = std::get_if<AcceptedEvent>(&event)) {
            resident = &accepted->definition;
        } else if (const auto* replaced = std::get_if<ReplacedEvent>(&event)) {
            // A kept-handle re-price has the same handle but a new birth and
            // priority: only the successor can describe this book version.
            resident = &replaced->successor_definition;
        }
        if (!resident || !*resident || (*resident)->handle != row.handle()
            || (*resident)->birth != row.birth()
            || (*resident)->priority != row.definition->priority) return row;
        // LiveRequest's first field is its definition. Compare that entire
        // serialized prefix, including request (an owner's external openings
        // among it, handle by handle), predecessor, origin, root and
        // kept_binding; the decoded owner survives every mismatch unchanged.
        DefinitionMatchAr match{bytes.data(), bytes.size()};
        match(*resident);
        if (match.matched) row.definition = *resident;
        return row;
    }
    // Replaces the serialized row of the live entry at `at`, keeping its
    // priority. Bytes of the same length (a refreshed allowance usually is)
    // overwrite the old ones in place; anything else is appended and the
    // entry points at it, the old bytes becoming garbage.
    void rewrite(uint64_t at, BookSlot slot, const LiveRequest& row) {
        assert((slot.dormancy & 1u) == 0);  // only BookClose refreshes rewrite
        ByteWriter out;
        SaveAr ar{out};
        ar(row);
        if (out.size() == slot.length) {
            blobs.write(slot.offset, out.data(), out.size());
            return;
        }
        const uint64_t old_length = slot.length;
        slot.offset = blobs.append(out.data(), out.size());
        slot.length = out.size();
        rows.set(at, slot);
        garbage += old_length;
    }
    // The spilled row filed under `priority`.
    bool find(uint64_t priority, BookSlot* slot, uint64_t* at) const {
        const auto found = rows.find(book_key(priority));
        if (!found) return false;
        const BookSlot value = rows.get(*found);
        if (value.state != 0) return false;
        *slot = value;
        *at = *found;
        return true;
    }
    uint64_t priority_of(uint64_t incarnation) const {
        const auto found = links.find(book_key(incarnation));
        if (found) {
            const IncarnationSlot link = links.get(*found);
            if (link.state == 0) return link.priority;
        }
        return incarnation;
    }
    // The entry at `at` stops being a spilled row: it came back, or it ended.
    // Two steps, so the caller can make its own change between them. The row
    // entry is the one that publishes: retire_row writes first and may throw
    // while the caller has changed nothing; retired then counts the dead entry
    // and tombstones the link of a re-priced row. A link left standing by a
    // failed write is harmless: a lookup through it names a priority whose row
    // entry is dead, and BookSpill::find answers no row.
    void retire_row(uint64_t at, BookSlot slot) {
        slot.state = 1;
        rows.set(at, slot);
    }
    void retired(const BookSlot& slot, uint64_t priority) noexcept {
        garbage += slot.length;
        ++gone;
        if (priority == slot.incarnation) return;
        try {
            const auto found = links.find(book_key(slot.incarnation));
            if (found) {
                IncarnationSlot link = links.get(*found);
                link.state = 1;
                links.set(*found, link);
            }
        } catch (...) {
        }
    }
    uint64_t alive_from(uint64_t at) const {
        while (at != 0 && rows.get(at).state != 0) at = rows.next_sorted(at);
        return at;
    }
    uint64_t first_alive() const { return alive_from(rows.first_sorted()); }
    uint64_t next_alive(uint64_t at) const { return alive_from(rows.next_sorted(at)); }
    uint64_t priority_at(uint64_t at) const { return book_key_value(rows.key(at)); }
    // Keeps a copy of a spilled row alive for a reader; the last kSide copies
    // stay valid. keep hands the copy to a reader that holds it itself (the
    // ring and the reader share it, so it lives until both let go); stash
    // answers a bare pointer.
    std::shared_ptr<LiveRequest> keep(LiveRequest row) const {
        auto kept = std::make_shared<LiveRequest>(std::move(row));
        side[side_next] = kept;
        side_next = (side_next + 1) % side.size();
        return kept;
    }
    const LiveRequest* stash(LiveRequest row) const { return keep(std::move(row)).get(); }
    bool worth_compacting() const {
        return gone > 2 * alive + 64 || garbage > blobs.size() / 2 + (uint64_t{1} << 20);
    }
    // Rebuilds both tables and the blob file from the entries that still
    // stand. Table offsets taken before are no longer valid.
    void compact() {
        detail::DiskIdTable<BookSlot> fresh_rows;
        detail::DiskIdTable<IncarnationSlot> fresh_links;
        ExtFile fresh_blobs;
        if (!directory.empty()) {
            fresh_rows.set_directory(directory);
            fresh_links.set_directory(directory);
            fresh_blobs.set_directory(directory);
        }
        for (uint64_t at = first_alive(); at != 0; at = next_alive(at)) {
            BookSlot slot = rows.get(at);
            std::vector<unsigned char> bytes(static_cast<std::size_t>(slot.length));
            blobs.read(slot.offset, bytes.data(), bytes.size());
            slot.offset = fresh_blobs.append(bytes.data(), bytes.size());
            const std::string key = rows.key(at);
            const uint64_t priority = book_key_value(key);
            fresh_rows.insert(key, slot);
            if (priority != slot.incarnation) {
                IncarnationSlot link;
                link.priority = priority;
                fresh_links.insert(book_key(slot.incarnation), link);
            }
        }
        rows = std::move(fresh_rows);
        links = std::move(fresh_links);
        blobs = std::move(fresh_blobs);
        gone = 0;
        garbage = 0;
        ++generation;
    }
};
