// B2-05: BASE clears sink receipts only after the last begin refusal. A
// refused reused run can therefore publish the prior generation's trailer.
#include "report_modes_fix_fixture.hpp"

using namespace report_modes_fix;

namespace {
class RepeatBeginHost final : public Host {
public:
    bool repeat_consumed_run = false;
    NativeRunSpecError refusal = NativeRunSpecError::None;

    void prepare_native_begin(const NativeBeginArgs&) override {
        if (repeat_consumed_run)
            refusal = configure_native(spec(1)).validation.error;
    }
};
}

int main() {
    return test_main("test_report_sink_receipt_after_refused_begin", [] {
        RepeatBeginHost host;
        host.close_first = true;
        configure(host);
        const auto path = sink_path("refused-begin");
        install_sink(host, path);
        const auto bars = tape(8);
        run(host, bars);
        Report first;
        host.fill_report(&first.value);
        const auto original = receipt(host);
        require(original.complete == 1 && original.count_e == 8 && original.count_t == 1,
                "run one did not produce nonempty complete E/T evidence");
        require(summary(host).complete == 1 && diagnostics(host).complete == 1,
                "run one lacks summary or diagnostics");
        const auto original_bytes = read_sink(path);
        require_trailer(original_bytes);
        const int calls = host.calculations;

        host.repeat_consumed_run = true;
        host.run(bars.data(), static_cast<int>(bars.size()));
        require(host.refusal == NativeRunSpecError::RunNumberNotAboveConsumedHighWater,
                "second begin did not reach the consumed-run refusal");
        require(!host.last_error().empty() && host.calculations == calls,
                "refused begin ran callbacks");
        require(host.native_consumed_high_water() == 1, "refused begin consumed a run number");
        const auto got = receipt(host);
        const auto folded = summary(host);
        const auto provenance = diagnostics(host);
        require(got.complete == 0 && got.count_e == 0 && got.count_t == 0
                    && got.sha256_hex[0] == '\0', "refused begin exposed run one's receipt");
        require(folded.complete == 0 && folded.count_t == 0,
                "refused begin exposed run one's summary");
        require(provenance.complete == 0 && provenance.count_e == 0,
                "refused begin exposed run one's diagnostics");
        require(read_sink(path) == original_bytes, "refused begin wrote the old sink");
    });
}
