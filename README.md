# edgevision-gateway

Long-lived integration project for the embedded Linux learning plan.

## Current baseline

- W04 bounded-queue asynchronous logger
- SIGINT/SIGTERM graceful shutdown
- Reusable IPv4 TCP/UDP wrapper in `modules/net`
- TCP/UDP examples and abnormal-path tests
- `gateway --smoke` loopback request/response verification
- 100-connection, 100 MiB TCP stress test with payload and FD validation
- CMake build, CTest, ASan and UBSan verification

## Build and verify

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Run the Gateway smoke test:

```bash
./build/gateway --smoke
```

Run `./build/gateway`, then press `Ctrl+C` to verify a clean stop.

## Documentation

- [Compilation, CMake, CTest and Sanitizers](docs/build-cmake-ctest-sanitizers.md)
- [D29 network entry validation](docs/d29-network-validation.md)
- [D29 OK1126B-S board validation evidence](docs/d29-ok1126b-board-evidence.md)
