"""Expand the sorted CMake test registrations at their include site."""
from pathlib import Path
from typing import Iterator

from check_lanes_cmake_d import CheckFailure, Statement, _check_include_loop, _statements


def registration_statements(path: Path) -> Iterator[tuple[Path, Statement]]:
    """Yield (list file, command), retaining file names and source lines."""
    def read(source: Path) -> list[Statement]:
        try:
            return _statements(source, source.read_text(), mask_quotes=True)
        except (OSError, UnicodeError) as error:
            raise ValueError(f'{source}: cannot read CMake registrations: {error}') from error

    try:
        commands = read(path)
        index = 0
        while index < len(commands):
            command = commands[index]
            if (command.name.lower() == 'file'
                    and command.body.split()[:2] == ['GLOB', 'LANE_CMAKE']):
                _check_include_loop(commands[index:index + 5], False)
                directory = path.parent / 'lanes.d'
                files = sorted(child for child in directory.iterdir()
                               if child.name.endswith('.cmake'))
                if not files:
                    raise ValueError(f'{path}:{command.line}: no CMake registration files '
                                     f'in {directory}')
                for child in files:
                    for included in read(child):
                        yield child, included
                index += 5
            else:
                if command.name.lower() == 'include' and '${lane_file}' in command.body:
                    raise ValueError(f'{path}:{command.line}: include without a registration glob')
                yield path, command
                index += 1
    except (CheckFailure, OSError) as error:
        raise ValueError(f'cannot read test registrations from {path}: {error}') from error
