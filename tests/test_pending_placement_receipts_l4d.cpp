// A29 native-route twin: replacement receipts retain incarnation and priority.
#include "l8d_twin_support.hpp"

#include <cstdio>

using namespace pineforge;
using namespace pineforge::l8d_test;

namespace {
int passed = 0, failed = 0;
#define CHECK(value) do { if (value) ++passed; else { ++failed; std::fprintf(stderr, "FAIL %d %s\n", __LINE__, #value); } } while (0)

class Book final : public source::L4dPineHost {
public:
    Book() { configure_pine_strategy(fixed_config()); }
    std::vector<pf_pending_order_v1_t> rows;
    pf_pending_order_v1_t mirror(const std::string& id) const {
        pf_pending_order_v1_t result{};
        for (std::size_t i = 0; i < rows.size(); ++i) if (id == rows[i].id) {
            result = rows[i];
            CHECK(strategy_pending_order_get(static_cast<BacktestEngine*>(const_cast<Book*>(this)), static_cast<int>(i),&result,sizeof result)==0);
            return result;
        }
        return result;
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        strategy_entry("E", true, missing, 110.0, 1.0);
        strategy_entry("E", true, missing, 111.0, 2.0);
        rows = pending_rows(this);
        if (!rows.empty()) (void)mirror("E");
    }
};
} // namespace

int main() {
    Book book; const Bar bar = point(100, 60'000); book.run(&bar, 1);
    CHECK(book.last_error().empty());
    CHECK(!book.rows.empty());
    const auto* row = find(book.rows, "E");
    CHECK(row != nullptr);
    if (row) {
        CHECK(row->incarnation != 0);
        CHECK(row->replaced_order_incarnation != 0);
        CHECK(row->created_seq != 0);
        CHECK(row->qty == 2.0);
    }
    return failed == 0 ? 0 : 1;
}
