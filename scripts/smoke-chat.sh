#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEMO_CHAT_BIN="${DEMO_CHAT_BIN:-${ROOT_DIR}/build/demo-chat}"
BOOTSTRAP_ADDR="${BOOTSTRAP_ADDR:-/ip4/127.0.0.1/tcp/9900}"
TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-45}"
MESSAGE="smoke-${RANDOM}-${RANDOM}-$(date +%s)"

if [[ ! -x "${DEMO_CHAT_BIN}" ]]; then
  echo "demo-chat binary not found or not executable: ${DEMO_CHAT_BIN}" >&2
  exit 1
fi

WORK_DIR="$(mktemp -d)"
ALICE_LOG="${WORK_DIR}/alice.log"
BOB_LOG="${WORK_DIR}/bob.log"
ALICE_STDIN="${WORK_DIR}/alice.stdin"
BOB_STDIN="${WORK_DIR}/bob.stdin"

pids=()
writer_pids=()

cleanup() {
  local status=$?
  for pid in "${writer_pids[@]:-}"; do
    if kill -0 "${pid}" 2>/dev/null; then
      kill "${pid}" 2>/dev/null || true
    fi
  done
  for pid in "${pids[@]:-}"; do
    if kill -0 "${pid}" 2>/dev/null; then
      kill "${pid}" 2>/dev/null || true
    fi
  done
  for pid in "${pids[@]:-}"; do
    wait "${pid}" 2>/dev/null || true
  done
  for pid in "${writer_pids[@]:-}"; do
    wait "${pid}" 2>/dev/null || true
  done

  if [[ ${status} -ne 0 ]]; then
    echo "smoke test failed; logs are in ${WORK_DIR}" >&2
    for log in "${ALICE_LOG}" "${BOB_LOG}"; do
      if [[ -f "${log}" ]]; then
        echo "===== ${log} =====" >&2
        tail -n 120 "${log}" >&2 || true
      fi
    done
  else
    rm -rf "${WORK_DIR}"
  fi
}
trap cleanup EXIT

assert_processes_alive() {
  local pid stat
  for pid in "${pids[@]:-}"; do
    if ! kill -0 "${pid}" 2>/dev/null; then
      echo "demo-chat process exited early: pid ${pid}" >&2
      return 1
    fi
    stat="$(ps -o stat= -p "${pid}" 2>/dev/null || true)"
    if [[ "${stat}" == Z* ]]; then
      echo "demo-chat process exited early: pid ${pid}" >&2
      return 1
    fi
  done
}

wait_for_file() {
  local path="$1"
  local deadline=$((SECONDS + TIMEOUT_SECONDS))
  while (( SECONDS < deadline )); do
    assert_processes_alive
    [[ -s "${path}" ]] && return 0
    sleep 0.2
  done
  echo "timed out waiting for file: ${path}" >&2
  return 1
}

wait_for_log() {
  local path="$1"
  local pattern="$2"
  local deadline=$((SECONDS + TIMEOUT_SECONDS))
  while (( SECONDS < deadline )); do
    assert_processes_alive
    if [[ -f "${path}" ]] && grep -Fq "${pattern}" "${path}"; then
      return 0
    fi
    sleep 0.2
  done
  echo "timed out waiting for '${pattern}' in ${path}" >&2
  return 1
}

run_with_fifo_stdin() {
  local log="$1"
  local fifo="$2"
  shift 2
  (
    cd "${WORK_DIR}"
    DEMO_CHAT_DISABLE_DISCOVERY_LOOKUP=1 stdbuf -oL -eL "$@" <"${fifo}"
  ) >"${log}" 2>&1 &
  pids+=("$!")
}

mkfifo "${ALICE_STDIN}" "${BOB_STDIN}"

tail -f /dev/null >"${ALICE_STDIN}" &
writer_pids+=("$!")
tail -f /dev/null >"${BOB_STDIN}" &
writer_pids+=("$!")

run_with_fifo_stdin \
  "${ALICE_LOG}" \
  "${ALICE_STDIN}" \
  "${DEMO_CHAT_BIN}" --id alice --nick Alice --bootstrap --listen "${BOOTSTRAP_ADDR}"

ALICE_PEER_FILE="${WORK_DIR}/.demo-chat/state/alice/bootstrap-peer.txt"
wait_for_file "${ALICE_PEER_FILE}"
wait_for_log "${ALICE_LOG}" "Demo Chat: ready"
ALICE_PEER="$(cat "${ALICE_PEER_FILE}")"
ALICE_PEER_ID="${ALICE_PEER%%@*}"
exec 3>"${ALICE_STDIN}"

run_with_fifo_stdin \
  "${BOB_LOG}" \
  "${BOB_STDIN}" \
  "${DEMO_CHAT_BIN}" --id bob --nick Bob --bootstrap-peer "${ALICE_PEER}"

wait_for_log "${BOB_LOG}" "Demo Chat: ready"
wait_for_log "${BOB_LOG}" "Demo Chat: connected to ${ALICE_PEER_ID}"

# Give GossipSub and service discovery a short window to connect peers before
# publishing. Send a few times to avoid a transient mesh-formation race.
sleep 2
for _ in 1 2 3; do
  assert_processes_alive
  printf '%s\n' "${MESSAGE}" >&3
  sleep 1
  if grep -Fq "${MESSAGE}" "${BOB_LOG}"; then
    echo "smoke test passed: Bob received '${MESSAGE}'"
    exit 0
  fi
done

wait_for_log "${BOB_LOG}" "${MESSAGE}"
echo "smoke test passed: Bob received '${MESSAGE}'"
