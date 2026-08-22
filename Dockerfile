# =============================================================================
# ForgeKV — Production Dockerfile
# Multi-stage build: builder compiles the server; runtime image runs it.
# =============================================================================

# ---------------------------------------------------------------------------
# Stage 1: builder
# Compile forgekv_server from source using CMake + GCC on Debian Bookworm.
# All build tools and source files are left behind in this stage.
# ---------------------------------------------------------------------------
FROM debian:bookworm-slim AS builder

# Install only what the C++20 / CMake build needs.
# build-essential: gcc, g++, make, libc-dev
# cmake: CMake 3.25+ (bookworm ships 3.25.1)
# No other libraries required — httplib is a vendored single-header.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy every directory that CMakeLists.txt references as source roots.
# CMakeLists.txt defines targets from tests/, examples/, and benchmarks/
# even though we only build forgekv_server; CMake configure-time validation
# requires all referenced source files to exist on disk.
COPY CMakeLists.txt ./
COPY src/           src/
COPY include/       include/
COPY third_party/   third_party/
COPY tests/         tests/
COPY examples/      examples/
COPY benchmarks/    benchmarks/

# Configure a Release build and compile only the forgekv_server target.
# CMake configure runs for all targets (source validation), but only
# forgekv_server and its dependency forgekv_core are compiled.
RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build --target forgekv_server --parallel

# ---------------------------------------------------------------------------
# Stage 2: runtime
# Minimal Debian image; only the compiled binary and its libc dependency.
# ---------------------------------------------------------------------------
FROM debian:bookworm-slim AS runtime

# forgekv_server needs the system C++ runtime (libstdc++6).
# libstdc++6 is typically pre-installed on bookworm-slim, but listed
# explicitly to make the runtime dependency visible and reproducible.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

# Run as a non-root user for security.
RUN useradd --system --create-home --shell /bin/false forgekv
USER forgekv

# Working directory is the container's data directory.
# forgekv.wal and forgekv.wal.snapshot will be written here.
# Mount a volume at /data to persist WAL across container restarts.
WORKDIR /data

# Copy the compiled server binary from the builder stage.
COPY --from=builder /src/build/forgekv_server /usr/local/bin/forgekv_server

# Document the default port. The actual port is read from the PORT
# environment variable (or defaults to 8080) by the application itself.
# Do NOT hardcode the port here — cloud platforms inject PORT at runtime.
EXPOSE 8080

# Start the server. Uses the PORT env var if set, otherwise defaults to 8080.
# The server binds to 0.0.0.0 (all interfaces) as implemented in main.cpp.
CMD ["/usr/local/bin/forgekv_server"]
