<#
.SYNOPSIS
  プラグインを x64 / x86 でビルドし、配布用の zip と SHA-256 を dist/ に作る。
.DESCRIPTION
  Release の GitHub Actions（.github/workflows/release.yml）から呼ばれる。ローカルでも同じものを作れる。
  version.h の PLUGIN_VER_STR と -Version が一致しないときは止める（タグと DLL の版ずれを防ぐ）。

  dist/<product>-<version>.zip
    x64/<plugin>.dll       … 64-bit 版 EmEditor 用
    x86/<plugin>.dll       … 32-bit 版 EmEditor 用
    README.md / LICENSE / THIRD_PARTY_NOTICES.md
  dist/<product>-<version>.sha256.txt … zip と各 DLL の SHA-256
.EXAMPLE
  .\scripts\package.ps1 -Plugin claude_chat -Version 1.0.0
#>
param(
    [Parameter(Mandatory)][string]$Plugin,      # plugins/ のフォルダ名（例: claude_chat）
    [Parameter(Mandatory)][string]$Version      # 例: 1.0.0
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$pluginDir = Join-Path $root "plugins\$Plugin"
if (-not (Test-Path $pluginDir)) { throw "プラグインがありません: $pluginDir" }

# version.h と照合
$verLine = Select-String -Path "$pluginDir\version.h" -Pattern 'PLUGIN_VER_STR\s+"([^"]+)"'
$fileVersion = $verLine.Matches[0].Groups[1].Value
if ($fileVersion -ne $Version) { throw "version.h の版（$fileVersion）と指定の版（$Version）が一致しません" }

$product = $Plugin.Replace('_', '-')            # 配布物・タグの名前（claude-chat など）
$name = "$product-$Version"
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist $name
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory "$stage\x64", "$stage\x86" -Force | Out-Null

foreach ($p in @(@{ Platform = 'x64'; Dir = 'x64' }, @{ Platform = 'Win32'; Dir = 'x86' })) {
    & (Join-Path $PSScriptRoot 'build.ps1') -Plugin $Plugin -Configuration Release -Platform $p.Platform
    Copy-Item (Join-Path $root "out\$($p.Platform)\Release\$Plugin.dll") "$stage\$($p.Dir)\"
}
Copy-Item "$pluginDir\README.md" "$stage\README.md"
Copy-Item (Join-Path $root 'LICENSE'), (Join-Path $root 'THIRD_PARTY_NOTICES.md') $stage

$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip

$lines = @(
    "$((Get-FileHash $zip -Algorithm SHA256).Hash.ToLower())  $name.zip"
    "$((Get-FileHash "$stage\x64\$Plugin.dll" -Algorithm SHA256).Hash.ToLower())  x64/$Plugin.dll"
    "$((Get-FileHash "$stage\x86\$Plugin.dll" -Algorithm SHA256).Hash.ToLower())  x86/$Plugin.dll"
)
$sumFile = Join-Path $dist "$name.sha256.txt"
Set-Content $sumFile $lines -Encoding ascii

Write-Host "作成: $zip" -ForegroundColor Green
$lines | ForEach-Object { Write-Host "  $_" }
