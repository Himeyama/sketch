<#
.SYNOPSIS
  www スケッチをコンパイルし、data/ から LittleFS イメージを作る。

.DESCRIPTION
  成果物は build/ に出力される:
    build/www.ino.bin   スケッチ
    build/littlefs.bin  data/ の中身
  どちらも upload.ps1 が書き込む。

.EXAMPLE
  .\compile.ps1
  .\compile.ps1 -FilesystemOnly    # data/ から littlefs.bin だけ作り直す
  .\compile.ps1 -Fqbn esp32:esp32:esp32s3:PartitionScheme=huge_app
#>
[CmdletBinding()]
param(
    # BLE と Wi-Fi の同居でスケッチが約 1.7MB になり、既定の 1.3MB には収まらない
    [string]$Fqbn = 'esp32:esp32:esp32:PartitionScheme=huge_app',
    [switch]$SketchOnly,
    [switch]$FilesystemOnly,
    [switch]$Clean
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')

if ($SketchOnly -and $FilesystemOnly) {
    throw '-SketchOnly と -FilesystemOnly は同時に指定できません。'
}

$sketchDir = $PSScriptRoot
$dataDir = Join-Path $sketchDir 'data'
$buildDir = Join-Path $sketchDir 'build'
$imagePath = Join-Path $buildDir 'littlefs.bin'

$buildSketch = -not $FilesystemOnly
$buildFilesystem = -not $SketchOnly

Write-Host "FQBN: $Fqbn"
Write-Host ""

if ($Clean -and (Test-Path $buildDir)) {
    Remove-Item -Recurse -Force $buildDir
}

if ($buildSketch) {
    $arduinoCli = Get-ArduinoCli
    Write-Host "--- スケッチをコンパイル ($arduinoCli)"

    # --output-dir は指定ディレクトリを作り直すので、littlefs.bin は必ずこの後で生成する
    & $arduinoCli compile --fqbn $Fqbn --output-dir $buildDir $sketchDir
    Assert-LastExitCode 'コンパイル'
    Write-Host ""
}

if ($buildFilesystem) {
    if (-not (Test-Path $dataDir)) { throw "data/ がありません: $dataDir" }
    if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Force $buildDir | Out-Null }

    $partition = Get-FilesystemPartition -Fqbn $Fqbn
    Write-Host ("--- LittleFS イメージを作成 ({0} @ 0x{1:X} / {2:N0} bytes)" -f $partition.Name, $partition.Offset, $partition.Size)

    # ESP32 の LittleFS はブロック 4096 / ページ 256 固定
    & (Get-MkLittleFs) -c $dataDir -b 4096 -p 256 -s $partition.Size $imagePath
    Assert-LastExitCode 'LittleFS イメージの作成'

    Write-Host ("littlefs.bin: {0:N0} bytes" -f (Get-Item $imagePath).Length)
    Write-Host ""
}

Write-Host "ビルド完了。" -ForegroundColor Green
