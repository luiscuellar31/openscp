## Why

<!-- What problem does this solve? Link an issue if relevant. -->

## What changed

<!-- Describe the affected components and observable behavior. -->

## Verification

<!-- Required for code and release PRs: both commands below must pass.
The local CI script may pass with SFTP skipped. That is acceptable only when
the GitHub Actions SFTP integration test runs and passes; a CI skip does not
count. Add other commands run when relevant.
For documentation-only changes, check links and the diff instead. -->

```sh
./scripts/check_ci_local.sh --clean --full --werror
./scripts/checks/cpp-quality.sh --format --cppcheck
```

## Checklist

- [ ] This pull request targets `dev`.
- [ ] I reviewed the full diff and understand the changes I am submitting,
  including any AI-assisted work.
- [ ] I reviewed `docs/ARCHITECTURE.md` and updated it if responsibilities,
  important flows, persistence, or navigation paths changed.
- [ ] I updated user-facing documentation and translation catalogs where
  behavior or user-visible strings changed.
