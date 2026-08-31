#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../lib/version.sh"

# Build the non-Qt libraries shipped inside official macOS artifacts. Homebrew
# bottles target the runner that produced them, so they cannot be used when the
# application advertises compatibility with an older macOS release.

OPENSSL_VERSION="3.5.8"
OPENSSL_SHA256="a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2"
LIBSSH2_VERSION="1.11.1"
LIBSSH2_SHA256="d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7"
TINYXML2_VERSION="11.0.0"
TINYXML2_SHA256="5556deb5081fb246ee92afae73efd943c889cef0cafea92b0b82422d6a18f289"

PREFIX="${1:-${OPENSCP_DEPS_PREFIX:-}}"
ARCH="${CMAKE_OSX_ARCHITECTURES:-$(uname -m)}"
DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-12.0}"

log() { printf '\033[1;34m[mac-deps]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[mac-deps]\033[0m %s\n' "$*" >&2; exit 1; }

[[ "$(uname -s)" == "Darwin" ]] || die "This script must run on macOS"
[[ -n "$PREFIX" ]] || die "Usage: $0 <absolute-install-prefix>"
[[ "$PREFIX" == /* && "$PREFIX" != "/" ]] ||
  die "The install prefix must be an absolute, non-root path"
case "$ARCH" in
  arm64|x86_64) ;;
  *) die "Official dependency builds require one architecture: arm64 or x86_64" ;;
esac
[[ "$DEPLOYMENT_TARGET" =~ ^[0-9]+\.[0-9]+$ ]] ||
  die "Invalid MACOSX_DEPLOYMENT_TARGET: $DEPLOYMENT_TARGET"

for command_name in cmake curl lipo make otool shasum tar; do
  command -v "$command_name" >/dev/null 2>&1 ||
    die "Missing required tool: $command_name"
done

work_dir="$(mktemp -d)"
cleanup() { rm -rf "$work_dir"; }
trap cleanup EXIT

jobs="$(sysctl -n hw.logicalcpu 2>/dev/null || printf '2')"
common_cflags="-mmacosx-version-min=${DEPLOYMENT_TARGET}"

download_verified() {
  local url="$1"
  local expected_sha="$2"
  local output="$3"
  curl --fail --location --proto '=https' --tlsv1.2 \
    --retry 3 --retry-all-errors "$url" --output "$output"
  printf '%s  %s\n' "$expected_sha" "$output" | shasum -a 256 --check
}

mkdir -p "$PREFIX"

log "Downloading and verifying pinned source archives"
download_verified \
  "https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz" \
  "$OPENSSL_SHA256" "$work_dir/openssl.tar.gz"
download_verified \
  "https://libssh2.org/download/libssh2-${LIBSSH2_VERSION}.tar.gz" \
  "$LIBSSH2_SHA256" "$work_dir/libssh2.tar.gz"
download_verified \
  "https://github.com/leethomason/tinyxml2/archive/refs/tags/${TINYXML2_VERSION}.tar.gz" \
  "$TINYXML2_SHA256" "$work_dir/tinyxml2.tar.gz"

log "Building OpenSSL ${OPENSSL_VERSION} for macOS ${DEPLOYMENT_TARGET} (${ARCH})"
tar -xzf "$work_dir/openssl.tar.gz" -C "$work_dir"
openssl_target="darwin64-${ARCH}-cc"
(
  cd "$work_dir/openssl-${OPENSSL_VERSION}"
  env MACOSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
    CFLAGS="$common_cflags" \
    CXXFLAGS="$common_cflags" \
    LDFLAGS="$common_cflags" \
    ./Configure "$openssl_target" \
      --prefix="$PREFIX" \
      --openssldir="$PREFIX/ssl" \
      shared no-apps no-tests
  make -j"$jobs"
  make install_sw
)

log "Building tinyxml2 ${TINYXML2_VERSION}"
tar -xzf "$work_dir/tinyxml2.tar.gz" -C "$work_dir"
cmake -S "$work_dir/tinyxml2-${TINYXML2_VERSION}" \
  -B "$work_dir/tinyxml2-build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
  -DBUILD_SHARED_LIBS=ON \
  -DBUILD_STATIC_LIBS=OFF \
  -Dtinyxml2_BUILD_TESTING=OFF
cmake --build "$work_dir/tinyxml2-build" --parallel "$jobs"
cmake --install "$work_dir/tinyxml2-build"

log "Building libssh2 ${LIBSSH2_VERSION} with the pinned OpenSSL"
tar -xzf "$work_dir/libssh2.tar.gz" -C "$work_dir"
cmake -S "$work_dir/libssh2-${LIBSSH2_VERSION}" \
  -B "$work_dir/libssh2-build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_PREFIX_PATH="$PREFIX" \
  -DOPENSSL_ROOT_DIR="$PREFIX" \
  -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
  -DBUILD_SHARED_LIBS=ON \
  -DBUILD_EXAMPLES=OFF \
  -DBUILD_TESTING=OFF \
  -DCRYPTO_BACKEND=OpenSSL \
  -DENABLE_ZLIB_COMPRESSION=OFF
cmake --build "$work_dir/libssh2-build" --parallel "$jobs"
cmake --install "$work_dir/libssh2-build"


log "Verifying architecture and deployment target of installed libraries"
validated_count=0
while IFS= read -r -d '' dylib; do
  dylib_archs="$(lipo -archs "$dylib")"
  [[ "$dylib_archs" == "$ARCH" ]] ||
    die "Unexpected architecture in $dylib: $dylib_archs (expected $ARCH)"
  versions="$(otool -l "$dylib" | awk '
    $1 == "cmd" {
      in_build_version = ($2 == "LC_BUILD_VERSION")
      in_legacy_version = ($2 == "LC_VERSION_MIN_MACOSX")
      next
    }
    in_build_version && $1 == "minos" { print $2; in_build_version = 0 }
    in_legacy_version && $1 == "version" { print $2; in_legacy_version = 0 }
  ')"
  [[ -n "$versions" ]] || die "No deployment target found in $dylib"
  while IFS= read -r version; do
    if version_is_greater "$version" "$DEPLOYMENT_TARGET"; then
      die "$dylib requires macOS $version (allowed: $DEPLOYMENT_TARGET)"
    fi
  done <<< "$versions"
  validated_count=$((validated_count + 1))
done < <(find "$PREFIX/lib" -maxdepth 1 -type f -name '*.dylib' -print0)
[[ $validated_count -ge 4 ]] ||
  die "Expected at least four installed dylibs, found $validated_count"

cat > "$PREFIX/openscp-dependencies.txt" <<EOF
macOS deployment target: ${DEPLOYMENT_TARGET}
architecture: ${ARCH}
OpenSSL: ${OPENSSL_VERSION} sha256:${OPENSSL_SHA256}
libssh2: ${LIBSSH2_VERSION} sha256:${LIBSSH2_SHA256}
tinyxml2: ${TINYXML2_VERSION} sha256:${TINYXML2_SHA256}
EOF

log "Dependencies installed under $PREFIX"
