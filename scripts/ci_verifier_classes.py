"""D1 verifier self-test tiers, shared by preflight and the shard runner.

Measured totals come from CI-SLIM/serial-test_ci_verify.log:429-608. FAST
requires a measured passing runtime below the policy threshold. ReceiptReuse
only measured errors, so it stays SLOW. The three classes with no printed
duration follow the supervisor's explicit hidden-duration exception.
"""
from __future__ import annotations

from typing import Mapping, Sequence

FAST_CLASSES = frozenset({
    'ConfigValidation',
    'ScratchGuardRows',
    'RowFloorConfig',
    'ProfileOptions',
    'RunnerSanitizerCoverage',
    'RuntimeBudgetLanes',
    'CopyCacheIdentity',
    'HistoricalProviderPinTable',
    'HistoricalProviderPins',
    'CAbiRuntimeInventory',
    'GitObjectDiscovery',
    'FloorDocsPointAtTheTable',
    'DiagnosticsCollection',
    'CTestDeadlines',
})
SLOW_CLASSES = frozenset({
    'ReceiptReuse',
    'DriverOrderingAndAggregation',
    'RowFloorDriver',
    'PrivateFixtureFloors',
    'PrExclusionWithFixturesAbsent',
    'LiveProfilesWithFixturesAbsent',
    'ExamplesAssertLive',
    'StaleBinaries',
    'ReceiptRecipe',
})


def verifier_class_tier(env: Mapping[str, str]) -> str:
    """Only an explicit PR deferral selects FAST; local/full events run ALL."""
    if (env.get('GITHUB_EVENT_NAME') == 'pull_request'
            and env.get('PINEFORGE_VERIFIER_SLOW') == 'false'):
        return 'fast'
    return 'all'


def validate_class_partition(discovered: set[str]) -> None:
    """Fail on unclassified, stale or multiply classified TestCase classes."""
    overlap = FAST_CLASSES & SLOW_CLASSES
    missing = discovered - (FAST_CLASSES | SLOW_CLASSES)
    stale = (FAST_CLASSES | SLOW_CLASSES) - discovered
    if overlap or missing or stale:
        raise ValueError(f'verifier class partition: overlap={sorted(overlap)}, '
                         f'unclassified={sorted(missing)}, stale={sorted(stale)}')


def select_test_ids(test_ids: Sequence[str], tier: str = 'all') -> list[str]:
    """Defer only named SLOW classes; a newly discovered class always runs."""
    if tier == 'all':
        return list(test_ids)
    if tier == 'fast':
        return [test_id for test_id in test_ids
                if test_id.rsplit('.', 2)[-2] not in SLOW_CLASSES]
    raise ValueError(f'unknown verifier class tier: {tier}')
