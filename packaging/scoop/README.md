# scoop

`wsldisk.json` is a Scoop manifest. It is not in a bucket yet — see below.

## The placeholder version

`version` is `0.0.0` and the hashes are zeros, deliberately. There is nothing
honest to put there until a release exists, and a manifest pointing at a
download that 404s is worse than one that obviously has not been filled in yet.

That is also why this file is not installable as it stands — `scoop install`
against this URL would fetch a 404 and fail on the hash. `release.yml` fills in
the version and both hashes from the published `SHA256SUMS` and attaches the
result to the run as the `package-manifests` artifact; that copy is the one to
install from, or to open a bucket pull request with:

```powershell
scoop install .\wsldisk.json   # from the package-manifests artifact
```

The checked-in file stays at `0.0.0`. It is a template, and a repo-convention
test asserts it, because the substitution finds the URLs by replacing that
literal string.

## checkver and autoupdate

`checkver` watches the GitHub releases feed; `autoupdate` derives the download
URLs from `$version` and reads the hashes out of the `SHA256SUMS` asset
published alongside them. That is why `SHA256SUMS` is a release asset in its own
right rather than only pasted into the release notes: Scoop's updater reads it.

To check the two blocks against a real release once one exists:

```powershell
scoop update wsldisk   # in a bucket
.\bin\checkver.ps1 wsldisk -Update   # in a bucket checkout
```

## Getting into a bucket

Two routes, in increasing order of effort:

1. **A bucket in this repository.** `scoop bucket add wsldisk https://github.com/wslkit/wsldisk` works if the manifest sits in a `bucket/` directory at the root. That is a decision about repository layout, so it is not done here.
2. **ScoopInstaller/Extras.** The usual home for a tool like this. It wants a manifest that already works, a release history, and `checkver`/`autoupdate` that their bot can run — which is why those blocks are written now rather than later.
