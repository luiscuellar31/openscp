#!/usr/bin/env bash

set -euo pipefail

log() {
  printf '[protocol-it] %s\n' "$*"
}

die() {
  printf '[protocol-it] error: %s\n' "$*" >&2
  exit 1
}

BUILD_DIR="${1:-build}"
STATE_DIR="${OPENSCP_PROTOCOL_IT_STATE_DIR:-/tmp/openscp-protocol-it}"
CA_CERT="${STATE_DIR}/certs/ca.crt"

FTP_PORT="${OPENSCP_IT_FTP_PORT:-2121}"
FTPS_EXPLICIT_PORT="${OPENSCP_IT_FTPS_EXPLICIT_PORT:-${OPENSCP_IT_FTPS_PORT:-2122}}"
FTPS_IMPLICIT_PORT="${OPENSCP_IT_FTPS_IMPLICIT_PORT:-2990}"
WEBDAV_PORT="${OPENSCP_IT_WEBDAV_PORT:-18443}"

IT_USER="${OPENSCP_PROTOCOL_IT_USER:-openscp_protocol_it}"
IT_PASSWORD="${OPENSCP_PROTOCOL_IT_PASSWORD:-openscp-protocol-password}"

FTP_TEST="${BUILD_DIR}/tests/openscp_ftp_integration_tests"
FTPS_TEST="${BUILD_DIR}/tests/openscp_ftps_integration_tests"
WEBDAV_TEST="${BUILD_DIR}/tests/openscp_webdav_integration_tests"

for test_binary in "$FTP_TEST" "$FTPS_TEST" "$WEBDAV_TEST"; do
  [[ -x "$test_binary" ]] ||
    die "required integration test binary is missing: ${test_binary}"
done
[[ -r "$CA_CERT" ]] ||
  die "temporary integration CA is missing: ${CA_CERT}"

log "running FTP integration directly"
env \
  OPENSCP_IT_FTP_HOST=127.0.0.1 \
  OPENSCP_IT_FTP_PORT="$FTP_PORT" \
  OPENSCP_IT_FTP_USER="$IT_USER" \
  OPENSCP_IT_FTP_PASS="$IT_PASSWORD" \
  OPENSCP_IT_FTP_REMOTE_BASE=/workspace \
  "$FTP_TEST"

log "running explicit FTPS integration directly"
env \
  OPENSCP_IT_FTPS_HOST=127.0.0.1 \
  OPENSCP_IT_FTPS_PORT="$FTPS_EXPLICIT_PORT" \
  OPENSCP_IT_FTPS_USER="$IT_USER" \
  OPENSCP_IT_FTPS_PASS="$IT_PASSWORD" \
  OPENSCP_IT_FTPS_REMOTE_BASE=/workspace \
  OPENSCP_IT_FTPS_MODE=explicit \
  OPENSCP_IT_FTPS_VERIFY_PEER=1 \
  OPENSCP_IT_FTPS_CA_CERT="$CA_CERT" \
  "$FTPS_TEST"

log "running implicit FTPS integration directly"
env \
  OPENSCP_IT_FTPS_HOST=127.0.0.1 \
  OPENSCP_IT_FTPS_PORT="$FTPS_IMPLICIT_PORT" \
  OPENSCP_IT_FTPS_USER="$IT_USER" \
  OPENSCP_IT_FTPS_PASS="$IT_PASSWORD" \
  OPENSCP_IT_FTPS_REMOTE_BASE=/workspace \
  OPENSCP_IT_FTPS_MODE=implicit \
  OPENSCP_IT_FTPS_VERIFY_PEER=1 \
  OPENSCP_IT_FTPS_CA_CERT="$CA_CERT" \
  "$FTPS_TEST"

log "running HTTPS WebDAV integration directly"
env \
  OPENSCP_IT_WEBDAV_HOST=127.0.0.1 \
  OPENSCP_IT_WEBDAV_PORT="$WEBDAV_PORT" \
  OPENSCP_IT_WEBDAV_USER="$IT_USER" \
  OPENSCP_IT_WEBDAV_PASS="$IT_PASSWORD" \
  OPENSCP_IT_WEBDAV_REMOTE_BASE=/workspace \
  OPENSCP_IT_WEBDAV_SCHEME=https \
  OPENSCP_IT_WEBDAV_BASE_PATH=/openscp-dav \
  OPENSCP_IT_WEBDAV_VERIFY_PEER=1 \
  OPENSCP_IT_WEBDAV_CA_CERT="$CA_CERT" \
  "$WEBDAV_TEST"

log "all direct protocol integration tests passed"
