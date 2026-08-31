# Platform Compatibility Policy

This document defines the compatibility contract for OpenSCP 1.0.0. A build
made by a third party may have a different baseline; the rows below describe
the official project artifacts and the continuously tested source build.

| Target | 1.0.0 baseline | How it is enforced |
| --- | --- | --- |
| macOS `.app` / `.dmg` / `.pkg` (arm64 and x86_64) | macOS 12.0+ | Qt 6.8.3 and pinned non-Qt dependencies are built for 12.0; every bundled Mach-O is checked before upload. |
| Linux AppImage x86_64 | glibc 2.28+ | Built in a digest-pinned Rocky Linux 8 container; all bundled ELF objects are audited for `GLIBC_*` and `GLIBCXX_*`. |
| Linux AppImage aarch64 | glibc 2.39+ (Ubuntu 24.04 baseline) | Built on the Ubuntu 24.04 arm64 runner and audited before upload. |
| Native Linux source build | Ubuntu 22.04 toolchain and newer | CMake configure, hardening, build, and unit tests run on Ubuntu 22.04 in CI. Other distributions are community-supported. |
| Snap | `core24` plus the declared content runtime | Runtime-managed; it does not inherit the host's Qt or glibc in the same way as a native build. |
| Flatpak | Manifest's KDE runtime branch | Runtime-managed and independent of most host libraries. |
| Windows | Not supported in 1.0.0 | CMake rejects Windows unless a port developer explicitly enables the experimental scaffold. |

Official self-contained releases use Qt 6.8.3. Community builds intentionally
remain free to compile against another compatible Qt 6; set
`OPENSCP_ENFORCE_RECOMMENDED_QT_VERSION=ON` only when reproducing the official
artifact policy.

Compatibility with an old operating system does not mean that the operating
system itself still receives security updates. Because OpenSCP handles remote
credentials, unsupported host systems are best-effort targets and should not
be treated as a substitute for a maintained OS.

## Release gates

- `scripts/verify_macos_bundle.sh` rejects a Mach-O deployment target above the
  version advertised by the app and rejects unresolved or machine-local paths.
- `scripts/check_linux_abi.sh` rejects an ELF requirement above the configured
  glibc ceiling and checks that bundled `libstdc++` satisfies all `GLIBCXX`
  requirements.
- `OPENSCP_FORTIFY_SOURCE_LEVEL=AUTO` selects level 3 when the libc and compiler
  fully support it and safely falls back to level 2 otherwise.
- Release workflows enforce Qt 6.8.3 for the self-contained macOS and AppImage
  artifacts.

These baselines may be raised in a future major or minor release, but must not
change silently within an already published release line.
