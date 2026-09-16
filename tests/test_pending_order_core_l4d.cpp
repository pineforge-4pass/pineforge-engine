// A29 native-route twin: quantity intent and bracket ownership use live rows.
#include "l8d_twin_support.hpp"

#include <cstdio>

using namespace pineforge;
using namespace pineforge::l8d_test;

namespace {
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "FAIL %d %s\n", __LINE__, #x); } } while (0)

class Book final : public source::L4dPineHost {
public:
    Book() { configure_pine_strategy(fixed_config()); }
    std::vector<pf_pending_order_v1_t> rows;
    pf_pending_order_v1_t mirror(const std::string& id) const {
        pf_pending_order_v1_t result{};
        for (std::size_t i = 0; i < rows.size(); ++i) if (id == rows[i].id) {
            result = rows[i];
            CHECK(strategy_pending_order_get(const_cast<Book*>(this),int(i),&result,sizeof(result))==0);
            return result;
        }
        return result;
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        strategy_entry("E", true, missing, 120.0, 4.0);
        strategy_exit("X", "E", missing, 90.0, missing, missing, missing, 50.0);
        rows = pending_rows(this);
        if (!rows.empty()) (void)mirror(rows.front().id);
    }
};
} // namespace

int main() {
    Book book; const Bar bar = point(100, 60'000); book.run(&bar, 1);
    CHECK(book.last_error().empty());
    CHECK(book.rows.size() >= 1);
    const auto* entry = find(book.rows, "E");
    CHECK(entry != nullptr);
    if (entry) {
        CHECK(entry->qty == 4.0);
        CHECK(entry->stop_price == 120.0);
        CHECK(entry->incarnation != 0);
    }
    return failures == 0 ? 0 : 1;
}
