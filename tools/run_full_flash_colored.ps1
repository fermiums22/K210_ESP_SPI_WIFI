param(
    [string]$Port = "COM12",
    [switch]$NoFullFlash
)

$ErrorActionPreference = "Continue"

try {
    [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
    $OutputEncoding = [System.Text.UTF8Encoding]::new($false)
} catch {
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$EspRepo = Resolve-Path (Join-Path $ScriptDir "..")
$Workspace = Split-Path -Parent $EspRepo
$K210Repo = Join-Path $Workspace "K210_AI_V7s_Plus"

if (-not (Test-Path $K210Repo)) {
    $K210Repo = "D:\w_space\K210_AI_V7s_Plus"
}
if (-not (Test-Path $K210Repo)) {
    throw "K210 repo not found. Expected sibling repo or D:\w_space\K210_AI_V7s_Plus"
}

$LogDir = Join-Path $EspRepo "logs\one_click"
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$LogFile = Join-Path $LogDir "full_flash_$Stamp.log"

function Strip-Ansi([string]$s) {
    return ($s -replace "`e\[[0-9;?]*[ -/]*[@-~]", "")
}

function Get-LineColor([string]$s) {
    if ($s -match "(?i)(FATAL|ERROR|\bERR\b|FAILED|FAIL|timeout|command RX timeout|KSD:ERR)") { return "Red" }
    if ($s -match "(?i)(WARNING|WARN|full 1 MB|erase|Deprecation Warning)") { return "Yellow" }
    if ($s -match "(?i)(SUCCESS|DONE|OK:|KSD:OK|KSD:SIZE|ESP flash result: OK|kesp:)") { return "Green" }
    if ($s -match "(?i)(KSD:|sd-uart|command RX|flash\.json|\bGET\b|\bPUT\b|RESET)") { return "Cyan" }
    if ($s -match "(?i)(esp-uart|ets Jan|boot mode|~ld|kesp:)") { return "Magenta" }
    if ($s -match "�|��|☻|☺") { return "DarkYellow" }
    if ($s -match "(?i)(\[sd\])") { return "DarkCyan" }
    if ($s -match "(?i)(\[main\])") { return "Gray" }
    return "White"
}

function Write-LogLine([string]$Line) {
    $clean = Strip-Ansi $Line
    Add-Content -Path $LogFile -Value $clean -Encoding UTF8
    Write-Host $clean -ForegroundColor (Get-LineColor $clean)
}

function Run-Step([string]$Title, [string]$WorkDir, [string]$Command) {
    Write-LogLine ""
    Write-LogLine "========== $Title ==========" 
    Write-LogLine "DIR: $WorkDir"
    Write-LogLine "CMD: $Command"

    Push-Location $WorkDir
    try {
        $cmdOut = & cmd.exe /d /c $Command 2>&1
        $code = $LASTEXITCODE
        foreach ($line in $cmdOut) {
            Write-LogLine ([string]$line)
        }
    } finally {
        Pop-Location
    }

    if ($null -eq $code) { $code = 0 }
    Write-LogLine "EXIT: $code"
    if ($code -ne 0) {
        throw "Step failed: $Title, exit code $code"
    }
}

$ExitCode = 0
try {
    Write-LogLine "=== K210 + ESP8285 one-click colored runner ==="
    Write-LogLine "Port: $Port"
    Write-LogLine "ESP repo: $EspRepo"
    Write-LogLine "K210 repo: $K210Repo"
    Write-LogLine "Log file: $LogFile"

    Run-Step "K210 git pull" $K210Repo "git pull"
    Run-Step "K210 build" $K210Repo "build_k210.bat"
    Run-Step "K210 flash" $K210Repo "flash_k210.bat $Port"

    Run-Step "ESP git pull" $EspRepo "git pull"
    Run-Step "ESP payload build" $EspRepo "build_esp_payload.bat"

    $uploadArgs = $Port
    if (-not $NoFullFlash) {
        $uploadArgs = "$uploadArgs --full-flash"
    }
    Run-Step "ESP payload upload through K210" $EspRepo "upload_esp_payload_uart.bat $uploadArgs"

    Write-LogLine ""
    Write-LogLine "ALL STEPS DONE"
} catch {
    $ExitCode = 1
    Write-LogLine ""
    Write-LogLine "FATAL: $($_.Exception.Message)"
} finally {
    Write-Host ""
    Write-Host "Saved log:" -ForegroundColor Cyan
    Write-Host $LogFile -ForegroundColor Cyan
    Write-Host ""
    Write-Host "Send this .log file to ChatGPT." -ForegroundColor Yellow
}

exit $ExitCode
