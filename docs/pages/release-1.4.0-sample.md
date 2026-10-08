# Planned 1.4.0 — public provenance sample {#release_140_sample}

@tableofcontents

This synthetic sample compares the released **1.3.0 source pair** with the
**unreleased 1.4.0 candidate pair**. Both were built and installed on the same
x86-64 Ubuntu 24.04 spot host with GCC 13.3.0, then run through their own
installed `docker/entrypoint.sh` and `run_json.py`. It is a report-shape
example: 40 constant bars, no trades, no performance measurement and no
private source or feed.

| Pair | Engine commit | Codegen commit |
|---|---|---|
| Released 1.3.0 sources | `ec5d8bf46210bac103870ecf6fc74865cad66c8d` | `0736180e4b1ae8068df222f72ccc6a8a64471d3f` |
| Planned 1.4.0 candidate | `b3192bfc2f5a24bf4efd6d1d01e01e8fe619dfed` | `bfc4ddce453db1ba810692af9b9ba3310d87f0ac` |

These are fresh builds from pinned source, not downloaded release binaries.
Both candidate VERSION files still say 1.3.0. With
`PINEFORGE_VERSION_SOURCE=FILE`, the installed candidate reports version
`1.3.0` and engine commit `b3192bf`; the old build reports `1.3.0` and
`ec5d8bf`. The candidate output has not been relabeled as a tagged artifact.

## Shared inputs

The exact Pine source is:

```pine
//@version=6
strategy("REL-140 public provenance sample", initial_capital=10000)
length = input.int(14, "Length")
shade = input.color(color.red, "Color")
first = input.int(5, "Period")
second = input.int(10, "Period")
plot(length + first + second, color=shade)
```

The feed has header `timestamp,open,high,low,close,volume` and 40 rows. Row
`i`, for `i = 0..39`, is `i*60000,100,101,99,100,1000`, with decimal integer
text and LF line endings. Input/script timeframes are both `1`.
The inputs argument is `{"Length":"21"}` and the overrides argument is
`{"initial_capital":"12000"}` for both pairs.

| Input bytes | SHA-256 |
|---|---|
| Pine source, final LF included | `b34d4e3c310e87ce94f27c8e48deca1f40e3f50e31e4ff06218febe822e188e7` |
| Feed CSV | `fc4119e01f5d24563e82af6b10d492176796f9e39a34683bb32f1312197e654f` |
| Inputs argument, no trailing LF | `6cf090c3c7827f6250219c10504a736f79705ea303fa5772742c80def2ac097f` |
| Overrides argument, no trailing LF | `3b8c0336852d0702202b2b3fd7d0c1c20b4156a1090ebaf9a82d59cfc6f51282` |

## Ordinary Pine invocation

Both installed entrypoints successfully transpile, compile and run the
unmodified Pine source (exit 0, 40 bars, zero trades). The old report has a
fingerprint; the candidate report has **`fingerprint: null`**. On the
candidate's checked-settings path, a repeated title makes the native receipt
ambiguous, so provenance normalization refuses the complete fingerprint.
This does not turn the successful backtest into a failed run.
Rename one of the two `Period` inputs so their titles are distinct to obtain
a fingerprint on the checked path. This remedy is specific to the repeated
title; an unresolved legacy row, shown below, can remain fingerprinted.

## Matched legacy-settings invocation

To expose the per-title legacy resolution records, each pair's installed
transpile-only entrypoint emitted the same Pine source to C++. In each
emission, every occurrence of these four names was prefixed with
`rel140_legacy_`: `strategy_settings_api_version`, `strategy_create_checked`,
`strategy_set_input_checked`, `strategy_set_override_checked`. The effective
settings receipt export was kept. Each pair's installed entrypoint then
consumed that C++ with the same feed and settings. Both invocations exit 0
and truthfully report `transpiled_from_pine: false`: this invocation consumed
C++, even though a prior invocation generated it from Pine.

The following are exact value excerpts from the two real stdout reports,
with indentation added. They are not reconstructed expectations.

**Released 1.3.0 pair — `fingerprint.provenance.inputs`:**

```json
{
  "Length": {"type":"int","default":14,"value":"21"},
  "Color": {"type":"int64","default":"pine_color::red","value":"pine_color::red"},
  "Period": {"type":"int","default":5,"value":5}
}
```

**Candidate pair — the same field:**

```json
{
  "Length": {"type":"int","default":14,"value":21},
  "Color": {
    "type":"int64","default":null,"value":null,
    "resolution":{"status":"unresolved","reason":"foreign_unverified_source","raw_default":"pine_color::red"}
  },
  "Period": {
    "type":"int","default":null,"value":null,
    "resolution":{"status":"unresolved","reason":"duplicate_title","raw_default":"5","distinct_native_inputs":2}
  }
}
```

In the old `provenance.applied`, Length and initial capital are strings
`"21"` and `"12000"`. In the candidate they are JSON numbers `21` and
`12000.0`. Both reports' top-level `applied_inputs` / `applied_overrides`
remain `{"Length":"21"}` / `{"initial_capital":"12000"}`.
The unresolved Color does not claim a native color value. Period's count
is the two distinct native `(type, default)` pairs, not a first-wins value.
The receipt in supplied C++ is self-attested by that module.

## Retained output identities

All four original stdout files were harvested unmodified. Their hashes
identify the recorded files, including their paths and elapsed-time fields;
rerunning need not reproduce those incidental bytes.

| Original stdout | Bytes | SHA-256 |
|---|---:|---|
| `old-pine.stdout` | 8,176 | `1fb6b2d9d754f02850096edcf2f143f058c931430ab7992917e389962ef10229` |
| `candidate-pine.stdout` | 5,372 | `d8b9f467d8508ab31b1cc387294c400fc7415e377909732e4d6a231a9f5ed3f8` |
| `old-legacy.stdout` | 8,177 | `4cd8e24d9cd956c463a663d79ae299a437b7ab1018b53a6d4deb60f74ec5624a` |
| `candidate-legacy.stdout` | 8,690 | `5fe9f12aaaddce0a60bc22ce2ecf1baf10c0c285c17a9aa6daf4f3cf137ce0e4` |

The legacy fingerprints are
`sha256:64bb282e5e067f1f187bec950679314d18354320461456880b6363679d2fbaa0`
and `sha256:e597bf50d7edceef4aa80ebdeacf679dc9bdfc7f88511d203201b604d2cccb06`.
Hashing each original decoded token reproduces its digest. Their difference
also includes generated-source and engine identity changes, so it does not
isolate the effect of typing alone. See the [harness contract](../../docker/README.md#backtest-fingerprint)
for source trust, unresolved values and the complete-fingerprint refusal
conditions.

## Failure contrasts

A second bounded spot run used these exact source pairs and feed bytes for
six requests on each pair, with explicit inputs/overrides replacing the
earlier values. The [release notes](../../CHANGELOG.md#executed-setting-and-self-stop-contrasts)
record every observed outcome and the candidate catalog classes. The first
three requests return old success reports and candidate `setting_rejected`
errors; both invalid-capital variants already fail on the old pair, and the
deliberate script stop fails on both pairs. All 12 original stdout/stderr
files, source bytes, actual compiled source, exits and request/install
identities were harvested after each invocation. Their manifest SHA-256 is
`06bc5c9fe93cca91baacb8f766098ff32f7f3d39877faa893ae25fb80e93c6ad`.
The 38-code candidate catalog SHA-256 is
`dcd701fe4bdd57a38d6858e96f719c1001f07f4a035f6b8ad25d720d4b83f601`.
No failure envelope or absent old code/args was synthesized.
