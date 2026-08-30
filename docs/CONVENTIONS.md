# OpenSCP Engineering Conventions

These rules apply to first-party code in `core/`, `ui/`, and `tests/`. The
automated gates intentionally discover tracked files instead of maintaining an
allowlist.

## C++ style

- Use C++20 and the repository `.clang-format` configuration.
- Use English for identifiers, code comments, log messages, and CMake errors.
- Prefer anonymous namespaces for translation-unit-private declarations.
- Use `kCamelCase` for constants and a trailing `_` for data members.
- Keep header declarations and source definitions in the same order.
- Group includes mechanically: project headers, Qt, third parties, then the
  standard library.
- Keep new or substantially changed functions below the configured
  `clang-tidy` size thresholds when practical.
- Preserve explicit ownership. Use `UniqueFile` for `FILE*`, `SecureString` for
  secrets, and RAII for locks and protocol handles.

Format every tracked `.cpp`, `.cc`, `.cxx`, `.h`, `.hh`, `.hpp`, and `.inc`
file with LLVM 17. Check the repository with:

```bash
./scripts/check_cpp_quality.sh --format
```

## Warnings and static analysis

All first-party targets inherit shared warning and hardening interface targets.
`OPENSCP_WERROR` is optional for normal local development and mandatory in CI.

```bash
cmake -S . -B build-werror \
  -DOPENSCP_BUILD_TESTS=ON \
  -DOPENSCP_WERROR=ON
cmake --build build-werror --parallel
./scripts/check_cpp_quality.sh --tidy --cppcheck \
  --build-dir build-werror
```

Do not silence a diagnostic globally to accommodate new code. Narrow
suppression requires a documented false positive or a compatibility reason.

## Tests

Register tests through `openscp_add_test(...)`. Configuration fails if a
tracked `*_tests.cpp` is not registered. Link the owning production target and
use `tests/TestHarness.hpp` for shared test context and application setup.

Tests should cover:

- success, rejection, cancellation, and stale-result paths;
- boundary and malformed inputs;
- concurrent or reentrant behavior where synchronization is involved;
- large collections and repeated deltas for hot-path code;
- protocol-specific semantics without weakening the shared contract.

Integration tests may return the configured skip code only when their external
service or credentials are genuinely unavailable. CI protocol jobs invoke
their binaries directly so missing infrastructure is a failure.

## Concurrency

- Document every permitted nested mutex order next to the owned mutexes.
- Never call external clients, presentation callbacks, or emit Qt signals while
  holding a queue/state mutex unless the class contract explicitly requires it.
- Prefer immutable snapshots and ID-scoped deltas over sharing mutable task
  references.
- Cancellation must wake blocked waiters and leave later operations usable.
- Add overlapping-operation and disconnect tests for new coordinators.

## Security and persistence

- Remote paths must pass through the shared normalizer and protocol-specific
  root/base adapters.
- Secrets must not be persisted directly in site or queue formats.
- Settings, queue files, and any enabled secret fallback must be owner-only on
  Unix.
- Preserve existing persistence schemas unless a versioned migration is part
  of the change.
- Avoid avoidable copies of `SessionOptions` and secure values.

## Commits and validation

Use Conventional Commits and keep mechanical formatting separate from behavior
changes. Before opening a pull request to `dev`, run:

```bash
./scripts/check_ci_local.sh --clean --full --werror
./scripts/check_cpp_quality.sh --format --tidy --cppcheck \
  --build-dir build-ci-local
git diff --check
```

Protocol integration, sanitizers, translation generation, and platform
hardening checks remain required for changes that reach their corresponding
code paths.
