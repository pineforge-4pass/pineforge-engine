#!/usr/bin/env bash
# Internal release transport. Requires the workflow's authenticated digest pull.
# No registry login, pull, package installation, host compiler, or image-ID fallback.
set -euo pipefail
export LC_ALL=C LANG=C

readonly image_digest=sha256:72fb3f2a4d3409e6050b17530585992784a21a4955b368cb8f8b392981abc780
# Docker's .Id identifies the image config, separately from the registry manifest.
readonly image_config_id=sha256:9b1e6c557f68a8b0f1577293d95aae135ade1f9c5499d817f6df100d1cf616bd
readonly image=${ENGINE_TOOLCHAIN_IMAGE:-}
readonly shared_root=/home/runner/work

fail() {
    printf 'prebuilt-bookworm: %s\n' "$*" >&2
    exit 1
}

valid_version() {
    [[ "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-rc\.[1-9][0-9]*)?$ ]] \
        || fail "invalid release version"
}

safe_component() {
    local lower=${1,,}
    case "$lower" in
        ''|.|..|.git|.ssh|.aws|.codex|.agents|.config|.gnupg|.kube|.azure|.netrc|credentials|keys|secrets|.env*|*.pem|*.key|id_rsa*|id_dsa*|id_ecdsa*|id_ed25519*)
            fail "protected or unsafe path component: $1" ;;
    esac
    [[ "$1" != *$'\n'* && "$1" != *$'\r'* && "$1" != *$'\t'* ]] \
        || fail "control character in path"
}

safe_relative() {
    local rest=$1 component
    [[ -n "$rest" && "$rest" != /* && "$rest" != */ && "$rest" != *//* ]] \
        || fail "invalid relative tree path"
    while [[ "$rest" == */* ]]; do
        component=${rest%%/*}
        safe_component "$component"
        rest=${rest#*/}
    done
    safe_component "$rest"
}

safe_absolute() {
    local path=$1 rest component current=
    [[ "$path" == /* && "$path" != *','* && "$path" != */ && "$path" != *//* ]] \
        || fail "path must be absolute, normalized and mount-safe: $path"
    rest=${path#/}
    while :; do
        component=${rest%%/*}
        safe_component "$component"
        current+=/"$component"
        [[ ! -L "$current" ]] || fail "symlink path refused: $current"
        [[ -e "$current" ]] || fail "missing path: $current"
        if [[ "$rest" != */* ]]; then break; fi
        [[ -d "$current" ]] || fail "non-directory ancestor: $current"
        rest=${rest#*/}
    done
}

shared_directory() {
    safe_absolute "$1"
    [[ "$1" == "$shared_root/"* && -d "$1" ]] \
        || fail "directory is outside declared shared workspace/temp: $1"
}

admit_image() {
    local evidence=$1
    [[ -n "$image" ]] || fail "ENGINE_TOOLCHAIN_IMAGE is required"
    [[ "$image" == ?*"@$image_digest" && "$image" != *[[:space:]]* && "${image%@*}" != *'@'* ]] \
        || fail "ENGINE_TOOLCHAIN_IMAGE must be a repository@$image_digest"
    safe_absolute "$evidence"
    docker images --digests --no-trunc > "$evidence/docker-images.txt"
    cat "$evidence/docker-images.txt"
    docker info --format '{{.Driver}}' > "$evidence/docker-storage-driver.txt"
    cat "$evidence/docker-storage-driver.txt"
    docker version --format '{{.Client.Version}} {{.Server.Version}}' > "$evidence/docker-versions.txt"
    cat "$evidence/docker-versions.txt"
    if docker image inspect "$image" > "$evidence/docker-image-inspect.json" 2> "$evidence/docker-image-inspect.stderr"; then
        inspect_rc=0
    else
        inspect_rc=$?
    fi
    cat "$evidence/docker-image-inspect.json"
    cat "$evidence/docker-image-inspect.stderr" >&2
    if [[ "$inspect_rc" != 0 ]]; then
        printf 'prebuilt-bookworm: same-daemon preload canary dependency: digest is not addressable; no image-ID fallback\n' >&2
        exit "$inspect_rc"
    fi
    if docker image inspect --format '{{.Id}}' "$image" \
        > "$evidence/docker-image-id.txt" 2> "$evidence/docker-image-id.stderr"; then
        image_id_rc=0
    else
        image_id_rc=$?
    fi
    cat "$evidence/docker-image-id.txt"
    cat "$evidence/docker-image-id.stderr" >&2
    printf '%s\n' "$image_id_rc" > "$evidence/docker-image-id.exitcode"
    if [[ "$image_id_rc" != 0 ]]; then exit "$image_id_rc"; fi
    [[ "$(cat "$evidence/docker-image-id.txt")" == "$image_config_id" ]] \
        || fail "preloaded image Id differs from pinned config ID"
    [[ "$(docker image inspect --format '{{.Os}}/{{.Architecture}}' "$image")" == linux/amd64 ]] \
        || fail "preloaded image must be linux/amd64"
    docker image inspect --format '{{range .RepoDigests}}{{println .}}{{end}}' "$image" \
        > "$evidence/docker-repo-digests.txt"
    grep -Fx "$image" "$evidence/docker-repo-digests.txt" \
        || fail "same-daemon preload canary dependency: RepoDigests missing exact pin; classic image-ID fallback disabled"
}

check_bookworm_toolchain() {
    # All compiler/header/library resolution occurs in the immutable image.
    # Network is disabled and FetchContent is disconnected even on setup failure.
    export PATH=/usr/bin:/bin TMPDIR=/output/tmp
    mkdir /output/tmp
    cat /etc/os-release /etc/engine-toolchain-packages.txt /etc/engine-toolchain-glibc.txt
    grep -Fx 'ID=debian' /etc/os-release
    grep -Fx 'VERSION_ID="12"' /etc/os-release
    [[ "$(getconf GNU_LIBC_VERSION)" == 'glibc 2.36' ]] || fail "glibc version mismatch"
    [[ "$(dpkg-query -W -f='${Version}' libc6)" == 2.36-9+deb12u14 ]] \
        || fail "libc6 package mismatch"
    [[ "$(dpkg-query -W -f='${Version}' libeigen3-dev)" == 3.4.0-4 ]] \
        || fail "Eigen package mismatch"
    [[ "$(gcc -dumpfullversion)" == 12.2.0 && "$(g++ -dumpfullversion)" == 12.2.0 ]] \
        || fail "compiler version mismatch"
    cmake --version
    cmake --version | grep -Fx 'cmake version 3.25.1'
    python3 --version
    [[ "$(python3 --version)" == 'Python 3.11.2' ]] || fail "Python version mismatch"
    ld --version
    ld --version | grep -E '^GNU ld .* 2\.40$'
    dpkg-query -W libc6 libeigen3-dev gcc g++ cmake python3 binutils git tar xz-utils gzip file make
    local macros=/usr/include/eigen3/Eigen/src/Core/util/Macros.h
    grep -Eq '^#define EIGEN_WORLD_VERSION[[:space:]]+3[[:space:]]*$' "$macros"
    grep -Eq '^#define EIGEN_MAJOR_VERSION[[:space:]]+4[[:space:]]*$' "$macros"
    grep -Eq '^#define EIGEN_MINOR_VERSION[[:space:]]+0[[:space:]]*$' "$macros"
}

inside_build() {
    [[ $# == 4 ]] || fail "inside build requires version, short SHA, dirty flag and tag"
    local version=$1 short_sha=$2 dirty=$3 tag=$4
    valid_version "$version"
    [[ "$short_sha" =~ ^[0-9a-f]{7,40}$ && "$dirty" =~ ^(ON|OFF)$ && "$tag" == "v$version" ]] \
        || fail "invalid inside source identity"
    [[ "$(id -u):$(id -g)" == 1001:123 ]] || fail "builder UID:GID must be 1001:123"
    [[ -d /source && -d /output && ! -e /source/.git && ! -L /source/.git ]] \
        || fail "source export must have no checkout metadata"
    safe_absolute /source
    safe_absolute /output
    [[ ! -e /output/build && ! -e /output/stage ]] || fail "reused build or staging path"
    [[ "$(cat /source/VERSION)" == "$version" ]] || fail "export VERSION identity mismatch"
    [[ -f /output/identity.cmake && ! -L /output/identity.cmake ]] \
        || fail "missing regular identity input"
    grep -Fx "set(PINEFORGE_VERSION_GIT_SHA \"$short_sha\")" /output/identity.cmake
    grep -Fx "set(PINEFORGE_VERSION_DIRTY $dirty)" /output/identity.cmake

    check_bookworm_toolchain
    local stage="/output/stage/pineforge-${tag}-linux-x86_64"
    cmake -S /source -B /output/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=/usr/bin/gcc \
        -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
        -DEigen3_DIR=/usr/share/eigen3/cmake \
        -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
        -DPINEFORGE_BUILD_TESTS=OFF \
        -DPINEFORGE_BUILD_TUTORIAL=OFF \
        -DPINEFORGE_VERSION_SOURCE=FILE \
        -DCMAKE_PROJECT_INCLUDE=/output/identity.cmake \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_INSTALL_PREFIX="$stage"
    grep -Fx 'Eigen3_DIR:PATH=/usr/share/eigen3/cmake' /output/build/CMakeCache.txt
    cmake --build /output/build -j 4
    cmake --install /output/build
    python3 /source/scripts/release_version.py check-install --prefix "$stage" --expect "$version"
    grep -Eq "^#define PINEFORGE_GIT_SHA[[:space:]]+\"${short_sha}\"$" \
        "$stage/include/pineforge/version.h"
    for library in libpineforge.a libpineforge_kernel.a libpineforge_window_plan.so; do
        [[ -s "$stage/lib/$library" && -f "$stage/lib/$library" && ! -L "$stage/lib/$library" ]] \
            || fail "missing installed library: $library"
    done
}

inside_consume() {
    [[ $# == 1 ]] || fail "inside archive consumer requires tag"
    local tag=$1
    [[ "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-rc\.[1-9][0-9]*)?$ ]] \
        || fail "invalid archive consumer tag"
    [[ "$(id -u):$(id -g)" == 1001:123 ]] || fail "consumer UID:GID must be 1001:123"
    [[ -d /source && -d /output && ! -L /source && ! -L /output ]] \
        || fail "consumer source/output mounts are invalid"
    safe_absolute /source
    safe_absolute /output
    [[ -f /source/scripts/check_shipped_archive_runtime.sh
        && ! -L /source/scripts/check_shipped_archive_runtime.sh ]] \
        || fail "missing regular archive consumer helper"
    check_bookworm_toolchain
    exec /bin/bash /source/scripts/check_shipped_archive_runtime.sh --inside "$tag"
}

consume_archive() {
    [[ $# == 5 ]] || fail "archive consumer requires workspace, runner temp, tarball, checksum and tag"
    local workspace=$1
    local runner_temp=$2
    local tarball=$3
    local shasum=$4
    local tag=$5
    [[ "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-rc\.[1-9][0-9]*)?$ ]] \
        || fail "invalid archive consumer tag"
    local base="pineforge-${tag}-linux-x86_64"
    [[ "$tarball" == "${base}.tar.gz" && "$shasum" == "${tarball}.sha256" ]] \
        || fail "archive consumer inputs do not name the packed x86_64 tarball"
    local host_uid host_gid
    host_uid=$(id -u)
    host_gid=$(id -g)
    [[ "$host_uid" == 1001 ]] || fail "consumer host UID must be 1001"
    shared_directory "$workspace"
    shared_directory "$runner_temp"
    [[ -w "$runner_temp" ]] || fail "runner temp is not writable"
    safe_absolute "$workspace/$tarball"
    safe_absolute "$workspace/$shasum"
    [[ -f "$workspace/$tarball" && ! -L "$workspace/$tarball" ]] \
        || fail "packed archive is not a regular file"
    [[ -f "$workspace/$shasum" && ! -L "$workspace/$shasum" ]] \
        || fail "packed archive checksum is not a regular file"
    local run_root
    run_root=$(mktemp -d "$runner_temp/archive-runtime.XXXXXXXX")
    shared_directory "$run_root"
    local output_dir="$run_root/output"
    local evidence="$run_root/evidence"
    local source_dir="$run_root/source"
    mkdir "$output_dir" "$evidence" "$source_dir"
    printf 'uid=%s\ngid=%s\n' "$host_uid" "$host_gid" > "$evidence/host-identity.txt"
    # Export only the packed bytes and consumer inputs, without checkout
    # metadata, credentials or the source build/staging directories.
    local input
    for input in "$tarball" "$shasum" examples/native/hello_kernel.cpp \
        scripts/build_prebuilt_bookworm.sh scripts/check_shipped_archive_runtime.sh; do
        safe_absolute "$workspace/$input"
        [[ -f "$workspace/$input" && ! -L "$workspace/$input" ]] \
            || fail "archive consumer input is not a regular file: $input"
        mkdir -p "$source_dir/$(dirname "$input")"
        cp "$workspace/$input" "$source_dir/$input"
    done
    shared_directory "$source_dir"
    admit_image "$evidence"
    if docker run --rm --pull=never --platform linux/amd64 --network none --user 1001:123 \
        --read-only --cap-drop ALL --security-opt no-new-privileges \
        --mount "type=bind,source=$source_dir,target=/source,readonly" \
        --mount "type=bind,source=$output_dir,target=/output" \
        --workdir /source "$image" /bin/bash /source/scripts/build_prebuilt_bookworm.sh \
        --inside-consume "$tag" \
        > "$evidence/consumer.stdout" 2> "$evidence/consumer.stderr"; then
        consumer_rc=0
    else
        consumer_rc=$?
    fi
    cat "$evidence/consumer.stdout"
    cat "$evidence/consumer.stderr" >&2
    printf '%s\n' "$consumer_rc" > "$evidence/consumer.exitcode"
    if [[ "$consumer_rc" != 0 ]]; then exit "$consumer_rc"; fi
    printf 'prebuilt-bookworm: shipped archive consumer completed; evidence=%s\n' "$evidence"
    return "$consumer_rc"
}

if [[ "${1:-}" == --inside ]]; then
    shift
    inside_build "$@"
    exit 0
fi

if [[ "${1:-}" == --inside-consume ]]; then
    shift
    inside_consume "$@"
    exit "$?"
fi

if [[ "${1:-}" == --consume ]]; then
    shift
    consume_archive "$@"
    exit "$?"
fi

[[ $# == 6 ]] || fail "requires workspace, runner temp, emitted SHA, version, tag and dry-run flag"
workspace=$1
runner_temp=$2
source_sha=$3
version=$4
tag=$5
dry_run=$6
valid_version "$version"
[[ "$source_sha" =~ ^[0-9a-f]{40}$ && "$tag" == "v$version" && "$dry_run" =~ ^(true|false)$ ]] \
    || fail "invalid emitted source identity or dry-run flag"
shared_directory "$workspace"
shared_directory "$runner_temp"
[[ -w "$runner_temp" ]] || fail "runner temp is not writable"
safe_absolute "${GITHUB_ENV:?GITHUB_ENV is required}"
[[ "$GITHUB_ENV" == "$shared_root/"* && -f "$GITHUB_ENV" && -w "$GITHUB_ENV" ]] \
    || fail "GITHUB_ENV must be a regular writable shared file"
host_uid=$(id -u)
host_gid=$(id -g)
[[ "$host_uid" == 1001 ]] || fail "runner UID must be 1001"
[[ "$(git -C "$workspace" rev-parse --show-toplevel)" == "$workspace" ]] \
    || fail "workspace is not the checkout root"
[[ "$(git -C "$workspace" rev-parse HEAD)" == "$source_sha" ]] || fail "emitted HEAD mismatch"
source_tree=$(git -C "$workspace" rev-parse "${source_sha}^{tree}")
[[ "$source_tree" =~ ^[0-9a-f]{40}$ ]] || fail "invalid source tree identity"
run_root=$(mktemp -d "$runner_temp/prebuilt-bookworm.XXXXXXXX")
shared_directory "$run_root"
source_dir="$run_root/source"
output_dir="$run_root/output"
evidence="$run_root/evidence"
mkdir "$source_dir" "$output_dir" "$evidence"
git -C "$workspace" ls-tree -rz "$source_sha" > "$evidence/source-tree.z"
# Refuse protected paths and links before diff could read their checkout bytes.
while IFS= read -r -d '' entry; do
    metadata=${entry%%$'\t'*}
    path=${entry#*$'\t'}
    [[ "$metadata" != "$entry" ]] || fail "malformed tree record"
    mode=${metadata%% *}
    remainder=${metadata#* }
    kind=${remainder%% *}
    oid=${remainder#* }
    [[ "$oid" =~ ^[0-9a-f]{40}$ ]] || fail "invalid tracked object identity"
    safe_relative "$path"
    if [[ "$mode" == 160000 && "$kind" == commit ]]; then
        safe_absolute "$workspace/$path"
        [[ -d "$workspace/$path" ]] || fail "gitlink is not a real directory: $path"
    elif [[ "$kind" != blob || ! "$mode" =~ ^100(644|755)$ ]]; then
        fail "symlink or unsupported tracked tree mode: $mode $kind $path"
    fi
done < "$evidence/source-tree.z"
if [[ "$dry_run" == false ]]; then
    [[ "$(git -C "$workspace" rev-parse "refs/tags/${tag}^{commit}")" == "$source_sha" ]] \
        || fail "release tag differs from emitted commit"
fi
if ! git -C "$workspace" diff --quiet "$source_sha" -- . ':!VERSION'; then
    fail "tracked checkout differs from emitted commit outside VERSION"
fi
committed_version=$(git -C "$workspace" show "${source_sha}:VERSION")
valid_version "$committed_version"
if [[ "$dry_run" == false && "$committed_version" != "$version" ]]; then
    fail "committed VERSION differs from release version"
fi
safe_absolute "$workspace/VERSION"
[[ -f "$workspace/VERSION" ]] || fail "checkout VERSION is not a regular file"
[[ "$(cat "$workspace/VERSION")" == "$version" ]] \
    || fail "checkout VERSION differs from validated release version"
short_sha=$(git -C "$workspace" rev-parse --short=7 "$source_sha")
[[ "$short_sha" =~ ^[0-9a-f]{7,40}$ ]] || fail "invalid short SHA"
dirty=OFF
# FILE mode still observes git describe's dirty bit. Mirror the existing helper
# after the workflow's dry-run VERSION write, without exporting any .git data.
if git -C "$workspace" describe --tags --match 'v*' --abbrev=7 --dirty \
    > "$evidence/git-describe.stdout" 2> "$evidence/git-describe.stderr"; then
    describe_rc=0
else
    describe_rc=$?
fi
printf '%s\n' "$describe_rc" > "$evidence/git-describe.exitcode"
description=$(cat "$evidence/git-describe.stdout")
if [[ "$describe_rc" != 0 ]]; then description=; fi
if [[ "$description" == *-dirty ]]; then dirty=ON; fi

printf 'head=%s\ntree=%s\nversion=%s\ntag=%s\ndry_run=%s\nshort_sha=%s\ndirty=%s\nimage=%s\nuid_gid=%s:%s\n' \
    "$source_sha" "$source_tree" "$version" "$tag" "$dry_run" "$short_sha" "$dirty" "$image" "$host_uid" "$host_gid" \
    > "$evidence/source-identity.txt"
printf 'uid=%s\ngid=%s\n' "$host_uid" "$host_gid" > "$evidence/host-identity.txt"
# Read only immutable tracked blobs, never checkout files, git config, LFS
# filters, export-subst, untracked files or initialized submodule contents.
while IFS= read -r -d '' entry; do
    metadata=${entry%%$'\t'*}
    path=${entry#*$'\t'}
    [[ "$metadata" != "$entry" ]] || fail "malformed tree record"
    mode=${metadata%% *}
    remainder=${metadata#* }
    kind=${remainder%% *}
    oid=${remainder#* }
    [[ "$oid" =~ ^[0-9a-f]{40}$ ]] || fail "invalid tracked object identity"
    safe_relative "$path"
    if [[ "$mode" == 160000 && "$kind" == commit ]]; then
        # A gitlink is a commit plus a real directory, never a blob/file.
        # These data-only trees are not build inputs; do not fetch/read them.
        safe_absolute "$workspace/$path"
        [[ -d "$workspace/$path" ]] || fail "gitlink is not a real directory: $path"
        mkdir -p "$source_dir/$path"
        printf '%s %s\n' "$oid" "$path" >> "$evidence/gitlinks.txt"
    elif [[ "$kind" == blob && "$mode" =~ ^100(644|755)$ ]]; then
        mkdir -p "$(dirname "$source_dir/$path")"
        git -C "$workspace" cat-file blob "$oid" > "$source_dir/$path"
        chmod "${mode#100}" "$source_dir/$path"
    else
        fail "symlink or unsupported tracked tree mode: $mode $kind $path"
    fi
done < "$evidence/source-tree.z"
[[ -f "$source_dir/VERSION" && ! -e "$source_dir/.git" && ! -L "$source_dir/.git" ]] \
    || fail "invalid credential-free source export"
[[ "$(cat "$source_dir/VERSION")" == "$committed_version" ]] || fail "exported VERSION mismatch"
if [[ "$dry_run" == true ]]; then printf '%s\n' "$version" > "$source_dir/VERSION"; fi
for input in CMakeLists.txt VERSION cmake/PineForgeVersion.cmake \
    scripts/build_prebuilt_bookworm.sh scripts/release_version.py; do
    safe_absolute "$source_dir/$input"
    [[ -f "$source_dir/$input" ]] || fail "missing regular exported build input: $input"
done
printf 'set(PINEFORGE_VERSION_GIT_SHA "%s")\nset(PINEFORGE_VERSION_DIRTY %s)\n' \
    "$short_sha" "$dirty" > "$output_dir/identity.cmake"
shared_directory "$source_dir"
shared_directory "$output_dir"

admit_image "$evidence"
if docker run --rm --pull=never --platform linux/amd64 --network none --user 1001:123 \
    --read-only --cap-drop ALL --security-opt no-new-privileges \
    --mount "type=bind,source=$source_dir,target=/source,readonly" \
    --mount "type=bind,source=$output_dir,target=/output" \
    --workdir /source "$image" /bin/bash /source/scripts/build_prebuilt_bookworm.sh \
    --inside "$version" "$short_sha" "$dirty" "$tag" \
    > "$evidence/builder.stdout" 2> "$evidence/builder.stderr"; then
    build_rc=0
else
    build_rc=$?
fi
cat "$evidence/builder.stdout"
cat "$evidence/builder.stderr" >&2
printf '%s\n' "$build_rc" > "$evidence/builder.exitcode"
if [[ "$build_rc" != 0 ]]; then exit "$build_rc"; fi
stage="$output_dir/stage/pineforge-${tag}-linux-x86_64"
shared_directory "$stage"
printf 'STAGE_DIR=%s\n' "$stage" >> "$GITHUB_ENV"
printf 'prebuilt-bookworm: build transport completed; all-library ELF gate still required; evidence=%s\n' "$evidence"
