<#
.SYNOPSIS
    Mirrors the working tree into .\src and publishes that snapshot.

.DESCRIPTION
    The working tree at the repository root is the development copy: it holds the build
    outputs, the vcpkg cache and the local experiments. .\src is the published source tree
    with its own git history, and it is what gets pushed.

    This script mirrors every tracked source directory from the root into .\src, excluding
    build outputs, package caches and the nested repository itself, then commits the result
    and pushes it.

.PARAMETER Message
    Commit message. Defaults to a timestamped snapshot message.

.PARAMETER SkipPush
    Commit locally without pushing.

.EXAMPLE
    .\Scripts\sync-src.ps1
    .\Scripts\sync-src.ps1 -Message "Fix caption geometry for 24H2 ARM64"
    .\Scripts\sync-src.ps1 -SkipPush
#>
[CmdletBinding()]
param(
    [string]$Message = '',
    [switch]$SkipPush
)

# git writes progress and line-ending notices to stderr, and Windows PowerShell turns those into
# terminating errors under 'Stop'. Rely on $LASTEXITCODE for every native call instead.
$ErrorActionPreference = 'Continue'

$root = Split-Path -Parent $PSScriptRoot
$target = Join-Path $root 'src'

if (-not (Test-Path -LiteralPath $target)) {
    throw "Missing publish tree: $target"
}
if (-not (Test-Path -LiteralPath (Join-Path $target '.git'))) {
    throw "The publish tree is not a git repository: $target"
}

# Directory names are matched against the source root, so 'src' also stops the mirror from
# copying the publish tree into itself. '*.cso' files are committed HLSL build outputs that
# never belong in a source snapshot.
#
# '.git' must be excluded as a FILE, not only as a directory: this working tree is a linked
# worktree, so its '.git' is a gitdir pointer file, and a mirror would replace the publish
# tree's real '.git' directory with it, silently redirecting every later commit into the
# outer repository.
& robocopy $root $target /MIR /XD Build Cache packages vcpkg_installed __pycache__ src `
    /XF .git *.user *.suo *.cso /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
if ($LASTEXITCODE -ge 8) {
    throw "robocopy failed with exit code $LASTEXITCODE"
}
if ((Get-Item -LiteralPath (Join-Path $target '.git') -Force) -isnot [System.IO.DirectoryInfo]) {
    throw "The mirror replaced $target\.git with the working tree's gitdir pointer. Delete .git there, re-run 'git init', restore the remote, and commit again."
}

Push-Location $target
try {
    & git add -A
    if ($LASTEXITCODE -ne 0) {
        throw "git add failed with exit code $LASTEXITCODE"
    }

    $changes = & git status --porcelain
    if (-not $changes) {
        Write-Host 'Publish tree already matches the working tree; nothing to commit.'
        return
    }

    if (-not $Message) {
        $Message = 'Sync source snapshot ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
    }
    Write-Host "Committing: $Message"
    & git commit -m $Message
    if ($LASTEXITCODE -ne 0) {
        throw "git commit failed with exit code $LASTEXITCODE"
    }

    if ($SkipPush) {
        Write-Host 'Skipping push (-SkipPush).'
        return
    }

    & git push origin main
    if ($LASTEXITCODE -ne 0) {
        throw "git push failed with exit code $LASTEXITCODE"
    }
    Write-Host 'Published to origin/main.'
}
finally {
    Pop-Location
}
