#!/usr/bin/env bash
# scripts/coverage.sh — measure source-level coverage of the runtime.
#
# Configures + builds the runtime and the ctest suite with coverage
# instrumentation (Clang llvm-profile or GCC gcov), runs every test, and
# emits a per-file coverage summary plus a sortable totals table. By
# default reports go under `build-cov/coverage/`.
#
# Honours these env vars:
#   BUILD_DIR     — CMake build directory (default: build-cov)
#   COMPILER      — clang | gcc (default: auto-detect from $CC/$CXX, then prefer clang)
#   JOBS          — parallel build jobs (default: $(nproc) or 4)
#   SKIP_BUILD    — set to 1 to reuse an existing instrumented build
#   SKIP_TESTS    — set to 1 to reuse existing .profraw / .gcda data
#   FORMAT        — text | html (default: text). html requires lcov+genhtml or gcovr.
#
# Outputs:
#   $BUILD_DIR/coverage/totals.txt        — per-file line / region / function totals
#   $BUILD_DIR/coverage/per-file/*.txt    — annotated source listings
#   $BUILD_DIR/coverage/uncovered.txt     — files sorted ascending by line coverage

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BUILD_DIR="${BUILD_DIR:-build-cov}"
FORMAT="${FORMAT:-text}"
if command -v nproc >/dev/null 2>&1; then
    JOBS="${JOBS:-$(nproc)}"
else
    JOBS="${JOBS:-4}"
fi

# ---- compiler selection ----------------------------------------------
COMPILER="${COMPILER:-}"
if [[ -z "$COMPILER" ]]; then
    if [[ "${CXX:-}" == *clang++* ]] || [[ "${CC:-}" == *clang* ]]; then
        COMPILER=clang
    elif [[ "${CXX:-}" == *g++* ]] || [[ "${CC:-}" == *gcc* ]]; then
        COMPILER=gcc
    elif command -v clang++ >/dev/null 2>&1; then
        COMPILER=clang
    elif command -v g++ >/dev/null 2>&1; then
        COMPILER=gcc
    else
        echo "coverage.sh: no clang++ or g++ on PATH" >&2
        exit 1
    fi
fi

case "$COMPILER" in
    clang)
        CC_BIN="${CC:-clang}"
        CXX_BIN="${CXX:-clang++}"
        # Apple Clang ships llvm-cov/llvm-profdata under xcrun; mainline
        # clang puts them on PATH directly. Prefer xcrun on macOS so we
        # match the toolchain that built the .profraw files.
        if [[ "$(uname -s)" == "Darwin" ]] && command -v xcrun >/dev/null 2>&1; then
            LLVM_COV=( xcrun llvm-cov )
            LLVM_PROFDATA=( xcrun llvm-profdata )
        else
            LLVM_COV=( llvm-cov )
            LLVM_PROFDATA=( llvm-profdata )
        fi
        ;;
    gcc)
        CC_BIN="${CC:-gcc}"
        CXX_BIN="${CXX:-g++}"
        ;;
    *)
        echo "coverage.sh: COMPILER must be 'clang' or 'gcc' (got '$COMPILER')" >&2
        exit 1
        ;;
esac

echo "==> coverage.sh: COMPILER=$COMPILER  BUILD_DIR=$BUILD_DIR  FORMAT=$FORMAT"

# ---- configure + build ------------------------------------------------
if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
    rm -rf "$BUILD_DIR"
    CC="$CC_BIN" CXX="$CXX_BIN" cmake -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DPINEFORGE_BUILD_TESTS=ON \
        -DPINEFORGE_ENABLE_COVERAGE=ON \
        > "$BUILD_DIR.cmake.log" 2>&1 \
        || { cat "$BUILD_DIR.cmake.log"; exit 1; }
    rm -f "$BUILD_DIR.cmake.log"
    cmake --build "$BUILD_DIR" -j "$JOBS"
fi

COV_DIR="$BUILD_DIR/coverage"
mkdir -p "$COV_DIR/per-file"

# ---- run the test suite ----------------------------------------------
if [[ "${SKIP_TESTS:-0}" != "1" ]]; then
    if [[ "$COMPILER" == "clang" ]]; then
        # ctest spawns each test from its build subdir, so LLVM_PROFILE_FILE
        # MUST be absolute or the .profraw files end up scattered under
        # build-cov/tests/<relative-path>/. %p disambiguates by pid; %m
        # disambiguates by binary so two tests don't clobber each other.
        ABS_RAW_DIR="$(cd "$ROOT_DIR" && cd "$(dirname "$COV_DIR/raw")" && pwd)/raw"
        export LLVM_PROFILE_FILE="$ABS_RAW_DIR/pf-%p-%m.profraw"
        rm -rf "$ABS_RAW_DIR"
        mkdir -p "$ABS_RAW_DIR"
    else
        # Wipe any previous .gcda counters so re-runs don't sum on top.
        find "$BUILD_DIR" -name "*.gcda" -delete
    fi
    ctest --test-dir "$BUILD_DIR" --output-on-failure
fi

# ---- merge + report ---------------------------------------------------
if [[ "$COMPILER" == "clang" ]]; then
    PROFDATA="$COV_DIR/pf.profdata"
    "${LLVM_PROFDATA[@]}" merge -sparse "$COV_DIR"/raw/*.profraw -o "$PROFDATA"

    # Build the binary list. CMAKE_RUNTIME_OUTPUT_DIRECTORY redirects
    # all test binaries under $BUILD_DIR/bin/. llvm-cov needs every
    # binary that produced a .profraw because each contains its own slice
    # of coverage mappings.
    BINS=()
    shopt -s nullglob
    for t in "$BUILD_DIR"/bin/test_*; do
        if [[ -x "$t" && ! -d "$t" ]]; then
            BINS+=( -object "$t" )
        fi
    done
    shopt -u nullglob
    if [[ ${#BINS[@]} -eq 0 ]]; then
        echo "coverage.sh: no test binaries found under $BUILD_DIR/bin/" >&2
        exit 1
    fi

    # llvm-cov report wants explicit files (not directories). Enumerate
    # the runtime source tree; -ignore-filename-regex still carves out
    # Eigen FetchContent + system headers in case llvm-cov resolves any
    # via #include chains.
    SHARED_FILTER=(
        -ignore-filename-regex='(_deps/|/build|/build-cov/|/tests/|usr/include|/SDKs/|llvm/|/c\+\+/)'
    )
    SOURCES=()
    for f in src/*.cpp src/*.hpp include/pineforge/*.hpp include/pineforge/*.h; do
        [[ -f "$f" ]] && SOURCES+=( "$f" )
    done

    {
        echo "PineForge coverage — $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "Compiler: $($CXX_BIN --version | head -n1)"
        echo
        "${LLVM_COV[@]}" report "${BINS[@]:1}" \
            -instr-profile="$PROFDATA" \
            "${SHARED_FILTER[@]}" \
            "${SOURCES[@]}"
    } | tee "$COV_DIR/totals.txt"

    # Per-file annotated listings (text, fast to grep).
    "${LLVM_COV[@]}" show "${BINS[@]:1}" \
        -instr-profile="$PROFDATA" \
        "${SHARED_FILTER[@]}" \
        -format=text \
        -output-dir="$COV_DIR/per-file" \
        "${SOURCES[@]}" \
        > /dev/null

    # Sortable per-file totals (lowest-covered first → easy hole-spotter).
    # The text report rows look like:
    #   src/engine_orders.cpp                        87       12    86.21%   ...
    # so column 4 is line-coverage percent.
    "${LLVM_COV[@]}" report "${BINS[@]:1}" \
        -instr-profile="$PROFDATA" \
        "${SHARED_FILTER[@]}" \
        "${SOURCES[@]}" \
        | awk '/^[A-Za-z0-9_\/\.\-]+\.(cpp|hpp|h|cc|c)[[:space:]]/ {print $0}' \
        | sort -k4n \
        > "$COV_DIR/uncovered.txt"

    if [[ "$FORMAT" == "html" ]]; then
        "${LLVM_COV[@]}" show "${BINS[@]:1}" \
            -instr-profile="$PROFDATA" \
            "${SHARED_FILTER[@]}" \
            -format=html \
            -output-dir="$COV_DIR/html" \
            "${SOURCES[@]}"
        echo
        echo "==> HTML report: $COV_DIR/html/index.html"
    fi
else
    if ! command -v gcovr >/dev/null 2>&1; then
        echo "coverage.sh: gcovr is required for GCC coverage reporting" >&2
        echo "             (e.g. brew install gcovr  /  apt install gcovr)" >&2
        exit 1
    fi
    {
        echo "PineForge coverage — $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "Compiler: $($CXX_BIN --version | head -n1)"
        echo
        gcovr --root . \
              --filter '^(src|include)/' \
              --exclude '_deps' \
              --print-summary \
              "$BUILD_DIR" 2>/dev/null
    } | tee "$COV_DIR/totals.txt"

    gcovr --root . \
          --filter '^(src|include)/' \
          --exclude '_deps' \
          --sort-uncovered \
          --txt "$COV_DIR/uncovered.txt" \
          "$BUILD_DIR" >/dev/null

    if [[ "$FORMAT" == "html" ]]; then
        gcovr --root . \
              --filter '^(src|include)/' \
              --exclude '_deps' \
              --html-details "$COV_DIR/html/index.html" \
              "$BUILD_DIR" >/dev/null
        echo
        echo "==> HTML report: $COV_DIR/html/index.html"
    fi
fi

echo
echo "==> totals       : $COV_DIR/totals.txt"
echo "==> uncovered    : $COV_DIR/uncovered.txt   (lowest-covered first)"
[[ -d "$COV_DIR/per-file" ]] && echo "==> per-file dir : $COV_DIR/per-file/"
