# Scratch receipt binding at begin

Arena C++ hosts install `detail::ScratchTrialConfig` with
`BacktestEngine::set_scratch_trial` after choosing each effective trial and before
calling a run or stream begin. Copy the complete JSON document returned by the
module's existing `strategy_order_shapes_receipt` export into
`order_shapes_receipt`. Supply the separate effective-trial `LiveBounds` proof
and, when accepting explicit input values equal to defaults, the module's
declared input defaults. A missing document is an empty string, not permission
to use P0. Installing this setup selects the receipt protocol for subsequent
begins, including begins on a reused handle.

The engine reads the additive `scratch_facts` object before provider setup or
bar zero. All four fields must be present with their declared JSON types.
Duplicate root/object fields, malformed JSON, unknown versions, noncanonical
hex, incomplete rosters, changed/unproved inputs and opaque run overrides route
to `ScratchTrialClassification::needs_file_capable_worker`. The existing V
reader and F decoder/decision/derivation supply certificate validation and all
capacity calculations. An explicit producer `out_of_subset` stays OUT even
when its certificate has complete rosters.

On IN, the engine constructs a fresh slab directly from `ScratchFinitePlan`'s
table manifest. It bypasses the legacy all-zero P0 sentinel. Existing provider
and kernel binders receive this owner before semantic begin; neither a row
operation nor a later bar changes its allocation. Each begin re-derives its
own plan, and each trial owns its receipt/default metadata and mutable slab.

On OUT, the engine returns before provider setup, run reset, report creation
or any backend bind. `scratch_trial_binding()` supplies the typed classification,
reason and the original `ScratchFactsError` when applicable, with no finite
plan or bound slab. The run has a nonzero `last_run_status()`. The host must
dispatch that trial to its file-capable worker or report this typed result as
failure. The existing error channel additionally carries `setting_unsupported`
and `scratch trial needs file-capable worker`; it adds no C failure code or
export. H does not implement the file backend or its scheduler.

This base has F's separate live-slot plan but only the historical-R root
binder. Missing live proofs therefore select F's external route. Nonzero
root/alias plans also route OUT with `root_backend_unavailable` until R supplies
its reusable binder. A proved zero-root/alias plan can bind and run now.
Live slots are never substituted for historical R, and R is never estimated
from bars. This is an integration boundary, not a claim that rootful programs
already have a finite execution backend.

The C scratch descriptor retains its legacy behavior for hosts that have not
installed arena trial setup. It is not an entry point for certificate plans.
The module loader must pass the existing receipt into the C++ setup seam;
this change neither looks up module symbols from the kernel nor changes the
codegen or public C ABI.
