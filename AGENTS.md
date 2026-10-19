# Working in this repository

- For component ownership and cross-layer changes, consult
  `docs/ARCHITECTURE.md`.
- For development checks, engineering rules, pull requests, translations, and
  commit conventions, consult `CONTRIBUTING.md`.
- Prefer readable, maintainable code consistent with the surrounding style over
  fewer lines.
- Keep each change to what the task needs. Do not rename, restructure, or
  reformat code the task does not touch; suggest such cleanups separately.
- Reuse the helpers and patterns already in the surrounding code before adding
  new ones, and do not add an abstraction or dependency for a single use.
- Keep comments short, simple, and easy to understand; go longer only when the
  reason needs it. Explain why, not what the code does or how it changed; a
  regression test may describe the failure it guards against.
- Review `docs/ARCHITECTURE.md` for each change; update it when responsibilities,
  important flows, persistence, or navigation paths change.
- Keep user-facing documentation aligned with behavior changes, and update
  translations when user-visible strings change.
- Add repository documentation only when it provides lasting value to users,
  contributors, or maintainers; prefer existing documents. Keep investigation
  reports and one-off benchmark results outside the repository unless explicitly
  asked to include them.
