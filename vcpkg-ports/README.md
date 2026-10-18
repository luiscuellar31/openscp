# vcpkg overlay ports

`vcpkg.json` makes vcpkg take the ports in this folder before its own.

`openssl` is vcpkg's port from the `builtin-baseline` commit, unchanged except
for `version` in `openssl/vcpkg.json` and the source archive's `SHA512` in
`openssl/portfile.cmake`. It keeps Windows builds on OpenSSL's 3.5
long-term support series, at the release that the macOS release builds use
(`OPENSSL_VERSION` in `scripts/ci/build_macos_release_deps.sh`), while vcpkg
itself has moved on to a series with shorter support.

`scripts/release/update-pinned-deps.py` moves both pins to the newest 3.5
release once the new sources pass the checks that its description lists. The
**Pinned Dependency Updates** workflow runs it every Monday and opens a pull
request against `dev` when the dependencies then build on macOS and Windows as
release builds do. To move to another series, change `OPENSSL_SERIES` in the
script.

When a new baseline changes vcpkg's own port in more than its version and
`SHA512`, the Windows CI job warns about each file that differs
(`scripts/ci/check_vcpkg_overlay.ps1`). Copy the port again from the new
baseline and run the script, which sets `version` and `SHA512` back to the
newest 3.5 release. Then build on Windows, where vcpkg applies the port's
patches to those sources.
