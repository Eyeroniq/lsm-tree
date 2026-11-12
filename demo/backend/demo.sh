#!/usr/bin/env bash
# lsm-tree backend demo: every claim in the README, run for real.
#
#   ./demo/backend/demo.sh            # everything, in order
#   ./demo/backend/demo.sh crash      # one section
#
# Sections: tests sanitizers crash powercut mutation benchmark service recovery
# Linux (or WSL) with g++/clang, make, python3, and for `recovery` a JDK 25
# and Maven. Run from the repository root.
set -euo pipefail

if [[ -t 1 || -n "${FORCE_COLOR:-}" ]]; then
  BOLD=$'\e[1m'; DIM=$'\e[2m'; GREEN=$'\e[32m'; RED=$'\e[31m'
  CYAN=$'\e[36m'; VIOLET=$'\e[35m'; RESET=$'\e[0m'
else
  BOLD=""; DIM=""; GREEN=""; RED=""; CYAN=""; VIOLET=""; RESET=""
fi

heading() { printf '\n%s%s▍ %s%s\n' "$BOLD" "$VIOLET" "$1" "$RESET"; }
ok() { printf '%s%s✔ %s%s\n' "$BOLD" "$GREEN" "$1" "$RESET"; }
fail() { printf '%s%s✘ %s%s\n' "$BOLD" "$RED" "$1" "$RESET"; exit 1; }
show() { printf '%s$ %s%s\n' "$DIM" "$*" "$RESET"; }

tests() {
  heading "8,000+ assertions: recovery, levels, snapshots, concurrency"
  make test
}

sanitizers() {
  heading "The same suite under AddressSanitizer + UBSan, then ThreadSanitizer"
  make asan-test CXX=clang++
  make tsan-test CXX=clang++
}

service() {
  heading "The Spring Boot service, calling the engine through Java's FFM API"
  [[ -f build/liblsm.so ]] || make -s shared
  (cd server && mvn -B verify 2>&1) | grep -E "Tests run: [0-9]+, Failures|BUILD (SUCCESS|FAILURE)"
}

crash() {
  heading "SIGKILL the writer at random moments; no acknowledged write may be lost"
  make crash-test CRASH_ROUNDS="${CRASH_ROUNDS:-100}"
}

powercut() {
  heading "Simulated power cuts: only fsynced data and names survive"
  make powercut-test POWERCUT_ROUNDS="${POWERCUT_ROUNDS:-60}"
}

mutation() {
  heading "Put back the bug the power-cut test found, and watch it get caught"
  local work
  work=$(mktemp -d)
  tar --exclude='./build*' --exclude=./server --exclude=./.git -cf - . | tar -xf - -C "$work"
  python3 - "$work/src/lsm.cpp" <<'EOF'
import sys
path = sys.argv[1]
text = open(path).read()
fix = "  try {\n    sync_directory();\n  } catch (...) {\n    ::close(fd);\n    throw;\n  }\n"
assert text.count(fix) == 1, "pattern not found"
open(path, "w").write(text.replace(fix, ""))
EOF
  echo "removed from rotate_wal(): the directory fsync that makes a new WAL's name durable"
  echo "building the broken engine..."
  (cd "$work" && make -s build/lsm_tool build/powercut.so build/powercut_snapshot >/dev/null 2>&1) ||
    fail "the broken copy did not build"
  show "make powercut-test   # against the broken engine"
  local output status=0
  output=$(cd "$work" && python3 scripts/powercut_torture.py --binary ./build/lsm_tool \
    --preload ./build/powercut.so --snapshot ./build/powercut_snapshot --rounds 25 2>&1) || status=$?
  rm -rf "$work"
  if [[ $status -eq 0 ]]; then
    fail "the broken engine survived: the test missed the bug"
  fi
  printf '%s%s%s\n' "$RED" "$(grep -oE "power cut [0-9]+ lost data: .*" <<<"$output" | tail -1)" "$RESET"
  grep -q "lost data" <<<"$output" || fail "the harness failed for another reason"
  ok "caught: without that fsync, a power cut loses acknowledged writes"
}

benchmark() {
  heading "20,000 fsynced writes, then 20,000 random point reads"
  make benchmark
}

recovery() {
  heading "kill -9 the HTTP service; acknowledged writes survive the restart"
  local jar=server/target/lsm-tree-server-0.1.0.jar
  local port=${PORT:-18181}
  [[ -f build/liblsm.so ]] || make -s shared
  [[ -f $jar ]] || (cd server && mvn -B -q package -DskipTests)
  local data pid
  data=$(mktemp -d)
  start() {
    java --enable-native-access=ALL-UNNAMED -jar "$jar" --lsm.data-dir="$data" \
      --server.port="$port" >"$data.log" 2>&1 &
    pid=$!
    until curl -fs "localhost:$port/actuator/health" >/dev/null 2>&1; do sleep 0.2; done
    echo "service up on :$port (pid $pid)"
  }
  start
  local keys=(user:1 user:2 user:3)
  local values=("Ada Lovelace" "Grace Hopper" "Barbara Liskov")
  for i in 0 1 2; do
    show "curl -X PUT --data-binary '${values[$i]}' localhost:$port/api/v1/kv/${keys[$i]}"
    printf '%s\n' "$(curl -s -o /dev/null -w 'HTTP %{http_code}' -X PUT \
      --data-binary "${values[$i]}" "localhost:$port/api/v1/kv/${keys[$i]}")"
  done
  show "kill -9 $pid"
  kill -9 "$pid"
  wait "$pid" 2>/dev/null || true
  printf '%s%s✘ service killed with SIGKILL%s\n' "$BOLD" "$RED" "$RESET"
  start
  local all=1
  for i in 0 1 2; do
    show "curl localhost:$port/api/v1/kv/${keys[$i]}"
    local got
    got=$(curl -s "localhost:$port/api/v1/kv/${keys[$i]}")
    printf '%s\n' "$got"
    [[ "$got" == "${values[$i]}" ]] || all=0
  done
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  rm -rf "$data" "$data.log"
  [[ $all == 1 ]] && ok "all three writes survived kill -9" || fail "a write was lost"
}

sections=(tests sanitizers crash powercut mutation benchmark service recovery)
if [[ $# -eq 0 ]]; then
  for section in "${sections[@]}"; do "$section"; done
else
  for section in "$@"; do
    [[ " ${sections[*]} " == *" $section "* ]] || { echo "unknown section: $section" >&2; exit 2; }
    "$section"
  done
fi
