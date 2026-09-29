## Why

<!-- What problem does this solve? Link an issue if relevant. -->

## What changed

<!-- Describe the affected components and observable behavior. -->

## Verification

<!-- Paste the terminal commands actually run and say whether they passed.
For code/build changes, report the local baseline from CONTRIBUTING.md:
./scripts/check_ci_local.sh --clean --full --werror
./scripts/checks/cpp-quality.sh --format
./scripts/checks/shell-quality.sh
git diff --check origin/dev...HEAD  (use origin/main for PRs to main)
Add focused tests for changed behavior. For docs-only changes, check links and
the diff. Explain relevant checks that failed or were not run. -->

## Checklist

- [ ] This pull request targets `dev`.
- [ ] I reviewed the full diff and understand the changes I am submitting,
  including any AI-assisted work.
- [ ] I reviewed `docs/ARCHITECTURE.md` and updated it if responsibilities,
  important flows, persistence, or navigation paths changed.
- [ ] I updated user-facing documentation and translation catalogs where
  behavior or user-visible strings changed.
