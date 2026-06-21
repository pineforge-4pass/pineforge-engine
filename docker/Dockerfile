# PineForge runtime base image — PURE RUNTIME, no transpiler.
#
# Stage 1 builds libpineforge.a and stages the public headers.
# Stage 2 is a thin runtime base: g++, Eigen, python3, the prebuilt static
# library, and the public headers. It compiles + runs a *pre-transpiled*
# generated.cpp against libpineforge.a (the C ABI).
#
# This image deliberately does NOT bundle the PineScript->C++ transpiler
# (pineforge-codegen) and has NO Pine entrypoint. The full `.pine -> trades`
# pipeline (transpiler + entrypoint + JSON harness) lives in the combined
# image **ghcr.io/pineforge-4pass/pineforge-release**, which builds FROM this
# base. Run a `.pine` there, not here.
#
# Build args (release.yml passes these; sane defaults for local builds):
#   PINEFORGE_VERSION  semver "MAJOR.MINOR.PATCH" (defaults to VERSION file)
#   PINEFORGE_GIT_SHA  full commit sha
#
# Build:
#   docker build -t pineforge-engine -f docker/Dockerfile .

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

RUN cmake -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DPINEFORGE_BUILD_TESTS=OFF \
        -DPINEFORGE_BUILD_TUTORIAL=OFF \
    && cmake --build build --target pineforge -j

# Stage the generated version header alongside the curated ones so
# stage 2 can copy a single tree.
RUN cp build/include/pineforge/version.h include/pineforge/version.h

# === Stage 2: runtime base ============================================
FROM debian:bookworm-slim

ARG PINEFORGE_VERSION=unknown
ARG PINEFORGE_GIT_SHA=unknown
ARG PINEFORGE_BUILD_DATE=unknown

LABEL org.opencontainers.image.title="pineforge-engine" \
      org.opencontainers.image.description="Deterministic PineScript v6 backtest runtime (pure base; transpiler ships in pineforge-release)" \
      org.opencontainers.image.version="${PINEFORGE_VERSION}" \
      org.opencontainers.image.revision="${PINEFORGE_GIT_SHA}" \
      org.opencontainers.image.created="${PINEFORGE_BUILD_DATE}" \
      org.opencontainers.image.source="https://github.com/pineforge-4pass/pineforge-engine" \
      org.opencontainers.image.licenses="Apache-2.0"

# g++ + Eigen to compile a pre-transpiled generated.cpp against the static lib.
# python3 stays: the pineforge-release image builds FROM this base and its
# JSON harness (run_json.py) needs it — keeping it here avoids re-installing it
# in the release layer. The bare engine image carries NO transpiler and NO Pine
# entrypoint (see header); those live in pineforge-release.
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ \
        libeigen3-dev \
        python3 \
    && rm -rf /var/lib/apt/lists/* /usr/share/doc /usr/share/man /usr/share/locale /usr/share/info

# Bundle the prebuilt static library and the public headers (curated +
# generated version.h). Consumers link their own compiled strategy against this.
RUN mkdir -p /opt/pineforge/lib /opt/pineforge/include
COPY --from=builder /src/build/lib/libpineforge.a /opt/pineforge/lib/
COPY --from=builder /src/include/pineforge        /opt/pineforge/include/pineforge

ENV PINEFORGE_PREFIX=/opt/pineforge \
    PINEFORGE_VERSION=${PINEFORGE_VERSION} \
    PINEFORGE_GIT_SHA=${PINEFORGE_GIT_SHA}

# Drop root.
RUN useradd -r -u 10001 -s /usr/sbin/nologin pineforge
USER pineforge

# No ENTRYPOINT: this is a base/runtime image, not a standalone Pine tool.
# Make a bare `docker run` self-documenting instead of silently doing nothing.
CMD ["sh", "-c", "echo 'pineforge-engine is a pure runtime base (compile + run a pre-transpiled generated.cpp against /opt/pineforge/lib/libpineforge.a). For PineScript .pine -> backtest, use ghcr.io/pineforge-4pass/pineforge-release.' >&2; exit 64"]
