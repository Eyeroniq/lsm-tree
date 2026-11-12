#!/usr/bin/env bash
# End-to-end smoke test of the service image over real HTTP:
#   CRUD and scans, graceful restart, SIGKILL crash recovery, and the
#   directory lock refusing a second container on the same volume.
#
#   docker build -t lsm-tree-server .
#   scripts/smoke_test.sh lsm-tree-server
set -euo pipefail

IMAGE=${1:-lsm-tree-server}
PORT=${PORT:-18080}
BASE="http://localhost:${PORT}"
NAME="lsm-smoke-$$"
VOLUME="lsm-smoke-data-$$"
failures=0

cleanup() {
  docker rm -f "$NAME" "${NAME}-second" >/dev/null 2>&1 || true
  docker volume rm "$VOLUME" >/dev/null 2>&1 || true
}
trap cleanup EXIT

start() {
  docker run -d --name "$NAME" -p "${PORT}:8080" -v "${VOLUME}:/data" "$IMAGE" >/dev/null
  for _ in $(seq 1 90); do
    if curl -fsS "${BASE}/actuator/health" 2>/dev/null | grep -q '"UP"'; then
      return 0
    fi
    sleep 1
  done
  echo "service did not become healthy" >&2
  docker logs "$NAME" >&2 || true
  exit 1
}

remove() {
  docker rm "$NAME" >/dev/null
}

check() {
  local description=$1 expected=$2 actual=$3
  if [[ "$actual" == "$expected" ]]; then
    echo "ok   - ${description}"
  else
    echo "FAIL - ${description}: expected '${expected}', got '${actual}'"
    failures=$((failures + 1))
  fi
}

code() {
  curl -s -o /dev/null -w '%{http_code}' "$@"
}

body() {
  curl -fsS "$@"
}

echo "# start"
start

echo "# key-value operations"
check "PUT user:1" 204 "$(code -X PUT --data-binary 'Ada' "${BASE}/api/v1/kv/user:1")"
check "PUT user:2" 204 "$(code -X PUT --data-binary 'Grace' "${BASE}/api/v1/kv/user:2")"
check "PUT user:3" 204 "$(code -X PUT --data-binary 'Linus' "${BASE}/api/v1/kv/user:3")"
check "GET user:1" "Ada" "$(body "${BASE}/api/v1/kv/user:1")"
check "DELETE user:3" 204 "$(code -X DELETE "${BASE}/api/v1/kv/user:3")"
check "GET deleted user:3" 404 "$(code "${BASE}/api/v1/kv/user:3")"
check "GET missing key" 404 "$(code "${BASE}/api/v1/kv/nobody")"

scan=$(body "${BASE}/api/v1/kv?from=user:&to=user;")
check "scan keys" "user:1,user:2" "$(jq -r '[.items[].key] | join(",")' <<<"$scan")"
check "scan values" "Ada,Grace" "$(jq -r '[.items[].value] | join(",")' <<<"$scan")"
check "scan with end before begin" 400 "$(code "${BASE}/api/v1/kv?from=z&to=a")"

echo "# admin, metrics and docs"
check "POST flush" 204 "$(code -X POST "${BASE}/api/v1/admin/flush")"
check "stats sstableCount" 1 "$(body "${BASE}/api/v1/admin/stats" | jq -r '.sstableCount')"
check "metric lsm.sstables" 1 \
  "$(body "${BASE}/actuator/metrics/lsm.sstables" | jq -r '.measurements[0].value | floor')"
check "OpenAPI title" "LSM-tree key-value API" \
  "$(body "${BASE}/v3/api-docs" | jq -r '.info.title')"
check "Swagger UI" 200 "$(code -L "${BASE}/swagger-ui.html")"

echo "# graceful restart keeps data"
docker stop "$NAME" >/dev/null
remove
start
check "user:1 after restart" "Ada" "$(body "${BASE}/api/v1/kv/user:1")"
check "user:2 after restart" "Grace" "$(body "${BASE}/api/v1/kv/user:2")"
check "user:3 still deleted" 404 "$(code "${BASE}/api/v1/kv/user:3")"

echo "# SIGKILL keeps acknowledged writes"
check "PUT crash:1 (WAL only)" 204 "$(code -X PUT --data-binary 'survives' "${BASE}/api/v1/kv/crash:1")"
docker kill "$NAME" >/dev/null
remove
start
check "crash:1 after SIGKILL" "survives" "$(body "${BASE}/api/v1/kv/crash:1")"

echo "# directory lock refuses a second container"
set +e
second_output=$(timeout 120 docker run --name "${NAME}-second" -v "${VOLUME}:/data" "$IMAGE" 2>&1)
second_status=$?
set -e
check "second container exits with an error" "nonzero" \
  "$([[ $second_status -ne 0 && $second_status -ne 124 ]] && echo nonzero || echo "$second_status")"
check "second container reports the lock" "yes" \
  "$(grep -q 'database directory is already open' <<<"$second_output" && echo yes || echo no)"
check "first container still serves reads" "Ada" "$(body "${BASE}/api/v1/kv/user:1")"

if [[ $failures -ne 0 ]]; then
  echo "${failures} check(s) failed" >&2
  docker logs "$NAME" >&2 || true
  exit 1
fi
echo "all smoke checks passed"
