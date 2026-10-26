#Requires -Version 5.1
<#
.SYNOPSIS
    Build the distributable Pager Bridge package.
.DESCRIPTION
    Stages app payload (code + web UI + TLS certs + paired-device data +
    offline STT model + workspace) together with the one-click installer,
    then zips it.

    Layout produced:
      <OutDir>\pager-bridge-dist\
          install.cmd          <- double-click entry point on the new PC
          install.ps1
          app\
              pager_bridge\    <- full package (minus logs / __pycache__)
              pager_workspace\
      <OutDir>\pager-bridge.zip

    NOT included on purpose: AI agent backends (Hermes / pager-agents /
    WorkBuddy). They are separate products with machine-bound logins and
    several GB of state; install them on the new PC individually if needed.
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File build-package.ps1
    powershell -ExecutionPolicy Bypass -File build-package.ps1 -OutDir D:\transfer -NoZip
#>
[CmdletBinding()]
param(
    [string]$OutDir = ([Environment]::GetFolderPath('Desktop')),
    [switch]$NoZip
)

$ErrorActionPreference = 'Stop'

# Resolve everything relative to this script so no absolute (non-ASCII) paths
# are hard-coded:  scripts\build-package.ps1 -> pager_bridge\ -> project root.
$PkgRoot   = Split-Path $PSScriptRoot -Parent
if (-not (Test-Path (Join-Path $PkgRoot 'bridge.py'))) {
    throw "Cannot locate the pager_bridge package relative to $PSScriptRoot"
}
$ProjRoot  = Split-Path $PkgRoot -Parent
$Workspace = Join-Path $ProjRoot 'pager_workspace'
$Dist      = Join-Path $OutDir 'pager-bridge-dist'
$ZipPath   = Join-Path $OutDir 'pager-bridge.zip'

Write-Host "==> Source  : $PkgRoot"
Write-Host "==> Staging : $Dist"

if (Test-Path $Dist) { Remove-Item $Dist -Recurse -Force }
New-Item -ItemType Directory -Path (Join-Path $Dist 'app') -Force | Out-Null

# ---- copy app payload -------------------------------------------------------
$xd = @('__pycache__', '.pytest_cache')
$xf = @('*.log', '*.out', '*.err', 'probe_frames.jsonl', 'probe_final.txt')

robocopy $PkgRoot (Join-Path $Dist 'app\pager_bridge') /E /XD $xd /XF $xf /NFL /NDL /NJH /NJS | Out-Null
$rc1 = $LASTEXITCODE
if (Test-Path $Workspace) {
    robocopy $Workspace (Join-Path $Dist 'app\pager_workspace') /E /XD $xd /XF $xf /NFL /NDL /NJH /NJS | Out-Null
    $rc2 = $LASTEXITCODE
} else {
    New-Item -ItemType Directory -Path (Join-Path $Dist 'app\pager_workspace') -Force | Out-Null
    $rc2 = 0
}
if ($rc1 -gt 7 -or $rc2 -gt 7) { throw "robocopy failed (exit $rc1/$rc2)" }

# ---- privacy scrub (never ship chat/meeting/audit/credentials) ---------------
$stageBridge = Join-Path $Dist 'app\pager_bridge'
$scrubRemove = @(
    'data\audit.log',
    'data\sessions\transcript.jsonl',
    'data\workbuddy\credential-key.json'
)
foreach ($rel in $scrubRemove) {
    $p = Join-Path $stageBridge $rel
    if (Test-Path $p) { Remove-Item $p -Force }
}
$meetingsDir = Join-Path $stageBridge 'data\meetings'
if (Test-Path $meetingsDir) {
    Get-ChildItem $meetingsDir -File | Remove-Item -Force
}
@{
    'data\devices.json'         = '{"devices":[]}'
    'data\approval-policy.json' = '{"L1":true,"L2":true,"L3":true}'
    'data\macros.json'          = '{"echo_ok":{"label":"自检","cmd":"echo pager macro ok","cwd":""}}'
} | ForEach-Object {
    $_.GetEnumerator() | ForEach-Object {
        $out = Join-Path $stageBridge $_.Key
        $dir = Split-Path $out -Parent
        if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
        Set-Content -Path $out -Value $_.Value -Encoding UTF8
    }
}

# ---- copy installer ----------------------------------------------------------
Copy-Item (Join-Path $PSScriptRoot 'dist\install.ps1') $Dist -Force
Copy-Item (Join-Path $PSScriptRoot 'dist\install.cmd') $Dist -Force

# ---- sanity checklist ---------------------------------------------------------
$must = @(
    'app\pager_bridge\bridge.py',
    'app\pager_bridge\serve.py',
    'app\pager_bridge\static\index.html',
    'app\pager_bridge\models\faster-whisper-medium',
    'app\pager_bridge\certs\pager.crt',
    'app\pager_bridge\certs\pager.key',
    'app\pager_bridge\data\devices.json',
    'app\pager_bridge\agents.json',
    'install.ps1',
    'install.cmd'
)
$missing = 0
foreach ($m in $must) {
    if (Test-Path (Join-Path $Dist $m)) {
        Write-Host "    [OK]       $m"
    } else {
        Write-Host "    [MISSING]  $m" -ForegroundColor Yellow
        $missing++
    }
}
if ($missing -gt 0) { Write-Host "WARNING: $missing expected file(s) missing from package." -ForegroundColor Yellow }

$mb = [math]::Round((Get-ChildItem $Dist -Recurse -File | Measure-Object Length -Sum).Sum / 1MB, 1)
Write-Host "==> Staged $mb MB"

# ---- zip -----------------------------------------------------------------------
if (-not $NoZip) {
    if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
    if (Get-Command tar.exe -ErrorAction SilentlyContinue) {
        # bsdtar ships with Windows 10+. 'store' = no recompression: the 1.4 GB
        # model is already compressed, so deflate would only burn time.
        Write-Host "==> Creating $ZipPath (tar, store mode, fast) ..."
        & tar.exe -a --options zip:compression=store -cf $ZipPath -C $Dist .
        if ($LASTEXITCODE -ne 0) { throw "tar failed (exit $LASTEXITCODE)" }
    } else {
        Write-Host "==> Creating $ZipPath (Compress-Archive, may take a while) ..."
        Compress-Archive -Path (Join-Path $Dist '*') -DestinationPath $ZipPath -CompressionLevel Fastest
    }
    $zipMB = [math]::Round((Get-Item $ZipPath).Length / 1MB, 1)
    $hash  = (Get-FileHash $ZipPath -Algorithm SHA256).Hash
    Write-Host "==> ZIP: $zipMB MB"
    Write-Host "==> SHA256: $hash"
}

Write-Host ''
Write-Host 'Done. To deploy on a new PC:' -ForegroundColor Green
Write-Host '  1. copy pager-bridge.zip (or the pager-bridge-dist folder) to the new PC'
Write-Host '  2. extract it'
Write-Host '  3. double-click install.cmd'
