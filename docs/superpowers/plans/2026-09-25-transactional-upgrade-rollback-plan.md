# Transactional Gateway Upgrade and Rollback Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a board-side upgrade transaction that commits only a strictly newer healthy Gateway and otherwise restores the previous program and, only when changed, its SQLite file set.

**Architecture:** Keep policy in a new `upgrade-board.sh` coordinator and move reusable mechanics into three focused helpers: semantic version comparison, payload installation, and backup/database restoration. Tests run the real scripts inside temporary roots with fake `systemctl`, `gateway`, `gatewayctl`, and `sqlite3`, so no host service or production path is modified.

**Tech Stack:** POSIX `sh`, BusyBox-compatible utilities, systemd CLI, SQLite CLI, SHA-256 manifests, CTest-independent shell integration tests.

**Spec:** `docs/superpowers/specs/2026-09-25-transactional-upgrade-rollback-design.md`

## Global Constraints

- Only `upgrade_committed` returns zero; every other stable result returns non-zero.
- Upgrade is permitted only when `running_version < candidate_version`, with strict numeric `MAJOR.MINOR.PATCH` parsing.
- Keep only `/userdata/edgevision-gateway/upgrade-backup/previous`; never discard a valid previous backup before a complete replacement exists.
- Stop the candidate before comparing or restoring `.db`, `.db-wal`, and `.db-shm`.
- Restore the database file set only when existence, size, or SHA-256 differs from the backup manifest.
- Never use globs for database cleanup or restoration; accept only an exact database path under `EDGEVISION_STATE_DIR` (production default `/userdata/edgevision-gateway`).
- Preserve user configuration during successful upgrades; restore its backup only if installation migration changed it and rollback occurs.
- Keep `PrivateTmp=true`, `/run/edgevision-gateway/control.sock`, the current service user, SQLite schema, and MQTT behavior unchanged.
- All scripts must pass POSIX `sh -n` and BusyBox `sh -n`.

## Review Focus

- Version components with leading zeroes or very large values must compare numerically without shell octal interpretation or integer overflow; Task 1 pins these inputs.
- A malicious or malformed manifest database path must be rejected before any move, copy, or removal; Task 2 pins traversal, wildcard, newline, and out-of-root paths.
- Failure between renaming `previous` aside and publishing `previous.new` must restore the still-valid previous backup; Task 2 injects this failure.
- Signals or unexpected exits after stopping the old service must not leave a candidate running or silently report success; Task 4 exercises TERM cleanup and state-specific recovery.
- A successful `gatewayctl` reply from the wrong process/version must prevent commit and trigger rollback; Task 4 asserts the actual runtime version before the full health check.

---

### Task 1: Strict Numeric Version Helper

**Files:**
- Create: `deploy/edge-gateway-lite/scripts/version-utils.sh`
- Create: `deploy/edge-gateway-lite/tests/version-utils-test.sh`

**Interfaces:**
- Consumes: version strings supplied as positional arguments.
- Produces: `version_is_valid VERSION` and `version_compare LEFT RIGHT`, where compare prints `-1`, `0`, or `1` and rejects malformed input.

- [ ] **Step 1: Write the failing version test**

Create a table-driven POSIX shell test that sources the helper and asserts:

```sh
assert_compare -1 0.1.0 0.2.0
assert_compare 1 0.10.0 0.2.0
assert_compare 0 1.002.0003 1.2.3
assert_compare 1 999999999999999999999.0.0 2.0.0
assert_invalid ""
assert_invalid 1.2
assert_invalid 1.2.3.4
assert_invalid 1.2.x
assert_invalid -1.2.3
```

Implement `assert_compare` by capturing `version_compare`, and `assert_invalid` by requiring `version_is_valid` and `version_compare "$value" 1.0.0` both to fail.

- [ ] **Step 2: Run the test and verify RED**

Run: `sh deploy/edge-gateway-lite/tests/version-utils-test.sh`

Expected: FAIL because `scripts/version-utils.sh` does not exist.

- [ ] **Step 3: Implement validation and comparison without arithmetic conversion**

Use only digit-pattern validation and normalized decimal strings. Define these exact public helpers and keep normalization/comparison private to the file:

```sh
version_is_valid VERSION
version_compare LEFT RIGHT
```

Implement private `decimal_normalize` by repeatedly removing one leading zero while length is greater than one. Implement private `decimal_compare` by comparing normalized string lengths and then `LC_ALL=C` lexical order. Do not use `$((10#$value))`, which is not POSIX, or shell integer arithmetic, which can overflow. Temporarily set `IFS=.` only for `set -- $version`, restore it immediately, and reject anything except three all-digit non-empty components.

- [ ] **Step 4: Verify normal, leading-zero, huge, and malformed versions**

Run:

```sh
sh -n deploy/edge-gateway-lite/scripts/version-utils.sh \
    deploy/edge-gateway-lite/tests/version-utils-test.sh
busybox sh -n deploy/edge-gateway-lite/scripts/version-utils.sh \
    deploy/edge-gateway-lite/tests/version-utils-test.sh
sh deploy/edge-gateway-lite/tests/version-utils-test.sh
```

Expected: syntax checks and all table cases pass.

- [ ] **Step 5: Commit Task 1**

```sh
git add deploy/edge-gateway-lite/scripts/version-utils.sh \
    deploy/edge-gateway-lite/tests/version-utils-test.sh
git commit -m "feat: add strict gateway version comparison"
```

### Task 2: Atomic Previous Backup and Database File-Set Restore

**Files:**
- Create: `deploy/edge-gateway-lite/scripts/upgrade-backup.sh`
- Create: `deploy/edge-gateway-lite/tests/upgrade-backup-test.sh`

**Interfaces:**
- Consumes: `UPGRADE_INSTALL_DIR`, `UPGRADE_UNIT_FILE`, `UPGRADE_CONFIG_FILE`, `UPGRADE_DATABASE`, `UPGRADE_BACKUP_ROOT`, `UPGRADE_RUNNING_VERSION`, and `UPGRADE_CANDIDATE_VERSION`.
- Produces: `upgrade_backup_create`, `upgrade_database_changed`, `upgrade_restore_program`, `upgrade_restore_database_if_changed`, and a validated `previous/manifest`.

- [ ] **Step 1: Write failing backup lifecycle tests**

Build a temporary tree with an installed v0.1 marker, unit, config, and database trio. Source the real helper and cover these exact behaviors:

```text
create backup -> previous/manifest has complete=1 and hashes
create second backup -> only previous remains and contains the second snapshot
injected publish failure -> first previous remains byte-for-byte usable
unchanged db set -> upgrade_database_changed returns 1 and restore does not rewrite inode
changed .db -> returns 0 and restores all backed-up members
new -wal, missing -shm, changed -wal -> each returns 0 and restores exact original existence
```

Use `UPGRADE_TEST_FAIL_BACKUP_PUBLISH=1` as the test-only fault-injection input. Record an inode or checksum before the unchanged restore call to prove no overwrite occurred.

- [ ] **Step 2: Add malicious manifest/path tests and verify RED**

Require rejection, with no touched sentinel outside the state root, for:

```sh
UPGRADE_DATABASE=/etc/passwd
UPGRADE_DATABASE=/userdata/edgevision-gateway/../escape.db
UPGRADE_DATABASE='/userdata/edgevision-gateway/*.db'
UPGRADE_DATABASE="/userdata/edgevision-gateway/bad
name.db"
```

Run: `sh deploy/edge-gateway-lite/tests/upgrade-backup-test.sh`

Expected: FAIL because the backup helper does not exist.

- [ ] **Step 3: Implement exact-path validation and manifest access**

Define these exact helper interfaces:

```sh
upgrade_validate_database_path PATH STATE_ROOT
upgrade_manifest_get MANIFEST KEY
upgrade_record_file MANIFEST KEY PATH
upgrade_file_matches_manifest MANIFEST KEY PATH
```

Require the database path to begin with `EDGEVISION_STATE_DIR/`; production defaults that root to `/userdata/edgevision-gateway`, while tests point it at their temporary state root. Reject newline, `*`, `?`, `[`, `]`, `/../`, a trailing `/..`, and an empty basename. Store the path under the `database_path` key; parse only a fixed allowlist of keys by matching the literal `KEY=` prefix, require exactly one match, and never source the manifest as shell code.

- [ ] **Step 4: Implement backup publication with recovery**

`upgrade_backup_create` must:

1. Create `previous.new` under `UPGRADE_BACKUP_ROOT` with mode `0700`.
2. Copy the install tree, unit, configuration, and each existing database member.
3. Write hashes, sizes, existence flags, versions, exact paths, then `complete=1` last.
4. Rename existing `previous` to `previous.old`.
5. Rename `previous.new` to `previous`.
6. If step 5 or injected publication fails, rename `previous.old` back to `previous` and return non-zero.
7. Remove `previous.old` only after the new `previous/manifest` revalidates.

All destructive targets must first resolve under the exact configured backup root.

- [ ] **Step 5: Implement changed-only database restoration**

`upgrade_database_changed` compares existence, byte size, and SHA-256 for the main file and the two literal suffix paths. `upgrade_restore_database_if_changed` returns success immediately when unchanged. When changed, move the three exact current paths into `database-quarantine`, install the backed-up members, verify their hashes, and remove quarantine only after verification; on failure, retain quarantine and return non-zero.

- [ ] **Step 6: Run backup, safety, and portability tests**

Run:

```sh
sh -n deploy/edge-gateway-lite/scripts/upgrade-backup.sh \
    deploy/edge-gateway-lite/tests/upgrade-backup-test.sh
busybox sh -n deploy/edge-gateway-lite/scripts/upgrade-backup.sh \
    deploy/edge-gateway-lite/tests/upgrade-backup-test.sh
sh deploy/edge-gateway-lite/tests/upgrade-backup-test.sh
```

Expected: every lifecycle, sidecar, publication-failure, and malicious-path case passes.

- [ ] **Step 7: Commit Task 2**

```sh
git add deploy/edge-gateway-lite/scripts/upgrade-backup.sh \
    deploy/edge-gateway-lite/tests/upgrade-backup-test.sh
git commit -m "feat: add safe gateway upgrade backups"
```

### Task 3: Separate Payload Installation from Service Activation

**Files:**
- Create: `deploy/edge-gateway-lite/scripts/install-payload.sh`
- Create: `deploy/edge-gateway-lite/tests/install-payload-test.sh`
- Modify: `deploy/edge-gateway-lite/install-board.sh`

**Interfaces:**
- Consumes: `EDGEVISION_BUNDLE` plus test-overridable `EDGEVISION_CONFIG_DIR`, `EDGEVISION_INSTALL_DIR`, `EDGEVISION_STATE_DIR`, and `EDGEVISION_UNIT_DIR`.
- Produces: `install-payload.sh`, which validates and copies a bundle but never calls `systemctl`; `install-board.sh` remains the first-install entry point and activates only after payload installation succeeds.

- [ ] **Step 1: Write a failing isolated payload test**

Create a complete fake bundle and recording fake `systemctl`. Run `install-payload.sh` into temporary destination roots and assert:

```text
all SHA256SUMS validate before any destination changes
gateway, gatewayctl, libraries, scripts, unit, README, and checksum manifest copy with exact modes
existing field config is preserved
old default EDGEVISION_BUNDLE line is migrated exactly once
no systemctl call occurs
missing helper or checksum mismatch leaves all destination sentinels unchanged
```

- [ ] **Step 2: Run the payload test and verify RED**

Run: `sh deploy/edge-gateway-lite/tests/install-payload-test.sh`

Expected: FAIL because `install-payload.sh` does not exist.

- [ ] **Step 3: Extract validation and copying into `install-payload.sh`**

Move the current bundle prechecks, `sha256sum -c`, directory creation, file installation, first-config creation, exact legacy-config migration, and unit installation into the new script. Defaults remain the production paths; test overrides are accepted only through the four named environment variables. The script must not invoke `systemctl`, sleep, or run health checks.

- [ ] **Step 4: Reduce `install-board.sh` to composition**

After its root check, run:

```sh
"$bundle/scripts/install-payload.sh"
"$install_dir/scripts/service-activate.sh" edge-gateway-lite.service
sleep 3
systemctl is-active --quiet edge-gateway-lite.service
"$install_dir/scripts/health-check.sh"
```

Preserve its current first-install behavior and diagnostics. Do not add rollback to this entry point; transactional upgrades use `upgrade-board.sh`.

- [ ] **Step 5: Verify payload isolation and existing activation behavior**

Run:

```sh
sh deploy/edge-gateway-lite/tests/install-payload-test.sh
sh deploy/edge-gateway-lite/tests/service-activation-test.sh
sh -n deploy/edge-gateway-lite/install-board.sh \
    deploy/edge-gateway-lite/scripts/install-payload.sh
```

Expected: payload tests pass, no systemctl call comes from the payload helper, and existing activation tests remain green.

- [ ] **Step 6: Commit Task 3**

```sh
git add deploy/edge-gateway-lite/scripts/install-payload.sh \
    deploy/edge-gateway-lite/tests/install-payload-test.sh \
    deploy/edge-gateway-lite/install-board.sh
git commit -m "refactor: separate gateway payload installation"
```

### Task 4: Transaction Coordinator and Rollback Outcomes

**Files:**
- Create: `deploy/edge-gateway-lite/upgrade-board.sh`
- Create: `deploy/edge-gateway-lite/tests/upgrade-transaction-test.sh`

**Interfaces:**
- Consumes: version and backup helper interfaces from Tasks 1-2, `install-payload.sh` from Task 3, the deployed service, and the existing health-check command.
- Produces: stable result lines `upgrade_committed`, `precheck_failed`, `backup_failed_old_restored`, `upgrade_failed_rollback_ok`, `upgrade_failed_rollback_unhealthy`, or `rollback_failed`.

- [ ] **Step 1: Write the transaction harness and precheck RED cases**

The harness creates fake v0.1 installed files, a v0.2 bundle, a fake database, and recording tools. Make fake `systemctl` track `active_version`; make fake `gatewayctl get_version` print that version. Assert checksum failure, malformed/equal/lower candidate versions, unhealthy old service, corrupt database, and insufficient backup space all return `precheck_failed` without recording `stop`.

Expose only these test seams:

```text
EDGEVISION_CONFIG, EDGEVISION_BACKUP_ROOT, EDGEVISION_CONFIG_DIR,
EDGEVISION_INSTALL_DIR, EDGEVISION_STATE_DIR, EDGEVISION_UNIT_DIR,
UPGRADE_TEST_AVAILABLE_BYTES, UPGRADE_TEST_SIGNAL_AFTER_STATE
```

- [ ] **Step 2: Add successful commit and rollback RED cases**

Assert literal order for success:

```text
precheck -> stop old -> backup -> install -> daemon-reload -> enable -> restart
-> runtime version v0.2 -> candidate health -> upgrade_committed
```

Then cover candidate start failure, runtime still reporting v0.1, full health failure, unchanged database, changed main DB, added WAL, removed SHM, program restore failure, database restore failure, and old health failure caused by a missing serial device.

- [ ] **Step 3: Implement prechecks and disk-space calculation**

`upgrade-board.sh` must require root, validate the bundle before modification, obtain candidate version from the bundle, obtain running version via the deployed socket, call `version_compare`, run the current health check and database integrity check, and calculate required backup bytes from the install tree, unit, config, and existing database members. Production free space comes from `df -Pk "$backup_parent"`; tests may supply `UPGRADE_TEST_AVAILABLE_BYTES`.

- [ ] **Step 4: Implement state-aware cleanup and backup failure recovery**

Maintain a simple state variable (`PRECHECK`, `OLD_STOPPED`, `BACKUP_READY`, `CANDIDATE_INSTALLED`, `CANDIDATE_RUNNING`, `ROLLING_BACK`). Install traps for `HUP INT TERM`. A signal after the old service stops must invoke the same recovery path as a normal failure; before that point it only returns failure. If backup creation fails, never install the candidate: restart the old service, require old runtime version and full health, then emit `backup_failed_old_restored` or `rollback_failed`.

- [ ] **Step 5: Implement candidate verification and rollback**

After payload installation, explicitly reload, enable, and restart. Require `gatewayctl get_version` to equal the candidate before running the complete health check. Any failure then stops the candidate, restores program/unit/config, restores the database only when `upgrade_database_changed` says it changed, reloads and starts the old service, checks the old runtime version, and runs the old health check.

Map outcomes exactly:

```text
candidate runtime and health pass                 -> upgrade_committed, exit 0
rollback files/version/full health pass           -> upgrade_failed_rollback_ok, exit 1
rollback files/version pass but full health fails -> upgrade_failed_rollback_unhealthy, exit 1
any restore or old-version check fails            -> rollback_failed, exit 1
```

- [ ] **Step 6: Verify signal recovery, wrong-process detection, and all outcomes**

Run:

```sh
sh -n deploy/edge-gateway-lite/upgrade-board.sh \
    deploy/edge-gateway-lite/tests/upgrade-transaction-test.sh
busybox sh -n deploy/edge-gateway-lite/upgrade-board.sh \
    deploy/edge-gateway-lite/tests/upgrade-transaction-test.sh
sh deploy/edge-gateway-lite/tests/upgrade-transaction-test.sh
```

Expected: every precheck, success, backup failure, candidate failure, unchanged/changed database, wrong runtime version, TERM recovery, unhealthy rollback, and rollback failure case passes.

- [ ] **Step 7: Commit Task 4**

```sh
git add deploy/edge-gateway-lite/upgrade-board.sh \
    deploy/edge-gateway-lite/tests/upgrade-transaction-test.sh
git commit -m "feat: add transactional gateway upgrades"
```

### Task 5: Bundle Integration, Operator Documentation, and Full Verification

**Files:**
- Modify: `deploy/edge-gateway-lite/prepare-bundle.sh`
- Modify: `deploy/edge-gateway-lite/README.md`
- Modify: `docs/d49-command-control-center.md`
- Test: all deployment shell tests and project CTest suites.

**Interfaces:**
- Consumes: final scripts and stable result lines from Tasks 1-4.
- Produces: a self-contained release bundle with first-install and transactional-upgrade entry points plus operator guidance.

- [ ] **Step 1: Extend bundle assembly and its smoke assertions**

Require and copy `upgrade-board.sh`, `install-payload.sh`, `version-utils.sh`, and `upgrade-backup.sh` with mode `0755`. Generate `SHA256SUMS` after all files are present. Extend the temporary bundle smoke test to assert each new file exists, is executable, and passes `sha256sum -c`.

- [ ] **Step 2: Document the exact operational contract**

Update the deployment README with:

```text
first install: install-board.sh
upgrade: upgrade-board.sh
backup: /userdata/edgevision-gateway/upgrade-backup/previous
commit gate: running_version < candidate_version and every safety/health check passes
database restore: only when .db/.db-wal/.db-shm set differs
all six stable result lines and operator response
rollback_unhealthy means files/version restored but environment still unhealthy
rollback_failed requires manual intervention and preservation of quarantine/backup
```

Update the command-control document only where it explains runtime-version verification; do not duplicate the full deployment manual.

- [ ] **Step 3: Run complete verification**

Run:

```sh
cmake --build build -j2
ctest --test-dir build --output-on-failure
cmake --build build-service --target gateway gatewayctl -j2
ctest --test-dir build-service -R gateway_command_socket --output-on-failure
for test in deploy/edge-gateway-lite/tests/*-test.sh; do sh "$test"; done
sh -n deploy/edge-gateway-lite/prepare-bundle.sh \
    deploy/edge-gateway-lite/install-board.sh \
    deploy/edge-gateway-lite/upgrade-board.sh \
    deploy/edge-gateway-lite/scripts/*.sh \
    deploy/edge-gateway-lite/tests/*-test.sh
busybox sh -n deploy/edge-gateway-lite/upgrade-board.sh \
    deploy/edge-gateway-lite/scripts/version-utils.sh \
    deploy/edge-gateway-lite/scripts/upgrade-backup.sh
systemd-analyze verify deploy/edge-gateway-lite/systemd/edge-gateway-lite.service
git diff --check
```

Expected: both builds pass, 24 default CTests pass, the command-socket integration passes, every deployment test passes, Shell syntax checks pass, unit verification reports no project-unit error, and no whitespace errors remain.

- [ ] **Step 4: Review the final diff against the spec**

Confirm all eleven spec sections have an implementation or documentation counterpart; no glob removes database files; `PrivateTmp=true` remains; the deployed socket remains under `/run`; successful upgrades preserve field config; only one complete previous backup remains; and unrelated user files are unstaged.

- [ ] **Step 5: Commit Task 5**

```sh
git add deploy/edge-gateway-lite/prepare-bundle.sh \
    deploy/edge-gateway-lite/README.md \
    docs/d49-command-control-center.md
git commit -m "docs: describe transactional gateway upgrades"
```
