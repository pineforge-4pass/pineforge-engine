// A4 referent: the source provider's session key is the native example-style
// session + "@" + timezone identity, not an opaque unrelated literal.
#include <pineforge/source/pine_adapter.hpp>

#include <cstdio>

using namespace pineforge;
using namespace pineforge::source;

namespace {
int failures = 0;
#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #expression); ++failures; } } while (false)
}

int main() {
    const Bar bars[] = {
        {100, 101, 99, 100, 1, 0},
        {101, 102, 100, 101, 60'000},
    };
    NativeBeginArgs args{};
    args.bars = bars;
    args.n = 2;
    args.input_tf = "1";
    args.script_tf = "1";
    StagedConfiguration staged{};
    staged.syminfo.session = "0930-1600";
    staged.syminfo.timezone = "America/New_York";
    PineExecutionAdapter adapter;
    const NativeRunSpec first = adapter.project(PineStrategyConfig{}, staged, args);
    const std::string expected = staged.syminfo.session + "@" + staged.syminfo.timezone;
    CHECK(first.identity.session_key == expected);
    CHECK(first.identity.run_number == 1);
    const NativeRunSpec second = adapter.project(PineStrategyConfig{}, staged, args);
    CHECK(second.identity.session_key == expected);
    CHECK(second.identity.run_number == 2);
    std::printf("native session-key derivation: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
