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
BOOTSTRAP_LOG="${WORK_DIR}/bootstrap.log"
ALICE_LOG="${WORK_DIR}/alice.log"
BOB_LOG="${WORK_DIR}/bob.log"
BOOTSTRAP_STDIN="${WORK_DIR}/bootstrap.stdin"
ALICE_STDIN="${WORK_DIR}/alice.stdin"
BOB_STDIN="${WORK_DIR}/bob.stdin"

pids=()
fds=()

cleanup() {
  local status=$?
  for pid in "${pids[@]:-}"; do
    if kill -0 "${pid}" 2>/dev/null; then
      kill "${pid}" 2>/dev/null || true
    fi
  done
  for pid in "${pids[@]:-}"; do
    wait "${pid}" 2>/dev/null || true
  done
  for fd in "${fds[@]:-}"; do
    eval "exec ${fd}>&-" 2>/dev/null || true
  done

  if [[ ${status} -ne 0 ]]; then
    echo "smoke test failed; logs are in ${WORK_DIR}" >&2
    for log in "${BOOTSTRAP_LOG}" "${ALICE_LOG}" "${BOB_LOG}"; do
      if [[ -f "${log}" ]]; then
        echo "===== ${log} =====" >&2
        tail -n 80 "${log}" >&2 || true
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

mkfifo "${BOOTSTRAP_STDIN}" "${ALICE_STDIN}" "${BOB_STDIN}"
exec {BOOTSTRAP_FD}<>"${BOOTSTRAP_STDIN}"
exec {ALICE_FD}<>"${ALICE_STDIN}"
exec {BOB_FD}<>"${BOB_STDIN}"
fds+=("${BOOTSTRAP_FD}" "${ALICE_FD}" "${BOB_FD}")

(
  cd "${WORK_DIR}"
  "${DEMO_CHAT_BIN}" --id bootstrap --bootstrap --listen "${BOOTSTRAP_ADDR}" <"${BOOTSTRAP_STDIN}"
) >"${BOOTSTRAP_LOG}" 2>&1 &
pids+=("$!")

BOOTSTRAP_PEER_FILE="${WORK_DIR}/.demo-chat/state/bootstrap/bootstrap-peer.txt"
wait_for_file "${BOOTSTRAP_PEER_FILE}"
BOOTSTRAP_PEER="$(cat "${BOOTSTRAP_PEER_FILE}")"
BOOTSTRAP_PEER_ID="${BOOTSTRAP_PEER%%@*}"

(
  cd "${WORK_DIR}"
  "${DEMO_CHAT_BIN}" --id bob --nick Bob --bootstrap-peer "${BOOTSTRAP_PEER}" <"${BOB_STDIN}"
) >"${BOB_LOG}" 2>&1 &
pids+=("$!")

(
  cd "${WORK_DIR}"
  "${DEMO_CHAT_BIN}" --id alice --nick Alice --bootstrap-peer "${BOOTSTRAP_PEER}" <"${ALICE_STDIN}"
) >"${ALICE_LOG}" 2>&1 &
pids+=("$!")

wait_for_log "${BOB_LOG}" "Demo Chat: ready"
wait_for_log "${ALICE_LOG}" "Demo Chat: ready"
wait_for_log "${BOB_LOG}" "Demo Chat: connected to ${BOOTSTRAP_PEER_ID}"
wait_for_log "${ALICE_LOG}" "Demo Chat: connected to ${BOOTSTRAP_PEER_ID}"

# Give GossipSub and service discovery a short window to connect peers before
# publishing. Send a few times to avoid a transient mesh-formation race.
sleep 2
for _ in 1 2 3; do
  assert_processes_alive
  printf '%s\n' "${MESSAGE}" >&"${ALICE_FD}"
  sleep 1
  if grep -Fq "${MESSAGE}" "${BOB_LOG}"; then
    echo "smoke test passed: Bob received '${MESSAGE}'"
    exit 0
  fi
done

wait_for_log "${BOB_LOG}" "${MESSAGE}"
echo "smoke test passed: Bob received '${MESSAGE}'"
