#!/usr/bin/env sh
set -eu

pc_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
supervisor=$pc_root/scripts/run-dedicated-server.sh
fake=$pc_root/tests/fixtures/fake_authority.sh
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT INT TERM HUP
rom=$scratch/test.z64
: > "$rom"

run_failure_case() {
    name=$1
    mode=$2
    expected=$3
    state=$scratch/$name
    mkdir -p "$state"
    set +e
    BTGA_FAKE_AUTHORITY_MODE=$mode \
    BTGA_SERVER_MAX_FAILURES=1 \
    BTGA_SERVER_STABLE_SECONDS=300 \
    BTGA_SERVER_HANG_SECONDS=1 \
    BTGA_SERVER_STARTUP_HEALTH_SECONDS=2 \
        "$supervisor" "$rom" "$fake" "$state" \
        >"$state/test.stdout" 2>"$state/test.stderr"
    result=$?
    set -e
    [ "$result" -eq 1 ]
    grep -q "$expected" "$state/test.stderr"
    [ "$(find "$state/logs" -name '*.stderr.log' | wc -l)" -eq 2 ]
}

run_failure_case crash crash 'exceeded 1 consecutive failures'
run_failure_case stall stall 'stopped advancing its main loop'
run_failure_case delete-health delete-health 'stopped advancing its main loop'
run_failure_case no-health no-health 'never published main-loop health'

mutable=$scratch/mutable-authority.sh
cp "$fake" "$mutable"
chmod +x "$mutable"
mutation_state=$scratch/binary-mutation
mkdir -p "$mutation_state"
set +e
BTGA_FAKE_AUTHORITY_MODE=mutate \
BTGA_SERVER_MAX_FAILURES=10 \
BTGA_SERVER_STABLE_SECONDS=300 \
    "$supervisor" "$rom" "$mutable" "$mutation_state" \
    >"$mutation_state/test.stdout" 2>"$mutation_state/test.stderr"
result=$?
set -e
[ "$result" -eq 1 ]
grep -q 'authority binary changed' "$mutation_state/test.stderr"
[ "$(find "$mutation_state/logs" -name '*.stderr.log' | wc -l)" -eq 1 ]

persistent=$scratch/persistent-authority.sh
cp "$fake" "$persistent"
chmod +x "$persistent"
persistent_state=$scratch/persistent-identity
mkdir -p "$persistent_state"
set +e
BTGA_FAKE_AUTHORITY_MODE=crash \
BTGA_SERVER_MAX_FAILURES=1 \
BTGA_SERVER_STABLE_SECONDS=300 \
    "$supervisor" "$rom" "$persistent" "$persistent_state" \
    >"$persistent_state/first.stdout" 2>"$persistent_state/first.stderr"
first_result=$?
set -e
[ "$first_result" -eq 1 ]
[ -s "$persistent_state/runtime-identity.txt" ]
printf '\n# new build\n' >> "$persistent"
set +e
BTGA_FAKE_AUTHORITY_MODE=crash \
    "$supervisor" "$rom" "$persistent" "$persistent_state" \
    >"$persistent_state/second.stdout" 2>"$persistent_state/second.stderr"
second_result=$?
set -e
[ "$second_result" -eq 1 ]
grep -q 'state belongs to a different executable or ROM' \
    "$persistent_state/second.stderr"
[ "$(find "$persistent_state/logs" -name '*.stderr.log' | wc -l)" -eq 2 ]

lock_state=$scratch/locked
mkdir -p "$lock_state"
exec 8>"$lock_state/supervisor.lock"
flock -n 8
set +e
"$supervisor" "$rom" /bin/false "$lock_state" \
    >"$lock_state/test.stdout" 2>"$lock_state/test.stderr"
result=$?
set -e
[ "$result" -eq 1 ]
grep -q 'another dedicated authority supervisor owns' "$lock_state/test.stderr"

echo "dedicated supervisor tests passed"
