# Security Policy

## Reporting a Vulnerability

Please report suspected vulnerabilities privately through
[GitHub Security Advisories](https://github.com/luiscuellar31/openscp/security/advisories/new).
Do not open a public issue until a fix has been released or the maintainers
confirm that public discussion is safe.

If you find a security problem:

1. Open a private vulnerability report using the link above.
2. Describe impact, reproduction steps, and the affected version/commit.
3. Include only sanitized logs and test data.
4. Allow the maintainers time to investigate and coordinate a release before
   publishing details.

## What to Include in the Report

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

Do not post credentials, private keys, exploit details, or sensitive data in
public issues.
If secrets were accidentally shared, revoke/rotate them immediately and update the report with sanitized data.
