"""Follow the sample-deviation branches with a logical INT_MAX-element window."""
from pathlib import Path
import subprocess
import sys
import tempfile

from test_ta_full_window_arithmetic import body, one


def program():
    source = (Path(__file__).resolve().parents[1] / "src/ta_volatility_trend.cpp").read_text()
    # Take the whole unbiased update through its last warm-up return. The
    # two-pass calculation and the held value are identified by branch only.
    update = one(r"(?m)^    auto& buffer = \*buffer_;(.*?)^    double sum = 0\.0;",
                 body(source, "StdDev::compute"))
    replay = one(r"(?m)^    auto& buffer = \*buffer_;(.*)\Z", body(source, "StdDev::recompute"))
    return r"""
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>

enum Branch { unseeded, held, calculate };
template<class> Branch na() { return unseeded; }
bool is_na(double src) { return std::isnan(src); }

struct Window {
    std::size_t n;
    std::size_t size() const { return n; }
    void push_back(double) { ++n; }
    double front() const { return 17.0; }
    void pop_front() { --n; }
    void pop_back() { --n; }
    void push_front(double) { ++n; }
};

struct Sample {
    Window buffer;
    int length_ = INT_MAX;
    bool pushed_ = false, evicted_ = false;
    double evicted_value_ = 0.0;
    Branch held_stdev() const { return held; }
    Branch compute(double src) {
""" + update + r"""
        return calculate;
    }
    Branch recompute(double src) {
""" + replay + r"""
    }
};

int main() {
    static_assert(INT_MAX == 2147483647);
    constexpr auto full = static_cast<std::size_t>(INT_MAX);
    static_assert(full < std::numeric_limits<std::size_t>::max());
    const double missing = std::numeric_limits<double>::quiet_NaN();
    Sample sample{{full - 2}};
    int failures = 0;
    bool controls_ok = true;
    auto check = [&](const char* label, Branch (Sample::*step)(double), double src,
                     std::size_t size, bool evict, Branch want, bool control = false) {
        if (!controls_ok) {
            std::printf("NOT COMPARED %s: preceding control failed\n", label);
            return;
        }
        const Branch got = (sample.*step)(src);
        const bool ok = sample.buffer.size() == size && sample.pushed_ == !is_na(src)
            && sample.evicted_ == evict && (!evict || sample.evicted_value_ == 17.0) && got == want;
        const char* branches[] = {"unseeded", "held", "calculate"};
        std::printf("%s %s: size=%zu pushed=%d evicted=%d branch=%s\n",
                    ok ? "PASS" : "FAIL", label, sample.buffer.size(), sample.pushed_,
                    sample.evicted_, branches[got]);
        failures += !ok;
        if (control) controls_ok = ok;
    };
    check("control partial push", &Sample::compute, 1, full - 1, false, unseeded, true);
    check("control partial na", &Sample::compute, missing, full - 1, false, unseeded, true);
    check("control seed without eviction", &Sample::compute, 2, full, false, calculate, true);
    check("full push evicts oldest", &Sample::compute, 3, full, true, calculate);
    check("valid replay evicts oldest", &Sample::recompute, 4, full, true, calculate);
    check("na holds after full push", &Sample::compute, missing, full, false, held);
    return failures != 0;
}
"""


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("NOT COMPARED sample-deviation window: expected the C++ compiler")
    try:
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "sample-window"
            subprocess.run([sys.argv[1], "-std=c++17", "-x", "c++", "-", "-o", executable],
                           input=program(), text=True, check=True)
            sys.exit(subprocess.run([executable]).returncode)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(f"NOT COMPARED sample-deviation window: {error}")
