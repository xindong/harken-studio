[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateNotNullOrEmpty()]
    [string]$Command,

    [Parameter(Position = 1)]
    [ValidateNotNullOrEmpty()]
    [string]$ArgsJson = "{}",

    [string]$RequestId = "",

    [ValidateSet("127.0.0.1", "localhost", "::1")]
    [string]$HostName = "127.0.0.1",

    [ValidateRange(1, 65535)]
    [int]$Port = 9527,

    [ValidateRange(1, 300000)]
    [int]$ConnectTimeoutMs = 30000,

    [ValidateRange(1, 300000)]
    [int]$ResponseTimeoutMs = 30000,

    [switch]$AsObject
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 3.0

try {
    $argsObject = $ArgsJson | ConvertFrom-Json
}
catch {
    throw "ArgsJson must be valid JSON: $($_.Exception.Message)"
}

$trimmedArgsJson = $ArgsJson.TrimStart()
if (-not $trimmedArgsJson.StartsWith("{")) {
    throw "ArgsJson must contain a JSON object."
}

if ([string]::IsNullOrWhiteSpace($RequestId)) {
    $RequestId = [Guid]::NewGuid().ToString("N")
}

$request = [ordered]@{
    cmd = $Command
    args = $argsObject
    request_id = $RequestId
}

$client = $null
$stream = $null
$writer = $null
$reader = $null
$deadline = [DateTime]::UtcNow.AddMilliseconds($ConnectTimeoutMs)
$lastConnectError = $null

do {
    $candidate = [System.Net.Sockets.TcpClient]::new()
    try {
        $candidate.Connect($HostName, $Port)
        $client = $candidate
        break
    }
    catch {
        $lastConnectError = $_.Exception.Message
        $candidate.Dispose()
        if ([DateTime]::UtcNow -ge $deadline) {
            break
        }
        Start-Sleep -Milliseconds 100
    }
} while ($true)

if ($null -eq $client) {
    throw "Harken Studio IPC did not become available at ${HostName}:$Port " +
          "within $ConnectTimeoutMs ms. Last error: $lastConnectError"
}

try {
    $stream = $client.GetStream()
    $stream.ReadTimeout = $ResponseTimeoutMs
    $stream.WriteTimeout = $ResponseTimeoutMs

    $utf8 = [System.Text.UTF8Encoding]::new($false)
    $writer = [System.IO.StreamWriter]::new($stream, $utf8, 4096, $true)
    $writer.NewLine = "`n"
    $writer.AutoFlush = $true
    $reader = [System.IO.StreamReader]::new(
        $stream, $utf8, $false, 4096, $true)

    $writer.WriteLine(($request | ConvertTo-Json -Depth 100 -Compress))
    $responseJson = $reader.ReadLine()
    if ([string]::IsNullOrWhiteSpace($responseJson)) {
        throw "Harken Studio IPC returned an empty response."
    }

    try {
        $responseObject = $responseJson | ConvertFrom-Json
    }
    catch {
        throw "Harken Studio IPC returned invalid JSON: $responseJson"
    }

    if ($null -eq $responseObject -or
        $responseObject.PSObject.Properties.Name -notcontains "status") {
        throw "Harken Studio IPC response has no status field: $responseJson"
    }

    if ($responseObject.status -ne "ok") {
        $code = if ($responseObject.PSObject.Properties.Name -contains "code") {
            [string]$responseObject.code
        }
        else {
            "UNKNOWN_ERROR"
        }
        $message = if ($responseObject.PSObject.Properties.Name -contains "message") {
            [string]$responseObject.message
        }
        else {
            "No error message was returned."
        }
        throw "Harken Studio IPC command '$Command' failed: $code - $message"
    }

    if ($AsObject) {
        $responseObject
    }
    else {
        $responseJson
    }
}
finally {
    if ($null -ne $reader) { $reader.Dispose() }
    if ($null -ne $writer) { $writer.Dispose() }
    if ($null -ne $stream) { $stream.Dispose() }
    if ($null -ne $client) { $client.Dispose() }
}
