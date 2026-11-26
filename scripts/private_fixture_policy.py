"""The fixture mode shared by verification, its source guards and Python tests."""
from __future__ import annotations

import os
import re
from pathlib import Path


def private_fixtures_overlay() -> bool:
    return bool(os.environ.get('PINEFORGE_PRIVATE_FIXTURES', '').strip())


def fixture_revision() -> str:
    # The mode may be a flag or a revision. Never report a boolean, an
    # arbitrary string or engine HEAD as the fixture repository's revision.
    for key in ('PINEFORGE_PRIVATE_FIXTURES', 'PINEFORGE_PRIVATE_FIXTURES_SHA'):
        value = os.environ.get(key, '').strip()
        if re.fullmatch(r'[0-9a-fA-F]{7,40}', value) and value.strip('0'):
            return value
    return '(revision not supplied)'


def fixture_available(present: bool, path: str | Path) -> bool:
    """An optional fixture becomes a prerequisite when private mode is requested."""
    if not present and private_fixtures_overlay():
        raise FileNotFoundError(f'NOT COMPARED: private-fixtures at {fixture_revision()} '
                                f'requires {path}; input absent')
    return present
