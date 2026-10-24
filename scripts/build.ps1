<#
.SYNOPSIS
  plugins/ 以下のプラグインをビルドする。
.EXAMPLE
  .\scripts\build.ps1                         # 全プラグイン Release|x64
  .\scripts\build.ps1 -Plugin hello -Configuration Debug
  .\scripts\build.ps1 -Platform Win32         # 32bit 版 EmEditor 向け
#>
param(
    [string]$Plugin = '*',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [ValidateSet('x64', 'Win32')][string]$Platform = 'x64'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild が見つかりません。Visual Studio (C++ によるデスクトップ開発) をインストールしてください。' }

& (Join-Path $PSScriptRoot 'fetch-deps.ps1')   # WebView2 SDK など、git に入れていない依存物

$projects = Get-ChildItem (Join-Path $root "plugins\$Plugin\*.vcxproj")
if (-not $projects) { throw "プロジェクトが見つかりません: plugins\$Plugin" }

$failed = @()
foreach ($proj in $projects) {
    Write-Host "==> $($proj.BaseName) [$Configuration|$Platform]" -ForegroundColor Cyan
    & $msbuild $proj.FullName -nologo -verbosity:minimal -m "-p:Configuration=$Configuration" "-p:Platform=$Platform"
    if ($LASTEXITCODE -ne 0) { $failed += $proj.BaseName }
}

if ($failed) { throw "ビルド失敗: $($failed -join ', ')" }
Write-Host "出力先: $(Join-Path $root "out\$Platform\$Configuration")" -ForegroundColor Green
