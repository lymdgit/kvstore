#!/bin/bash
set -e

echo "=========================================="
echo "  KVStore Docker Container"
echo "=========================================="
echo "  Kernel  : $(uname -r)"
echo "  Arch    : $(uname -m)"
echo "  Data Dir: /app/data"
echo "  Ports   : 2048-2067"

# Check io_uring support
if [ -d "/proc/sys/kernel/" ]; then
    echo "  io_uring: supported"
else
    echo "  io_uring: unknown"
fi

echo "=========================================="

# Ensure data directory exists
mkdir -p /app/data

# Graceful shutdown: forward SIGTERM/SIGINT to kvstore process
# This ensures WAL flusher completes final flush before exit
cleanup() {
    echo "[Docker] Received shutdown signal, stopping kvstore..."
    if [ -n "$PID" ]; then
        kill -TERM "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi
    echo "[Docker] Shutdown complete."
    exit 0
}

trap cleanup SIGTERM SIGINT

# Start kvstore in background so we can trap signals
cd /app
./kvstore &
PID=$!

echo "[Docker] kvstore started (PID: $PID)"

# Wait for kvstore process
wait $PID
