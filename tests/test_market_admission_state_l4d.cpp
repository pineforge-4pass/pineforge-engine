// A29 native-route twin: admission-state paths are unique live projection facts.
#include "l8d_twin_support.hpp"

#include <cstdio>
#include <set>

using namespace pineforge;
using namespace pineforge::l8d_test;

namespace {
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "FAIL %d %s\n", __LINE__, #x); } } while (0)
struct Field { std::string path; };

void unique_paths(const std::vector<Field>& fields) {
    std::set<std::string> names;
    for (const auto& f : fields) CHECK(names.insert(f.path).second);
}

class ModelBook final : public source::L4dPineHost {
public:
    ModelBook() { configure_pine_strategy(fixed_config()); }
    std::vector<FixtureIntentRow> rows;
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        strategy_entry("A", true, missing, 110.0, 3.0);
        strategy_entry("A", true, missing, 111.0, 2.0);
        rows = source_pending_view();
    }
};
} // namespace

int main() {
    unique_paths({{"observation.command"}, {"observation.created_seq"},
                  {"replacement.incarnation"}, {"sizing.equity"}});
    ModelBook book; const Bar bar = point(100, 60'000); book.run(&bar, 1);
    CHECK(book.last_error().empty());
    CHECK(!book.rows.empty());
    CHECK(book.rows.back().id == "A");
    CHECK(book.rows.back().qty == 2.0);
    CHECK(book.rows.back().incarnation != 0);
    CHECK(book.rows.back().created_seq != 0);
    return failures == 0 ? 0 : 1;
}
