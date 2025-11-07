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

## Licensing

By contributing, you agree that your contributions will be licensed under the same license as OpenSCP (GPLv3).

---

Thank you for helping make OpenSCP better!
Every contribution, big or small, is deeply appreciated.
