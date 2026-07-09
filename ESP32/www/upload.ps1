<#
.SYNOPSIS
  compile.ps1 でビルドしてから ESP32 に書き込む。

.DESCRIPTION
  既定ではスケッチと LittleFS イメージの両方を書き込む。
  data/index.html だけ直したときは -FilesystemOnly が速い。

.EXAMPLE
  .\upload.ps1
  .\upload.ps1 -Port COM5
  .\upload.ps1 -FilesystemOnly
  .\upload.ps1 -SkipCompile    # build/ の成果物をそのまま書き込む
  .\upload.ps1 -Erase          # NVS ごと消して管理パスワードと Wi-Fi 設定を初期化
#>
[CmdletBinding()]
param(
    [string]$Port,
    [string]$Fqbn = 'esp32:esp32:esp32:PartitionScheme=huge_app',
    [int]$Baud = 921600,
    [switch]$SketchOnly,
    [switch]$FilesystemOnly,
    # build/ を捨ててからビルドし直す
    [switch]$Clean,
    # ビルドを省いて build/ の成果物をそのまま書き込む
    [switch]$SkipCompile,
    # フラッシュ全体を消去する。保存済みの Wi-Fi 設定と管理パスワード (NVS) も消える
    [switch]$Erase
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')

if ($SketchOnly -and $FilesystemOnly) {
    throw '-SketchOnly と -FilesystemOnly は同時に指定できません。'
}
if ($SkipCompile -and $Clean) {
    throw '-SkipCompile と -Clean は同時に指定できません。'
}

$buildDir = Join-Path $PSScriptRoot 'build'
$imagePath = Join-Path $buildDir 'littlefs.bin'
$sketchBin = Join-Path $buildDir 'www.ino.bin'

$uploadSketch = -not $FilesystemOnly
$uploadFilesystem = -not $SketchOnly

# 書き込むものだけをビルドする。ビルドが落ちたらデバイスには触れずに終わる。
# compile.ps1 は失敗すると throw するので、ここで終了コードを見る必要はない
if (-not $SkipCompile) {
    & (Join-Path $PSScriptRoot 'compile.ps1') `
        -Fqbn $Fqbn -Clean:$Clean -SketchOnly:$SketchOnly -FilesystemOnly:$FilesystemOnly
    Write-Host ""
}

if (-not $Port) { $Port = Resolve-SerialPort -ArduinoCli (Get-ArduinoCli) }

if ($uploadSketch -and -not (Test-Path $sketchBin)) {
    throw "$sketchBin がありません。-SkipCompile を外すか .\compile.ps1 を実行してください。"
}
if ($uploadFilesystem -and -not (Test-Path $imagePath)) {
    throw "$imagePath がありません。-SkipCompile を外すか .\compile.ps1 を実行してください。"
}

Write-Host "Port: $Port"
Write-Host "FQBN: $Fqbn"
Write-Host ""

if ($Erase) {
    Write-Host "--- フラッシュを消去 (NVS も消えます)" -ForegroundColor Yellow
    & (Get-EspTool) --port $Port --baud $Baud erase-flash
    Assert-LastExitCode 'フラッシュの消去'
    Write-Host ""
}

if ($uploadSketch) {
    Write-Host "--- スケッチを書き込み"
    & (Get-ArduinoCli) upload --fqbn $Fqbn --port $Port --input-dir $buildDir
    Assert-LastExitCode 'スケッチの書き込み'
    Write-Host ""
}

if ($uploadFilesystem) {
    $partition = Get-FilesystemPartition -Fqbn $Fqbn
    $imageSize = (Get-Item $imagePath).Length

    # compile.ps1 と upload.ps1 で -Fqbn が食い違うと、ここでサイズが合わなくなる
    if ($imageSize -gt $partition.Size) {
        throw ("littlefs.bin ({0:N0} bytes) がパーティション ({1:N0} bytes) を超えています。compile.ps1 を同じ -Fqbn で実行してください。" -f $imageSize, $partition.Size)
    }

    Write-Host ("--- LittleFS を書き込み (0x{0:X})" -f $partition.Offset)
    $offsetHex = '0x{0:X}' -f $partition.Offset
    & (Get-EspTool) --port $Port --baud $Baud write-flash $offsetHex $imagePath
    Assert-LastExitCode 'LittleFS の書き込み'
    Write-Host ""
}

Write-Host "完了。setup.html をブラウザで開いてください。" -ForegroundColor Green
