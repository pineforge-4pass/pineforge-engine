// CR-307 C0182. Expected Release wall time <15s; estimate, not measured here.
// Execute every existing bracket witness verbatim, then exercise cold records.
#define main original_toolkit_bracket_main
#include "test_native_toolkit_bracket.cpp"
#undef main
#include "order_book_fixture.hpp"

#include <map>
#include <vector>

namespace {
using order_book_test::Book;
using order_book_test::key;

// Reference transcription of feae33694's map algorithm, deliberately separate
// from the product storage abstraction. Receipt comparison uses fresh hosts
// with the SAME run identity, requests, bar and command order.
class MapBook {
    struct Entry { no::RequestHandle handle; bool pending_until_armed; };
    Host* host_;
    std::map<std::string, Entry> entries_;
    static bool pending(const no::Request& request) {
        const auto* wait = std::get_if<no::WaitForApplied>(&request.owner);
        return wait && wait->visibility == no::NativeArmVisibility::PendingUntilArmed;
    }
    bool working(const no::RequestHandle& target) const {
        for (const auto& row : host_->native_working_requests())
            if (row.definition && row.definition->handle == target) return true;
        return false;
    }
public:
    explicit MapBook(Host& host) : host_(&host) {}
    tk::OrderBookOutcome submit_or_replace_outcome(const std::string& key,
                                                  const no::Request& request) {
        tk::OrderBookOutcome out;
        const auto found = entries_.find(key);
        if (found != entries_.end()) {
            if (found->second.pending_until_armed || working(found->second.handle)) {
                const auto result = host_->replace(found->second.handle, request);
                out.replace = result;
                if (result.status == no::ReplaceStatus::Replaced && result.successor) {
                    found->second = Entry{*result.successor, pending(request)};
                    out.action = tk::OrderBookAction::Replaced;
                    return out;
                }
                if (result.status == no::ReplaceStatus::ReplaceRejected) {
                    out.action = tk::OrderBookAction::ReplaceRejected;
                    return out;
                }
            }
            entries_.erase(found);
        }
        const auto result = host_->submit(request);
        out.submit = result;
        if (result.status != no::SubmitStatus::Accepted || !result.handle) {
            out.action = tk::OrderBookAction::SubmitRejected;
            return out;
        }
        entries_.emplace(key, Entry{*result.handle, pending(request)});
        out.action = tk::OrderBookAction::SubmitAccepted;
        return out;
    }
    tk::OrderBookCancel cancel_outcome(const std::string& key) {
        tk::OrderBookCancel out;
        const auto found = entries_.find(key);
        if (found == entries_.end()) return out;
        out.known = true;
        const auto target = found->second;
        entries_.erase(found);
        if (!target.pending_until_armed && !working(target.handle)) return out;
        out.result = host_->cancel(target.handle);
        return out;
    }
    std::optional<no::RequestHandle> handle(const std::string& key) const {
        const auto found = entries_.find(key);
        if (found == entries_.end()) return std::nullopt;
        return found->second.handle;
    }
};

struct Wire {
    std::string bytes;
    void word(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8)
            bytes.push_back(static_cast<char>((value >> shift) & 255));
    }
    void handle(const no::RequestHandle& value) {
        word(value.run.session_key.size());
        bytes.append(value.run.session_key.data(), value.run.session_key.size());
        word(value.run.run_number);
        word(value.incarnation);
    }
    template<class T, class F> void optional(const std::optional<T>& value, F emit) {
        word(value.has_value());
        if (value) emit(*value);
    }
    void submit(const no::SubmitResult& value) {
        word(static_cast<std::uint64_t>(value.status));
        word(value.event_ordinal);
        optional(value.handle, [&](const auto& h) { handle(h); });
        optional(value.reason, [&](auto r) { word(static_cast<std::uint64_t>(r)); });
    }
    void outcome(const tk::OrderBookOutcome& value) {
        word(static_cast<std::uint64_t>(value.action));
        optional(value.replace, [&](const auto& r) {
            word(static_cast<std::uint64_t>(r.status));
            word(r.event_ordinal);
            optional(r.successor, [&](const auto& h) { handle(h); });
            optional(r.reason, [&](auto reason) { word(static_cast<std::uint64_t>(reason)); });
        });
        optional(value.submit, [&](const auto& s) { submit(s); });
    }
    void cancel(const tk::OrderBookCancel& value) {
        word(value.known);
        optional(value.result, [&](const auto& r) {
            word(static_cast<std::uint64_t>(r.status)); word(r.event_ordinal);
        });
    }
    void bracket(const tk::BracketReceipt& value) {
        handle(value.parent);
        for (const auto& h : {value.take_profit, value.stop_loss, value.trail})
            optional(h, [&](const auto& present) { handle(present); });
        for (const auto which : {tk::BracketLeg::TakeProfit, tk::BracketLeg::StopLoss,
                                 tk::BracketLeg::Trail}) {
            const auto& leg = value.outcome(which);
            word(static_cast<std::uint64_t>(leg.state));
            optional(leg.result, [&](const auto& s) { submit(s); });
        }
    }
    void events(const Host& host) {
        for (const auto& row : host.native_events(0)) {
            if (!row.command) continue;
            // Production codec: declared fields, exact floating bits,
            // optional presence, session TEXT; no pointer or padding bytes.
            const auto source = no::encode_journal_record(*row.command);
            word(source.size());
            std::vector<std::byte> encoded(static_cast<std::size_t>(source.size()));
            REQUIRE(source.read(0, {encoded.data(), encoded.size()})
                    == detail::ScratchStatus::ok);
            bytes.append(reinterpret_cast<const char*>(encoded.data()), encoded.size());
        }
    }
};

template<class B> std::string receipt_replay() {
    Host host;
    B book(host);
    Wire wire;
    host.beginning = [&](Host& base) {
        const auto parent = put(base, tx(1.0, "entry"));
        tk::BracketSpec bracket;
        bracket.parent = parent;
        bracket.visibility = no::NativeArmVisibility::PendingUntilArmed;
        no::Request tp{no::Reduce{no::OwnerOpenedUnits{}}, "tp", "bracket"};
        tp.trigger = no::Limit{120.0};
        no::Request sl{no::Reduce{no::OwnerOpenedUnits{}}, "sl", "bracket"};
        sl.trigger = no::Stop{80.0};
        bracket.take_profit = tp;
        bracket.stop_loss = sl;
        wire.bracket(tk::submit_bracket(base, bracket));

        auto hidden = sl;
        hidden.owner = no::WaitForApplied{parent, no::NativeArmVisibility::PendingUntilArmed};
        const auto first = book.submit_or_replace_outcome("hidden", hidden);
        wire.outcome(first);
        REQUIRE(first.handle());
        REQUIRE(base.cancel(*first.handle()).status == no::CancelStatus::Cancelled);
        const auto ended = book.submit_or_replace_outcome("ended", order_book_test::resting());
        REQUIRE(ended.handle());
        wire.outcome(ended);
        REQUIRE(base.cancel(*ended.handle()).status == no::CancelStatus::Cancelled);
        // More than two full windows of OTHER keys evicts both bindings.
        for (std::size_t i = 0; i < 128; ++i) {
            const auto out = book.submit_or_replace_outcome(key(i), order_book_test::resting());
            REQUIRE(out.handle());
            REQUIRE(base.cancel(*out.handle()).status == no::CancelStatus::Cancelled);
        }
        CHECK(book.handle("ended") == ended.handle());
        const auto before = base.native_events(0).size();
        const auto known = book.cancel_outcome("ended");
        const auto unknown = book.cancel_outcome("never");
        const auto tombstone = book.cancel_outcome("ended");
        CHECK(known.known && !known.commanded());
        CHECK(!unknown.known && !unknown.commanded());
        CHECK(!tombstone.known && !tombstone.commanded());
        CHECK(before == base.native_events(0).size());
        wire.cancel(known); wire.cancel(unknown); wire.cancel(tombstone);
        const auto events_before_copy = base.native_events(0).size();
        B copied(book);
        B moved(std::move(copied));
        // Sorted copy can warm "hidden" last. Evict it again without commands
        // so this replace must restore the pending bit from the copied file.
        for (std::size_t i = 0; i < 128; ++i) CHECK(moved.handle(key(i)).has_value());
        CHECK(events_before_copy == base.native_events(0).size());
        const auto revived = moved.submit_or_replace_outcome("hidden", hidden);
        REQUIRE(revived.replace && revived.submit);
        CHECK(revived.replace->status == no::ReplaceStatus::NotWorking);
        CHECK(revived.submit->status == no::SubmitStatus::Accepted);
        CHECK(revived.replace->event_ordinal < revived.submit->event_ordinal);
        CHECK(book.handle("hidden") == first.handle()); // independent cold copy
        wire.outcome(revived);
        wire.cancel(moved.cancel_outcome("hidden"));

        const auto live = book.submit_or_replace_outcome("live", order_book_test::resting());
        wire.outcome(live);
        auto bad = tx(0.0, "bad");
        const auto refused = book.submit_or_replace_outcome("live", bad);
        CHECK(refused.action == tk::OrderBookAction::ReplaceRejected);
        CHECK(book.handle("live") == live.handle());
        wire.outcome(refused);
        wire.cancel(book.cancel_outcome("live"));
        wire.outcome(book.submit_or_replace_outcome("bad", bad));
        // Ended hidden binding: fallback submit refusal leaves it absent.
        const auto doomed = book.submit_or_replace_outcome("doomed", hidden);
        REQUIRE(doomed.handle());
        REQUIRE(base.cancel(*doomed.handle()).status == no::CancelStatus::Cancelled);
        const auto fallback = book.submit_or_replace_outcome("doomed", bad);
        REQUIRE(fallback.replace && fallback.submit);
        CHECK(fallback.replace->status == no::ReplaceStatus::NotWorking);
        CHECK(fallback.action == tk::OrderBookAction::SubmitRejected);
        CHECK(!book.handle("doomed"));
        wire.outcome(doomed); wire.outcome(fallback);
    };
    run(host, spec("cr307-identical-receipts"), {100.0});
    completed(host);
    wire.events(host);
    return wire.bytes;
}

void byte_identical_receipts_after_eviction() {
    const auto reference = receipt_replay<MapBook>();
    const auto scratch = receipt_replay<Book>();
    CHECK(!reference.empty());
    CHECK(reference == scratch);
}

void copy_move_keys_and_counts() {
    static_assert(std::is_copy_constructible_v<Book> && std::is_copy_assignable_v<Book>);
    static_assert(std::is_nothrow_move_constructible_v<Book>);
    static_assert(std::is_nothrow_move_assignable_v<Book>);
    Host host;
    Book book(host);
    host.beginning = [&](Host& base) {
        for (std::size_t i = 0; i < 1000; ++i) {
            CHECK(!book.handle(key(i)));
            CHECK(!book.cancel_outcome(key(i)).known);
        }
        CHECK(book.empty() && order_book_test::resident(book) == 0);
        std::uint64_t first = 0;
        for (std::size_t i = 0; i < 257; ++i) {
            const auto out = book.submit_or_replace(key(i), order_book_test::resting());
            REQUIRE(out);
            if (i == 0) first = out->incarnation;
            REQUIRE(base.cancel(*out).status == no::CancelStatus::Cancelled);
        }
        const std::string long_key(4097, 'x');
        const std::string binary("key\0tail", 8);
        const auto empty = book.submit_or_replace("", order_book_test::resting());
        const auto long_handle = book.submit_or_replace(long_key, order_book_test::resting());
        const auto binary_handle = book.submit_or_replace(binary, order_book_test::resting());
        REQUIRE(empty && long_handle && binary_handle);
        REQUIRE(base.cancel(*empty).status == no::CancelStatus::Cancelled);
        REQUIRE(base.cancel(*long_handle).status == no::CancelStatus::Cancelled);
        REQUIRE(base.cancel(*binary_handle).status == no::CancelStatus::Cancelled);
        CHECK(book.size() == 260);
        book.forget(key(17)); // copy omits a tombstone, not an ended binding
        book.forget(key(17));
        CHECK(book.size() == 259);
        Book copy(book);
        Book assigned(base);
        assigned = copy;
        const auto verify = [&](const Book& candidate) {
            CHECK(candidate.size() == 259);
            CHECK(candidate.handle("") == empty);
            CHECK(candidate.handle(long_key) == long_handle);
            CHECK(candidate.handle(binary) == binary_handle);
            CHECK(!candidate.handle("key"));
            for (std::size_t i = 257; i-- != 0;) {
                const no::RequestHandle expected{empty->run, first + i};
                if (i == 17) CHECK(!candidate.contains(key(i)));
                else CHECK(candidate.handle(key(i)) == expected);
            }
            CHECK(order_book_test::resident(candidate) <= order_book_test::kResidentLimit);
        };
        verify(copy); verify(assigned);
        copy.forget("");
        CHECK(book.handle("") == empty && !copy.contains(""));
        CHECK(assigned.handle("") == empty);
        Book moved(std::move(assigned));
        CHECK(assigned.empty() && order_book_test::resident(assigned) == 0);
        verify(moved);
        assigned = std::move(moved);
        CHECK(moved.empty());
        verify(assigned);
        // Move assignment over an already spilled destination releases its file.
        moved = book;
        moved = std::move(assigned);
        verify(moved);
        // Moved-from storage is reusable with the original host pointer.
        const auto reused = assigned.submit_or_replace("reuse", order_book_test::resting());
        REQUIRE(reused);
        CHECK(assigned.size() == 1);
        CHECK(assigned.cancel("reuse") == no::CancelStatus::Cancelled);
        CHECK(assigned.empty());
        // Rebinding a tombstone increments logical size exactly once.
        const auto rebound = book.submit_or_replace(key(17), order_book_test::resting());
        REQUIRE(rebound);
        CHECK(book.size() == 260);
        CHECK(book.cancel(key(17)) == no::CancelStatus::Cancelled);
        CHECK(book.size() == 259);
        for (std::size_t i = 0; i < 257; ++i) book.forget(key(i));
        book.forget(""); book.forget(long_key); book.forget(binary);
        CHECK(book.empty() && book.size() == 0);
        const auto residents = order_book_test::resident(book);
        for (std::size_t i = 0; i < 1000; ++i) CHECK(!book.contains("absent-" + key(i)));
        CHECK(order_book_test::resident(book) == residents);
    };
    run(host, spec("cr307-copy"), {100.0});
    completed(host);
}

struct OrderedKey {
    int value;
    bool operator<(const OrderedKey& other) const { return value < other.value; }
};
void generic_ordered_key() {
    Host host;
    tk::OrderBook<OrderedKey> book(host);
    host.beginning = [&](Host& base) {
        const auto handle = book.submit_or_replace({7}, order_book_test::resting());
        REQUIRE(handle);
        REQUIRE(base.cancel(*handle).status == no::CancelStatus::Cancelled);
        auto copy = book;
        CHECK(copy.handle({7}) == handle && copy.size() == 1);
        const auto ended = copy.cancel_outcome({7});
        CHECK(ended.known && !ended.commanded());
        CHECK(book.contains({7}) && copy.empty());
    };
    run(host, spec("cr307-generic-key"), {100.0});
    completed(host);
}

void book_cancel_negative_control_and_adversarial_keys() {
    Host host;
    Book book(host);
    host.beginning = [&](Host& base) {
        // This control already passes with the old map. Unlike the growth
        // witness, cancellation through the book deliberately removes history.
        for (std::size_t i = 0; i < 2000; ++i) {
            REQUIRE(book.submit_or_replace(key(i), order_book_test::resting()));
            CHECK(book.size() == 1);
            CHECK(book.cancel(key(i)) == no::CancelStatus::Cancelled);
            CHECK(book.empty());
        }
        CHECK(order_book_test::resident(book) <= order_book_test::kResidentLimit);
        const std::string ids[] = {"c", "a", "b", "f", "e", "d", "g"};
        std::map<std::string, no::RequestHandle> expected;
        for (const auto& id : ids) {
            const auto handle = book.submit_or_replace(id, order_book_test::resting());
            REQUIRE(handle);
            expected.emplace(id, *handle);
            REQUIRE(base.cancel(*handle).status == no::CancelStatus::Cancelled);
        }
        Book copy(book); // sorted streaming walk skips all 2000 tombstones
        CHECK(copy.size() == 7 && book.size() == 7);
        for (const auto& pair : expected) CHECK(copy.handle(pair.first) == pair.second);
        CHECK(!copy.contains(key(1999)));
    };
    auto setup = spec("cr307-cancel-control");
    setup.event_retention = NativeEventRetention::Window;
    run(host, setup, {100.0});
    completed(host);
}
}

int main() {
    (void)original_toolkit_bracket_main(); // every original assertion unchanged
    test("cold byte-identical receipts", byte_identical_receipts_after_eviction);
    test("spilled copy move keys and counts", copy_move_keys_and_counts);
    test("generic ordered key fallback", generic_ordered_key);
    test("book cancel control and adversarial keys",
         book_cancel_negative_control_and_adversarial_keys);
    std::printf("OrderBook semantics: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
