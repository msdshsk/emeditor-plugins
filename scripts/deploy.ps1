<#
.SYNOPSIS
  ビルド済みプラグイン DLL を deploy\<Platform>\ にコピーする。
.DESCRIPTION
  EmEditor は PlugIns フォルダに DLL を置いただけでは認識しない（「プラグインの設定 > 追加」で登録が必要）。
  そこで、リポジトリ内の固定フォルダ deploy\<Platform>\ に配置し、EmEditor にはそのパスを 1 回だけ登録する。
  Debug/Release を切り替えても配置先のパスは変わらないので、再登録は不要。

  登録済みの DLL は EmEditor の起動中ロックされるので、上書きできなければ中断する。
  未登録の DLL（初回配置）は EmEditor を起動したままでもコピーできる。
.EXAMPLE
  .\scripts\deploy.ps1 -Plugin markdown_preview
  .\scripts\deploy.ps1 -Plugin markdown_preview -Configuration Debug   # .pdb も一緒にコピーする
  .\scripts\deploy.ps1 -Plugin *                            # ビルド済みの全プラグイン
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)][string]$Plugin,
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [ValidateSet('x64', 'Win32')][string]$Platform = 'x64'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$outDir = Join-Path $root "out\$Platform\$Configuration"
$deployDir = Join-Path $root "deploy\$Platform"

$dlls = Get-ChildItem (Join-Path $outDir "$Plugin.dll") -ErrorAction SilentlyContinue
if (-not $dlls) { throw "DLL がありません。先にビルドしてください: $outDir\$Plugin.dll" }

New-Item -ItemType Directory -Force $deployDir | Out-Null
foreach ($dll in $dlls) {
    $files = @($dll.FullName)
    $pdb = [IO.Path]::ChangeExtension($dll.FullName, '.pdb')
    $deployedPdb = Join-Path $deployDir (Split-Path $pdb -Leaf)
    if ($Configuration -eq 'Debug' -and (Test-Path $pdb)) {
        $files += $pdb
    } elseif (Test-Path $deployedPdb) {
        # Release を配置するときは、前回の Debug の .pdb を残さない
        if ($PSCmdlet.ShouldProcess($deployedPdb, 'Remove stale pdb')) { Remove-Item $deployedPdb }
    }
    foreach ($f in $files) {
        if ($PSCmdlet.ShouldProcess($deployDir, "Copy $(Split-Path $f -Leaf)")) {
            try {
                Copy-Item $f $deployDir -Force
            } catch [System.IO.IOException] {
                throw "上書きできません（EmEditor が DLL を使用中）。EmEditor をトレイ常駐も含めて終了してから再実行してください。`n$_"
            }
            Write-Host "コピー: $(Split-Path $f -Leaf) -> $deployDir" -ForegroundColor Green
        }
    }
}
Write-Host '初回のみ: EmEditor の「プラグイン > プラグインの設定 > 追加」で上記フォルダの DLL を登録する'
