#!/usr/bin/env bash
# Four observed cases for the published x86_64 prebuilt archives.
#
# An issue reported that the x86_64 archive of release v1.10.0 needs glibc 2.38
# (the __isoc23_* symbols), so a program cannot be linked against it on older
# distributions. This script shows what a user sees. It downloads the
# published archives of v1.10.0 and v1.12.0, then builds and runs one small
# consumer program against each of them inside the stock Ubuntu 22.04
# (glibc 2.35) and Debian 12 (glibc 2.36) images, using only the g++, cmake,
# make and libeigen3-dev packages that those distributions ship.
#
#   case         image          archive   expected
#   old-ubuntu   ubuntu:22.04   v1.10.0   fails, output names __isoc23_ or GLIBC_2.38
#   old-debian   debian:12      v1.10.0   fails, output names __isoc23_ or GLIBC_2.38
#   new-ubuntu   ubuntu:22.04   v1.12.0   works
#   new-debian   debian:12      v1.12.0   works
#
# The two old cases are the control: they show that this method can see the
# reported defect. The expectations are fixed in this file and are never
# changed to match what is observed.
#
# The consumer. The program is examples/native/hello_kernel.cpp, read with
# "git show" from the tag that matches the archive, so the headers and the
# archive agree. It is built and run the way the release workflow's own archive
# consumer does it (scripts/check_shipped_archive_runtime.sh, started by
# scripts/build_prebuilt_bookworm.sh): check the sidecar digest, unpack the
# archive, compile the program with the same g++ options and link it against
# the whole of lib/libpineforge.a, then run it. A working consumer prints the
# line "closed trades: 1". That helper does not exist at the v1.10.0 tag, so
# its compile-and-run method is applied here, unchanged, to both archives.
#
# Differences from the release consumer. The release consumer runs in a pinned
# toolchain image, without network, as an unprivileged user, on a read-only
# root file system, and it first checks the exact toolchain versions of that
# image. None of that can hold when the point is to use the distributions' own
# images and packages. So here:
#   - the stock image runs as root with network access, because apt installs
#     the packages;
#   - the compiler is the distribution's own g++, and no other toolchain is used;
#   - each step is its own "docker exec" started from this script, so that the
#     exit status and complete output of every step are recorded separately;
#   - tar adds --no-same-owner, so the unpacked files belong to the extracting
#     user, as they do for the release consumer;
#   - the layout check covers only the two paths the compile line uses.
#
# How a case is judged. Only the records in the evidence directory are used:
#   WORKS               the build exits 0, the run exits 0, and the run's stdout
#                       has the line "closed trades: 1".
#   FAILS-AS-REPORTED   the build or the run exits non-zero and its own output
#                       names __isoc23_ or GLIBC_2.38.
#   ERROR               anything else: a failed download, digest check, pull or
#                       package install, a missing file, another compile error,
#                       or a failure that names neither symbol.
# An ERROR is never counted as a case that failed as expected.
#
# Usage: scripts/diag_prebuilt_four_case.sh <evidence-dir>
#
# The evidence directory must not exist yet; the script creates it. The host
# needs docker, curl, sha256sum, tar and git, runs on x86_64, and has a checkout
# with the tags v1.10.0 and v1.12.0.
#
# Exit status: 0 when the four cases match the table above; 1 when all four ran
# and at least one differs; 2 on any ERROR, including any unexpected command
# failure.
#
# Reads only: the file examples/native/hello_kernel.cpp at the two tags (through
# git), the two archives and their .sha256 sidecars that it downloads (the host
# only lists an archive, it never extracts one), and its own records. Writes
# only under the evidence directory, and starts containers.
set -Eeuo pipefail
export LC_ALL=C LANG=C

readonly release_url=https://github.com/pineforge-4pass/pineforge-engine/releases/download
readonly consumer_source=examples/native/hello_kernel.cpp
readonly old_tag=v1.10.0
readonly new_tag=v1.12.0
# Identity of the published v1.10.0 archive.
readonly old_bytes=8403177
readonly old_sha256=cb412fbae9e8ba515de41dc31026c77d6c5e309693247bee26bb93c67d3c3543
# A failure counts as the reported one only when its own output names one of these.
readonly reported_pattern='__isoc23_|GLIBC_2\.38'
# The line a working consumer prints.
readonly success_line='closed trades: 1'

# Case name, image, archive tag, expected class. Fixed; never edited from what
# is observed.
readonly cases=(
    "old-ubuntu ubuntu:22.04 $old_tag FAILS-AS-REPORTED"
    "old-debian debian:12 $old_tag FAILS-AS-REPORTED"
    "new-ubuntu ubuntu:22.04 $new_tag WORKS"
    "new-debian debian:12 $new_tag WORKS"
)

# The steps that run inside a container, one "docker exec" each.
readonly apt_script='
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive LC_ALL=C LANG=C
apt-get -o Acquire::Retries=5 update
apt-get -o Acquire::Retries=5 install --yes --no-install-recommends g++ cmake make libeigen3-dev
'

# $1 is the archive base name. The checksum check and the unpack command follow
# the release workflow's archive consumer.
readonly unpack_script='
set -euo pipefail
export LC_ALL=C LANG=C
base=$1
cd /source
sha256sum --check --strict "${base}.tar.gz.sha256"
mkdir /work
tar --extract --gzip --no-same-owner --file "/source/${base}.tar.gz" --directory /work
test -d "/work/${base}/include/pineforge"
test -s "/work/${base}/lib/libpineforge.a"
'

# $1 is the archive base name. The compile and link command is the release
# workflow's archive consumer's, option for option.
readonly build_script='
set -euo pipefail
export LC_ALL=C LANG=C PATH=/usr/bin:/bin
prefix=/work/$1
g++ -std=c++17 -O2 -ffp-contract=off \
    -I"$prefix/include" -I/usr/include/eigen3 \
    /source/examples/native/hello_kernel.cpp \
    -Wl,--whole-archive "$prefix/lib/libpineforge.a" \
    -Wl,--no-whole-archive -ldl -Wl,-Map,/work/hello_kernel.link.map \
    -o /work/hello_kernel
'

evidence=
live_container=

# die <message>: report an ERROR of the run and stop with exit status 2.
die() {
    printf 'diag-prebuilt-four-case: ERROR: %s\n' "$*" >&2
    if [[ -n "${evidence:-}" && -d "${evidence:-}" ]]; then
        printf '%s\n' "$*" >> "$evidence/run-error.txt"
    fi
    exit 2
}

# Any command failure that no step below handles is an ERROR of the run (2),
# never the "cases differ" status (1).
on_unexpected_error() {
    local status=$1 line=$2
    trap - ERR
    die "unexpected command failure (exit status $status) at line $line"
}
trap 'on_unexpected_error "$?" "$LINENO"' ERR

# Stop the running container, if any, however the script ends.
cleanup() {
    if [[ -n "$live_container" ]]; then
        docker stop --time 1 "$live_container" > /dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

# capture <label> <command...>
# Runs the command with its complete stdout and stderr kept in <label>.stdout
# and <label>.stderr, and the command's own exit status in <label>.exitcode.
# It returns 0 whatever the command did; the caller reads the record.
capture() {
    local label=$1 status
    shift
    if "$@" > "$label.stdout" 2> "$label.stderr"; then
        status=0
    else
        status=$?
    fi
    printf '%s\n' "$status" > "$label.exitcode"
}

# step_ok <dir> <step>: true when <step>.exitcode exists in <dir> and holds 0.
step_ok() {
    [[ -f "$1/$2.exitcode" && "$(<"$1/$2.exitcode")" == 0 ]]
}

# recorded_exit <dir> <step>: the recorded exit status, or "not-run".
recorded_exit() {
    if [[ -f "$1/$2.exitcode" ]]; then
        cat "$1/$2.exitcode"
    else
        printf 'not-run\n'
    fi
}

# first_line <file>: the first line of a record, or "not recorded".
first_line() {
    if [[ -s "$1" ]]; then
        head -n 1 "$1"
    else
        printf 'not recorded\n'
    fi
}

# download <url> <output file> <log file>: fail on any HTTP error, follow
# redirects over https only, retry a bounded number of times.
download() {
    curl --fail --silent --show-error --location \
        --proto '=https' --proto-redir '=https' \
        --retry 5 --retry-delay 5 --retry-connrefused \
        --output "$2" \
        --write-out 'http_code=%{http_code} bytes=%{size_download} url=%{url_effective}\n' \
        "$1" > "$3" 2>&1
}

# prepare_tag <tag>: take the consumer source from the tag, download the
# archive and its sidecar, and verify them. Any failure here is an ERROR of the
# run, before any case starts.
prepare_tag() {
    local tag=$1
    local base="pineforge-${tag}-linux-x86_64"
    local indir="$evidence/inputs/$tag"
    local recdir="$evidence/downloads/$tag"
    local name digest bytes source_sha verified_by
    printf '== preparing %s\n' "$tag"
    mkdir -p "$indir/$(dirname "$consumer_source")" "$recdir"

    git -C "$repo" rev-parse --verify --quiet "refs/tags/${tag}^{commit}" \
        > "$recdir/tag-commit.txt" \
        || die "tag $tag is not in this checkout; a full-history checkout with tags is required"
    git -C "$repo" show "refs/tags/${tag}:${consumer_source}" > "$indir/$consumer_source" \
        || die "$consumer_source does not exist at tag $tag; no other version is substituted"
    source_sha=$(sha256sum "$indir/$consumer_source")
    {
        printf 'tag: %s\n' "$tag"
        printf 'tag commit: %s\n' "$(<"$recdir/tag-commit.txt")"
        printf 'consumer source: %s\n' "$consumer_source"
        printf 'consumer source blob: %s\n' \
            "$(git -C "$repo" rev-parse "refs/tags/${tag}:${consumer_source}")"
        printf 'consumer source sha256: %s\n' "${source_sha%% *}"
    } > "$recdir/consumer-source.txt"

    for name in "${base}.tar.gz" "${base}.tar.gz.sha256"; do
        download "$release_url/$tag/$name" "$indir/$name" "$recdir/$name.curl.txt" \
            || die "download failed: $release_url/$tag/$name (see $name.curl.txt)"
    done

    (cd "$indir" && sha256sum --check --strict "${base}.tar.gz.sha256") \
        > "$recdir/sidecar-check.txt" 2>&1 \
        || die "${base}.tar.gz does not match its published .sha256 sidecar (see sidecar-check.txt)"
    digest=$(sha256sum "$indir/${base}.tar.gz")
    digest=${digest%% *}
    bytes=$(stat -c %s "$indir/${base}.tar.gz")
    if [[ "$tag" == "$old_tag" ]]; then
        [[ "$digest" == "$old_sha256" && "$bytes" == "$old_bytes" ]] \
            || die "${base}.tar.gz is $bytes bytes with sha256 $digest; the pinned identity is $old_bytes bytes and $old_sha256"
        verified_by='published sidecar and pinned digest'
    else
        verified_by='published sidecar'
    fi
    # Record what the verified archive holds. Nothing is extracted on the host.
    capture "$recdir/archive-listing" tar --list --gzip --file "$indir/${base}.tar.gz"
    step_ok "$recdir" archive-listing \
        || die "${base}.tar.gz cannot be listed (see archive-listing.stderr)"
    printf '%s\t%s\t%s\t%s\t%s\n' "$tag" "${base}.tar.gz" "$bytes" "$digest" "$verified_by" \
        >> "$evidence/downloads.tsv"
}

# pull_image <image> <case dir>: up to three attempts. Every attempt's output
# goes to pull.log and the last attempt's exit status to pull.exitcode.
pull_image() {
    local image=$1 cdir=$2 attempt status=1
    : > "$cdir/pull.log"
    for attempt in 1 2 3; do
        printf 'attempt %s\n' "$attempt" >> "$cdir/pull.log"
        if docker pull --platform linux/amd64 "$image" >> "$cdir/pull.log" 2>&1; then
            status=0
            break
        else
            status=$?
        fi
        sleep 10
    done
    printf '%s\n' "$status" > "$cdir/pull.exitcode"
}

# case_commands <name> <image> <tag> <case dir>
# Runs the steps of one case and records each. It stops after the first step
# that fails, and it never judges anything.
case_commands() {
    local name=$1 image=$2 tag=$3 cdir=$4
    local base="pineforge-${tag}-linux-x86_64"
    local container="four-case-$name"

    pull_image "$image" "$cdir"
    step_ok "$cdir" pull || return 0
    capture "$cdir/image-repo-digest" docker image inspect \
        --format '{{range .RepoDigests}}{{println .}}{{end}}' "$image"
    step_ok "$cdir" image-repo-digest || return 0

    capture "$cdir/start" docker run --detach --rm --pull=never --platform linux/amd64 \
        --name "$container" \
        --mount "type=bind,source=$evidence/inputs/$tag,target=/source,readonly" \
        "$image" sleep infinity
    step_ok "$cdir" start || return 0
    live_container=$container

    # One deliberate crash: the record of this step must hold exactly 7, which
    # proves that a failing command is recorded as non-zero.
    capture "$cdir/selfcheck-crash" docker exec "$container" \
        bash -c 'echo deliberate crash row >&2; exit 7'

    capture "$cdir/apt" docker exec "$container" bash -c "$apt_script"
    step_ok "$cdir" apt || return 0
    capture "$cdir/ldd-version" docker exec "$container" ldd --version
    capture "$cdir/gxx-version" docker exec "$container" g++ --version
    capture "$cdir/unpack" docker exec "$container" bash -c "$unpack_script" bash "$base"
    step_ok "$cdir" unpack || return 0
    capture "$cdir/build" docker exec "$container" bash -c "$build_script" bash "$base"
    step_ok "$cdir" build || return 0
    capture "$cdir/run" docker exec -e LC_ALL=C -e LANG=C "$container" /work/hello_kernel
}

# stop_container <case dir>: stop the running container, if any.
stop_container() {
    if [[ -n "$live_container" ]]; then
        docker stop --time 1 "$live_container" > "$1/stop.log" 2>&1 || true
        live_container=
    fi
}

# classify_case <case dir>
# Sets case_class and case_reason from the records in the case directory only.
classify_case() {
    local cdir=$1 step status
    case_class=ERROR
    case_reason=
    for step in pull image-repo-digest start selfcheck-crash apt ldd-version gxx-version unpack; do
        if [[ ! -f "$cdir/$step.exitcode" ]]; then
            case_reason="no record for step $step"
            return 0
        fi
        status=$(<"$cdir/$step.exitcode")
        if [[ "$step" == selfcheck-crash ]]; then
            if [[ "$status" != 7 ]]; then
                case_reason="the recorder reported exit status $status for a deliberate exit 7"
                return 0
            fi
        elif [[ "$status" != 0 ]]; then
            case_reason="step $step exited with status $status"
            return 0
        fi
    done
    if [[ ! -s "$cdir/image-repo-digest.stdout" ]]; then
        case_reason='no repo digest recorded for the image'
        return 0
    fi

    if [[ ! -f "$cdir/build.exitcode" ]]; then
        case_reason='no record for step build'
        return 0
    fi
    status=$(<"$cdir/build.exitcode")
    if [[ "$status" != 0 ]]; then
        if grep -Eq "$reported_pattern" "$cdir/build.stdout" "$cdir/build.stderr"; then
            case_class=FAILS-AS-REPORTED
        else
            case_reason="the build exited with status $status and its output names neither __isoc23_ nor GLIBC_2.38"
        fi
        return 0
    fi

    if [[ ! -f "$cdir/run.exitcode" ]]; then
        case_reason='no record for step run'
        return 0
    fi
    status=$(<"$cdir/run.exitcode")
    if [[ "$status" == 0 ]]; then
        if grep -Fxq "$success_line" "$cdir/run.stdout"; then
            case_class=WORKS
        else
            case_reason="the run exited with status 0 but its stdout has no line '$success_line'"
        fi
    elif grep -Eq "$reported_pattern" "$cdir/run.stdout" "$cdir/run.stderr"; then
        case_class=FAILS-AS-REPORTED
    else
        case_reason="the run exited with status $status and its output names neither __isoc23_ nor GLIBC_2.38"
    fi
}

# write_record <case dir> <name> <image> <tag>: the per-case summary of what was
# recorded, read back from the case's own record files.
write_record() {
    local cdir=$1 name=$2 image=$3 tag=$4
    {
        printf 'case: %s\n' "$name"
        printf 'image: %s\n' "$image"
        printf 'archive: pineforge-%s-linux-x86_64.tar.gz\n' "$tag"
        printf 'image repo digest as pulled: %s\n' "$(first_line "$cdir/image-repo-digest.stdout")"
        printf 'ldd --version, first line: %s\n' "$(first_line "$cdir/ldd-version.stdout")"
        printf 'g++ --version, first line: %s\n' "$(first_line "$cdir/gxx-version.stdout")"
        printf 'build exit status: %s\n' "$(recorded_exit "$cdir" build)"
        printf 'run exit status: %s\n' "$(recorded_exit "$cdir" run)"
        printf 'class: %s\n' "$case_class"
        if [[ -n "$case_reason" ]]; then
            printf 'error reason: %s\n' "$case_reason"
        fi
    } > "$cdir/record.txt"
}

# run_case <name> <image> <tag>: run one case, then judge it from its records.
run_case() {
    local name=$1 image=$2 tag=$3
    local cdir="$evidence/cases/$name"
    mkdir -p "$cdir"
    printf '== running case %s (%s, archive %s)\n' "$name" "$image" "$tag"
    case_commands "$name" "$image" "$tag" "$cdir"
    stop_container "$cdir"
    classify_case "$cdir"
    write_record "$cdir" "$name" "$image" "$tag"
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$image" "$tag" \
        "$(recorded_exit "$cdir" build)" "$(recorded_exit "$cdir" run)" "$case_class" \
        > "$cdir/row.tsv"
    cat "$cdir/record.txt"
}

[[ $# == 1 ]] || die "usage: $0 <evidence-dir> (the directory must not exist yet)"
# The only path taken from the command line is this new directory; refuse any
# path with a hidden component or an .env name.
case "/$1/" in
    */.*|*.env/*)
        die "refusing an evidence path with a hidden component or an .env name: $1" ;;
esac
for tool in docker curl sha256sum tar git; do
    command -v "$tool" > /dev/null || die "missing host tool: $tool"
done
[[ ! -e "$1" ]] || die "evidence directory already exists: $1"
mkdir -p "$1"
evidence=$(cd "$1" && pwd)
[[ "$evidence" != *,* ]] || die "the evidence path must not contain a comma: $evidence"
mkdir "$evidence/downloads" "$evidence/inputs" "$evidence/cases"

capture "$evidence/host-arch" uname -m
[[ "$(<"$evidence/host-arch.stdout")" == x86_64 ]] || die "an x86_64 host is required"
capture "$evidence/host-docker-version" docker version --format '{{.Client.Version}} {{.Server.Version}}'
step_ok "$evidence" host-docker-version \
    || die "docker is not usable on this host (see host-docker-version.stderr)"
capture "$evidence/host-docker-storage-driver" docker info --format '{{.Driver}}'

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(git -C "$script_dir" rev-parse --show-toplevel) \
    || die "this script must run from a git checkout"

printf 'tag\tfile\tbytes\tsha256\tverified_by\n' > "$evidence/downloads.tsv"
prepare_tag "$old_tag"
prepare_tag "$new_tag"
printf '== downloads.tsv\n'
cat "$evidence/downloads.tsv"

for row in "${cases[@]}"; do
    read -r name image tag _ <<< "$row"
    run_case "$name" "$image" "$tag"
done

# The table is written only now, from the records the cases left behind.
{
    printf 'case\timage\ttag\tbuild_exit\trun_exit\tclass\n'
    for row in "${cases[@]}"; do
        read -r name _ <<< "$row"
        cat "$evidence/cases/$name/row.tsv"
    done
} > "$evidence/cases.tsv"
printf '== cases.tsv\n'
cat "$evidence/cases.tsv"

errors=0
mismatches=0
: > "$evidence/summary.txt"
for row in "${cases[@]}"; do
    read -r name image tag expected <<< "$row"
    observed=$(cut -f6 "$evidence/cases/$name/row.tsv")
    if [[ "$observed" == ERROR ]]; then
        verdict=ERROR
        errors=$((errors + 1))
    elif [[ "$observed" == "$expected" ]]; then
        verdict=as-expected
    else
        verdict=NOT-AS-EXPECTED
        mismatches=$((mismatches + 1))
    fi
    printf '%s (%s, archive %s): expected %s, observed %s: %s\n' \
        "$name" "$image" "$tag" "$expected" "$observed" "$verdict" >> "$evidence/summary.txt"
done
if (( errors > 0 )); then
    final='ERROR: at least one case is an ERROR, so this run proves nothing (exit 2)'
    exit_status=2
elif (( mismatches > 0 )); then
    final='NOT AS EXPECTED: at least one case differs from the table (exit 1)'
    exit_status=1
else
    final='AS EXPECTED: the old archive fails as reported on both distributions and the new archive works on both (exit 0)'
    exit_status=0
fi
printf '%s\n' "$final" >> "$evidence/summary.txt"
printf '== summary\n'
cat "$evidence/summary.txt"
exit "$exit_status"
