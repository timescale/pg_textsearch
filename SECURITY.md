# Security Policy

## Supported Versions

| Version | Supported          |
| ------- | ------------------ |
| 0.0.x   | :white_check_mark: |

## Reporting a Vulnerability

If you discover a security vulnerability in pg_textsearch, please report it
responsibly by emailing **security@tigerdata.com**.

Please include:
- A description of the vulnerability
- Steps to reproduce
- Potential impact
- Any suggested fixes (optional)

We will acknowledge receipt within 48 hours and aim to provide a fix or
mitigation within 30 days for confirmed vulnerabilities.

Please do not disclose the vulnerability publicly until we have had a chance
to address it.

## Security Considerations

pg_textsearch is a Postgres extension that runs with the same privileges as
the Postgres server. When using this extension:

- Follow Postgres security best practices
- Limit `CREATE EXTENSION` privileges to trusted users
- Review index configurations before deployment
- Keep Postgres and pg_textsearch updated to the latest versions
