# s1a_ticks_invalid_ordinary

Ordinary run, with no selected-window flags. This probe is ready for P0's ordinary driver. Pinned before implementation/execution: strategy_value_invalid, field=exit.loss_ticks, reason=nonfinite.

Runtime exp(10000) gives positive infinity. visit_call.py:3424-3441 passes loss unchanged as loss_ticks when stop is absent; this source supplies no stop.

Forty synthetic one-minute bars: timestamp=1704067200000+i*60000, O=100 H=101 L=99 C=100 V=1000. The invalid command executes at index 2. Paired native row: exit.loss_ticks/ordinary/+inf.
