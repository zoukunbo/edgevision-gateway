# systemd Command Socket and Upgrade Health Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the systemd-managed Gateway command socket reachable to board-side maintenance tools and reject upgrades whose running process version differs from the installed candidate.

**Architecture:** Gateway and `gatewayctl` share one environment-aware socket-path helper. Local runs retain `/tmp/edgevision-study.sock`; the systemd deployment pins `/run/edgevision-gateway/control.sock` and owns its directory. Installation explicitly restarts the service, while the health check compares the installed binary version with the live command response and accumulates all failures before exiting.

**Tech Stack:** C11, POSIX Unix-domain sockets, POSIX `sh`, CMake/CTest, systemd.

**Spec:** `docs/superpowers/specs/2026-09-25-systemd-command-socket-upgrade-health-design.md`

## Global Constraints

- Keep `PrivateTmp=true`.
- Default local socket remains `/tmp/edgevision-study.sock` when `EDGEVISION_COMMAND_SOCKET` is unset or empty.
- The deployed service socket is `/run/edgevision-gateway/control.sock`.
- Do not change MQTT command semantics, database schema, service user, UART/GPIO permissions, or implement automatic rollback.
- Health probes accumulate failures and return once at the end.
- Shell implementation and test scaffolding are mentor-provided; do not attribute them as independent learner implementation.
- Preserve unrelated working-tree changes and stage only files belonging to each task.

## Review Focus

- `EDGEVISION_COMMAND_SOCKET` unset or empty must use the local `/tmp` default; Task 1 tests both cases.
- A custom socket path containing a long value must be rejected before truncation; Task 1 tests a path at or beyond `sockaddr_un.sun_path` capacity.
- `gatewayctl` success with empty or `ok version=` output must fail health; Task 3 tests both.
- An already-active old service must be restarted after files are replaced; Task 2 verifies the explicit `enable` then `restart` sequence.
- A missing or not-yet-created deployed socket must report a running-version probe failure while later database checks still execute; Task 3 tests both outputs.

---

### Task 1: Shared Socket Path and Real Client/Server Communication

**Files:**
- Create: `core/command_socket_path.h`
- Modify: `core/gateway_workers.c:34,811-835`
- Modify: `examples/gatewayctl.c:1-140`
- Create: `tests/gateway_command_socket_test.sh`
- Modify: `CMakeLists.txt:493-501`

**Interfaces:**
- Produces: `edgevision_command_socket_path()` returning the non-empty environment value or `/tmp/edgevision-study.sock`.
- Consumes: environment variable `EDGEVISION_COMMAND_SOCKET`.

- [ ] **Step 1: Add a failing real-process integration test**

Create `tests/gateway_command_socket_test.sh`. It must create a temporary directory, export `EDGEVISION_COMMAND_SOCKET=$tmp/control.sock`, start the supplied Gateway executable with a simulated source and temporary database/log, wait at most three seconds for the socket, invoke the supplied `gatewayctl get_version`, assert the last line is `ok version=$PROJECT_VERSION`, then terminate and reap Gateway in a trap. Add two direct client cases: an empty environment value must fall back to `/tmp/edgevision-study.sock`, while a generated value whose length is at least 108 bytes must return non-zero with `command socket path too long` rather than connect to a truncated path.

Register it with target paths rather than hard-coded build paths:

```cmake
add_test(
    NAME gateway_command_socket
    COMMAND ${CMAKE_COMMAND} -E env
        GATEWAY_EXE=$<TARGET_FILE:gateway>
        GATEWAYCTL_EXE=$<TARGET_FILE:gatewayctl>
        PROJECT_VERSION=${PROJECT_VERSION}
        sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/gateway_command_socket_test.sh
)
```

- [ ] **Step 2: Run the new test and verify RED**

Run:

```sh
cmake --build build --target gateway gatewayctl
ctest --test-dir build -R '^gateway_command_socket$' --output-on-failure
```

Expected: FAIL because Gateway and `gatewayctl` still ignore `EDGEVISION_COMMAND_SOCKET` and use `/tmp/edgevision-study.sock`.

- [ ] **Step 3: Add the shared path helper and use it on both sides**

Create `core/command_socket_path.h`:

```c
#ifndef EDGEVISION_COMMAND_SOCKET_PATH_H
#define EDGEVISION_COMMAND_SOCKET_PATH_H

#include <stdlib.h>

#define EDGEVISION_COMMAND_SOCKET_ENV "EDGEVISION_COMMAND_SOCKET"
#define EDGEVISION_COMMAND_SOCKET_DEFAULT "/tmp/edgevision-study.sock"

static inline const char *edgevision_command_socket_path(void)
{
    const char *path = getenv(EDGEVISION_COMMAND_SOCKET_ENV);
    return path != NULL && path[0] != '\0'
        ? path
        : EDGEVISION_COMMAND_SOCKET_DEFAULT;
}

#endif
```

Include it from `core/gateway_workers.c` and `examples/gatewayctl.c`. Replace both hard-coded deployed command paths with `edgevision_command_socket_path()`. Before `snprintf`, reject paths whose length is at least `sizeof(addr.sun_path)` and return a clear error instead of binding or connecting to a truncated path. Give `gatewayctl` access to `core/`:

```cmake
target_include_directories(gatewayctl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core
)
```

- [ ] **Step 4: Verify GREEN and existing command behavior**

Run:

```sh
cmake --build build --target gateway gatewayctl
ctest --test-dir build -R '^gateway_command_socket$' --output-on-failure
ctest --test-dir build -R 'gateway|graceful_shutdown' --output-on-failure
```

Expected: all selected tests pass; the response version equals `${PROJECT_VERSION}`.

- [ ] **Step 5: Commit only Task 1 files**

```sh
git add core/command_socket_path.h core/gateway_workers.c examples/gatewayctl.c \
    tests/gateway_command_socket_test.sh CMakeLists.txt
git commit -m "feat: make gateway command socket configurable"
```

### Task 2: systemd Runtime Directory and Explicit Upgrade Restart

**Files:**
- Modify: `deploy/edge-gateway-lite/systemd/edge-gateway-lite.service:16-33`
- Create: `deploy/edge-gateway-lite/scripts/service-activate.sh`
- Modify: `deploy/edge-gateway-lite/prepare-bundle.sh:40-90`
- Modify: `deploy/edge-gateway-lite/install-board.sh:80-103`
- Create: `deploy/edge-gateway-lite/tests/service-activation-test.sh`

**Interfaces:**
- Consumes: `EDGEVISION_COMMAND_SOCKET` from Task 1.
- Produces: systemd-owned `/run/edgevision-gateway/control.sock` and `service-activate.sh SERVICE`, which reloads systemd, enables the unit, and restarts it.

- [ ] **Step 1: Write a failing service activation test**

Create `service-activation-test.sh`. It places a recording `systemctl` executable first in `PATH`, runs the real `service-activate.sh edge-gateway-lite.service`, and asserts the literal call order:

```text
daemon-reload
enable edge-gateway-lite.service
restart edge-gateway-lite.service
```

The fake `systemctl` appends `"$*"` to `$SYSTEMCTL_CALLS`. When `$SYSTEMCTL_FAIL_RESTART=1` and `$1=restart`, it returns 1; its `status` branch returns 0. The second test invocation sets that flag and asserts `service-activate.sh` returns non-zero after recording both `restart` and `--no-pager --full status`. Keep filesystem installation outside this test; it tests only service activation.

- [ ] **Step 2: Run the test and verify RED**

Run:

```sh
sh deploy/edge-gateway-lite/tests/service-activation-test.sh
```

Expected: FAIL because the current script calls `enable --now` and never calls `restart`.

- [ ] **Step 3: Implement the unit and activation changes**

Add to `[Service]`:

```ini
RuntimeDirectory=edgevision-gateway
RuntimeDirectoryMode=0755
Environment=EDGEVISION_COMMAND_SOCKET=/run/edgevision-gateway/control.sock
```

Keep `PrivateTmp=true`. Create the production helper:

```sh
#!/bin/sh
set -eu

service=${1:-edge-gateway-lite.service}
systemctl daemon-reload
systemctl enable "$service"
if ! systemctl restart "$service"; then
    systemctl --no-pager --full status "$service" || true
    exit 1
fi
```

Use `"$service"` for both enable and restart in the actual file. Add this helper to `prepare-bundle.sh` and to the install script's prechecks/copies. After installing the unit, `install-board.sh` invokes `"$install_dir/scripts/service-activate.sh" edge-gateway-lite.service`. Do not make install destinations configurable and do not weaken the root check.

- [ ] **Step 4: Verify GREEN and syntax**

Run:

```sh
sh deploy/edge-gateway-lite/tests/service-activation-test.sh
sh -n deploy/edge-gateway-lite/prepare-bundle.sh \
    deploy/edge-gateway-lite/install-board.sh \
    deploy/edge-gateway-lite/scripts/service-activate.sh
systemd-analyze verify deploy/edge-gateway-lite/systemd/edge-gateway-lite.service
```

Expected: test passes; Shell syntax is valid; unit verification has no errors. If `systemd-analyze` is unavailable, record the skip rather than claiming unit verification.

- [ ] **Step 5: Commit only Task 2 files**

```sh
git add deploy/edge-gateway-lite/systemd/edge-gateway-lite.service \
    deploy/edge-gateway-lite/scripts/service-activate.sh \
    deploy/edge-gateway-lite/prepare-bundle.sh \
    deploy/edge-gateway-lite/install-board.sh \
    deploy/edge-gateway-lite/tests/service-activation-test.sh
git commit -m "fix: restart gateway during board upgrades"
```

### Task 3: Strict Live-Version Health Check

**Files:**
- Modify: `deploy/edge-gateway-lite/scripts/health-check.sh:16-100`
- Modify: `deploy/edge-gateway-lite/tests/health-check-version-test.sh`

**Interfaces:**
- Consumes: installed `gateway`, installed `gatewayctl`, and deployed command socket `/run/edgevision-gateway/control.sock`.
- Produces: accumulated `failed=1` for every invalid candidate or running-version result.

- [ ] **Step 1: Extend the existing test with failing edge cases**

Run the real health script with controlled fake external tools. Add cases for:

```text
gatewayctl exit 0, stdout empty       -> FAIL running version reply is empty
gatewayctl output "ok version="       -> FAIL running version is empty
gatewayctl exit non-zero              -> FAIL running version probe failed
gatewayctl socket missing             -> version failure plus database integrity output
```

Each case asserts both final exit status and the relevant diagnostic. The missing-socket case also asserts `OK database integrity=ok` to prove later checks still run.

- [ ] **Step 2: Run the test and verify RED**

Run:

```sh
deploy/edge-gateway-lite/tests/health-check-version-test.sh
```

Expected: at least the empty-output and empty-version cases fail because the current implementation does not call `check_fail` for them.

- [ ] **Step 3: Implement strict parsing and deployed socket selection**

Set:

```sh
command_socket=${EDGEVISION_COMMAND_SOCKET:-/run/edgevision-gateway/control.sock}
```

Invoke the client with that environment:

```sh
runtime_output=$(EDGEVISION_COMMAND_SOCKET="$command_socket" \
    "$bundle/bin/gatewayctl" get_version 2>&1) || {
    check_fail "running version probe failed"
    runtime_output=""
}
```

Explicitly fail empty output, reject replies other than `ok version=*`, reject an empty value after the prefix, then compare the non-empty running version to the non-empty candidate version. Preserve the existing final `failed` check and all hardware/database probes.

- [ ] **Step 4: Verify GREEN and Shell portability**

Run:

```sh
sh -n deploy/edge-gateway-lite/scripts/health-check.sh \
    deploy/edge-gateway-lite/tests/health-check-version-test.sh
deploy/edge-gateway-lite/tests/health-check-version-test.sh
busybox sh -n deploy/edge-gateway-lite/scripts/health-check.sh \
    deploy/edge-gateway-lite/tests/health-check-version-test.sh
```

Expected: all health scenarios pass. If BusyBox is unavailable, record the skip.

- [ ] **Step 5: Commit only Task 3 files**

```sh
git add deploy/edge-gateway-lite/scripts/health-check.sh \
    deploy/edge-gateway-lite/tests/health-check-version-test.sh
git commit -m "fix: verify the running gateway version"
```

### Task 4: Deployment Documentation and Full Verification

**Files:**
- Modify: `deploy/edge-gateway-lite/README.md`
- Modify: `docs/tutorials/command-control-center.md`

**Interfaces:**
- Consumes: final paths and commands from Tasks 1-3.
- Produces: operator-facing explanation of local `/tmp` fallback, deployed `/run` socket, explicit restart, and health failure semantics.

- [ ] **Step 1: Update documentation**

Document that the bundle contains both `gateway` and `gatewayctl`; systemd creates `/run/edgevision-gateway`; the deployed client uses `/run/edgevision-gateway/control.sock`; manual development defaults to `/tmp/edgevision-study.sock`; and install failure does not imply automatic rollback.

- [ ] **Step 2: Run complete verification**

Run:

```sh
cmake --build build
ctest --test-dir build --output-on-failure
sh -n deploy/edge-gateway-lite/prepare-bundle.sh \
    deploy/edge-gateway-lite/install-board.sh \
    deploy/edge-gateway-lite/scripts/run-gateway.sh \
    deploy/edge-gateway-lite/scripts/health-check.sh \
    deploy/edge-gateway-lite/tests/health-check-version-test.sh \
    deploy/edge-gateway-lite/tests/service-activation-test.sh
deploy/edge-gateway-lite/tests/health-check-version-test.sh
deploy/edge-gateway-lite/tests/service-activation-test.sh
git diff --check
```

Expected: build succeeds; all CTest and Shell tests pass; no syntax or whitespace errors.

- [ ] **Step 3: Review the final diff against the spec**

Confirm: `PrivateTmp=true` remains; no schema/MQTT/service-user changes; `/tmp` remains the local fallback; `/run` is used by the deployed service and health check; `restart` replaces an active old process; all invalid version replies fail; no unrelated files are staged.

- [ ] **Step 4: Commit Task 4 documentation**

```sh
git add deploy/edge-gateway-lite/README.md docs/tutorials/command-control-center.md
git commit -m "docs: describe deployed command socket health checks"
```
