# Checked generated-strategy settings

This is an additive, opt-in extension to the generated strategy C ABI, not a
kernel execution change or a native capability receipt. Regenerate and relink
the strategy with the paired codegen and engine to obtain it. The legacy
factory, setters and batch calls keep their successful-call behavior and
default computations; all generated C boundaries now contain C++ exceptions.

Discover `strategy_settings_api_version()` with `dlsym`; version 1 is
`PF_SETTINGS_API_VERSION`. This does not increment `PF_ABI_VERSION`. An older
strategy without the symbol still works with the legacy host interface.

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
The Pine `step` is a UI increment, not an enforced numeric lattice.

Query a receipt with NULL/0 and a non-NULL `required` pointer. The
`PF_SETTINGS_BUFFER_TOO_SMALL` result supplies the required byte count,
including NUL. Allocate that many bytes and read again. Short buffers are
cleared, never filled with partial JSON. Receipt version 1 has ordered `inputs`
and `overrides` arrays. Each row includes `name`, `type`, `default`,
`effective_value`, `supported`, `options`, `option_values`, `min`, `max`, `step`.
Enum `option_values` records the actual indices of an explicitly restricted
options list, rather than renumbering the remaining members.
Values are canonical serialized strings; absent numeric constraints are null.
Every declared input and every supported strategy override is listed, even
when the host supplies none. The receipt reads existing state; it does not
introduce a second durable configuration store.

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
