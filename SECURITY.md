# Security policy

## Supported versions

Until 1.0, only the latest release gets fixes. After 1.0, the current minor
release and the one before it are supported.

| Version | Supported |
|---|---|
| 0.x (pre-release) | latest tag only |

## Reporting a vulnerability

Report privately through GitHub's
[security advisory form](https://github.com/wslkit/wsldisk/security/advisories/new).
Please do not open a public issue for anything exploitable.

Include, if you can:

- `wsldisk --version` and your Windows and WSL versions (`wsl --version`)
- the exact command line, and whether it ran elevated
- what the tool did versus what it should have done
- a minimal reproduction; a throwaway distro is ideal

You should get an acknowledgement within 5 working days and a fix or a plan
within 30 days. Credit in the release notes unless you would rather not be named.

## What counts as a vulnerability here

`wsldisk` runs on the machine of the person invoking it, so the interesting
classes are:

- **Data destruction or corruption** of a distribution's disk that the command
  did not clearly ask for -- for example a `move` that deletes the source before
  the copy is verified, or a `shrink` that truncates below the filesystem size.
- **Privilege issues** around the elevation path: the elevated helper acting on
  input an unprivileged process controls, or the named-pipe IPC being reachable
  by another user.
- **Path handling** that escapes the intended directory: junctions, symlinks,
  `\?\` prefixes or crafted registry `BasePath` values causing writes outside
  the distribution's own location.
- **Untrusted input parsing** -- registry values, `wsl.exe` output, snapshot
  manifests -- leading to memory corruption.

Not vulnerabilities: needing administrator rights for operations that genuinely
require them, the tool refusing to run against a distribution that is in use, or
data loss from `--allow-unsafe`-style flags the user opted into after the warning.

## Hardening this project relies on

- 100% line/branch/function coverage of `src/`, enforced on every pull request
- Contract tests against the real Win32 API and end-to-end tests on throwaway distributions
- CodeQL (`security-extended`) on pull requests and weekly
- AddressSanitizer on every pull request; nightly fuzzing of every parser
- Release artifacts carry SBOMs and build provenance attestations
