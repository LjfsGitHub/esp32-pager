#Requires -Version 5.1
<#
.SYNOPSIS
    Pager Bridge 一键安装脚本 (Windows 10/11)
.DESCRIPTION
    1. 查找(或用 winget 自动安装) Python 3.12
    2. 复制程序到安装目录(含网页、配对证书、已配对设备数据、离线语音模型)
    3. 安装 Python 依赖
    4. 为语音转写创建独立虚拟环境 (faster-whisper), 并修正 speech.py 的路径
    5. 放行防火墙 TCP 8766 (手表通信)
    6. 创建启动脚本和桌面快捷方式; 可选开机自启
.PARAMETER InstallDir
    安装目录, 默认 ~\Documents\pager-app
.PARAMETER SkipSTT
    跳过离线语音转写安装(手表语音转文字将不可用, 其余功能正常)
.PARAMETER AutoStart
    注册「登录时自动启动」计划任务
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File install.ps1
    powershell -ExecutionPolicy Bypass -File install.ps1 -InstallDir D:\pager-app -AutoStart
#>
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:USERPROFILE 'Documents\pager-app'),
    [switch]$SkipSTT,
    [switch]$AutoStart
)

$ErrorActionPreference = 'Stop'

function Write-Step([string]$msg) { Write-Host ''; Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Ok([string]$msg)   { Write-Host "    [OK] $msg" -ForegroundColor Green }
function Write-Warn2([string]$msg){ Write-Host "    [警告] $msg" -ForegroundColor Yellow }
function Stop-Fatal([string]$msg) { Write-Host "    [失败] $msg" -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------- 0. 包完整性
$Src = Join-Path $PSScriptRoot 'app'
if (-not (Test-Path (Join-Path $Src 'pager_bridge\bridge.py'))) {
    Stop-Fatal "install.ps1 必须放在解压后的安装包根目录(同级应有 app\ 子目录)。"
}
$hasModel = Test-Path (Join-Path $Src 'pager_bridge\models\faster-whisper-medium')

# ---------------------------------------------------------------- 1. Python
Write-Step '检查 Python (需要 3.10+, 推荐 3.12)'

$script:PythonExe  = $null
$script:PythonArgs = @()

function Test-Python([string]$exe, [string[]]$prefixArgs) {
    try {
        $out = & $exe @prefixArgs -c "import sys; print('%d.%d' % sys.version_info[:2])" 2>$null
        if ($LASTEXITCODE -ne 0 -or -not $out) { return $null }
        $p = ($out | Select-Object -First 1).Trim().Split('.')
        if ([int]$p[0] -eq 3 -and [int]$p[1] -ge 10) { return "$([int]$p[0]).$([int]$p[1])" }
    } catch {}
    return $null
}

function Find-Python {
    $candidates = @(
        @{ Exe = 'py';     Args = @('-3.12') },
        @{ Exe = 'py';     Args = @('-3') },
        @{ Exe = 'python'; Args = @() },
        @{ Exe = (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python312\python.exe'); Args = @() },
        @{ Exe = (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python311\python.exe'); Args = @() },
        @{ Exe = 'C:\Python312\python.exe'; Args = @() }
    )
    foreach ($c in $candidates) {
        $v = Test-Python $c.Exe $c.Args
        if ($v) {
            $script:PythonExe  = $c.Exe
            $script:PythonArgs = $c.Args
            Write-Ok "找到 Python $v  ($($c.Exe) $($c.Args -join ' '))"
            return
        }
    }
}

Find-Python

if (-not $script:PythonExe) {
    Write-Warn2 '未找到 Python, 尝试用 winget 自动安装 Python 3.12 ...'
    if (Get-Command winget.exe -ErrorAction SilentlyContinue) {
        & winget install -e --id Python.Python.3.12 --scope user --silent `
            --accept-source-agreements --accept-package-agreements
        Find-Python
    }
    if (-not $script:PythonExe) {
        Stop-Fatal 'Python 自动安装失败。请手动安装 Python 3.12 (https://www.python.org/downloads/, 勾选 Add to PATH) 后重跑本脚本。'
    }
}

function Invoke-Python([string[]]$rest) {
    $all = $script:PythonArgs + $rest
    & $script:PythonExe @all
}

# ---------------------------------------------------------------- 2. 复制文件
Write-Step "复制程序到 $InstallDir"
if (Test-Path (Join-Path $InstallDir 'pager_bridge')) {
    Write-Warn2 '目标目录已存在 pager_bridge, 将合并覆盖。'
}
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

robocopy (Join-Path $Src 'pager_bridge') (Join-Path $InstallDir 'pager_bridge') /E /NFL /NDL /NJH /NJS | Out-Null
$rc1 = $LASTEXITCODE
if (Test-Path (Join-Path $Src 'pager_workspace')) {
    robocopy (Join-Path $Src 'pager_workspace') (Join-Path $InstallDir 'pager_workspace') /E /NFL /NDL /NJH /NJS | Out-Null
    $rc2 = $LASTEXITCODE
} else {
    New-Item -ItemType Directory -Force -Path (Join-Path $InstallDir 'pager_workspace') | Out-Null
    $rc2 = 0
}
if ($rc1 -gt 7 -or $rc2 -gt 7) { Stop-Fatal "文件复制失败 (robocopy 退出码 $rc1/$rc2)。" }
Write-Ok '程序文件就绪 (含配对证书和已配对设备数据, 手表免重新配对)'

# ---------------------------------------------------------------- 3. Python 依赖
Write-Step '安装 Python 依赖 (fastapi / uvicorn / edge-tts / wetext ...)'
Invoke-Python @('-m', 'pip', 'install', '--upgrade', 'pip', '--disable-pip-version-check', '-q')
$req = Join-Path $InstallDir 'pager_bridge\requirements.txt'
$pipArgs = @('-m', 'pip', 'install', '--disable-pip-version-check', '-r', $req, 'edge-tts', 'comtypes', 'wetext', 'numpy')
Invoke-Python $pipArgs
if ($LASTEXITCODE -ne 0) {
    Write-Warn2 '首次安装失败, 加 --user 重试 ...'
    Invoke-Python ($pipArgs + '--user')
    if ($LASTEXITCODE -ne 0) { Stop-Fatal '依赖安装失败, 请检查网络后重跑本脚本。' }
}
Write-Ok '依赖安装完成'

# ---------------------------------------------------------------- 4. 语音转写
$sttReady = $false
if ($SkipSTT) {
    Write-Warn2 '已按 -SkipSTT 跳过语音转写安装, 手表语音转文字不可用。'
} elseif (-not $hasModel) {
    Write-Warn2 '安装包内缺少 models\faster-whisper-medium, 跳过语音转写。'
} else {
    Write-Step '安装离线语音转写 (faster-whisper, 独立虚拟环境)'
    $venvDir = Join-Path $InstallDir 'stt-venv'
    Invoke-Python @('-m', 'venv', $venvDir)
    $venvPy = Join-Path $venvDir 'Scripts\python.exe'
    if (-not (Test-Path $venvPy)) {
        Write-Warn2 '虚拟环境创建失败, 跳过语音转写。'
    } else {
        & $venvPy -m pip install --disable-pip-version-check -q faster-whisper
        if ($LASTEXITCODE -ne 0) {
            Write-Warn2 'faster-whisper 安装失败(可能是网络问题), 语音转写不可用, 其余功能不受影响。'
        } else {
            $gpu = Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue |
                   Where-Object { $_.Name -match 'NVIDIA' } | Select-Object -First 1
            if ($gpu) {
                Write-Ok '检测到 NVIDIA 显卡, 安装 CUDA 加速组件 ...'
                & $venvPy -m pip install --disable-pip-version-check -q nvidia-cublas-cu12 nvidia-cudnn-cu12
            } else {
                Write-Ok '未检测到 NVIDIA 显卡, 语音转写将用 CPU 运行(较慢)。'
            }
            # speech.py 里的 _STT_PY 是硬编码绝对路径, 指向本机的旧 venv, 这里改写成新 venv
            $speechFile = Join-Path $InstallDir 'pager_bridge\speech.py'
            $newLine = '_STT_PY = Path(r"' + $venvPy + '")'
            $code = Get-Content $speechFile -Raw -Encoding UTF8
            $code = [regex]::Replace($code, '(?m)^_STT_PY\s*=\s*Path\(r?"[^"]*"\)', { param($m) $newLine })
            Set-Content $speechFile -Value $code -Encoding UTF8 -NoNewline
            $sttReady = $true
            Write-Ok '语音转写就绪'
        }
    }
}

# ---------------------------------------------------------------- 5. 防火墙
Write-Step '放行防火墙 TCP 8766 (手表通信端口)'
$ruleName = 'Pager Bridge Device (TCP 8766)'
$fwDone = $false
try {
    if (-not (Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -DisplayName $ruleName -Direction Inbound -Protocol TCP `
            -LocalPort 8766 -Action Allow -Profile Any -ErrorAction Stop | Out-Null
    }
    $fwDone = $true
} catch {
    Write-Warn2 '需要管理员权限, 弹出 UAC 请点击「是」...'
    try {
        Start-Process powershell -Verb RunAs -Wait -ArgumentList `
            '-NoProfile', '-WindowStyle', 'Hidden', '-Command',
            "New-NetFirewallRule -DisplayName '$ruleName' -Direction Inbound -Protocol TCP -LocalPort 8766 -Action Allow -Profile Any"
    } catch {}
    if (Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue) { $fwDone = $true }
}
if ($fwDone) { Write-Ok '防火墙规则已配置' }
else { Write-Warn2 '防火墙规则未配置。手表连不上时, 请在「高级安全防火墙」手动放行 TCP 8766。' }

# ---------------------------------------------------------------- 6. 启动脚本 / 快捷方式
Write-Step '创建启动脚本和桌面快捷方式'

# 后台静默启动用 pythonw(无控制台窗口); py 启动器对应 pyw
$hiddenExe  = $script:PythonExe
$hiddenArgs = $script:PythonArgs
if ($script:PythonExe -ieq 'py' -and (Get-Command pyw.exe -ErrorAction SilentlyContinue)) {
    $hiddenExe = 'pyw'
} elseif ($script:PythonExe -match 'python\.exe$') {
    $cand = $script:PythonExe -replace 'python\.exe$', 'pythonw.exe'
    if (Test-Path $cand) { $hiddenExe = $cand; $hiddenArgs = @() }
}

$serverCmd = '"' + $script:PythonExe + '" ' + (($script:PythonArgs + @('-m', 'pager_bridge.serve')) -join ' ')
$hiddenCmd = '"' + $hiddenExe + '" ' + (($hiddenArgs + @('-m', 'pager_bridge.serve')) -join ' ')

$runServer = @"
@echo off
cd /d "%~dp0"
$serverCmd
echo.
echo Pager Bridge 已退出。
pause
"@
Set-Content (Join-Path $InstallDir 'run-server.cmd') -Value $runServer -Encoding ASCII

$startCmd = @"
@echo off
cd /d "%~dp0"
start "" /min "%~dp0run-server.cmd"
timeout /t 3 /nobreak >nul
start "" "http://127.0.0.1:8765/"
"@
Set-Content (Join-Path $InstallDir 'start.cmd') -Value $startCmd -Encoding ASCII

$runHidden = @"
@echo off
cd /d "%~dp0"
start "" /min /b $hiddenCmd
"@
Set-Content (Join-Path $InstallDir 'run-hidden.cmd') -Value $runHidden -Encoding ASCII

try {
    $ws = New-Object -ComObject WScript.Shell
    $lnkPath = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Pager Bridge.lnk'
    $sc = $ws.CreateShortcut($lnkPath)
    $sc.TargetPath = Join-Path $InstallDir 'start.cmd'
    $sc.WorkingDirectory = $InstallDir
    $sc.Description = 'Pager Bridge - 手表桥接服务'
    $sc.Save()
    Write-Ok '桌面快捷方式: Pager Bridge.lnk'
} catch {
    Write-Warn2 '桌面快捷方式创建失败, 可直接双击安装目录里的 start.cmd。'
}

if ($AutoStart) {
    & schtasks /create /f /tn 'Pager Bridge' /sc onlogon /tr ('"' + (Join-Path $InstallDir 'run-hidden.cmd') + '"') | Out-Null
    if ($LASTEXITCODE -eq 0) { Write-Ok '已注册开机自启任务 (登录当前用户后自动启动)' }
    else { Write-Warn2 '开机自启注册失败, 可稍后手动执行: schtasks /create /f /tn "Pager Bridge" /sc onlogon /tr "<安装目录>\run-hidden.cmd"' }
}

# ---------------------------------------------------------------- 7. 自检
Write-Step '自检: 导入服务模块'
Push-Location $InstallDir
try {
    Invoke-Python @('-c', 'import pager_bridge.serve, pager_bridge.device_api')
    if ($LASTEXITCODE -eq 0) { Write-Ok '模块自检通过' }
    else { Write-Warn2 '自检未通过, 可能是依赖没装全。先试着启动一次, 报错的话把窗口内容发回来。' }
} finally { Pop-Location }

# ---------------------------------------------------------------- 8. 汇总
Write-Host ''
Write-Host '============================================================' -ForegroundColor Cyan
Write-Host ' 安装完成' -ForegroundColor Green
Write-Host '============================================================' -ForegroundColor Cyan
Write-Host " 安装目录 : $InstallDir"
Write-Host ' 启动方式 : 双击桌面「Pager Bridge」, 或运行安装目录下的 start.cmd'
Write-Host ' 网页地址 : http://127.0.0.1:8765/  (仅本机可访问, 这是设计如此)'
Write-Host ' 手表连接 : 配对证书和已配对设备已随包迁移, 手表免重新配对;'
Write-Host '            保证手表和本电脑连同一个 WiFi 即可自动发现。'
if ($sttReady) { Write-Host ' 语音转写 : 已就绪 (离线 faster-whisper)' }
else { Write-Host ' 语音转写 : 未安装 (手表按键说话转文字不可用, 文字消息正常)' -ForegroundColor Yellow }
if ($AutoStart) { Write-Host ' 开机自启 : 已开启' }
Write-Host ''
Write-Host ' 注意: 网页里的 AI 代理 (Hermes / Cursor / Codex / Claude /' -ForegroundColor Yellow
Write-Host ' WorkBuddy) 是各自独立的软件, 本安装包不包含。需要哪个就在' -ForegroundColor Yellow
Write-Host ' 这台电脑上单独安装哪个; 不装的话网页里对应选项不可用, 不' -ForegroundColor Yellow
Write-Host ' 影响手表收发和网页本身。' -ForegroundColor Yellow
Write-Host '============================================================' -ForegroundColor Cyan
Write-Host ''

try {
    $ans = Read-Host '现在启动 Pager Bridge? [Y/n]'
    if ($ans -notmatch '^(n|N)') {
        Start-Process (Join-Path $InstallDir 'start.cmd')
        Write-Host '已启动, 浏览器会自动打开网页。' -ForegroundColor Green
    }
} catch {}
