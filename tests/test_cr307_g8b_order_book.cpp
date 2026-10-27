// Expected wall time: 2-15 seconds Release on a 10-core host (unmeasured).
// Existing explicit-cancel bound and stale-key contract, not automatic
// retirement or a universal live-id bound; see REPORT-cr307-g8b.md.
#include <pineforge/native_toolkit.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace pf = pineforge;
namespace no = pf::native_order;
namespace tk = pf::native_toolkit;
using Book = tk::OrderBook<std::string>;
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Entries { friend auto access(Entries); };
template struct Access<Entries, &Book::entries_>;

void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

class Host final : public pf::NativeStrategyHost {
public:
    Host() : book(*this) {}
    void on_native_bar(const pf::Bar&, const pf::NativeDecisionContext&) override {
        const auto id = "book-" + std::to_string(callbacks);
        no::Request request{no::Transact{1.0}, id, ""};
        request.trigger = no::Limit{1.0};
        const auto handle = book.submit_or_replace(id, request);
        require(handle.has_value() && native_working_requests().size() == 1,
                "fresh keyed order must actually be live");
        require((book.*access(Entries{})).size() == 1, "only this fresh key is resident");
        // A direct host cancel leaves an observable stale key. It must remain
        // known until book.cancel_outcome(), with no second kernel cancel.
        require(cancel(*handle).status == no::CancelStatus::Cancelled, "host cancelled order");
        require(book.handle(id) == handle && book.contains(id) && book.size() == 1,
                "stale binding remains observable");
        const auto known = book.cancel_outcome(id);
        require(known.known && !known.commanded() && known.event_ordinal() == 0,
                "stale known key is forgotten without a kernel command");
        const auto unknown = book.cancel_outcome(id);
        require(!unknown.known && !unknown.commanded(), "forgotten key becomes unknown");
        require((book.*access(Entries{})).empty() && native_working_requests().empty(),
                "explicit book retirement leaves no resident key or live request");
        ++callbacks;
    }
    std::size_t callbacks = 0;
    Book book;
};
}

int main() {
    try {
        for (const std::size_t count : {2000U, 8000U}) {
            pf::NativeRunSpec spec;
            spec.identity = {"cr307-g8b-book", 1};
            spec.input_tf = spec.script_tf = "1";
            spec.tickerid = "TEST:G8B";
            spec.timezone = "UTC"; spec.session = "24x7";
            spec.initial_capital = 10000.0;
            spec.point_value = spec.account_fx = 1.0;
            spec.price_tick = 0.25;
            std::vector<pf::Bar> bars(count);
            for (std::size_t i = 0; i < count; ++i)
                bars[i] = {100.0, 101.0, 99.0, 100.0, 1000.0,
                           1736121600000LL + static_cast<std::int64_t>(i) * 60000};
            Host host;
            require(host.configure_native(spec).status == pf::NativeSetupStatus::Applied,
                    "native book host configured");
            host.run(bars.data(), static_cast<int>(bars.size()));
            if (!host.last_error().empty()) throw std::runtime_error(host.last_error());
            require(host.callbacks == count && host.book.empty() && host.trade_count() == 0,
                    "every distinct id retired without filling");
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
