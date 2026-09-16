#!/usr/bin/env bash

set -euo pipefail

proxy_type="${1:?Usage: $0 <socks5|http> <ssh-key> <ssh-user> [test-binary]}"
ssh_key="${2:?Missing SSH key path}"
ssh_user="${3:?Missing SSH user}"
test_binary="${4:-./build/tests/openscp_sftp_integration_tests}"
proxy_host="127.0.0.1"
proxy_pid=""

cleanup() {
  if [[ -n "$proxy_pid" ]]; then
    kill "$proxy_pid" 2>/dev/null || true
    wait "$proxy_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

wait_for_port() {
  local port="$1"
  local attempts="$2"
  local delay="$3"
  local attempt
  for ((attempt = 1; attempt <= attempts; ++attempt)); do
    if nc -z "$proxy_host" "$port" >/dev/null 2>&1; then
      return 0
    fi
    sleep "$delay"
  done
  return 1
}

case "$proxy_type" in
  socks5)
    proxy_port=1081
    ssh \
      -i "$ssh_key" \
      -o BatchMode=yes \
      -o IdentitiesOnly=yes \
      -o ExitOnForwardFailure=yes \
      -o StrictHostKeyChecking=no \
      -o UserKnownHostsFile=/dev/null \
      -N -D "${proxy_host}:${proxy_port}" \
      -p 2222 "${ssh_user}@127.0.0.1" &
    proxy_pid=$!
    wait_for_port "$proxy_port" 20 0.5 || {
      printf 'SOCKS5 proxy tunnel did not start\n' >&2
      exit 1
    }
    OPENSCP_IT_SFTP_USER="$ssh_user" \
    OPENSCP_IT_PROXY_TYPE=socks5 \
    OPENSCP_IT_PROXY_HOST="$proxy_host" \
    OPENSCP_IT_PROXY_PORT="$proxy_port" \
      "$test_binary"
    ;;
  http)
    proxy_port=18081
    proxy_user=openscp_proxy
    proxy_pass=openscp_proxy_pass
    IT_HTTP_PROXY_PORT="$proxy_port" \
    IT_HTTP_PROXY_USER="$proxy_user" \
    IT_HTTP_PROXY_PASS="$proxy_pass" \
      python3 scripts/ci/http_connect_proxy.py &
    proxy_pid=$!
    wait_for_port "$proxy_port" 40 0.25 || {
      printf 'HTTP CONNECT proxy did not start\n' >&2
      exit 1
    }
    OPENSCP_IT_SFTP_USER="$ssh_user" \
    OPENSCP_IT_PROXY_TYPE=http \
    OPENSCP_IT_PROXY_HOST="$proxy_host" \
    OPENSCP_IT_PROXY_PORT="$proxy_port" \
    OPENSCP_IT_PROXY_USER="$proxy_user" \
    OPENSCP_IT_PROXY_PASS="$proxy_pass" \
      "$test_binary"
    ;;
  *)
    printf 'Unsupported proxy type: %s\n' "$proxy_type" >&2
    exit 2
    ;;
esac
