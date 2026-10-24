<#
.SYNOPSIS
  plugins/hello を雛形に、新しいプラグインのフォルダを作る。
.EXAMPLE
  .\scripts\new-plugin.ps1 -Name wordstat -MenuText "Word Stat"
#>
param(
    # DLL 名・フォルダ名になる。英小文字/数字/アンダースコア推奨
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z][A-Za-z0-9_]*$')][string]$Name,
    [string]$MenuText = $Name
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$src = Join-Path $root 'plugins\hello'
$dst = Join-Path $root "plugins\$Name"
if (Test-Path $dst) { throw "既に存在します: $dst" }

New-Item -ItemType Directory $dst | Out-Null
Copy-Item "$src\resource.h", "$src\version.h", "$src\toolbar.bmp" $dst
Copy-Item "$src\hello.cpp" "$dst\$Name.cpp"
Copy-Item "$src\hello.rc" "$dst\$Name.rc"
Copy-Item "$src\hello.vcxproj" "$dst\$Name.vcxproj"

$utf8 = [Text.UTF8Encoding]::new($false)
function Edit-File($path, [scriptblock]$transform) {
    $text = [IO.File]::ReadAllText($path, $utf8)
    [IO.File]::WriteAllText($path, (& $transform $text), $utf8)
}

$guid = [guid]::NewGuid().ToString().ToUpper()
Edit-File "$dst\$Name.vcxproj" {
    param($t)
    $t -replace '\{[0-9A-F-]{36}\}', "{$guid}" `
       -replace '<RootNamespace>hello</RootNamespace>', "<RootNamespace>$Name</RootNamespace>" `
       -replace 'Include="hello\.(cpp|rc)"', "Include=`"$Name.`$1`""
}
Edit-File "$dst\version.h" {
    param($t)
    $t -replace '"hello\.dll"', "`"$Name.dll`""
}
Edit-File "$dst\$Name.rc" {
    param($t)
    $t -replace 'IDS_MENU_TEXT(\s+)"Hello World"', "IDS_MENU_TEXT`$1`"$MenuText`"" `
       -replace '(IDS_STATUS_MESSAGE\s+)".*"', "`$1`"$MenuText`"" `
       -replace 'EmEditor plug-in: Hello World', "EmEditor plug-in: $MenuText"
}
Edit-File "$dst\$Name.cpp" {
    param($t)
    $t -replace '(?m)^// Hello World プラグイン。.*\r?\n//\r?\n', "// $MenuText プラグイン`r`n//`r`n"
}

Set-Content "$dst\README.md" -Encoding utf8 -Value @"
# $Name

$MenuText

## 機能

TODO

## 使い方

TODO
"@

& (Join-Path $PSScriptRoot 'update-sln.ps1')

Write-Host "作成: $dst" -ForegroundColor Green
Write-Host "次: .\scripts\build.ps1 -Plugin $Name"
