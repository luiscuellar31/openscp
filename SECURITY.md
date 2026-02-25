# Security Policy

## Reporting a Vulnerability

To keep public traceability and clear prioritization, this repository handles security reports as **GitHub issues**.

If you find a security problem:

1. Open a new issue on GitHub.
2. Add the `security` label (the project's custom label).
3. Include the `[SECURITY]` prefix in the issue title.
4. Describe impact, reproduction steps, and the affected version/commit.

The `security` label is the official mechanism used to prioritize and filter these reports.

## What to Include in the Issue

- Observed behavior and expected behavior.
- Potential risk/impact (for example: credential exposure, host-key validation bypass, etc.).
- Environment details: operating system, OpenSCP version, and authentication method.
- Minimal reproducible evidence (sanitized logs, screenshots, clear steps).
- If available, a temporary mitigation/workaround.

## Scope and Supported Versions

Priority is given to:

- `main` (stable branch).
- The latest published release.
- `dev` when the issue affects recent development changes.

## Response Process

- Initial triage target: within 72 hours.
- Severity/priority confirmation: as soon as possible based on impact.
- Fix timing: handled according to severity and technical complexity.

## Responsible Disclosure

Do not post credentials, private keys, or sensitive data in issues.
If secrets were accidentally shared, revoke/rotate them immediately and update the report with sanitized data.
