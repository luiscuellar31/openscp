# vcpkg overlay ports

`vcpkg.json` makes vcpkg take the ports in this folder before its own.

`openssl` is vcpkg's port from the `builtin-baseline` commit, unchanged except
for `version` in `openssl/vcpkg.json` and the source archive's `SHA512` in
`openssl/portfile.cmake`. It keeps Windows builds on OpenSSL's 3.5
long-term support series, at the release that the macOS release builds use
(`OPENSSL_VERSION` in `scripts/ci/build_macos_release_deps.sh`), while vcpkg
itself has moved on to a series with shorter support.

To move to another 3.5 release:

1. Check the release tarball against its published SHA-256 and update
   `OPENSSL_VERSION` and `OPENSSL_SHA256` in the macOS script.
2. Set `version` in `openssl/vcpkg.json`, and in `openssl/portfile.cmake` the
   SHA-512 of `https://github.com/openssl/openssl/archive/openssl-<version>.tar.gz`,
   the tag archive that the port downloads. Its files should be identical to
   those of the release tarball.
3. Build on Windows, where vcpkg applies the port's patches to the new sources.

When vcpkg changes its own port, copy it again from the new baseline and redo
step 2.
