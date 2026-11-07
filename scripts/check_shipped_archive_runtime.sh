#!/usr/bin/env bash
# Consume the exact x86_64 release archive from the release pack step.
# The caller supplies the immutable Bookworm image and mounts the packed inputs
# read-only; this helper never builds the engine or uses the staging directory.
set -euo pipefail
export LC_ALL=C LANG=C

fail() {
    printf 'shipped-archive-runtime: %s\n' "$*" >&2
    exit 1
}

inside_consumer() {
    [[ $# == 1 ]] || fail "inside archive consumer requires tag"
    local tag=$1
    [[ "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-rc\.[1-9][0-9]*)?$ ]] \
        || fail "invalid archive consumer tag"
    [[ "$(id -u):$(id -g)" == 1001:123 ]] \
        || fail "consumer UID:GID must be 1001:123"
    [[ -d /source && -d /output && ! -L /source && ! -L /output ]] \
        || fail "consumer source/output mounts are invalid"

    local base="pineforge-${tag}-linux-x86_64"
    local archive="/source/${base}.tar.gz"
    local checksum="${archive}.sha256"
    [[ -f "$archive" && ! -L "$archive" ]] \
        || fail "packed archive is missing or not regular"
    [[ -f "$checksum" && ! -L "$checksum" ]] \
        || fail "packed archive checksum is missing or not regular"
    [[ -f /source/examples/native/hello_kernel.cpp
        && ! -L /source/examples/native/hello_kernel.cpp ]] \
        || fail "existing native host source is missing or not regular"

    [[ "$(wc -l < "$checksum")" == 1 ]] \
        || fail "packed archive checksum must contain one record"
    local checksum_digest checksum_name checksum_extra
    read -r checksum_digest checksum_name checksum_extra < "$checksum"
    [[ "$checksum_digest" =~ ^[0-9a-f]{64}$ \
        && "$checksum_name" == "${base}.tar.gz" \
        && -z "$checksum_extra" ]] \
        || fail "packed archive checksum names an unexpected file"
    (
        cd /source
        sha256sum --check --strict "${base}.tar.gz.sha256"
    )

    export PATH=/usr/bin:/bin TMPDIR=/output/tmp
    [[ -d /output/tmp && -w /output/tmp ]] \
        || fail "consumer temporary directory is unavailable"
    local unpacked=/output/unpacked
    mkdir "$unpacked"
    tar --extract --gzip --file "$archive" --directory "$unpacked"
    local prefix="$unpacked/$base"
    [[ -d "$prefix" && ! -L "$prefix" ]] \
        || fail "packed archive root is missing"
    [[ -d "$prefix/include/pineforge" && ! -L "$prefix/include/pineforge" ]] \
        || fail "packed archive headers are missing"
    [[ -f "$prefix/lib/libpineforge.a" && ! -L "$prefix/lib/libpineforge.a"
        && -s "$prefix/lib/libpineforge.a" ]] \
        || fail "packed archive full library is missing"
    [[ -f "$prefix/lib/libpineforge_kernel.a" \
        && ! -L "$prefix/lib/libpineforge_kernel.a" \
        && -s "$prefix/lib/libpineforge_kernel.a" ]] \
        || fail "packed archive kernel library is missing"
    [[ -f "$prefix/lib/cmake/PineForge/PineForgeConfig.cmake"
        && ! -L "$prefix/lib/cmake/PineForge/PineForgeConfig.cmake" ]] \
        || fail "packed archive CMake package is missing"

    local consumer=/output/hello_kernel
    local link_map=/output/hello_kernel.link.map
    g++ -std=c++17 -O2 -ffp-contract=off \
        -I"$prefix/include" -I/usr/include/eigen3 \
        /source/examples/native/hello_kernel.cpp \
        -Wl,--whole-archive "$prefix/lib/libpineforge.a" \
        -Wl,--no-whole-archive -ldl -Wl,-Map,"$link_map" \
        -o "$consumer"

    if "$consumer" > /output/hello_kernel.stdout 2> /output/hello_kernel.stderr; then
        run_rc=0
    else
        run_rc=$?
    fi
    cat /output/hello_kernel.stdout
    cat /output/hello_kernel.stderr >&2
    printf '%s\n' "$run_rc" > /output/hello_kernel.exitcode
    if [[ "$run_rc" != 0 ]]; then exit "$run_rc"; fi
    printf 'shipped-archive-runtime: consumer completed; archive=%s\n' "$archive"
}

if [[ "${1:-}" == --inside ]]; then
    shift
    inside_consumer "$@"
    exit 0
fi

fail "run through the pinned release consumer"
