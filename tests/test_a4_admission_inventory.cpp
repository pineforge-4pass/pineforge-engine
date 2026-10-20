// P37/P38/P40 fail-before target: 6cfcff0696. Run the old-header compilation
// remotely before applying the implementation; no local execution is allowed.
#include <pineforge/compat/pine/market_admission.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
std::uint64_t fold(std::uint64_t digest, const admission::Event& event) {
    BrokerStateHashSink sink;
    sink.h = digest;
    admission::reflect(event, "event", [&](const admission::Field& field) {
        sink.s(field.path); sink.u(field.value.index());
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::uint64_t>) sink.u(value);
            else if constexpr (std::is_same_v<T, std::int64_t>) sink.i(value);
            else if constexpr (std::is_same_v<T, double>) sink.d(value);
            else sink.s(value);
        }, field.value);
    });
    return sink.h;
}
admission::Event command(admission::Journal& journal, int bar, std::size_t width) {
    auto observed = std::make_shared<admission::CommandObservation>();
    observed->command = journal.next_sequence();
    observed->bar = bar;
    observed->id = "command-" + std::to_string(observed->command);
    observed->kind = admission::CommandKind::Raw;
    observed->configuration.default_quantity_type = 1;
    observed->configuration.default_quantity_value = 100.0;
    observed->configuration.pyramiding = 1;
    observed->configuration.long_margin = 100.0;
    observed->configuration.short_margin = 100.0;
    admission::CommandEvent event;
    event.observation = observed;
    event.outcome = admission::Outcome::RejectedAffordability;
    for (std::size_t i = 0; i < width; ++i) {
        admission::BookObservation old;
        old.incarnation = i + 1;
        old.priority = static_cast<std::int64_t>(width - i);
        old.bar = bar;
        old.id = "before-" + std::to_string(i);
        old.birth = OrderBirth::chart_evaluation(bar, 1736121600000LL + bar * 60000LL);
        event.before.push_back(std::move(old));
        if (i % 2 == 0) event.removed.push_back(i + 1);
    }
    return event;
}

void journal_population_and_prefix() {
    admission::Journal journal, full;
    full.retain_full_observation_history();
    std::vector<admission::Event> reference;
    constexpr std::size_t width = 137, count = 257;
    std::uint64_t digest = 1469598103934665603ULL;
    for (std::size_t i = 0; i < count; ++i) {
        auto event = command(journal, 3, width);
        auto diagnostic = command(full, 3, width);
        digest = fold(digest, event);
        reference.push_back(event);
        journal.append(std::move(event));
        full.append(std::move(diagnostic));
    }
    require(journal.events().size() == count, "P37 reaches all live events before retirement");
    require(journal.resident_bytes() <= journal.resident_limit(), "events and nested books have a fixed resident bound");
    std::uint64_t replay = 1469598103934665603ULL;
    std::size_t index = 0;
    for (const auto& event : journal.events()) {
        require(admission::sequence(event) == admission::sequence(reference[index++]), "journal sequence order");
        const auto& row = std::get<admission::CommandEvent>(event);
        require(row.before.size() == width && row.removed.size() == (width + 1) / 2,
                "all nested observations survive spill");
        replay = fold(replay, event);
    }
    require(replay == digest && index == count, "reflection emits the original full field stream");
    journal.retire_prefix(count, digest, count);
    full.retire_prefix(count, digest, count);
    require(journal.events().empty() && journal.retired_prefix().count == count
            && journal.retired_prefix().digest == digest
            && journal.retired_prefix().last_sequence == count
            && compat::pine::last_rejected_command_bar(journal) == 3,
            "L19e exact prefix retirement retains its digest and rejection index");
    require(full.events().size() == count && full.retired_prefix().count == 0,
            "Full override keeps diagnostic history");
    require(full.resident_bytes() <= full.resident_limit(), "Full does not turn off the resident bound");
    journal.reset();
    require(journal.event_count() == 0 && journal.sequence_frontier() == 1, "fresh-run reset");
}

void out_of_order_review_and_abandon() {
    admission::Journal journal;
    auto first = command(journal, 2, 11);
    auto second = command(journal, 2, 13);
    admission::ReviewEvent review;
    review.receipt = {journal.next_sequence(), admission::Checkpoint::ExplicitPair, 2, 0};
    for (int i = 0; i < 281; ++i) {
        admission::BookObservation row;
        row.incarnation = static_cast<std::uint64_t>(i + 1);
        row.id = "review-" + std::to_string(i);
        review.book.push_back(row);
        review.reviewed.push_back(std::move(row));
        review.resolutions.push_back({static_cast<std::uint64_t>(i + 1),
                                      admission::ResolutionKind::Original, 0, i, 0, 0.0});
        review.causes.push_back(i + 1);
    }
    const auto expected = fold(1469598103934665603ULL, admission::Event(review));
    journal.append(admission::Event(std::move(review)));
    journal.append(std::move(second));
    journal.append(std::move(first));
    require(journal.events().size() == 3, "three out-of-order completions are retained");
    require(admission::sequence(journal.events()[0]) == 1
            && admission::sequence(journal.events()[1]) == 2
            && admission::sequence(journal.events()[2]) == 3,
            "completion order does not change review order");
    require(fold(1469598103934665603ULL, journal.events()[2]) == expected,
            "review book, reviewed, resolution and cause ordering is exact");
    { auto unfinished = journal.reserve(); (void)unfinished; }
    require(!journal.has_unfinished_events(), "unwound reservation abandons only its own id");
    require(journal.resident_bytes() <= journal.resident_limit(), "wide review resident bound");
}

void reflected_workspace() {
    admission::Draft draft;
    auto observed = std::make_shared<admission::CommandObservation>();
    observed->command = 7;
    observed->id = "reflect-me";
    observed->original_sizing = admission::SizingObservation{3.0, 19.0, 5.0, 7.0, 11.0};
    draft.bind(observed);
    draft.reviewed({8, admission::Checkpoint::DefaultGross, 0, 7});
    const auto fields = admission::fields(draft);
    std::vector<admission::Field> reference;
    admission::reflect(draft, "draft", [&](const auto& field) { reference.push_back(field); });
    require(fields.size() == reference.size() && fields.size() > 50, "P38 reaches the entire reflected draft");
    require(fields.resident_bytes() <= fields.resident_limit(), "reflection collection uses fixed pages");
    std::size_t cursor = 0;
    for (const auto& field : fields) {
        require(field.path == reference[cursor].path && field.value.index() == reference[cursor].value.index(),
                "reflection path and variant tag order unchanged");
        ++cursor;
    }
    require(admission::read_string(fields, "draft.observation.id") == "reflect-me"
            && admission::read_double(fields, "draft.observation.original_sizing.mark") == 7.0
            && admission::read_unsigned(fields, "draft.review.sequence") == 8,
            "typed reflected readers retain their exact paths");
}

class WidePendingHost final : public source::PineStrategyHost {
public:
    explicit WidePendingHost(int width) : width_(width) {
        source::PineStrategyConfig configuration;
        configuration.pyramiding = width;
        configuration.initial_capital = 1e9;
        configure_pine_strategy(configuration);
        set_syminfo_timezone("UTC"); set_syminfo_session("24x7"); set_syminfo_mintick(0.25);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        for (int i = 0; i < width_; ++i)
            strategy_entry("pending-" + std::to_string(i), true, 1.0, admission::absent, 1.0);
        const auto& view = source_pending_view();
        require(view.size() == static_cast<std::size_t>(width_), "P40 reaches the live pending population");
        require(view.resident_bytes() <= view.resident_limit(), "pending owning compatibility view is paged");
        std::size_t visited = 0;
        visit_source_pending([&](const FixtureIntentRow& row) {
            require(row.id == "pending-" + std::to_string(visited++), "visitor retains pending queue order");
        });
        require(visited == view.size(), "visitor visits every pending row");
        reached = visited;
    }
    std::size_t reached = 0;
private:
    int width_;
};
void pending_projection() {
    const Bar bar{100.0, 101.0, 99.0, 100.0, 1.0, 1736121600000LL};
    WidePendingHost host(271);
    host.run(&bar, 1, "1", "1");
    require(host.last_error().empty() && host.reached == 271, "wide pending projection completed");
}
} // namespace

int main() {
    try {
        journal_population_and_prefix(); out_of_order_review_and_abandon();
        reflected_workspace(); pending_projection();
        std::puts("A4 admission inventory: journal, reflection and pending populations passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "A4 admission inventory: %s\n", error.what());
        return 1;
    }
}
