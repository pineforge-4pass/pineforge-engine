# Checked generated-strategy settings

This is an additive, opt-in extension to the generated strategy C ABI, not a
kernel execution change or a compiled capability receipt; the separate
[execution-capabilities extension](strategy-capabilities.md) supplies that proof. Regenerate and relink
the strategy with the paired codegen and engine to obtain it. The legacy
factory, setters and batch calls keep their successful-call behavior and
default computations; all generated C boundaries now contain C++ exceptions.
Legacy run and setter exceptions are readable through `strategy_get_last_error`,
including the entry point and `what()` (or `unknown C++ exception`). A thrown
legacy setter permanently blocks subsequent batch and stream begins on that
handle, independently of calls that clear `last_error_`. Batch calls return
the existing empty-report failure shape; checked batch calls return
`PF_SETTINGS_RUN_FAILED`. Recreate the handle to configure again. Non-throwing
legacy setters still retain their historical acceptance and silent-ignore rules.
Generated script preparation throws `checked_settings::LatchedSettingsFailure`
with the original setter message. With the paired engine, batch and stream
refuse through the same native-begin seam and report NOT_COMPLETED. Old engines
use the legacy batch wrapper's empty-report fallback instead. Standard and
unknown script-preparation exceptions also fail stream begin with the
preparation message and NOT_COMPLETED. Batch retains its historical behavior
for these ordinary exceptions: preparation is disabled, the report is empty,
the message is readable, and the run status is completed.

Every checked setter and receipt query on a latched handle returns
`PF_SETTINGS_RUN_FAILED` with the original message. A throwing legacy setter
mid-stream does not alter the active stream's begin-time settings: subsequent
pushes continue with those settings and may clear the diagnostic, but all later
begins remain refused. Legacy `stoi` exception wording depends on the standard
library (for example `stoi` or `stoi: no conversion`); the first message is
retained verbatim within the handle. Legacy batch catch paths zero the report.

Discover `strategy_settings_api_version()` with `dlsym`; version 1 is
`PF_SETTINGS_API_VERSION`. This does not increment `PF_ABI_VERSION`. An older
strategy without the symbol still works with the legacy host interface.
The runner refuses a present settings API version other than 1; it only warns
and falls back when the capability symbol is absent.

The declarations and status codes are in `pineforge/pineforge.h`:

```c
uint32_t strategy_settings_api_version(void);
int strategy_create_checked(const char* params_json, pf_strategy_t* out,
                            char* error, size_t error_capacity);
int strategy_set_input_checked(pf_strategy_t s, const char* key,
                               const char* value, char* error, size_t error_capacity);
int strategy_set_override_checked(pf_strategy_t s, const char* key,
                                  const char* value, char* error, size_t error_capacity);
int strategy_get_effective_settings(pf_strategy_t s, char* json, size_t capacity,
                                    size_t* required, char* error, size_t error_capacity);
int run_backtest_full_checked(pf_strategy_t s, pf_bar_t* bars, int n,
                             const char* input_tf, const char* script_tf,
                             int bar_magnifier, int magnifier_samples,
                             pf_magnifier_distribution_t magnifier_dist,
                             pf_report_t* out, char* error, size_t error_capacity);
```

Every checked call returns a `pf_settings_status_t` value. Zero means success;
invalid arguments, unsupported settings, exceptions, insufficient receipt
capacity and runtime-reported batch failures have distinct nonzero codes.
Errors use optional caller-owned buffers, always NUL-terminated when capacity
is nonzero. Standard and non-standard exceptions are caught without allocating
an error message; factories initialize the output handle to NULL.
The checked factory refuses nonempty reserved `params_json`; configure through
the checked setters instead of supplying a silently ignored JSON document.

## Validation and receipt

Configure before execution begins. Checked setters refuse unknown or ambiguous
input keys, unknown override keys, invalid options/enums/booleans, integer
overflow, non-finite numbers, numeric suffixes and declared range violations.
Booleans accept exactly `true`, `false`, `1`, `0`. Numeric values are complete
decimal strings, without whitespace. Enums accept declared qualified member
names or their declared numeric indices; strategy enum overrides retain their
documented aliases. A rejected value does not replace the prior setting.
Integer inputs also accept exactly integral decimal/exponent forms: `5.0` and
`5e0` both canonicalize to `5`; `5.5`, NaN/Inf and out-of-range integers are
refused without rounding. Enum labels use `Enum.member`, not Pine display titles
or unqualified member names; an invalid label reports `invalid enum option`.
The Pine `step` is a UI increment, not an enforced numeric lattice.

Timeframe, session and symbol strings pass through without semantic validation
in v1 (explicit declared option lists are still enforced). Time inputs use
signed 64-bit Unix epoch milliseconds encoded as decimal integers, not ISO date
strings. Color inputs use the generated getter's integer encoding: packed
`0xAARRGGBB` expressed in decimal (opaque red is `4294901760`), not hexadecimal
strings, CSS names or Pine expressions such as `color.red`.

Query a receipt with NULL/0 and a non-NULL `required` pointer. The
`PF_SETTINGS_BUFFER_TOO_SMALL` result supplies the required byte count,
including NUL. Allocate that many bytes and read again. Short buffers are
cleared, never filled with partial JSON. Receipt version 1 has ordered `inputs`
and `overrides` arrays. Each row includes `name`, `type`, `kind`, `default`,
`effective_value`, `supported`, `options`, `option_values`, `min`, `max`, `step`.
Enum `option_values` records the actual indices of an explicitly restricted
options list, rather than renumbering the remaining members.
Immutable identifier enum defaults are folded to their declared member for both
the getter and receipt. An unresolved default is marked unsupported and uses
`na` in the receipt without reading a script member; its legacy getter is
unchanged. Enum options are supported only when every option is a literal member
of the same enum. Merge the paired engine before releasing this codegen.
Values are canonical serialized strings; absent numeric constraints are null.
`type` is the generated storage type; `kind` is the manifest form type (for
example a color has storage `int` and form `string`). Override defaults describe
the script declaration, independently of effective post-override configuration.
Every declared input and every supported strategy override is listed, even
when the host supplies none. The receipt reads existing state; it does not
introduce a second durable configuration store.
For `input.source`, the receipt reports the stored selector, not a resolved
series. Checked setters constrain it to the scheduler's supported source names.
A legacy setter can store an unknown name while the scheduler falls back to
`close`; mixing legacy setters with the receipt is not a validation guarantee.

`pineforge-live` discovers the extension and uses checked creation/setters.
It refuses bad settings before opening/binding the ledger or delivering HTTP
events. The full effective receipt contributes to its deployment hash and is
returned as `effective_settings` in the run summary. A settings change therefore
cannot resume an incompatible ledger. An older strategy prints a warning and
keeps legacy settings behavior, with a null receipt and unchanged identity
construction. Existing ledgers from a newly regenerated strategy require a
new deployment/ledger because both the library and effective-settings identity
change. This does not claim native run-spec capability validation (a separate
extension).
