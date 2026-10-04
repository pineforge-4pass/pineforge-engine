# Generated capability join fixtures

The `generated/` Pine sources and C++ libraries are frozen F08 snapshots from
codegen `285ac035`, with only the original capability receipt. They intentionally
exercise new-runner/old-library compatibility. Regenerate using that codegen
revision and diff them after a fixture change;
do not edit generated C++ by hand. The runner E2E builds these snapshots so its
admission tests exercise codegen's actual receipt exports.

On a build host, from the engine checkout, with the frozen codegen checkout at
`../codegen-f08`, verify without changing the checkout. This compares tracked and
untracked files alike:

```sh
output=$(mktemp -d)
PYTHONPATH=../codegen-f08 python3 tests/fixtures/strategy_capabilities/regenerate.py \
    --output "$output"
diff -ru tests/fixtures/strategy_capabilities/generated "$output"
rm -r "$output"
```

This documents the regeneration check next to the fixtures rather than adding a
codegen-checkout dependency to the standalone engine CI. Execute it on the same
build host as the focused runner tests.

The runner E2E compiles these libraries and tests every refused declaration,
requests (including unpinned sites), the exact positional review reproduction,
two admitted no-POOC/no-request market-order classes, receipt identity, changed
receipt on resume, and a legacy library. The two market-order equivalence
CTest rows compare all trade times, prices, quantities, direction, PnL and
range-end flags at warmup boundaries of 3, 30 and 500 bars.

`../strategy_capabilities.cpp` remains a hand-written malformed-version/buffer
fixture only; it is not the source of the declaration-admission evidence.
