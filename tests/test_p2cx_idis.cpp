// CR-312 V3 prototype (not for merge): TradeLedger::entry_id_equals and
// BacktestEngine::closed_trade_entry_id_is.
//
// The oracle is never typed in. For every ledger row and every probe the candidate must equal
//     entry_id_at(i)            == std::string(text, length)   (ledger)
//     closed_trade_entry_id(i)  == std::string(text, length)   (engine host)
// where both sides are the EXISTING accessors; the id the test pushed into the row is a second,
// independent witness. The candidate is called first on each row, so a cold row is read by the
// candidate itself (the miss path) before the oracle warms it.
#include "p2cx_tledger_fixture.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <vector>

// Executable-local allocation counter, the allocation half of the idiom in
// test_p2cx_tledger_io.cpp: real storage still comes from malloc and production code has no
// counter, branch or hook. The nothrow forms are replaced too, so no block from a sanitizer's
// own new is released by the free() below.
namespace idis_alloc {
inline bool counting = false;
inline std::size_t count = 0;
} // namespace idis_alloc

void* operator new(std::size_t bytes) {
    void* result = std::malloc(bytes ? bytes : 1);
    if (!result) throw std::bad_alloc();
    if (idis_alloc::counting) ++idis_alloc::count;
    return result;
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    void* result = std::malloc(bytes ? bytes : 1);
    if (result && idis_alloc::counting) ++idis_alloc::count;
    return result;
}
void* operator new[](std::size_t bytes, const std::nothrow_t& tag) noexcept {
    return ::operator new(bytes, tag);
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }

namespace {
using namespace p2cx_tledger;
using Size = TradeLedger::size_type;

constexpr Size kRows = 300; // above 256: budget 128 collides, and 64 rows leave the resident window

struct Counted {
    Counted() {
        REQUIRE(!idis_alloc::counting);
        idis_alloc::count = 0;
        idis_alloc::counting = true;
    }
    ~Counted() { idis_alloc::counting = false; }
    Size finish() {
        idis_alloc::counting = false;
        return idis_alloc::count;
    }
};

// closed_trade_* are protected engine accessors: reach them through a derived host, as the
// fixture's Reader does for the existing ones.
struct IdHost : Reader {
    bool id_is(int at, const char* text, std::size_t length) const {
        return closed_trade_entry_id_is(at, text, length);
    }
    std::string id_of(int at) const { return closed_trade_entry_id(at); }
};

// The four strings of a row. remember() packs them into kCacheStringBytes: one byte more and the
// cache entry keeps only the numeric record and the id accessors fall back to get_string().
struct Shape {
    std::string id, entry_comment, exit_id, exit_comment;
    bool fits() const {
        return id.size() + entry_comment.size() + exit_id.size() + exit_comment.size()
            <= TradeLedger::kCacheStringBytes;
    }
};

// Position-dependent bytes (never constant), so a compare that stops early or reads the wrong
// offset cannot be right by accident. For equal `first`, pattern(127) is a prefix of pattern(128),
// which is a prefix of pattern(129).
std::string pattern(Size bytes, char first) {
    std::string text(bytes, '\0');
    for (Size k = 0; k < bytes; ++k) text[k] = static_cast<char>(first + static_cast<char>(k % 23));
    return text;
}

Trade make(int n, const Shape& shape) {
    Trade value = row(n);
    value.entry_id = shape.id;
    value.entry_comment = shape.entry_comment;
    value.exit_id = shape.exit_id;
    value.exit_comment = shape.exit_comment;
    return value;
}

// 23 shapes (prime, so any odd stride over them visits each of them). Where a comment follows
// the id in the packed buffer, it is chosen to spell a longer or shorter probe together with the id.
std::vector<Shape> shapes() {
    const std::size_t cap = TradeLedger::kCacheStringBytes;
    const std::string nul_mid("Lo\0ng", 5);
    const std::string nul_end("Long\0", 5);
    const std::string nul_only("\0", 1);
    const std::string nul_ext("Long\0er", 7);
    return {
        {"Long", "entry", "x1", ""},
        {"Short", "entry", "x2", ""},
        {"", "Long", "", ""},                                    // empty id; the next string spells "Long"
        {"L", "ong", "", ""},                                    // packed "Long", id only "L"
        {"Longer", "", "", ""},
        {"Long", "er", "", ""},                                  // packed "Longer", id only "Long"
        {nul_mid, "", "", ""},
        {nul_end, "", "", ""},
        {nul_only, "", "", ""},
        {nul_ext, "x", "", ""},
        {pattern(cap - 1, 'a'), "", "", ""},                     // 127 bytes: fits
        {pattern(cap, 'a'), "", "", ""},                         // 128 bytes: fits, buffer exactly full
        {pattern(cap + 1, 'a'), "", "", ""},                     // 129 bytes: does not fit
        {pattern(5000, 'a'), "", "", ""},                        // far wider than the cache and byte tail
        {"Long", std::string(cap - 4, 'E'), "", ""},             // four strings total 128: fits
        {"Long", std::string(cap - 3, 'E'), "", ""},             // total 129: short id, does not fit
        {"Long", std::string(60, 'E'), "x", std::string(100, 'F')},
        {"", std::string(200, 'E'), "", ""},                     // empty id, does not fit
        {"Short", "", "", std::string(200, 'F')},                // the exit comment overflows
        {pattern(100, 'b'), "", "", ""},                         // 100 bytes: fits, beyond any small-string buffer
        {pattern(100, 'b'), std::string(cap - 99, 'E'), "", ""}, // 100 + 29 = 129: does not fit
        {"long", "entry", "x3", ""},
        {"Lon", "entry", "x4", ""},
    };
}

void add_unique(std::vector<std::string>& ids, const std::string& id) {
    if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
}

std::vector<std::string> unique_ids(const std::vector<Shape>& list) {
    std::vector<std::string> ids;
    for (const auto& item : list) add_unique(ids, item.id);
    return ids;
}

struct Probe {
    std::string text;
    bool null_text = false; // hand the function a null pointer (length 0)
    const char* data() const { return null_text ? nullptr : text.data(); }
    std::size_t size() const { return text.size(); }
};

std::string flipped(std::string text, Size at) {
    text[at] = static_cast<char>(text[at] ^ 1);
    return text;
}

// Every id against every other, and around each id: longer by a byte or a NUL, a proper prefix,
// and one byte changed at the start, middle and end (equal length, so only a full compare sees it,
// including a byte after an embedded NUL).
std::vector<Probe> probes_for(const std::vector<std::string>& ids) {
    std::vector<Probe> out;
    const auto add = [&out](std::string text) { out.push_back(Probe{std::move(text)}); };
    for (const auto& id : ids) {
        add(id);
        add(id + "x");
        add(id + std::string(1, '\0'));
        if (id.empty()) continue;
        add(id.substr(0, id.size() - 1));
        add(flipped(id, 0));
        add(flipped(id, id.size() / 2));
        add(flipped(id, id.size() - 1));
    }
    add(std::string(1, '\0'));
    add(" Long");
    add("Long ");
    add("LONG");
    add("Longest");
    out.push_back(Probe{std::string(), true});
    return out;
}

// A host plus what each global row is supposed to hold.
struct Book {
    IdHost host;
    std::vector<Shape> shape;
    explicit Book(Size budget) {
        host.ledger().set_scratch_directory(".");
        host.ledger().set_cache_rows(budget);
    }
    void push(const Shape& next) {
        host.ledger().push_back(make(static_cast<int>(shape.size()), next));
        shape.push_back(next);
    }
    void rewind(Size end) {
        host.ledger().resize(end);
        shape.resize(end);
    }
    void grow_default(Size count) {
        host.ledger().resize(host.ledger().size() + count);
        shape.resize(host.ledger().size()); // resize() appends default rows: no strings
    }
};

void agree(const char* what, Size at, Size probe, bool got, bool want) {
    static int shown = 0;
    if (got != want && shown++ < 24)
        std::printf("  %s: row %zu probe %zu: got %d, oracle %d\n", what, at, probe,
                    static_cast<int>(got), static_cast<int>(want));
    CHECK(got == want);
}

// Candidate first (so it is the one that finds the row cold), then both oracles. Returns how many
// probes the candidate said yes to.
template<class Is, class IdOf>
Size check_row(const char* what, Size at, const std::string& pushed_id,
               const std::vector<Probe>& probes, Is candidate, IdOf oracle_id) {
    std::vector<char> got(probes.size());
    for (Size p = 0; p < probes.size(); ++p) got[p] = candidate(at, probes[p]) ? 1 : 0;
    const std::string stored = oracle_id(at);
    CHECK(stored == pushed_id);
    Size yes = 0;
    for (Size p = 0; p < probes.size(); ++p) {
        agree(what, at, p, got[p] != 0, stored == probes[p].text);
        agree("pushed", at, p, got[p] != 0, pushed_id == probes[p].text);
        yes += got[p] != 0 ? 1 : 0;
    }
    return yes;
}

Size check_ledger_row(Book& book, Size at, const std::vector<Probe>& probes) {
    auto& ledger = book.host.ledger();
    return check_row("ledger", at, book.shape[at].id, probes,
                     [&ledger](Size r, const Probe& p) {
                         return ledger.entry_id_equals(r, p.data(), p.size());
                     },
                     [&ledger](Size r) { return ledger.entry_id_at(r); });
}

Size check_engine_row(Book& book, Size at, const std::vector<Probe>& probes) {
    IdHost& host = book.host;
    return check_row("engine", at, book.shape[at].id, probes,
                     [&host](Size r, const Probe& p) {
                         return host.id_is(static_cast<int>(r), p.data(), p.size());
                     },
                     [&host](Size r) { return host.id_of(static_cast<int>(r)); });
}

// Every row of the unsealed window, through both entry points.
void verify(Book& book, const std::vector<Probe>& probes) {
    const auto& ledger = book.host.ledger();
    REQUIRE(ledger.size() == book.shape.size());
    for (Size i = ledger.base(); i < ledger.size(); ++i) {
        check_ledger_row(book, i, probes);
        check_engine_row(book, i, probes);
    }
}

// The addition is additive: the existing string accessors still return what was pushed.
void unchanged(const Book& book) {
    const auto& ledger = book.host.ledger();
    for (Size i = ledger.base(); i < ledger.size(); ++i)
        same_string_fields(ledger, i, make(static_cast<int>(i), book.shape[i]));
}

std::vector<Size> order_forward(Size n) {
    std::vector<Size> out(n);
    for (Size i = 0; i < n; ++i) out[i] = i;
    return out;
}
std::vector<Size> order_backward(Size n) {
    std::vector<Size> out(n);
    for (Size i = 0; i < n; ++i) out[i] = n - 1 - i;
    return out;
}
std::vector<Size> order_strided(Size n, Size stride) {
    std::vector<Size> out(n);
    std::vector<char> seen(n, 0);
    for (Size i = 0; i < n; ++i) {
        out[i] = i * stride % n;
        seen[out[i]] = 1;
    }
    REQUIRE(std::count(seen.begin(), seen.end(), char{1}) == static_cast<std::ptrdiff_t>(n));
    return out;
}

// Text of the coded logic_error the action throws; a return without a throw is a failure.
template<class F>
std::string refusal_text(F action) {
    try {
        action();
    } catch (const coded<std::logic_error>& error) {
        CHECK(classify_run_failure(error).code == RunFailureCode::engine_invariant);
        return error.what();
    }
    CHECK(false); // returned instead of refusing
    return std::string("<returned>");
}

// The candidate must refuse exactly as entry_id_at(): same type, code and text. Three probes, the
// first being the row's own id (so a candidate served from a still-valid cache entry would say yes).
void ledger_refuses_alike(const TradeLedger& ledger, Size at, const std::string& id) {
    const auto want = refusal_text([&] { (void)ledger.entry_id_at(at); });
    const auto exact = refusal_text([&] { (void)ledger.entry_id_equals(at, id.data(), id.size()); });
    const auto empty = refusal_text([&] { (void)ledger.entry_id_equals(at, nullptr, 0); });
    const auto other = refusal_text([&] { (void)ledger.entry_id_equals(at, "?", 1); });
    CHECK(exact == want);
    CHECK(empty == want);
    CHECK(other == want);
}

void engine_refuses_alike(const IdHost& host, int at, const std::string& id) {
    const auto want = refusal_text([&] { (void)host.id_of(at); });
    const auto got = refusal_text([&] { (void)host.id_is(at, id.data(), id.size()); });
    CHECK(got == want);
}

// ---------------------------------------------------------------------------------------------

void catalog_classes() {
    const auto pool = shapes();
    const std::size_t cap = TradeLedger::kCacheStringBytes;
    const auto count = [&pool](auto predicate) {
        Size n = 0;
        for (const auto& item : pool) n += predicate(item) ? 1 : 0;
        return n;
    };
    // Each class below is a code path of entry_id_equals; a row class that is not in the catalog
    // is a path the other scenarios would not reach.
    const Size full = count([&](const Shape& s) { return s.fits() && s.id.size() == cap; });
    const Size below = count([&](const Shape& s) { return s.fits() && s.id.size() == cap - 1; });
    const Size above = count([&](const Shape& s) { return !s.fits() && s.id.size() == cap + 1; });
    const Size wide = count([&](const Shape& s) { return !s.fits() && s.id.size() >= 5000; });
    const Size hit_long = count([&](const Shape& s) { return s.fits() && s.id.size() >= 24; });
    const Size miss_short = count([&](const Shape& s) {
        return !s.fits() && !s.id.empty() && s.id.size() <= 15;
    });
    const Size miss_empty = count([&](const Shape& s) { return !s.fits() && s.id.empty(); });
    const Size miss_mid = count([&](const Shape& s) {
        return !s.fits() && s.id.size() > 15 && s.id.size() <= cap;
    });
    const Size nul = count([&](const Shape& s) { return s.id.find('\0') != std::string::npos; });
    const Size exactly = count([&](const Shape& s) {
        return s.fits() && !s.id.empty() && s.id.size() + s.entry_comment.size() == cap;
    });
    const Size one_over = count([&](const Shape& s) {
        return !s.fits() && !s.id.empty() && s.id.size() + s.entry_comment.size() == cap + 1
            && s.exit_id.empty() && s.exit_comment.empty();
    });
    CHECK(full >= 1);
    CHECK(below >= 1);
    CHECK(above >= 1);
    CHECK(wide >= 1);
    CHECK(hit_long >= 1);
    CHECK(miss_short >= 1);
    CHECK(miss_empty >= 1);
    CHECK(miss_mid >= 1);
    CHECK(nul >= 1);
    CHECK(exactly >= 1);
    CHECK(one_over >= 1);
}

// kRows rows, every probe, three visiting orders, and an interleaved pass that misses on every
// access when the budget is small. Candidate first throughout.
void battery(Size budget) {
    const auto pool = shapes();
    const auto probes = probes_for(unique_ids(pool));
    Book ledger_book(budget), engine_book(budget);
    for (Size n = 0; n < kRows; ++n) {
        ledger_book.push(pool[n * 5 % pool.size()]);
        engine_book.push(pool[n * 5 % pool.size()]);
    }
    REQUIRE(ledger_book.host.ledger().cache_rows() == budget);
    REQUIRE(engine_book.host.ledger().size() == kRows);

    Size expected_yes = 0;
    for (Size i = 0; i < kRows; ++i)
        for (const auto& probe : probes) expected_yes += ledger_book.shape[i].id == probe.text ? 1 : 0;
    CHECK(expected_yes >= kRows); // the oracle says yes about as often as it says no: it is not constant
    CHECK(expected_yes < kRows * probes.size());

    const std::vector<std::vector<Size>> orders{
        order_forward(kRows), order_backward(kRows), order_strided(kRows, 37)};
    Size ledger_yes = 0, engine_yes = 0;
    for (const auto& order : orders) {
        for (const Size i : order) {
            ledger_yes += check_ledger_row(ledger_book, i, probes);
            engine_yes += check_engine_row(engine_book, i, probes);
        }
    }
    CHECK(ledger_yes == orders.size() * expected_yes);
    CHECK(engine_yes == orders.size() * expected_yes);

    // Every access a different row, so small budgets evict between a candidate call and the next.
    auto& ledger = ledger_book.host.ledger();
    for (Size p = 0; p < probes.size(); p += 3) {
        for (const Size i : orders[2]) {
            agree("ledger thrash", i, p, ledger.entry_id_equals(i, probes[p].data(), probes[p].size()),
                  ledger.entry_id_at(i) == probes[p].text);
            agree("engine thrash", i, p,
                  engine_book.host.id_is(static_cast<int>(i), probes[p].data(), probes[p].size()),
                  engine_book.host.id_of(static_cast<int>(i)) == probes[p].text);
        }
    }
    unchanged(ledger_book);
    unchanged(engine_book);
}

// Zero allocations on the cached path. Rows whose four strings fit the cache entry are warmed
// first (that is where the cache and the decoded strings are allocated), then the candidate is
// asked 4 x 4 questions per row inside one counted window. The control shows the counter sees the
// std::string that entry_id_at() builds for the same rows.
void hit_path_allocates_nothing() {
    const auto pool = shapes();
    Book book(TradeLedger::kDefaultCacheRows);
    for (Size n = 0; n < 100; ++n) book.push(pool[n * 5 % pool.size()]);
    auto& ledger = book.host.ledger();
    for (Size i = 0; i < ledger.size(); ++i) (void)ledger.entry_id_equals(i, nullptr, 0);

    std::vector<Size> fitting;
    std::vector<std::string> wrong; // the id with its last byte changed: forces the full compare
    Size beyond_small_string = 0;
    for (Size i = 0; i < ledger.size(); ++i) {
        const Shape& s = book.shape[i];
        if (!s.fits() || s.id.empty()) continue;
        fitting.push_back(i);
        wrong.push_back(flipped(s.id, s.id.size() - 1));
        if (s.id.size() >= 24) ++beyond_small_string;
    }
    REQUIRE(beyond_small_string > 0);

    Size control = 0;
    {
        Counted counted;
        for (const Size i : fitting) (void)ledger.entry_id_at(i);
        control = counted.finish();
    }
    CHECK(control >= beyond_small_string);

    Size asked = 0, agreed = 0, used = 0;
    {
        Counted counted;
        for (int pass = 0; pass < 4; ++pass) {
            for (Size k = 0; k < fitting.size(); ++k) {
                const Size i = fitting[k];
                const std::string& id = book.shape[i].id;
                agreed += ledger.entry_id_equals(i, id.data(), id.size()) ? 1 : 0;
                agreed += ledger.entry_id_equals(i, wrong[k].data(), wrong[k].size()) ? 0 : 1;
                agreed += book.host.id_is(static_cast<int>(i), id.data(), id.size()) ? 1 : 0;
                agreed += book.host.id_is(static_cast<int>(i), wrong[k].data(), wrong[k].size()) ? 0 : 1;
                asked += 4;
            }
        }
        used = counted.finish();
    }
    CHECK(asked == 16 * fitting.size());
    CHECK(agreed == asked);
    CHECK(used == 0);
}

// The calls generated code would make: a literal and its length.
template<std::size_t N>
void literal_agrees(const IdHost& host, int at, const char (&literal)[N]) {
    const bool got = host.id_is(at, literal, N - 1);
    CHECK(got == (host.id_of(at) == literal));
    CHECK(got == (host.id_of(at) == std::string(literal, N - 1)));
}

void literal_calls() {
    const auto pool = shapes();
    Book book(TradeLedger::kDefaultCacheRows);
    for (Size n = 0; n < kRows; ++n) book.push(pool[n * 5 % pool.size()]);
    Size longs = 0;
    for (int i = -2; i < static_cast<int>(kRows) + 2; ++i) {
        literal_agrees(book.host, i, "Long");
        literal_agrees(book.host, i, "Short");
        literal_agrees(book.host, i, "");
        literal_agrees(book.host, i, "L");
        literal_agrees(book.host, i, "Longer");
        literal_agrees(book.host, i, "Lon");
        if (i >= 0 && i < static_cast<int>(kRows) && book.host.id_is(i, "Long", 4)) ++longs;
    }
    Size expected = 0;
    for (const auto& item : book.shape) expected += item.id == "Long" ? 1 : 0;
    CHECK(longs == expected);
    CHECK(longs > 0);
}

void replace_and_modify() {
    const std::size_t cap = TradeLedger::kCacheStringBytes;
    const std::string nul_id("Mod\0id", 6);
    const std::vector<Shape> start{
        {"Long", "entry", "x1", ""},
        {"Short", "entry", "x2", ""},
        {"", "Long", "", ""},
        {pattern(100, 'b'), "", "", ""},
        {"Long", std::string(cap, 'E'), "", ""},          // does not fit
        {pattern(cap + 1, 'a'), "", "", ""},              // does not fit
        {"L", "ong", "", ""},
        {"Longer", "", "", ""},
    };
    struct Step {
        Size at;
        Shape to;
    };
    const std::vector<Step> steps{
        {0, {"Short", "entry", "x", ""}},                 // fits -> fits, other id
        {1, {"Shorty", std::string(cap, 'E'), "", ""}},   // fits -> does not fit; the slot keeps the packed bytes of "Short"
        {2, {"Long", "", "", ""}},                        // empty id -> id
        {3, {"", "", "", ""}},                            // 100-byte id -> empty id
        {4, {"Long", "entry", "", ""}},                   // does not fit -> fits, same id
        {5, {pattern(cap + 1, 'c'), "", "", ""}},         // does not fit -> does not fit, same length, other bytes
        {6, {"Long", "er", "", ""}},                      // neighbour no longer completes the id
        {7, {pattern(5000, 'a'), "", "", ""}},            // short -> far wider than the cache
    };
    std::vector<std::string> ids = unique_ids(start);
    for (const auto& step : steps) add_unique(ids, step.to.id);
    add_unique(ids, nul_id);
    const auto probes = probes_for(ids);

    for (const Size budget : {Size{4}, TradeLedger::kDefaultCacheRows}) {
        Book book(budget);
        for (const auto& item : start) book.push(item);
        verify(book, probes); // warms every row, so each replace below invalidates a live entry
        for (const auto& step : steps) {
            book.host.ledger().replace(step.at, make(static_cast<int>(step.at), step.to));
            book.shape[step.at] = step.to;
            verify(book, probes);
            unchanged(book);
        }
        auto& ledger = book.host.ledger();
        Trade numeric_only = ledger.read(2);
        numeric_only.pnl += 1.0; // same strings: the id extents are reused
        ledger.replace(2, numeric_only);
        verify(book, probes);
        ledger.modify(3, [&nul_id](Trade& value) { value.entry_id = nul_id; });
        book.shape[3].id = nul_id;
        verify(book, probes);
        unchanged(book);
    }
}

void rewind_and_regrowth() {
    const auto pool = shapes();
    const auto probes = probes_for(unique_ids(pool));
    for (const Size budget : {Size{128}, TradeLedger::kDefaultCacheRows}) {
        Book book(budget);
        for (Size n = 0; n < kRows; ++n) book.push(pool[n * 5 % pool.size()]);
        verify(book, probes); // warm
        // 300 -> 10 leaves the resident window; the surviving rows are served from scratch.
        book.rewind(10);
        verify(book, probes);
        // The same global indexes, other ids.
        for (Size n = 10; n < 150; ++n) book.push(pool[(n * 7 + 3) % pool.size()]);
        verify(book, probes);
        book.grow_default(5);
        verify(book, probes);
        book.rewind(20);
        verify(book, probes);
        const auto& ledger = book.host.ledger();
        ledger_refuses_alike(ledger, 20, std::string());
        ledger_refuses_alike(ledger, 21, std::string());
        unchanged(book);
    }
}

void sealed_rows_refuse_like_entry_id_at() {
    const auto pool = shapes();
    const auto probes = probes_for(unique_ids(pool));
    for (const Size budget : {Size{128}, TradeLedger::kDefaultCacheRows}) {
        Book book(budget);
        for (Size n = 0; n < 100; ++n) book.push(pool[n * 5 % pool.size()]);
        verify(book, probes); // all 100 rows are cached before the seal
        auto& ledger = book.host.ledger();
        ledger.seal(40);
        CHECK(ledger.base() == 40);
        // Publication changes no row content and the cache entries stay valid, yet the checked
        // accessors refuse sealed rows before they look at the cache.
        for (Size i = 0; i < 40; ++i) {
            ledger_refuses_alike(ledger, i, book.shape[i].id);
            engine_refuses_alike(book.host, static_cast<int>(i), book.shape[i].id);
        }
        verify(book, probes); // the unsealed rows 40..99 still answer exactly
        unchanged(book);
        ledger.seal(ledger.size());
        for (Size i = 0; i < ledger.size(); ++i) {
            ledger_refuses_alike(ledger, i, book.shape[i].id);
            engine_refuses_alike(book.host, static_cast<int>(i), book.shape[i].id);
        }
    }
}

void ledger_out_of_range() {
    const auto pool = shapes();
    {
        Book empty(TradeLedger::kDefaultCacheRows);
        ledger_refuses_alike(empty.host.ledger(), 0, std::string());
        ledger_refuses_alike(empty.host.ledger(), 1, std::string("Long"));
    }
    Book book(TradeLedger::kDefaultCacheRows);
    for (Size n = 0; n < 3; ++n) book.push(pool[n]);
    const auto& ledger = book.host.ledger();
    for (const Size at : {Size{3}, Size{4}, Size{1000}, std::numeric_limits<Size>::max()})
        ledger_refuses_alike(ledger, at, "Long");
    // The refused calls did not disturb the rows.
    const auto probes = probes_for(unique_ids(pool));
    verify(book, probes);
}

void engine_out_of_range() {
    // A missing row's id is the empty string, so it equals exactly the zero-length text; it does
    // not throw and is never equal to anything longer.
    IdHost host;
    host.ledger().set_scratch_directory(".");
    host.ledger().push_back(make(0, Shape{"Long", "entry", "x", ""}));
    host.ledger().push_back(make(1, Shape{"Short", "", "", ""}));
    host.ledger().push_back(make(2, Shape{}));
    const int size = 3;
    const std::string nul(1, '\0');
    CHECK(host.id_is(0, "Long", 4));
    CHECK(!host.id_is(0, "Short", 5));
    CHECK(host.id_is(1, "Short", 5));
    CHECK(host.id_is(2, "", 0));
    CHECK(!host.id_is(2, "Long", 4));
    for (const int at : {-1, -2, std::numeric_limits<int>::min(), size, size + 1, 1000,
                         std::numeric_limits<int>::max()}) {
        CHECK(host.id_of(at).empty());
        CHECK(host.id_is(at, nullptr, 0));
        CHECK(host.id_is(at, "", 0));
        CHECK(!host.id_is(at, "Long", 4));
        CHECK(!host.id_is(at, nul.data(), nul.size()));
        CHECK(host.id_is(at, nullptr, 0) == (host.id_of(at) == std::string()));
        CHECK(host.id_is(at, "Long", 4) == (host.id_of(at) == "Long"));
        CHECK(host.id_is(at, nul.data(), nul.size()) == (host.id_of(at) == nul));
    }
    IdHost bare; // no rows at all
    for (const int at : {-1, 0, 1}) {
        CHECK(bare.id_is(at, nullptr, 0));
        CHECK(bare.id_is(at, "", 0));
        CHECK(!bare.id_is(at, "Long", 4));
    }
}
} // namespace

int main() {
    test("catalog holds every row class the other scenarios rely on", catalog_classes);
    test("oracle agreement, 128-row cache, 300 rows (collisions)", [] { battery(128); });
    test("oracle agreement, 65,536-row cache", [] { battery(TradeLedger::kDefaultCacheRows); });
    test("oracle agreement, 7-row cache (modulo slots)", [] { battery(7); });
    test("cached path allocates nothing", hit_path_allocates_nothing);
    test("literal calls as generated code makes them", literal_calls);
    test("replace/modify of live cache entries", replace_and_modify);
    test("rewind below the resident window and regrowth", rewind_and_regrowth);
    test("sealed rows refuse exactly like entry_id_at", sealed_rows_refuse_like_entry_id_at);
    test("ledger rows outside the window refuse exactly like entry_id_at", ledger_out_of_range);
    test("engine index outside the ledger: false, or true for zero length", engine_out_of_range);
    std::printf("test_p2cx_idis: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
