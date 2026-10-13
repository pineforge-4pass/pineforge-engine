# s1a_price_invalid_ordinary

Ordinary run, with no selected-window flags. This probe is ready for P0's ordinary driver. Pinned before implementation/execution: strategy_value_invalid, field=entry.limit_price, reason=nonfinite.

math.exp is lowered to std::exp by pineforge-codegen-oss/pineforge_codegen/codegen/tables.py:1223 and visit_call.py:2051. close is a runtime series value. Every feed close is 100; exp(10000) overflows binary64 to positive infinity.

Forty synthetic one-minute bars: timestamp=1704067200000+i*60000, O=100 H=101 L=99 C=100 V=1000. The invalid command executes at index 2. Paired native row: entry.limit_price/ordinary/+inf.
