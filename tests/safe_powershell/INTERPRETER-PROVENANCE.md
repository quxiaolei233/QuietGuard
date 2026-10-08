# Validation interpreter provenance

This records the tool used for the 2026-10-03 Linux-only validation. It is separate from QuietGuard executable provenance.

- Interpreter: Microsoft PowerShell 7.6.6 Core, Linux x64 portable archive
- Host: Debian GNU/Linux 13, x64
- Official binary-archive instructions: https://learn.microsoft.com/en-us/powershell/scripting/install/alternate-install-methods?view=powershell-7.5
- Official release and published SHA-256 hashes: https://github.com/PowerShell/PowerShell/releases/tag/v7.6.6
- Exact archive URL resolved from those official sources: https://github.com/PowerShell/PowerShell/releases/download/v7.6.6/powershell-7.6.6-linux-x64.tar.gz
- Published and independently computed archive SHA-256 matched before extraction:

```text
ddbc4a2d113bbd46d283cfedcbcd117a70caefd7673f41f2b4e0000badf103bc
```

The download used HTTPS with TLS certificate verification. Archive member paths, link targets and entry types were checked before workspace-only extraction. No system package installation or global PATH change occurred. The interpreter reported version 7.6.6 when launched with `-NoLogo -NoProfile -NonInteractive` and workspace-local HOME/XDG/TMPDIR settings.

The verification was against Microsoft's release-page hash. No independent detached signature or artifact attestation was checked. A checksum verifies consistency with the cited source, not an additional publisher-signature guarantee.

No binary, archive, cache, generated assembly or installed dependency is committed here. Supply your own already trusted interpreter through `run.py --pwsh`. If choosing another official release, verify its corresponding published checksum rather than reusing the 7.6.6 hash. The runner records the actual interpreter version in each result and does not silently fetch or substitute one.
