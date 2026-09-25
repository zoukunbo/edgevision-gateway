#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
upgrade=$project_dir/deploy/edge-gateway-lite/upgrade-board.sh
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT HUP INT TERM

make_program()
{
    path=$1
    version=$2
    mkdir -p "$(dirname "$path")"
    cat >"$path" <<EOF
#!/bin/sh
[ "\${1:-}" = --version ] && printf 'edge-gateway-lite $version\n'
EOF
    chmod 0755 "$path"
}

make_gatewayctl()
{
    path=$1
cat >"$path" <<'EOF'
#!/bin/sh
[ ! -e "$TEST_ROOT/ready-count" ] || {
    count=$(cat "$TEST_ROOT/ready-count")
    [ "$count" -le 0 ] || {
        printf '%s\n' "$((count - 1))" >"$TEST_ROOT/ready-count"
        exit 1
    }
}
printf 'ok version=%s\n' "$(cat "$TEST_ROOT/active-version")"
EOF
    chmod 0755 "$path"
}

make_health()
{
    path=$1
    marker=$2
    printf '#!/bin/sh\n[ ! -e "$TEST_ROOT/%s" ]\n' "$marker" >"$path"
    chmod 0755 "$path"
}

prepare_case()
{
    rm -rf "$root/case"
    case_root=$root/case
    state=$case_root/state
    install_dir=$state/edge-gateway-lite
    bundle=$case_root/bundle
    config_dir=$case_root/etc
    unit_dir=$case_root/system
    backup_root=$state/upgrade-backup
    database=$state/gateway.db
    mkdir -p "$install_dir/bin" "$install_dir/lib" "$install_dir/config" \
        "$install_dir/scripts" "$install_dir/systemd" "$bundle/bin" "$bundle/lib" \
        "$bundle/config" "$bundle/scripts" "$bundle/systemd" "$config_dir" "$unit_dir" "$case_root/tools"
    make_program "$install_dir/bin/gateway" 0.1.0
    make_gatewayctl "$install_dir/bin/gatewayctl"
    make_health "$install_dir/scripts/health-check.sh" old-unhealthy
    printf 'old-program\n' >"$install_dir/marker"
    printf 'old-unit\n' >"$install_dir/systemd/edge-gateway-lite.service"
    printf 'old-readme\n' >"$install_dir/README.md"
    printf 'old-sums\n' >"$install_dir/SHA256SUMS"
    printf 'lib\n' >"$install_dir/lib/libmosquitto.so.1"
    printf 'template\n' >"$install_dir/config/edge-gateway-lite.env"
    printf 'old-unit\n' >"$unit_dir/edge-gateway-lite.service"
    printf 'db-old\n' >"$database"
    printf 'EDGEVISION_BUNDLE=%s\nEDGEVISION_STATE_DIR=%s\nEDGEVISION_DATABASE=%s\nEDGEVISION_SOURCE=simulated\n' \
        "$install_dir" "$state" "$database" >"$config_dir/edge-gateway-lite.env"

    make_program "$bundle/bin/gateway" 0.2.0
    make_gatewayctl "$bundle/bin/gatewayctl"
    make_health "$bundle/scripts/health-check.sh" candidate-unhealthy
    cp "$project_dir/deploy/edge-gateway-lite/scripts/install-payload.sh" "$bundle/scripts/"
    cp "$project_dir/deploy/edge-gateway-lite/scripts/service-activate.sh" "$bundle/scripts/"
    cp "$project_dir/deploy/edge-gateway-lite/scripts/version-utils.sh" "$bundle/scripts/"
    cp "$project_dir/deploy/edge-gateway-lite/scripts/upgrade-backup.sh" "$bundle/scripts/"
    printf '#!/bin/sh\nexit 0\n' >"$bundle/scripts/run-gateway.sh"
    chmod 0755 "$bundle/scripts/"*.sh
    printf 'lib\n' >"$bundle/lib/libmosquitto.so.1"
    printf 'template\n' >"$bundle/config/edge-gateway-lite.env"
    printf 'new-unit\n' >"$bundle/systemd/edge-gateway-lite.service"
    printf 'new-readme\n' >"$bundle/README.md"
    (cd "$bundle" && find bin lib config scripts systemd -type f -print | LC_ALL=C sort | xargs sha256sum >SHA256SUMS && sha256sum README.md >>SHA256SUMS)
    printf '0.1.0\n' >"$case_root/active-version"
    : >"$case_root/calls"

    cat >"$case_root/tools/systemctl" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >>"$TEST_ROOT/calls"
case "$1" in
    stop) exit 0 ;;
    restart)
        version=$($EDGEVISION_INSTALL_DIR/bin/gateway --version)
        raw=${version#edge-gateway-lite }
        [ ! -e "$TEST_ROOT/restart-fail-candidate" ] || [ "$raw" != 0.2.0 ] || exit 1
        [ ! -e "$TEST_ROOT/restart-fail-old" ] || [ "$raw" != 0.1.0 ] || exit 1
        active=${version#edge-gateway-lite }
        if [ "$active" = 0.2.0 ] && [ -e "$TEST_ROOT/wrong-runtime" ]; then active=0.1.0; fi
        printf '%s\n' "$active" >"$TEST_ROOT/active-version"
        [ ! -e "$TEST_ROOT/delay-ready" ] || printf '2\n' >"$TEST_ROOT/ready-count"
        if [ "${version#edge-gateway-lite }" = 0.2.0 ] && [ -e "$TEST_ROOT/change-db" ]; then
            printf 'candidate-write\n' >>"$EDGEVISION_STATE_DIR/gateway.db"
        fi
        [ ! -e "$TEST_ROOT/old-unhealthy-after" ] || {
            [ "${version#edge-gateway-lite }" != 0.2.0 ] || touch "$TEST_ROOT/old-unhealthy"
        }
        ;;
    is-active) exit 0 ;;
esac
exit 0
EOF
    chmod 0755 "$case_root/tools/systemctl"
    cat >"$case_root/tools/sqlite3" <<'EOF'
#!/bin/sh
[ ! -e "$TEST_ROOT/db-corrupt" ] && printf 'ok\n' || printf 'corrupt\n'
EOF
    chmod 0755 "$case_root/tools/sqlite3"
    cat >"$case_root/tools/id" <<'EOF'
#!/bin/sh
[ "${1:-}" = -u ] && printf '0\n'
EOF
    chmod 0755 "$case_root/tools/id"
}

run_upgrade()
{
    TEST_ROOT=$case_root EDGEVISION_BUNDLE=$bundle \
    EDGEVISION_CONFIG=$config_dir/edge-gateway-lite.env \
    EDGEVISION_BACKUP_ROOT=$backup_root EDGEVISION_CONFIG_DIR=$config_dir \
    EDGEVISION_INSTALL_DIR=$install_dir EDGEVISION_STATE_DIR=$state \
    EDGEVISION_UNIT_DIR=$unit_dir UPGRADE_TEST_AVAILABLE_BYTES=${UPGRADE_TEST_AVAILABLE_BYTES:-999999999} \
    UPGRADE_TEST_SIGNAL_AFTER_STATE=${UPGRADE_TEST_SIGNAL_AFTER_STATE:-} \
    UPGRADE_TEST_FAIL_BACKUP_PUBLISH=${UPGRADE_TEST_FAIL_BACKUP_PUBLISH:-0} \
    UPGRADE_READY_ATTEMPTS=${UPGRADE_READY_ATTEMPTS:-5} UPGRADE_READY_DELAY=0 \
    PATH="$case_root/tools:$PATH" sh "$upgrade"
}

expect_failure()
{
    expected=$1
    if run_upgrade >"$case_root/output" 2>&1; then
        echo "expected failure: $expected" >&2
        exit 1
    fi
    grep -Fx "$expected" "$case_root/output" >/dev/null
}

prepare_case
run_upgrade >"$case_root/output"
grep -Fx upgrade_committed "$case_root/output" >/dev/null
[ "$(cat "$case_root/active-version")" = 0.2.0 ]

prepare_case
touch "$case_root/candidate-unhealthy"
expect_failure upgrade_failed_rollback_ok
[ "$(cat "$case_root/active-version")" = 0.1.0 ]
grep -Fx old-program "$install_dir/marker" >/dev/null
grep -Fx db-old "$database" >/dev/null

prepare_case
touch "$case_root/change-db" "$case_root/candidate-unhealthy"
expect_failure upgrade_failed_rollback_ok
grep -Fx db-old "$database" >/dev/null
[ "$(wc -l <"$database")" -eq 1 ]

prepare_case
touch "$case_root/old-unhealthy-after" "$case_root/candidate-unhealthy"
expect_failure upgrade_failed_rollback_unhealthy

prepare_case
sed -i 's/0.2.0/0.1.0/' "$bundle/bin/gateway"
(cd "$bundle" && find bin lib config scripts systemd -type f -print | LC_ALL=C sort | xargs sha256sum >SHA256SUMS && sha256sum README.md >>SHA256SUMS)
expect_failure precheck_failed
if grep -Fx 'stop edge-gateway-lite.service' "$case_root/calls" >/dev/null; then
    echo 'precheck failure stopped old service' >&2
    exit 1
fi

prepare_case
touch "$case_root/db-corrupt"
expect_failure precheck_failed
if grep -Fx 'stop edge-gateway-lite.service' "$case_root/calls" >/dev/null; then
    echo 'database precheck failure stopped old service' >&2
    exit 1
fi

prepare_case
touch "$case_root/wrong-runtime"
expect_failure upgrade_failed_rollback_ok

prepare_case
UPGRADE_TEST_SIGNAL_AFTER_STATE=BACKUP_READY
export UPGRADE_TEST_SIGNAL_AFTER_STATE
expect_failure upgrade_failed_rollback_ok
unset UPGRADE_TEST_SIGNAL_AFTER_STATE

prepare_case
UPGRADE_TEST_SIGNAL_AFTER_STATE=OLD_STOPPED
export UPGRADE_TEST_SIGNAL_AFTER_STATE
expect_failure backup_failed_old_restored
unset UPGRADE_TEST_SIGNAL_AFTER_STATE
[ "$(cat "$case_root/active-version")" = 0.1.0 ]
grep -Fx 'restart edge-gateway-lite.service' "$case_root/calls" >/dev/null

prepare_case
touch "$case_root/delay-ready"
run_upgrade >"$case_root/output"
grep -Fx upgrade_committed "$case_root/output" >/dev/null

prepare_case
UPGRADE_TEST_AVAILABLE_BYTES=0
export UPGRADE_TEST_AVAILABLE_BYTES
expect_failure precheck_failed
unset UPGRADE_TEST_AVAILABLE_BYTES

prepare_case
dd if=/dev/zero of="$database-wal" bs=1024 count=64 2>/dev/null
UPGRADE_TEST_AVAILABLE_BYTES=32768
export UPGRADE_TEST_AVAILABLE_BYTES
expect_failure precheck_failed
unset UPGRADE_TEST_AVAILABLE_BYTES

prepare_case
UPGRADE_TEST_FAIL_BACKUP_PUBLISH=1
export UPGRADE_TEST_FAIL_BACKUP_PUBLISH
expect_failure backup_failed_old_restored
unset UPGRADE_TEST_FAIL_BACKUP_PUBLISH

prepare_case
touch "$case_root/restart-fail-candidate"
expect_failure upgrade_failed_rollback_ok

prepare_case
touch "$case_root/candidate-unhealthy" "$case_root/restart-fail-old"
expect_failure rollback_failed

printf 'PASS transactional upgrade commit and rollback outcomes\n'
