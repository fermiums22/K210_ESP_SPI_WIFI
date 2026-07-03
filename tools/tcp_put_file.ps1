param(
    [Parameter(Mandatory=$true, Position=0)] [string]$HostName,
    [Parameter(Mandatory=$true, Position=1)] [string]$LocalFile,
    [Parameter(Mandatory=$false, Position=2)] [string]$RemoteName,
    [int]$Port = 7777,
    [int]$TimeoutMs = 30000
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $LocalFile -PathType Leaf)) {
    throw "File not found: $LocalFile"
}
if ([string]::IsNullOrWhiteSpace($RemoteName)) {
    $RemoteName = Split-Path -Leaf $LocalFile
}

$fileInfo = Get-Item -LiteralPath $LocalFile
$size = [int64]$fileInfo.Length
Write-Host "TCP PUT $LocalFile -> ${HostName}:$Port/$RemoteName ($size bytes) [PowerShell/.NET]"

$client = [System.Net.Sockets.TcpClient]::new()
$iar = $client.BeginConnect($HostName, $Port, $null, $null)
if (-not $iar.AsyncWaitHandle.WaitOne($TimeoutMs)) {
    $client.Close()
    throw "TCP connect timeout to ${HostName}:$Port"
}
$client.EndConnect($iar)
$client.NoDelay = $true

$stream = $client.GetStream()
$stream.ReadTimeout = $TimeoutMs
$stream.WriteTimeout = $TimeoutMs

$header = [System.Text.Encoding]::ASCII.GetBytes("PUT $RemoteName $size`n")
$stream.Write($header, 0, $header.Length)

$buf = New-Object byte[] 16384
$fs = [System.IO.File]::OpenRead((Resolve-Path -LiteralPath $LocalFile))
$sent = 0L
$sw = [System.Diagnostics.Stopwatch]::StartNew()
try {
    while (($n = $fs.Read($buf, 0, $buf.Length)) -gt 0) {
        $stream.Write($buf, 0, $n)
        $sent += $n
        if (($sent -eq $size) -or (($sent % 32768) -eq 0)) {
            $kbps = ($sent / 1024.0) / [Math]::Max($sw.Elapsed.TotalSeconds, 0.001)
            Write-Host ("progress {0}/{1} {2:N1} KiB/s" -f $sent, $size, $kbps)
        }
    }
}
finally {
    $fs.Close()
}

$responseBytes = New-Object System.Collections.Generic.List[byte]
while ($true) {
    $b = $stream.ReadByte()
    if ($b -lt 0) { break }
    if ($b -eq 10) { break }
    if ($b -ne 13) { $responseBytes.Add([byte]$b) }
}
$response = [System.Text.Encoding]::UTF8.GetString($responseBytes.ToArray())
$elapsed = [Math]::Max($sw.Elapsed.TotalSeconds, 0.001)
Write-Host ("response: {0} ({1:N1}s, {2:N1} KiB/s)" -f ($(if ($response) { $response } else { '<empty>' }), $elapsed, (($size / 1024.0) / $elapsed))

$stream.Close()
$client.Close()

if (-not $response.StartsWith('OK')) {
    exit 1
}
exit 0
