# edgevision-gateway

Long-lived integration project for the embedded Linux learning plan.

## Current baseline

- W04 bounded-queue asynchronous logger
- SIGINT/SIGTERM graceful shutdown
- CMake build and CTest smoke tests

## Build and verify

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Run `./build/gateway`, then press `Ctrl+C` to verify a clean stop.
