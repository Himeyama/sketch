# compile.ps1 / upload.ps1 から dot-source して使う、ツール探索のヘルパー。
# 単体では何もしない。

Set-StrictMode -Version Latest

# Arduino IDE 2.x は arduino-cli.exe を同梱しているが PATH には通さない。
function Get-ArduinoCli {
    $onPath = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    $candidates = @(
        "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe",
        "$env:ProgramFiles\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { return $candidate }
    }
    throw "arduino-cli が見つかりません。PATH に追加するか、Arduino IDE 2.x をインストールしてください。"
}

# tools/<name>/<version>/ のうち最も新しいものを返す。mklittlefs のように
# バージョンが "4.0.2-db0513a" 形式で [version] に変換できないものもある。
function Get-NewestVersionDir {
    param([Parameter(Mandatory)][string]$Parent)

    if (-not (Test-Path $Parent)) { throw "見つかりません: $Parent" }
    $dirs = @(Get-ChildItem -Path $Parent -Directory)
    if ($dirs.Count -eq 0) { throw "バージョンディレクトリがありません: $Parent" }

    $parsed = $dirs | ForEach-Object {
        $version = $null
        [void][version]::TryParse(($_.Name -split '-')[0], [ref]$version)
        [pscustomobject]@{ Dir = $_; Version = $version }
    }
    $sorted = $parsed | Sort-Object @{ Expression = 'Version'; Descending = $true }, @{ Expression = { $_.Dir.Name }; Descending = $true }
    return $sorted[0].Dir.FullName
}

function Get-Esp32PackageRoot {
    $root = "$env:LOCALAPPDATA\Arduino15\packages\esp32"
    if (-not (Test-Path $root)) {
        throw "ESP32 コアが見つかりません。Arduino IDE のボードマネージャで esp32 を入れてください。"
    }
    return $root
}

function Get-Esp32CoreDir {
    return Get-NewestVersionDir (Join-Path (Get-Esp32PackageRoot) 'hardware\esp32')
}

function Get-MkLittleFs {
    $dir = Get-NewestVersionDir (Join-Path (Get-Esp32PackageRoot) 'tools\mklittlefs')
    return (Join-Path $dir 'mklittlefs.exe')
}

function Get-EspTool {
    $dir = Get-NewestVersionDir (Join-Path (Get-Esp32PackageRoot) 'tools\esptool_py')
    return (Join-Path $dir 'esptool.exe')
}

# FQBN の "PartitionScheme=huge_app" を取り出す。未指定ならコア既定の default。
function Get-PartitionScheme {
    param([Parameter(Mandatory)][string]$Fqbn)

    $parts = $Fqbn -split ':', 4
    if ($parts.Count -lt 4) { return 'default' }

    foreach ($option in ($parts[3] -split ',')) {
        $pair = $option -split '=', 2
        if ($pair.Count -eq 2 -and $pair[0] -eq 'PartitionScheme') { return $pair[1] }
    }
    return 'default'
}

# パーティション表からファイルシステム領域のオフセットとサイズを読む。
# ここを固定値にすると、Partition Scheme を変えたときに黙って壊れる。
function Get-FilesystemPartition {
    param([Parameter(Mandatory)][string]$Fqbn)

    $scheme = Get-PartitionScheme -Fqbn $Fqbn
    $csv = Join-Path (Get-Esp32CoreDir) "tools\partitions\$scheme.csv"
    if (-not (Test-Path $csv)) { throw "パーティション表が見つかりません: $csv" }

    foreach ($line in (Get-Content $csv)) {
        if ($line -match '^\s*#' -or -not $line.Trim()) { continue }

        $columns = ($line -split ',') | ForEach-Object { $_.Trim() }
        if ($columns.Count -lt 5) { continue }
        if ($columns[1] -ne 'data') { continue }
        if ($columns[2] -notin @('spiffs', 'littlefs')) { continue }

        return [pscustomobject]@{
            Name   = $columns[0]
            Offset = [Convert]::ToInt64($columns[3], 16)
            Size   = [Convert]::ToInt64($columns[4], 16)
            Csv    = $csv
        }
    }
    throw "$scheme.csv にファイルシステム用パーティションがありません。Partition Scheme を見直してください。"
}

function Resolve-SerialPort {
    param([Parameter(Mandatory)][string]$ArduinoCli)

    $output = & $ArduinoCli board list --format json | Out-String
    if ($LASTEXITCODE -ne 0) { throw "arduino-cli board list に失敗しました。" }

    $parsed = $output | ConvertFrom-Json
    $detected = @()
    if ($parsed.PSObject.Properties.Name -contains 'detected_ports') {
        $detected = @($parsed.detected_ports)
    } else {
        $detected = @($parsed)
    }

    $serial = @($detected | Where-Object { $_.port.protocol -eq 'serial' })
    if ($serial.Count -eq 0) {
        throw "シリアルポートが見つかりません。ESP32 を接続するか -Port COM3 のように指定してください。"
    }
    if ($serial.Count -gt 1) {
        $addresses = ($serial | ForEach-Object { $_.port.address }) -join ', '
        throw "シリアルポートが複数あります ($addresses)。-Port で指定してください。"
    }
    return $serial[0].port.address
}

# 直前のネイティブコマンドの終了コードを見て落とす。
function Assert-LastExitCode {
    param([Parameter(Mandatory)][string]$What)
    if ($LASTEXITCODE -ne 0) { throw "$What に失敗しました (exit $LASTEXITCODE)。" }
}
