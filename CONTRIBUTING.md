# Contributing to OpenSCP

First off, thank you for your interest in contributing to OpenSCP!
This project is open-source and welcomes ideas, improvements, and bug fixes from the community.
Please take a moment to read these guidelines before opening issues or pull requests.

---

## Branch Structure

- `main` → Stable branch.
  Contains only tested, working versions of OpenSCP.
  Do not submit pull requests directly to this branch (PRs to `main` will be retargeted or closed).

- `dev` → Active integration branch.
  All new features, fixes, and improvements should be based on `dev`.
  Once ready, changes from `dev` are merged into `main` for official releases.

Note: Merges to `main` are performed by the maintainer only.

### Stable Releases

If you need a fixed/stable version, please use the Releases page:

- Latest tagged builds: https://github.com/luiscuellar31/openscp/releases

Tags are immutable and represent tested snapshots you can depend on. The `main` branch remains stable but may move forward between releases.

---

## How to Contribute

1. Fork the repository to your own GitHub account.
2. Clone your fork locally:

   ```bash
   git clone https://github.com/<your-username>/openscp.git
   cd openscp
   ```

3. Create a new branch from `dev` for your change:

   ```bash
   git checkout dev
   git pull origin dev
   git checkout -b feature/your-feature-name
   ```

4. Make your changes, then commit using Conventional Commits:

   ```bash
   git add .
   git commit -m "feat: add new SFTP progress indicator"
   ```

5. Push your branch to your fork:

   ```bash
   git push origin feature/your-feature-name
   ```

6. On GitHub, open a Pull Request (PR):

   - Base branch: `dev` (all PRs must target `dev`)
   - Compare branch: `feature/your-feature-name`

---

## Issue Reporting and Labels

When opening an issue, please apply the label that best matches the report so triage is faster.

Recommended labels:

- `bug` for defects or regressions.
- `enhancement` for feature requests or UX improvements.
- `documentation` for docs/readme related changes.
- `question` for usage or behavior questions.
- `security` for potential security vulnerabilities.

Title prefixes like `[BUG]`, `[FEATURE]`, etc. are **not required** for regular issues.

Security reports are the only exception: follow [SECURITY.md](SECURITY.md), including the `security` label and the `[SECURITY]` title prefix.

---

## Keeping Your Fork Up to Date (`dev`)

Your fork only matches the main repository the first time you create it.
Before opening or updating a PR to `dev`, sync your local/fork `dev` branch with the upstream repository and then rebase your feature branch.

1. Add `upstream` once (if you do not have it yet):

   ```bash
   git remote add upstream https://github.com/luiscuellar31/openscp.git
   ```

2. Sync your local `dev` and your fork's `dev` with upstream:

   ```bash
   git fetch upstream
   git checkout dev
   git rebase upstream/dev
   git push origin dev
   ```

3. Rebase your working branch on top of the latest `upstream/dev`:

   ```bash
   git checkout feature/your-feature-name
   git rebase upstream/dev
   ```

4. Push your rebased branch:

   ```bash
   git push --force-with-lease origin feature/your-feature-name
   ```

Notes:
- `--force-with-lease` is required after a rebase if the branch was already pushed before.
- Do not use plain `--force`; `--force-with-lease` is safer.

---

## Code Style and Standards

- Follow Conventional Commits for commit messages.
- Keep code clean, consistent, and well-commented.
- Prefer descriptive variable and function names.
- Use English for all code and comments.

---

## Pull Request Guidelines

- Make sure your PR targets `dev`, not `main`.
- Keep PRs focused, one feature or fix per PR.
- Include a clear description of what was changed and why.
- If your PR addresses an issue, link it in the description (e.g., `Closes #42`).

---

## Local Validation (Required)

Before opening a PR, run at least the same baseline checks executed in CI:

```bash
cmake -S . -B build -DOPENSCP_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

If your change is platform-specific (e.g., macOS packaging), validate on that platform too.

### SFTP Integration Test (Optional Locally, Required in Linux/macOS CI)

`openscp_sftp_integration_tests` runs against a real SFTP server and is enabled in Linux/macOS CI.
Locally, it is skipped unless these variables are set.
You can authenticate with password (`OPENSCP_IT_SFTP_PASS`) or private key (`OPENSCP_IT_SFTP_KEY`):

```bash
export OPENSCP_IT_SFTP_HOST=127.0.0.1
export OPENSCP_IT_SFTP_PORT=2222
export OPENSCP_IT_SFTP_USER=openscp_it
export OPENSCP_IT_SFTP_PASS=<test-password>
# or: export OPENSCP_IT_SFTP_KEY=/path/to/private_key
# optional: export OPENSCP_IT_SFTP_KEY_PASSPHRASE=...
export OPENSCP_IT_REMOTE_BASE=/home/openscp_it/upload
ctest --test-dir build --output-on-failure
```

---

## Licensing

By contributing, you agree that your contributions will be licensed under the same license as OpenSCP (GPLv3).

---

Thank you for helping make OpenSCP better!
Every contribution, big or small, is deeply appreciated.
