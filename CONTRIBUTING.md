# Contributing to OpenSCP

OpenSCP welcomes focused bug fixes, documentation improvements, tests, and new
features. Public pull requests target `dev`; the maintainer promotes tested
work to `main` and creates release tags.

Never report a suspected vulnerability in a public issue or pull request. Use
the private process in [SECURITY.md](SECURITY.md).

## Workflow

Fork the repository, branch from the latest `dev`, and keep each pull request
focused on one change:

```bash
git clone https://github.com/<your-user>/openscp.git
cd openscp
git remote add upstream https://github.com/luiscuellar31/openscp.git
git fetch upstream
git switch -c feature/short-name upstream/dev
```

Use descriptive commits, preferably following Conventional Commits. Before
updating an existing pull request, rebase on `upstream/dev` and use
`git push --force-with-lease` if the rebased branch was already published.

## Build and test

The normal development entrypoints are:

```bash
./scripts/linux.sh dev
./scripts/macos.sh dev
```

Run the local CI baseline before opening a pull request:

```bash
./scripts/check_ci_local.sh --clean --full --werror
./scripts/checks/cpp-quality.sh --format
./scripts/checks/shell-quality.sh
git diff --check
```

Static analysis is available after configuring a build with a compilation
database:

```bash
./scripts/checks/cpp-quality.sh --tidy --cppcheck \
  --build-dir build-ci-local
```

Platform-specific changes must also be exercised on that platform. Complete
dependency and packaging instructions are in [docs/BUILDING.md](docs/BUILDING.md).

## Engineering rules

- Use C++20, English identifiers, and the repository's `.clang-format` file.
- Prefer clear ownership and RAII. Use `UniqueFile` for `FILE*` and
  `SecureString` for secrets.
- Keep functions focused; substantially changed functions should normally stay
  below the configured 120-line `clang-tidy` threshold.
- Keep protocol/network code out of models and widgets. The dependency
  direction is described in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
- Do not call remote clients, presentation callbacks, or emit Qt signals while
  holding queue/state mutexes.
- Preserve persisted formats unless the change includes an explicit migration.
- Never persist passwords, passphrases, or proxy credentials in plain settings
  or transfer-queue data.
- Add tests for success, failure, cancellation, malformed input, and concurrent
  behavior when those paths are relevant.
- Tests should link the production target that owns the behavior; do not compile
  private copies of production `.cpp` files.

Do not globally suppress a warning to accommodate new code. A narrow
suppression needs a documented compatibility reason or false positive.

## Translations

English strings live in the C++ sources. Spanish, French, and Portuguese Qt
Linguist catalogs under `translations/` are tracked; generated `.qm` files are
not.

Use `lupdate` and `lrelease` from the same Qt 6 installation used for the build:

```bash
export QT_TOOLS_DIR=/path/to/Qt/6.x/<platform>/bin
"$QT_TOOLS_DIR/lupdate" -version
"$QT_TOOLS_DIR/lrelease" -version
```

After adding, removing, or changing a user-visible string, run from the
repository root:

```bash
"$QT_TOOLS_DIR/lupdate" \
  -recursive \
  -extensions cpp,hpp \
  -locations absolute \
  -source-language en \
  ui \
  -ts \
  translations/openscp_es.ts \
  translations/openscp_fr.ts \
  translations/openscp_pt.ts
```

Review the diff in context. Preserve `%1`, `%2`, `%n`, newlines, rich-text tags,
and mnemonic markers such as `&`. New unfinished messages must be translated;
vanished messages should correspond to intentionally removed source strings.

```bash
rg -n 'type="unfinished"' translations
rg -n 'type="(vanished|obsolete)"' translations

mkdir -p build/translations-check
for locale in es fr pt; do
  "$QT_TOOLS_DIR/lrelease" -nounfinished \
    "translations/openscp_${locale}.ts" \
    -qm "build/translations-check/openscp_${locale}.qm"
done
```

Commit the source change and affected `.ts` files together. Test the changed
screen in every affected language; never commit `.qm` files or `build/` output.

## Integration tests

Real SFTP/SCP/FTP/FTPS/WebDAV tests skip locally when their service environment
is unavailable. CI invokes them directly so missing infrastructure is an error.

For an existing SFTP server, set `OPENSCP_IT_SFTP_HOST`,
`OPENSCP_IT_SFTP_PORT`, `OPENSCP_IT_SFTP_USER`, the remote base, and either a
password or private key before running CTest. See each integration test source
for its complete environment contract.

> [!WARNING]
> `scripts/ci/setup_protocol_services.sh` is intended only for a disposable
> Ubuntu VM or dedicated ephemeral runner. It uses `sudo`, creates or reuses a
> system user and resets its password, enables Apache modules, starts FTP/FTPS
> and WebDAV services, and leaves those changes active. Do not run it on a
> normal workstation or shared host.

The controlled Linux service flow is documented in
[docs/BUILDING.md](docs/BUILDING.md#protocol-integration-services).

## Pull-request checklist

- Target `dev`, explain the motivation, and link related issues.
- Keep behavior changes separate from broad mechanical formatting.
- Include tests or explain why the change cannot be tested automatically.
- Update user-facing documentation only where the source of truth changed.
- Confirm the relevant local checks and platform builds in the description.

## Licensing

Contributions follow the dual-licensing terms in
[docs/LICENSING.md](docs/LICENSING.md). Unless otherwise agreed in writing, a
submission is GPLv3-only and grants the maintainers the right to relicense it as
part of OpenSCP's commercial distribution. Only submit work you have the right
to contribute and identify third-party material for license review.
