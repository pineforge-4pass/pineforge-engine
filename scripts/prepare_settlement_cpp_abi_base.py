#!/usr/bin/env python3
"""Build the exact R2 settlement ABI provider from local Git objects, without tests.

Run explicitly before CTest. This script never fetches Git history or dependencies.
The exact Git object and Eigen must already be available to the current build.
"""
from __future__ import annotations
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import tarfile

BASE_COMMIT = 'e60e57156a54bb1c74c0c0a4f5a338fd924c046d'
BASE_TREE = 'e209c89035ec884774f8f3e68e1dc118f26aaf51'
ROOT = Path(__file__).resolve().parents[1]
HEADER_MANIFEST = ROOT / 'tests/fixtures/settlement_cpp_abi/e60e571/manifest.json'
COPY_CACHE = (
    'CMAKE_BUILD_TYPE', 'CMAKE_C_COMPILER', 'CMAKE_CXX_COMPILER',
    'CMAKE_C_COMPILER_TARGET', 'CMAKE_CXX_COMPILER_TARGET',
    'CMAKE_C_COMPILER_EXTERNAL_TOOLCHAIN', 'CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN',
    'CMAKE_C_FLAGS', 'CMAKE_CXX_FLAGS',
    'CMAKE_C_FLAGS_DEBUG', 'CMAKE_CXX_FLAGS_DEBUG',
    'CMAKE_C_FLAGS_RELEASE', 'CMAKE_CXX_FLAGS_RELEASE',
    'CMAKE_C_FLAGS_RELWITHDEBINFO', 'CMAKE_CXX_FLAGS_RELWITHDEBINFO',
    'CMAKE_C_FLAGS_MINSIZEREL', 'CMAKE_CXX_FLAGS_MINSIZEREL',
    'CMAKE_EXE_LINKER_FLAGS', 'CMAKE_STATIC_LINKER_FLAGS',
    'CMAKE_POSITION_INDEPENDENT_CODE', 'CMAKE_INTERPROCEDURAL_OPTIMIZATION',
    'CMAKE_OSX_ARCHITECTURES', 'CMAKE_OSX_SYSROOT', 'CMAKE_OSX_DEPLOYMENT_TARGET',
    'CMAKE_SYSROOT', 'CMAKE_TOOLCHAIN_FILE', 'CMAKE_PREFIX_PATH', 'Eigen3_DIR',
    'CMAKE_DISABLE_FIND_PACKAGE_Eigen3', 'FETCHCONTENT_SOURCE_DIR_EIGEN',
    'PINEFORGE_ENABLE_SANITIZERS', 'PINEFORGE_ENABLE_COVERAGE',
)


def identity(path: Path) -> dict:
    raw = path.read_bytes()
    return {'sha256': hashlib.sha256(raw).hexdigest(), 'bytes': len(raw)}


def read_cache(path: Path) -> dict[str, str]:
    found = {}
    for line in path.read_text().splitlines():
        if line and not line.startswith(('#', '//')) and ':' in line and '=' in line:
            name_type, value = line.split('=', 1)
            found[name_type.split(':', 1)[0]] = value
    return found


def current_eigen_source(cache: dict[str, str], current: Path) -> Path | None:
    """Reuse the current build's populated dependency without a network fetch."""
    candidates = [cache.get(key) for key in
                  ('FETCHCONTENT_SOURCE_DIR_EIGEN', 'Eigen3_SOURCE_DIR', 'eigen_SOURCE_DIR')]
    candidates.append(str(Path(cache.get('FETCHCONTENT_BASE_DIR') or current / '_deps') / 'eigen-src'))
    for value in candidates:
        if not value:
            continue
        path = Path(value).resolve()
        if (path / 'CMakeLists.txt').is_file() and (path / 'Eigen/Core').is_file():
            return path
    return None


def run(argv, *, cwd=None, timeout=60, log: Path | None = None) -> bytes:
    result = subprocess.run(argv, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    if log:
        log.write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f'command failed ({result.returncode}): {shlex.join(map(str, argv))}\n'
                           + result.stderr.decode('utf-8', 'replace')[-6000:])
    return result.stdout


def extract_tar(raw: bytes, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=False)
    with tarfile.open(fileobj=io.BytesIO(raw)) as archive:
        members = archive.getmembers()
        seen = set()
        for member in members:
            path = Path(member.name)
            if path.is_absolute() or '..' in path.parts or member.name in seen:
                raise RuntimeError('unsafe or duplicate archive member: ' + member.name)
            if not member.isdir() and not member.isfile():
                raise RuntimeError('ABI preparation refuses archive links/special files: ' + member.name)
            seen.add(member.name)
        for member in members:
            target = destination / member.name
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                stream = archive.extractfile(member)
                assert stream is not None
                target.write_bytes(stream.read())
                target.chmod(member.mode & 0o777)


def authenticate_headers(directory: Path) -> dict:
    manifest = json.loads(HEADER_MANIFEST.read_text())
    if manifest['commit'] != BASE_COMMIT or manifest['tree'] != BASE_TREE:
        raise RuntimeError('tracked R2 header pin changed')
    actual = {str(path.relative_to(directory)) for path in (directory / 'include').rglob('*') if path.is_file()}
    if actual != set(manifest['files']):
        raise RuntimeError('R2 header closure differs from the tracked exact file inventory')
    for name, expected in manifest['files'].items():
        raw = (directory / name).read_bytes()
        blob = hashlib.sha1(b'blob ' + str(len(raw)).encode() + b'\0' + raw).hexdigest()
        if identity(directory / name) != {'sha256': expected['sha256'], 'bytes': expected['bytes']} or blob != expected['git_blob']:
            raise RuntimeError('R2 header bytes differ: ' + name)
    return manifest


def compiler_identity(command: str) -> dict:
    resolved = shutil.which(command) if not Path(command).is_absolute() else command
    if not resolved:
        raise RuntimeError('compiler not found: ' + command)
    implementation = Path(resolved).resolve()
    if platform.system() == 'Darwin' and implementation.parent == Path('/usr/bin'):
        implementation = Path(run(['xcrun', '--find', Path(resolved).name]).decode().strip()).resolve()
    return {'command': str(resolved), 'implementation': str(implementation),
            **identity(implementation), 'version': run([command, '--version']).decode().strip(),
            'target': run([command, '-dumpmachine']).decode().strip()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-repo', type=Path, required=True)
    parser.add_argument('--current-build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    repo, current, output = args.source_repo.resolve(), args.current_build.resolve(), args.output.resolve()
    if output.exists():
        raise RuntimeError('output must be new; reuse its existing receipt explicitly instead of rebuilding over it')
    if not 1 <= args.jobs <= 64:
        raise RuntimeError('--jobs must be 1..64')
    cache = read_cache(current / 'CMakeCache.txt')
    if Path(cache['CMAKE_HOME_DIRECTORY']).resolve() != repo:
        raise RuntimeError('current CMake cache belongs to a different source repository')
    try:
        commit = run(['git', '-C', str(repo), 'rev-parse', BASE_COMMIT + '^{commit}']).decode().strip()
        tree = run(['git', '-C', str(repo), 'rev-parse', BASE_COMMIT + '^{tree}']).decode().strip()
    except RuntimeError as error:
        raise RuntimeError(f'exact R2 source is unavailable; use a full-history checkout in CI, '
                           f'or fetch the object in a local shallow clone: '
                           f'git fetch --no-tags --depth=1 origin {BASE_COMMIT}') from error
    if commit != BASE_COMMIT or tree != BASE_TREE:
        raise RuntimeError('R2 commit/tree mismatch')
    source_raw = run(['git', '-C', str(repo), 'archive', '--format=tar', BASE_COMMIT,
                      'CMakeLists.txt', 'VERSION', 'cmake', 'include', 'src'])
    output.mkdir(parents=True)
    (output / 'source.tar').write_bytes(source_raw)
    source, build = output / 'source', output / 'build'
    extract_tar(source_raw, source)
    authenticate_headers(source)
    settings = {key: cache[key] for key in COPY_CACHE if key in cache}
    eigen_source = current_eigen_source(cache, current)
    if eigen_source is not None:
        settings['FETCHCONTENT_SOURCE_DIR_EIGEN'] = str(eigen_source)
    settings.update({'PINEFORGE_BUILD_TESTS': 'OFF', 'PINEFORGE_BUILD_TUTORIAL': 'OFF',
                     'PINEFORGE_BUILD_CORPUS_STRATEGIES': 'OFF', 'PINEFORGE_BUILD_LIVE_RUNNER': 'OFF',
                     'PINEFORGE_BUILD_BENCH_STRATEGIES': 'OFF', 'PINEFORGE_BUILD_SPEED_BENCH': 'OFF',
                     'CMAKE_EXPORT_COMPILE_COMMANDS': 'ON', 'FETCHCONTENT_FULLY_DISCONNECTED': 'ON'})
    configure = ['cmake', '-S', str(source), '-B', str(build), '-G', cache['CMAKE_GENERATOR'],
                 *[f'-D{key}={value}' for key, value in settings.items()]]
    run(configure, timeout=120, log=output / 'configure.log')
    build_argv = ['cmake', '--build', str(build), '--target', 'pineforge', '--parallel', str(args.jobs)]
    run(build_argv, timeout=1800, log=output / 'build.log')
    library = build / 'lib/libpineforge.a'
    if not library.read_bytes().startswith(b'!<arch>\n'):
        raise RuntimeError('base library is not a full static archive')
    archive_members = run(['ar', '-t', str(library)]).decode().splitlines()
    if len(archive_members) < 20:
        raise RuntimeError('base archive is not the full R2 library')
    headers = output / 'r2-headers.tar'
    with tarfile.open(headers, 'w') as archive:
        archive.add(source / 'include', arcname='include', recursive=True)
    source_identity = {str(path.relative_to(source)): identity(path)['sha256']
                       for path in sorted(source.rglob('*')) if path.is_file()}
    receipt = {'schemaVersion': 'pineforge-settlement-abi-base/v1', 'commit': BASE_COMMIT, 'tree': BASE_TREE,
               'archive': str(library.relative_to(output)), 'archiveSha256': identity(library)['sha256'],
               'headers': 'r2-headers.tar', 'headersSha256': identity(headers)['sha256'],
               'generatedInclude': 'build/include',
               'generatedHeaderSha256': identity(build / 'include/pineforge/version.h')['sha256'],
               'sourceSha256': source_identity, 'sourceArchive': identity(output / 'source.tar'),
               'compiler': compiler_identity(cache['CMAKE_CXX_COMPILER']),
               'settings': settings, 'copiedCurrentCache': {key: cache[key] for key in COPY_CACHE if key in cache},
               'currentCacheSha256': identity(current / 'CMakeCache.txt')['sha256'],
               'baseCacheSha256': identity(build / 'CMakeCache.txt')['sha256'],
               'compileCommandsSha256': identity(build / 'compile_commands.json')['sha256'],
               'configureArgv': configure, 'buildArgv': build_argv, 'archiveMembers': archive_members,
               'executedBinaries': 0, 'networkFetches': 0,
               'sourceMode': 'immutable Git archive; generated version metadata uses VERSION fallback'}
    (output / 'receipt.json').write_text(json.dumps(receipt, indent=2, sort_keys=True) + '\n')
    print(str(output / 'receipt.json'))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
