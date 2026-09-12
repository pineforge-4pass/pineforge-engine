# Frozen e7d023d native host/event header closure

Exact public include closure of `<pineforge/native_host.hpp>` from
`e7d023dbdff1c98229155ec5bcdd1e4ac534f5fb` (tree
`0201bf052429490fb453bbfd6037e5afd1669626`). `NativeStrategyHost`,
`NativeMarketEvent`, and `native_events()` live in `engine_script_run_v12`.
Used only as a compile/link control so an old host that only reads returned
events cannot silently decode a later `CommandEvent` layout. No executable is
run.
