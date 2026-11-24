#include "waiting_exit_fixture.hpp"

#include <algorithm>

namespace {
using namespace waiting_exit;

class Reissue final : public Host {
public:
    explicit Reissue(bool split) : Host(false, 2), split(split) {}
    void issue(bool per_entry, const char* comment) {
        // The two lots filled at 100 and 102. Forty ticks place separate
        // targets ten points above those prices; 130 is one book order.
        strategy_exit("Guard", "", per_entry ? absent : 130, absent, absent, absent, absent, 100, comment,
                      absent, {}, per_entry ? 40 : absent);
    }
    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        need(bar == callbacks++, "missing or repeated chart bar");
        if (bar == 0) strategy_entry("A", true, absent, absent, 2);
        if (bar == 1) strategy_entry("B", true, absent, absent, 3);
        if (bar == 2) {
            const auto lots = native_open_lots(100);
            need(lots.size() == 2 && lots[0].entry_label == "A" && lots[0].entry_price == 100
                 && lots[1].entry_label == "B" && lots[1].entry_price == 102,
                 "expected two distinct opening prices");
            issue(!split, "old");
        }
        if (bar == 3) {
            strategy_order("Park", true, 1, 50);
            park = working("Park");
            int originals = 0;
            for (const auto& w : native_working_requests()) {
                const auto& request = w.definition->request;
                if (request.label != "Guard") continue;
                need(request.comment == "old" && std::holds_alternative<no::Limit>(request.trigger),
                     "unexpected original bracket definition");
                if (++originals == 1) old = w.definition->handle;
            }
            need(originals == (split ? 1 : 2) && projected("Guard") > 0,
                 "wrong initial whole or per-entry bracket");
            wait_for(old, park);
            before = book().last_ordinal();
            issue(split, "new");
            after = book().last_ordinal();
            hidden_live = book().find_live(old) != nullptr;
            for (const auto& w : book().live()) {
                if (w.request().label != "Guard") continue;
                if (w.request().comment == "old") ++old_live;
                if (w.request().comment == "new" && std::holds_alternative<no::Limit>(w.request().trigger))
                    ++new_live;
            }
            pf_pending_order_v1_t first{};
            new_projected = projected("Guard", &first) > 0 && std::string(first.comment) == "new";
        }
    }
    void check(Checks& checks) const {
        need(last_run_status() == 0 && last_error().empty() && callbacks == 5 && before && after >= before,
             "missing completed reissue");
        int user = 0;
        for (const auto& row : native_events(before)) {
            if (!row.command || row.ordinal > after) continue;
            const auto* end = std::get_if<no::CancelledEvent>(&*row.command);
            if (!end || end->handle() != old) continue;
            const auto* wait = std::get_if<no::Wait>(&end->prior_authority);
            if (wait && wait->parent == park && end->reason == no::CancelReason::User) ++user;
        }
        checks.equal("waiting limit cancelled at reissue", user, 1);
        checks.equal("original hidden handle live", hidden_live, false);
        checks.equal("live old definitions", old_live, 0);
        checks.equal("live new limits", new_live, split ? 2 : 1);
        checks.equal("new definition in projection", new_projected, true);
    }
private:
    const bool split;
    Handle park{}, old{};
    int callbacks = 0, old_live = 0, new_live = 0;
    std::uint64_t before = 0, after = 0;
    bool hidden_live = false, new_projected = false;
};
} // namespace

int main() {
    using namespace waiting_exit;
    int status = 0;
    for (bool split : {true, false}) {
        status = std::max(status, compare(split ? "hidden whole bracket split by entry"
                                               : "hidden per-entry bracket replaced by whole",
            [&](Checks& checks) {
                auto tape = bars<5>();
                tape[2] = {102, 103, 101, 102, 1000, tape[2].timestamp};
                Reissue host(split);
                host.run(tape.data(), static_cast<int>(tape.size()));
                need(host.last_error().empty(), host.last_error().c_str());
                host.check(checks);
            }));
    }
    return status;
}
