// A naive full-copy oracle exists ONLY in this test. Every visible field,
// tombstone and FIFO entry is compared after random operations and restores.
// This TU also compiles on frozen2: it is a semantic positive control there.
#include <pineforge/drawing.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
bool same(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }
bool equal(const LineRec& a, const LineRec& b) {
    return a.x1 == b.x1 && a.x2 == b.x2 && same(a.y1, b.y1) && same(a.y2, b.y2) &&
           a.xloc == b.xloc && a.ext_l == b.ext_l && a.ext_r == b.ext_r && a.alive == b.alive;
}
bool equal(const BoxRec& a, const BoxRec& b) {
    return a.left == b.left && a.right == b.right && same(a.top, b.top) &&
           same(a.bottom, b.bottom) && a.xloc == b.xloc && a.alive == b.alive;
}
bool equal(const LabelRec& a, const LabelRec& b) {
    return a.x == b.x && same(a.y, b.y) && a.xloc == b.xloc &&
           a.yloc == b.yloc && a.text == b.text && a.alive == b.alive;
}
bool equal(const LinefillRec& a, const LinefillRec& b) {
    return a.line1 == b.line1 && a.line2 == b.line2 && a.alive == b.alive;
}

template<class Rec> struct Oracle {
    std::vector<Rec> records;
    std::deque<std::int32_t> order;
    int cap;
    explicit Oracle(int cap_) : cap(std::max(1, cap_)) {}
    std::int32_t alloc(Rec rec) {
        while (order.size() >= static_cast<std::size_t>(cap)) {
            records[static_cast<std::size_t>(order.front())].alive = false;
            order.pop_front();
        }
        const auto id = static_cast<std::int32_t>(records.size());
        rec.alive = true;
        records.push_back(std::move(rec));
        order.push_back(id);
        return id;
    }
    void erase(std::int32_t id) {
        if (id < 0 || static_cast<std::size_t>(id) >= records.size() || !records[id].alive) return;
        records[id].alive = false;
        order.erase(std::find(order.begin(), order.end(), id));
    }
};

void edit(LineRec& r, std::uint32_t n) {
    r.x1 = -static_cast<std::int64_t>(n);
    r.x2 = n + 1;
    r.y1 = (n & 1) ? -0.0 : static_cast<double>(n) / 8.0;
    r.y2 = (n & 2) ? na<double>() : -static_cast<double>(n);
    r.xloc = (n & 4) ? XLoc::bar_time : XLoc::bar_index;
    r.ext_l = (n & 8) != 0;
    r.ext_r = (n & 16) != 0;
}
void edit(BoxRec& r, std::uint32_t n) {
    r.left = -static_cast<std::int64_t>(n);
    r.right = n + 7;
    r.top = (n & 1) ? na<double>() : -0.0;
    r.bottom = static_cast<double>(n) / 16.0;
    r.xloc = (n & 4) ? XLoc::bar_time : XLoc::bar_index;
}
void edit(LabelRec& r, std::uint32_t n) {
    r.x = n;
    r.y = (n & 1) ? -0.0 : na<double>();
    r.xloc = (n & 4) ? XLoc::bar_time : XLoc::bar_index;
    r.yloc = static_cast<YLoc>(n % 3);
    r.text.assign((n % 127) + 1, static_cast<char>(n % 251));
    r.text[r.text.size() / 2] = '\0';
}
void edit(LinefillRec& r, std::uint32_t n) {
    r.line1 = static_cast<std::int32_t>(n >> 1);
    r.line2 = -static_cast<std::int32_t>(n >> 2);
}

template<class Arena, class = void> struct HasHash : std::false_type {};
template<class Arena> struct HasHash<Arena,
    std::void_t<decltype(std::declval<const Arena&>().state_hash())>> : std::true_type {};

template<class Rec>
void compare(const DrawingArena<Rec>& actual, const Oracle<Rec>& expected) {
    require(actual.size() == static_cast<std::int32_t>(expected.records.size()), "version size differs");
    require(actual.order().size() == expected.order.size() &&
            std::equal(actual.order().begin(), actual.order().end(), expected.order.begin()),
            "version FIFO differs");
    for (std::int32_t id = 0; id < actual.size(); ++id) {
        require(equal(actual.at(id), expected.records[static_cast<std::size_t>(id)]),
                "version record bytes differ");
        require(actual.alive(id) == expected.records[static_cast<std::size_t>(id)].alive,
                "version tombstone differs");
    }
}

template<class Rec>
void hash_checks(DrawingArena<Rec>& actual, const DrawingArena<Rec>& saved) {
    if constexpr (HasHash<DrawingArena<Rec>>::value) {
        require(actual.state_hash() == saved.state_hash(), "restored drawing hash differs");
        const auto hash = actual.state_hash();
        Rec next = std::as_const(actual).at(0);
        edit(next, 0x7ffffe01U);
        actual.at(0) = std::move(next);
        require(actual.state_hash() != hash, "dead drawing edit is missing from hash");
        actual = saved;
        require(actual.state_hash() == hash, "hash changed across restore");
    }
}

std::uint32_t random_step(std::uint32_t& state) {
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return state;
}

template<class Rec>
void differential(int cap) {
    DrawingArena<Rec> live(cap);
    Oracle<Rec> oracle(cap);
    // Seed enough blocks for old-record writes, path copies and both caches.
    for (std::uint32_t n = 0; n < 160; ++n) {
        Rec rec{};
        edit(rec, n);
        require(live.alloc(rec) == oracle.alloc(rec), "seed id differs");
    }
    std::array<std::optional<DrawingArena<Rec>>, 4> versions;
    std::array<std::optional<Oracle<Rec>>, 4> saved;
    std::uint32_t random = 0x91e10da5U;
    for (std::uint32_t step = 0; step < 1200; ++step) {
        const auto bits = random_step(random);
        const auto slot = static_cast<std::size_t>((bits >> 8) % versions.size());
        const auto id = static_cast<std::int32_t>((bits >> 12) % oracle.records.size());
        switch (bits % 8) {
        case 0:
        case 1: {
            Rec rec{};
            edit(rec, bits);
            require(live.alloc(rec) == oracle.alloc(rec), "appended id differs");
            break;
        }
        case 2:
            live.erase(id);
            oracle.erase(id);
            live.erase(-1);
            oracle.erase(-1);
            break;
        case 3: {
            // A local value obeys at()'s working-view lifetime. Mutate dead
            // records just as often as live ones, including id zero.
            Rec rec = std::as_const(live).at(id);
            edit(rec, bits);
            live.at(id) = std::move(rec);
            edit(oracle.records[static_cast<std::size_t>(id)], bits);
            break;
        }
        case 4:
            versions[slot].emplace(live);
            saved[slot].emplace(oracle); // actual full-copy reference
            break;
        case 5:
            if (versions[slot]) {
                live = *versions[slot];
                oracle = *saved[slot];
                hash_checks(live, *versions[slot]);
            }
            break;
        case 6:
            if (versions[slot]) {
                // Copy a checkpoint, then fork it independently of live.
                auto branch = *versions[slot];
                auto reference = *saved[slot];
                branch.erase(0);
                reference.erase(0);
                Rec rec = std::as_const(branch).at(0);
                edit(rec, bits);
                branch.at(0) = std::move(rec);
                edit(reference.records[0], bits);
                compare(branch, reference);
                compare(*versions[slot], *saved[slot]);
            }
            break;
        default:
            live = live;
            compare(live, oracle);
            break;
        }
        compare(live, oracle);
        for (std::size_t i = 0; i < versions.size(); ++i)
            if (versions[i]) compare(*versions[i], *saved[i]);
    }
    // Move leaves both objects usable, with the original declared cap.
    auto moved = std::move(live);
    compare(moved, oracle);
    require(live.size() == 0 && live.order().empty(), "moved-from arena is not empty");
    Rec first{};
    edit(first, 51);
    require(live.alloc(first) == 0, "moved-from arena cannot be reused");
    moved = std::move(moved);
    compare(moved, oracle);
    std::printf("A5 DRAW versions cap=%d records=%zu random_steps=1200\n", cap, oracle.records.size());
}
} // namespace

int main() {
    try {
        for (int cap : {1, 17, 500, std::numeric_limits<int>::max()}) {
            differential<LineRec>(cap);
            differential<BoxRec>(cap);
            differential<LabelRec>(cap);
            differential<LinefillRec>(cap);
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "A5 DRAW versions: %s\n", error.what());
        return 1;
    }
}
