# PineForge runtime container.
#
# Stage 1 builds libpineforge.a and stages the public headers.
# Stage 2 is a thin runtime: g++, python3, the prebuilt static
# library, and the JSON harness. Each `docker run` compiles the
# user-supplied generated.cpp against libpineforge.a and runs it
# against the user-supplied OHLCV, emitting a JSON report on stdout.
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
        libeigen3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
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

# === Stage 2: runtime =================================================
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ \
        libeigen3-dev \
        python3 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Bundle the prebuilt static library, the public headers, and the
# JSON harness inside the image. Users only mount their own code.
RUN mkdir -p /opt/pineforge/lib /opt/pineforge/include /opt/pineforge/bin
COPY --from=builder /src/build/lib/libpineforge.a /opt/pineforge/lib/
COPY --from=builder /src/include/pineforge        /opt/pineforge/include/pineforge

COPY docker/run_json.py    /opt/pineforge/bin/run_json.py
COPY docker/entrypoint.sh  /opt/pineforge/bin/entrypoint.sh
RUN chmod +x /opt/pineforge/bin/entrypoint.sh /opt/pineforge/bin/run_json.py

ENV PINEFORGE_PREFIX=/opt/pineforge

ENTRYPOINT ["/opt/pineforge/bin/entrypoint.sh"]
