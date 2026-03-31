# =============================================================================
# KVStore Docker Build
# Multi-stage build: compile with liburing-dev, run with minimal runtime
# =============================================================================

# === Stage 1: Build ===
FROM ubuntu:22.04 AS builder

# Avoid interactive prompts during apt-get
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    gcc \
    make \
    liburing-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Copy source code
COPY . .

# Build NtyCo static library first
RUN cd NtyCo && make clean && make

# Build kvstore main binary + benchmark tool
RUN make clean && make

# === Stage 2: Runtime ===
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    liburing2 \
    netcat-openbsd \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy compiled binaries from builder
COPY --from=builder /build/kvstore /app/kvstore
COPY --from=builder /build/kv_benchmark /app/kv_benchmark

# Copy entrypoint script
COPY docker/entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh

# Create data directory for WAL persistence
RUN mkdir -p /app/data

# Expose kvstore listening ports (2048-2067)
EXPOSE 2048-2067

# Use entrypoint script for graceful signal handling
ENTRYPOINT ["/app/entrypoint.sh"]
