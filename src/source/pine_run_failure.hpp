#pragma once

// The run-failure code the Pine adapter's spec refusal carries (run_failure.hpp).
// Not installed API.

#include <pineforge/native_run_spec.hpp>
#include <pineforge/run_failure.hpp>

namespace pineforge::source {

// The code PineExecutionAdapter::project throws "Pine adapter produced invalid
// native run spec field N" with, chosen by the refused field:
//   symbol facts (5-14, 16-18)                -> symbol_metadata_rejected{field}
//   the quantity grid (22)                    -> lot_grid_rejected
//   the strategy's own settings (15, 19-21, 23, 25-28, 39-40, 43-60, 65)
//                                             -> strategy_settings_rejected{field}
//   the run options (3, 4, 29-33, 37)         -> run_options_rejected{option}
//   the installed symbol feeds (66-71)        -> symbol_feeds_refused
//                                                {kernel_refused_series, field}
//   every field the adapter builds itself     -> engine_invariant
// The argument is the field's name in snake_case, never its number.
RunFailureValue run_spec_field_failure(NativeRunSpecField field) noexcept;

// The symbol_feeds_refused `field` of an installed symbol feed's spec field
// (66-71), or nullptr for any other field.
const char* instrument_feed_field_name(NativeRunSpecField field) noexcept;

}  // namespace pineforge::source
