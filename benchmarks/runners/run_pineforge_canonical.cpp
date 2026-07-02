// Three-way indicator-comparison runner for PineForge.
//
// Reads the same OHLCV CSV as the PyneCore and PineTS canonical runners,
// drives the libpineforge `ta::*` classes bar-by-bar, and emits per-bar
// values to canonical_pineforge.csv with the schema:
//   bar_index, timestamp_ms,
//   ema21, sma21, rsi14, atr14,
//   macd_line, macd_signal, macd_hist,
//   bb_basis, bb_upper, bb_lower
//
// Build via the corpus CMake setup with -DPINEFORGE_BUILD_BENCHMARKS=ON
// (see benchmarks/CMakeLists.txt) or directly:
//   c++ -std=c++17 -O2 \
//       -I../include \
//       runners/run_pineforge_canonical.cpp \
//       -L../build/lib -Wl,-force_load,../build/lib/libpineforge.a \
//       -o runners/run_pineforge_canonical
#include <pineforge/ta.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace pineforge;

struct Bar {
    int64_t timestamp_ms;
    double open, high, low, close, volume;
};

static std::vector<Bar> read_ohlcv(const std::string& path) {
    std::vector<Bar> bars;
    std::ifstream f(path);
    if (!f) {
        std::cerr << "cannot open " << path << "\n";
        std::exit(1);
    }
    std::string line;
    std::getline(f, line); // header
    while (std::getline(f, line)) {
        Bar b{};
        char* p = const_cast<char*>(line.c_str());
        char* end;
        b.timestamp_ms = std::strtoll(p, &end, 10);  p = end + 1;
        b.open         = std::strtod(p, &end);       p = end + 1;
        b.high         = std::strtod(p, &end);       p = end + 1;
        b.low          = std::strtod(p, &end);       p = end + 1;
        b.close        = std::strtod(p, &end);       p = end + 1;
        b.volume       = std::strtod(p, nullptr);
        bars.push_back(b);
    }
    return bars;
}

static std::string fmt(double v) {
    if (std::isnan(v)) return "";
    std::ostringstream os;
    os << std::setprecision(10) << v;
    return os.str();
}

int main(int argc, char** argv) {
    const char* in_path  = argc > 1 ? argv[1]
        : "../corpus/data/derived/ohlcv_ETH-USDT-USDT_15m_window.csv";
    const char* out_path = argc > 2 ? argv[2]
        : "strategies/_indicators/canonical_pineforge.csv";

    auto bars = read_ohlcv(in_path);
    std::cout << "pineforge: loaded " << bars.size() << " bars from " << in_path << "\n";

    ta::EMA ema21(21);
    ta::SMA sma21(21);
    ta::RSI rsi14(14);
    ta::ATR atr14(14);
    ta::MACD macd(12, 26, 9);
    ta::BB bb(20, 2.0);

    std::ofstream out(out_path);
    if (!out) {
        std::cerr << "cannot write " << out_path << "\n";
        return 1;
    }
    out << "bar_index,timestamp_ms,ema21,sma21,rsi14,atr14,"
        << "macd_line,macd_signal,macd_hist,bb_basis,bb_upper,bb_lower\n";

    for (size_t i = 0; i < bars.size(); ++i) {
        const auto& b = bars[i];
        double v_ema21 = ema21.compute(b.close);
        double v_sma21 = sma21.compute(b.close);
        double v_rsi14 = rsi14.compute(b.close);
        double v_atr14 = atr14.compute(b.high, b.low, b.close);
        ta::MACDResult m = macd.compute(b.close);
        ta::BBResult bbr = bb.compute(b.close);

        out << i << ',' << b.timestamp_ms << ','
            << fmt(v_ema21) << ','
            << fmt(v_sma21) << ','
            << fmt(v_rsi14) << ','
            << fmt(v_atr14) << ','
            << fmt(m.macd_line) << ','
            << fmt(m.signal_line) << ','
            << fmt(m.histogram) << ','
            << fmt(bbr.middle) << ','
            << fmt(bbr.upper) << ','
            << fmt(bbr.lower) << '\n';
    }
    std::cout << "pineforge: wrote " << out_path << " (" << bars.size() << " bars × 10 cols)\n";
    return 0;
}
