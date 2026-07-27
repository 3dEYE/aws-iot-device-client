[CmdletBinding()]
param(
    [ValidateSet("All", "Build", "Test")]
    [string] $Mode = "All"
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path -LiteralPath (
    Join-Path $PSScriptRoot "../.."
)).Path
$volumeName = "aws-iot-device-client-build"
$imageName = "ghcr.io/openai/codex-universal:latest"

Get-Command docker -ErrorAction Stop | Out-Null

$dockerArguments = @(
    "run"
    "--rm"
    "--platform"
    "linux/amd64"
    "--entrypoint"
    "/bin/bash"
    "--mount"
    "type=bind,source=$repoRoot,target=/workspace"
    "--mount"
    "type=volume,source=$volumeName,target=/build"
    "--env"
    "CCACHE_DIR=/build/ccache"
    "--workdir"
    "/workspace"
    $imageName
    "/workspace/.codex/local/local-ci.sh"
    $Mode.ToLowerInvariant()
)

& docker @dockerArguments
if ($LASTEXITCODE -ne 0) {
    throw "Local Docker CI failed with exit code $LASTEXITCODE."
}
