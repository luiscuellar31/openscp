# Contributing to OpenSCP

Thanks for helping improve OpenSCP. Focused bug fixes, documentation updates,
tests, and new features are all welcome. Public pull requests target `dev`; the
maintainer promotes tested work to `main` and creates release tags.

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

Before updating an existing pull request, rebase on `upstream/dev` and use
`git push --force-with-lease` if the rebased branch was already published.

## Commits

Write commit messages in English using Conventional Commits. Use an optional
scope when it clarifies the affected area, and keep each commit focused on one
logical change:

```text
type(scope): imperative summary
type: imperative summary
```

Common types include `feat`, `fix`, `refactor`, `docs`, `test`, `ci`, and `chore`.
For a breaking change, add `!` after the type or scope and explain the impact
and migration in the commit body with a `BREAKING CHANGE:` footer.

## Build and test

The normal development entrypoints are:

```bash
./scripts/linux.sh dev
./scripts/macos.sh dev
```

Run the local CI baseline before opening a pull request:

```bash
./scripts/check_ci_local.sh --clean --full --werror
./scripts/checks/cpp-quality.sh --format --cppcheck
./scripts/checks/shell-quality.sh
git diff --check
```

`check_ci_local.sh` configures, builds, and runs the tests in the ignored
`build-ci-local/` directory by default. Its `--build-dir` option changes that
path.

For documentation-only changes, check the links and run `git diff --check`;
compiling the application is not needed to validate Markdown.

Static analysis is available after configuring a build with a compilation
database:

```bash
./scripts/checks/cpp-quality.sh --tidy --cppcheck \
  --build-dir build-ci-local
```

FTP and WebDAV are optional backends selected at configure time. Changes to
their `#if` branches must also build and pass tests with both disabled:

```bash
cmake -S . -B build-no-backends -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DOPENSCP_BUILD_TESTS=ON \
  -DOPENSCP_ENABLE_FTP_BACKEND=OFF \
  -DOPENSCP_ENABLE_WEBDAV_BACKEND=OFF
cmake --build build-no-backends --target openscp_core_tests
ctest --test-dir build-no-backends --output-on-failure \
  -R '^openscp_core_tests$'
```

Clang/libFuzzer harnesses cover the untrusted FTP and WebDAV listing parsers.
They are opt-in and never enter normal application or release builds:

```bash
CC=clang CXX=clang++ cmake -S . -B build-fuzz -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOPENSCP_BUILD_FUZZERS=ON
cmake --build build-fuzz --target \
  openscp_ftp_listing_fuzzer openscp_webdav_listing_fuzzer
mkdir -p build-fuzz/corpus/ftp build-fuzz/corpus/webdav
./build-fuzz/fuzz/openscp_ftp_listing_fuzzer \
  build-fuzz/corpus/ftp fuzz/corpus/ftp \
  -max_total_time=60 -timeout=5 -rss_limit_mb=2048
./build-fuzz/fuzz/openscp_webdav_listing_fuzzer \
  build-fuzz/corpus/webdav fuzz/corpus/webdav \
  -max_total_time=60 -timeout=5 -rss_limit_mb=2048
```

The first corpus directory is disposable working storage; the checked-in seed
corpus is passed second and remains unchanged.

Platform-specific changes must also be exercised on that platform. Complete
dependency and packaging instructions are in [docs/BUILDING.md](docs/BUILDING.md).
Changes to custom controls, focus behavior, or visual states must also follow
the keyboard and screen-reader matrix in
[docs/ACCESSIBILITY.md](docs/ACCESSIBILITY.md).

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
- Changes to persisted formats require explicit decisions about schema and
  versioning, together with tests for the resulting behavior.
- Never persist passwords, passphrases, or proxy credentials in plain settings
  or transfer-queue data.
- Add tests for success, failure, cancellation, malformed input, and concurrent
  behavior when those paths are relevant.
- Tests should link the production target that owns the behavior; do not compile
  private copies of production `.cpp` files.

Do not globally suppress a warning to accommodate new code. A narrow
suppression needs a documented compatibility reason or false positive.

## Translations

English strings live in the C++ sources. Spanish, Portuguese, French, and German
Qt Linguist catalogs under `translations/` are tracked; generated `.qm` files
are not.

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
  translations/openscp_de.ts \
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
for locale in de es fr pt; do
  "$QT_TOOLS_DIR/lrelease" -nounfinished \
    "translations/openscp_${locale}.ts" \
    -qm "build/translations-check/openscp_${locale}.qm"
done
```

Commit the source change and affected `.ts` files together. Test the changed
screen in every affected language; never commit `.qm` files or generated
`build*/` output.

## Integration tests

Real SFTP/SCP/FTP/FTPS/WebDAV tests skip locally when their service environment
is unavailable. CI invokes them directly so missing infrastructure is an error.

For an existing SFTP server, set `OPENSCP_IT_SFTP_HOST`,
`OPENSCP_IT_SFTP_PORT`, `OPENSCP_IT_SFTP_USER`, the remote base, and either a
password or private key before running CTest. See each integration test source
for the full list of variables it accepts.

For code and release pull requests, both commands in the
[pull request template](.github/pull_request_template.md) must pass. The local
CI script covers `openscp_sync_dialog_tests`, but SFTP may skip without a
configured server. A passing local script with SFTP skipped is acceptable only
if `openscp_sftp_integration_tests` runs and passes in GitHub Actions. An SFTP
skip in both places does not satisfy the requirement. Add any other commands
actually run to the PR's Verification block.

> [!WARNING]
> `scripts/ci/setup_protocol_services.sh` is intended only for a disposable
> Ubuntu VM or dedicated ephemeral runner. It uses `sudo`, creates or reuses a
> system user and resets its password, enables Apache modules, starts FTP/FTPS
> and WebDAV services, and leaves those changes active. Do not run it on a
> normal workstation or shared host.

The controlled Linux service flow is documented in
[docs/BUILDING.md](docs/BUILDING.md#protocol-integration-services).

## Pull requests and AI assistance

AI tools are welcome. Review the full diff and make sure you understand all
changes you submit, including AI-assisted work. Use the
[pull request template](.github/pull_request_template.md) to record why the
change is needed, what changed, and which checks you actually ran.

- Target `dev`, explain the motivation, and link related issues.
- Keep behavior changes separate from broad mechanical formatting.
- Include tests or explain why the change cannot be tested automatically.
- Review [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for each change. Update it
  in the same change if responsibilities, important flows, persistence, or
  navigation paths change; otherwise, no architecture edit is needed.
- Update user-facing documentation when behavior changes, and translation
  catalogs when user-visible strings change.
- List the relevant local checks and platform builds actually run, and explain
  any relevant checks not run.

## Licensing

Contributions follow the dual-licensing terms in
[docs/LICENSING.md](docs/LICENSING.md). Unless otherwise agreed in writing, a
submission is GPLv3-only and grants the maintainers the right to relicense it as
part of OpenSCP's commercial distribution. Only submit work you have the right
to contribute and identify third-party material for license review.
