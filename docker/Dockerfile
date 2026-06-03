# PineForge runtime container.
#
# Stage 1 builds libpineforge.a and stages the public headers.
# Stage 2 is a thin runtime: g++, python3, the prebuilt static
# library, and the JSON harness. Each `docker run` compiles the
# user-supplied generated.cpp against libpineforge.a and runs it
# against the user-supplied OHLCV, emitting a JSON report on stdout.
#
# Build args (release.yml passes these; sane defaults for local builds):
#   PINEFORGE_VERSION  semver "MAJOR.MINOR.PATCH" (defaults to VERSION file)
#   PINEFORGE_GIT_SHA  full commit sha
#
# Build:
#   docker build -t pineforge -f docker/Dockerfile .
# Run:
#   docker run --rm \
#     -v $(pwd)/strategy.cpp:/in/strategy.cpp:ro \
#     -v $(pwd)/ohlcv.csv:/in/ohlcv.csv:ro \
#     pineforge > report.json

# === Stage 1: build libpineforge.a =====================================
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ca-certificates \
        git \
        libeigen3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
# VERSION file is the version source-of-truth inside the build context;
# release.yml writes the bumped value before `docker build` so the
# generated <pineforge/version.h> matches the image tag.
COPY VERSION ./
COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY include ./include
COPY src ./src
COPY tests ./tests

RUN cmake -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DPINEFORGE_BUILD_TESTS=OFF \
        -DPINEFORGE_BUILD_TUTORIAL=OFF \
    && cmake --build build --target pineforge -j

# Stage the generated version header alongside the curated ones so
# stage 2 can copy a single tree.
RUN cp build/include/pineforge/version.h include/pineforge/version.h

# === Stage 2: runtime =================================================
FROM debian:bookworm-slim

ARG PINEFORGE_VERSION=unknown
ARG PINEFORGE_GIT_SHA=unknown
ARG PINEFORGE_BUILD_DATE=unknown

LABEL org.opencontainers.image.title="pineforge" \
      org.opencontainers.image.description="Deterministic PineScript v6 backtest runtime" \
      org.opencontainers.image.version="${PINEFORGE_VERSION}" \
      org.opencontainers.image.revision="${PINEFORGE_GIT_SHA}" \
      org.opencontainers.image.created="${PINEFORGE_BUILD_DATE}" \
      org.opencontainers.image.source="https://github.com/pineforge-4pass/pineforge-engine" \
      org.opencontainers.image.licenses="Apache-2.0"

# g++ + Eigen to compile the generated TU; python3 for the JSON harness and
# the bundled PineScript->C++ transpiler (pineforge-codegen, source-available
# on PyPI). With codegen baked in, the container accepts strategy.pine directly
# and transpiles locally — no hosted API, no API key, source never leaves the
# box. (bookworm python3 ships no pip and is PEP-668 externally-managed, hence
# python3-pip + --break-system-packages. codegen is pure-Python, zero deps.)
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ \
        libeigen3-dev \
        python3 \
        python3-pip \
        ca-certificates \
    && pip3 install --break-system-packages --no-cache-dir pineforge-codegen \
    && rm -rf /var/lib/apt/lists/*

# Bundle the prebuilt static library, the public headers (curated +
# generated version.h), and the JSON harness. Users only mount their
# own code.
RUN mkdir -p /opt/pineforge/lib /opt/pineforge/include /opt/pineforge/bin
COPY --from=builder /src/build/lib/libpineforge.a /opt/pineforge/lib/
COPY --from=builder /src/include/pineforge        /opt/pineforge/include/pineforge

COPY docker/run_json.py    /opt/pineforge/bin/run_json.py
COPY docker/entrypoint.sh  /opt/pineforge/bin/entrypoint.sh
RUN chmod +x /opt/pineforge/bin/entrypoint.sh /opt/pineforge/bin/run_json.py

ENV PINEFORGE_PREFIX=/opt/pineforge \
    PINEFORGE_VERSION=${PINEFORGE_VERSION} \
    PINEFORGE_GIT_SHA=${PINEFORGE_GIT_SHA}

ENTRYPOINT ["/opt/pineforge/bin/entrypoint.sh"]
