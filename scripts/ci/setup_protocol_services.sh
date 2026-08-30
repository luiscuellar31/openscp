#!/usr/bin/env bash

set -euo pipefail

log() {
  printf '[protocol-it] %s\n' "$*"
}

die() {
  printf '[protocol-it] error: %s\n' "$*" >&2
  exit 1
}

require_command() {
  command -v "$1" >/dev/null 2>&1 ||
    die "missing required command: $1"
}

wait_for_port() {
  local label="$1"
  local port="$2"
  local attempt
  for ((attempt = 1; attempt <= 40; ++attempt)); do
    if nc -z 127.0.0.1 "$port" >/dev/null 2>&1; then
      log "${label} is listening on 127.0.0.1:${port}"
      return 0
    fi
    sleep 0.25
  done
  die "${label} did not start on 127.0.0.1:${port}"
}

write_vsftpd_config() {
  local output_path="$1"
  local listen_port="$2"
  local local_root="$3"
  local passive_min="$4"
  local passive_max="$5"
  local log_path="$6"
  local tls_mode="$7"

  {
    printf '%s\n' \
      'listen=YES' \
      'listen_ipv6=NO' \
      'listen_address=127.0.0.1' \
      "listen_port=${listen_port}" \
      'background=YES' \
      'anonymous_enable=NO' \
      'local_enable=YES' \
      'write_enable=YES' \
      'local_umask=022' \
      'chroot_local_user=YES' \
      "local_root=${local_root}" \
      'pam_service_name=vsftpd' \
      'pasv_enable=YES' \
      'pasv_address=127.0.0.1' \
      "pasv_min_port=${passive_min}" \
      "pasv_max_port=${passive_max}" \
      'port_enable=NO' \
      'connect_from_port_20=NO' \
      'use_localtime=NO' \
      'xferlog_enable=YES' \
      'xferlog_std_format=NO' \
      'log_ftp_protocol=YES' \
      "vsftpd_log_file=${log_path}" \
      'ftpd_banner=OpenSCP protocol integration server'

    case "$tls_mode" in
      plain)
        printf '%s\n' 'ssl_enable=NO'
        ;;
      explicit)
        printf '%s\n' \
          'ssl_enable=YES' \
          'implicit_ssl=NO'
        ;;
      implicit)
        printf '%s\n' \
          'ssl_enable=YES' \
          'implicit_ssl=YES'
        ;;
      *)
        die "unknown vsftpd TLS mode: ${tls_mode}"
        ;;
    esac

    if [[ "$tls_mode" != "plain" ]]; then
      printf '%s\n' \
        "rsa_cert_file=${SERVER_CHAIN_CERT}" \
        "rsa_private_key_file=${SERVER_KEY}" \
        'force_local_logins_ssl=YES' \
        'force_local_data_ssl=YES' \
        'ssl_tlsv1=YES' \
        'ssl_sslv2=NO' \
        'ssl_sslv3=NO' \
        'require_ssl_reuse=NO'
    fi
  } >"$output_path"
}

launch_vsftpd() {
  local label="$1"
  local config_path="$2"
  local launch_error

  log "launching ${label}"
  if ! launch_error="$(sudo /usr/sbin/vsftpd "$config_path" 2>&1)"; then
    launch_error="${launch_error//$'\n'/; }"
    die "${label} process failed to launch: ${launch_error}"
  fi
}

dump_diagnostics() {
  local status="$1"
  if [[ $status -eq 0 ]]; then
    return
  fi
  printf '\n[protocol-it] service diagnostics\n' >&2
  if [[ -d "$LOG_DIR" ]]; then
    local log_file
    while IFS= read -r log_file; do
      printf '\n--- %s ---\n' "$log_file" >&2
      {
        sudo tail -n 100 "$log_file" || true
      } >&2
    done < <(find "$LOG_DIR" -maxdepth 1 -type f | sort)
  fi
  exit "$status"
}

[[ "$(uname -s)" == "Linux" ]] ||
  die "real protocol services are supported only on Linux"

for command_name in \
  apache2 a2enmod curl htpasswd nc openssl sudo vsftpd; do
  require_command "$command_name"
done

STATE_DIR="${OPENSCP_PROTOCOL_IT_STATE_DIR:-/tmp/openscp-protocol-it}"
CONFIG_DIR="${STATE_DIR}/config"
CERT_DIR="${STATE_DIR}/certs"
DATA_DIR="${STATE_DIR}/data"
LOG_DIR="${STATE_DIR}/logs"
APACHE_RUNTIME_DIR="${STATE_DIR}/apache-runtime"
APACHE_DOCUMENT_ROOT="${STATE_DIR}/apache-document-root"

FTP_PORT="${OPENSCP_IT_FTP_PORT:-2121}"
FTPS_EXPLICIT_PORT="${OPENSCP_IT_FTPS_EXPLICIT_PORT:-${OPENSCP_IT_FTPS_PORT:-2122}}"
FTPS_IMPLICIT_PORT="${OPENSCP_IT_FTPS_IMPLICIT_PORT:-2990}"
WEBDAV_PORT="${OPENSCP_IT_WEBDAV_PORT:-18443}"

FTP_PASSIVE_MIN=31100
FTP_PASSIVE_MAX=31119
FTPS_EXPLICIT_PASSIVE_MIN=31200
FTPS_EXPLICIT_PASSIVE_MAX=31219
FTPS_IMPLICIT_PASSIVE_MIN=31300
FTPS_IMPLICIT_PASSIVE_MAX=31319

IT_USER="${OPENSCP_PROTOCOL_IT_USER:-openscp_protocol_it}"
IT_PASSWORD="${OPENSCP_PROTOCOL_IT_PASSWORD:-openscp-protocol-password}"
WEBDAV_BASE_PATH="/openscp-dav"

CA_KEY="${CERT_DIR}/ca.key"
CA_CERT="${CERT_DIR}/ca.crt"
SERVER_KEY="${CERT_DIR}/server.key"
SERVER_CSR="${CERT_DIR}/server.csr"
SERVER_CERT="${CERT_DIR}/server.crt"
SERVER_CHAIN_CERT="${CERT_DIR}/server-chain.crt"

trap 'dump_diagnostics "$?"' EXIT

log "preparing isolated service state in ${STATE_DIR}"
install -d -m 0755 \
  "$STATE_DIR" "$CONFIG_DIR" "$CERT_DIR" "$DATA_DIR" "$LOG_DIR"
sudo install -d -o www-data -g www-data -m 0750 "$APACHE_RUNTIME_DIR"
sudo install -d -o root -g root -m 0555 "$APACHE_DOCUMENT_ROOT"

if ! id -u "$IT_USER" >/dev/null 2>&1; then
  sudo useradd --create-home --shell /bin/bash "$IT_USER"
fi
printf '%s:%s\n' "$IT_USER" "$IT_PASSWORD" | sudo chpasswd

for service_root in ftp ftps-explicit ftps-implicit; do
  sudo install -d -o root -g root -m 0755 "${DATA_DIR}/${service_root}"
  sudo install -d -o "$IT_USER" -g "$IT_USER" -m 0750 \
    "${DATA_DIR}/${service_root}/workspace"
done

sudo install -d -o www-data -g www-data -m 0750 \
  "${DATA_DIR}/webdav" "${DATA_DIR}/webdav/workspace"

log "generating a temporary CA and localhost server certificate"
openssl req -x509 -newkey rsa:2048 -sha256 -nodes \
  -keyout "$CA_KEY" \
  -out "$CA_CERT" \
  -days 2 \
  -subj '/CN=OpenSCP Protocol Integration CA' \
  -addext 'basicConstraints=critical,CA:TRUE' \
  -addext 'keyUsage=critical,keyCertSign,cRLSign' >/dev/null 2>&1

openssl req -new -newkey rsa:2048 -sha256 -nodes \
  -keyout "$SERVER_KEY" \
  -out "$SERVER_CSR" \
  -subj '/CN=localhost' \
  -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' >/dev/null 2>&1

{
  printf '%s\n' \
    'basicConstraints=critical,CA:FALSE' \
    'keyUsage=critical,digitalSignature,keyEncipherment' \
    'extendedKeyUsage=serverAuth' \
    'subjectAltName=DNS:localhost,IP:127.0.0.1'
} >"${CERT_DIR}/server.ext"

openssl x509 -req -sha256 \
  -in "$SERVER_CSR" \
  -CA "$CA_CERT" \
  -CAkey "$CA_KEY" \
  -CAcreateserial \
  -out "$SERVER_CERT" \
  -days 2 \
  -extfile "${CERT_DIR}/server.ext" >/dev/null 2>&1

{
  command cat "$SERVER_CERT" "$CA_CERT"
} >"$SERVER_CHAIN_CERT"
chmod 0600 "$CA_KEY" "$SERVER_KEY"
chmod 0644 "$CA_CERT" "$SERVER_CERT" "$SERVER_CHAIN_CERT"

FTP_CONFIG="${CONFIG_DIR}/vsftpd-ftp.conf"
FTPS_EXPLICIT_CONFIG="${CONFIG_DIR}/vsftpd-ftps-explicit.conf"
FTPS_IMPLICIT_CONFIG="${CONFIG_DIR}/vsftpd-ftps-implicit.conf"

write_vsftpd_config \
  "$FTP_CONFIG" "$FTP_PORT" "${DATA_DIR}/ftp" \
  "$FTP_PASSIVE_MIN" "$FTP_PASSIVE_MAX" "${LOG_DIR}/ftp.log" plain
write_vsftpd_config \
  "$FTPS_EXPLICIT_CONFIG" "$FTPS_EXPLICIT_PORT" \
  "${DATA_DIR}/ftps-explicit" \
  "$FTPS_EXPLICIT_PASSIVE_MIN" "$FTPS_EXPLICIT_PASSIVE_MAX" \
  "${LOG_DIR}/ftps-explicit.log" explicit
write_vsftpd_config \
  "$FTPS_IMPLICIT_CONFIG" "$FTPS_IMPLICIT_PORT" \
  "${DATA_DIR}/ftps-implicit" \
  "$FTPS_IMPLICIT_PASSIVE_MIN" "$FTPS_IMPLICIT_PASSIVE_MAX" \
  "${LOG_DIR}/ftps-implicit.log" implicit

log "starting FTP, explicit FTPS, and implicit FTPS"
launch_vsftpd "FTP" "$FTP_CONFIG"
launch_vsftpd "explicit FTPS" "$FTPS_EXPLICIT_CONFIG"
launch_vsftpd "implicit FTPS" "$FTPS_IMPLICIT_CONFIG"

wait_for_port "FTP" "$FTP_PORT"
wait_for_port "explicit FTPS" "$FTPS_EXPLICIT_PORT"
wait_for_port "implicit FTPS" "$FTPS_IMPLICIT_PORT"

log "configuring Apache WebDAV at ${WEBDAV_BASE_PATH}"
sudo a2enmod auth_basic authn_file dav dav_fs ssl >/dev/null

WEBDAV_PASSWORD_FILE="${CONFIG_DIR}/webdav.htpasswd"
printf '%s\n' "$IT_PASSWORD" |
  sudo htpasswd -ci "$WEBDAV_PASSWORD_FILE" "$IT_USER" >/dev/null
sudo chown root:www-data "$WEBDAV_PASSWORD_FILE"
sudo chmod 0640 "$WEBDAV_PASSWORD_FILE"

APACHE_CONFIG="${CONFIG_DIR}/apache2-webdav.conf"
{
  printf '%s\n' \
    'ServerRoot "/etc/apache2"' \
    "Define APACHE_RUN_DIR \"${APACHE_RUNTIME_DIR}\"" \
    "Define APACHE_LOCK_DIR \"${APACHE_RUNTIME_DIR}\"" \
    "Define APACHE_LOG_DIR \"${LOG_DIR}\"" \
    "DefaultRuntimeDir \"${APACHE_RUNTIME_DIR}\"" \
    "PidFile \"${APACHE_RUNTIME_DIR}/apache2.pid\"" \
    "Listen 127.0.0.1:${WEBDAV_PORT}" \
    'ServerName localhost' \
    'IncludeOptional /etc/apache2/mods-enabled/*.load' \
    'IncludeOptional /etc/apache2/mods-enabled/*.conf' \
    'User www-data' \
    'Group www-data' \
    "DocumentRoot \"${APACHE_DOCUMENT_ROOT}\"" \
    "ErrorLog \"${LOG_DIR}/apache-error.log\"" \
    'LogLevel info' \
    'LogFormat "%h %l %u %t \"%r\" %>s %b" protocol_it' \
    "CustomLog \"${LOG_DIR}/apache-access.log\" protocol_it" \
    "DavLockDB \"${APACHE_RUNTIME_DIR}/DavLock\"" \
    '<Directory />' \
    '    AllowOverride None' \
    '    Require all denied' \
    '</Directory>' \
    "<VirtualHost 127.0.0.1:${WEBDAV_PORT}>" \
    '    SSLEngine On' \
    "    SSLCertificateFile \"${SERVER_CERT}\"" \
    "    SSLCertificateKeyFile \"${SERVER_KEY}\"" \
    "    Alias \"${WEBDAV_BASE_PATH}\" \"${DATA_DIR}/webdav\"" \
    "    <Directory \"${DATA_DIR}/webdav\">" \
    '        DAV On' \
    '        DirectorySlash Off' \
    '        Options None' \
    '        AllowOverride None' \
    '        AuthType Basic' \
    '        AuthName "OpenSCP protocol integration"' \
    '        AuthBasicProvider file' \
    "        AuthUserFile \"${WEBDAV_PASSWORD_FILE}\"" \
    '        Require valid-user' \
    '    </Directory>' \
    '</VirtualHost>'
} >"$APACHE_CONFIG"

log "validating Apache WebDAV configuration"
sudo /usr/sbin/apache2 -t -f "$APACHE_CONFIG" ||
  die "Apache WebDAV configuration validation failed"
log "launching Apache WebDAV"
sudo /usr/sbin/apache2 -f "$APACHE_CONFIG" -k start ||
  die "Apache WebDAV process failed to launch"
wait_for_port "HTTPS WebDAV" "$WEBDAV_PORT"

log "running service-level smoke checks"
log "smoke check: FTP"
curl --fail --silent --show-error \
  --user "${IT_USER}:${IT_PASSWORD}" \
  --list-only "ftp://127.0.0.1:${FTP_PORT}/workspace/" >/dev/null

log "smoke check: explicit FTPS"
curl --fail --silent --show-error \
  --ssl-reqd \
  --cacert "$CA_CERT" \
  --user "${IT_USER}:${IT_PASSWORD}" \
  --list-only "ftp://127.0.0.1:${FTPS_EXPLICIT_PORT}/workspace/" >/dev/null

log "smoke check: implicit FTPS"
curl --fail --silent --show-error \
  --cacert "$CA_CERT" \
  --user "${IT_USER}:${IT_PASSWORD}" \
  --list-only "ftps://127.0.0.1:${FTPS_IMPLICIT_PORT}/workspace/" >/dev/null

log "smoke check: HTTPS WebDAV"
curl --fail --silent --show-error \
  --cacert "$CA_CERT" \
  --user "${IT_USER}:${IT_PASSWORD}" \
  --request PROPFIND \
  --header 'Depth: 1' \
  "https://127.0.0.1:${WEBDAV_PORT}${WEBDAV_BASE_PATH}/workspace" \
  >/dev/null

log "all protocol services passed their smoke checks"
trap - EXIT
