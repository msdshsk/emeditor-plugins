<#
.SYNOPSIS
  plugins/*/*.vcxproj から EmEditorPlugins.sln を再生成する。
.DESCRIPTION
  dotnet sln は vcxproj を読めない（VCTargetsPath が解決できない）ため、自前で生成する。
  .sln は手で編集せず、プラグインを追加・削除したらこのスクリプトを実行する。
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vcxprojType = '{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}'
$folderType = '{2150E333-8FDC-42A3-9474-1A3956D46DE8}'
$folderGuid = '{3E0F5A8C-6B1D-4C2E-9A57-0E4B6C1D2F70}'
$configs = @(
    @{ Sln = 'Debug|x64';   Proj = 'Debug|x64' },
    @{ Sln = 'Debug|x86';   Proj = 'Debug|Win32' },
    @{ Sln = 'Release|x64'; Proj = 'Release|x64' },
    @{ Sln = 'Release|x86'; Proj = 'Release|Win32' }
)

$projects = Get-ChildItem (Join-Path $root 'plugins\*\*.vcxproj') | Sort-Object Name | ForEach-Object {
    $guid = ([xml](Get-Content $_.FullName -Raw)).Project.PropertyGroup.ProjectGuid | Where-Object { $_ } | Select-Object -First 1
    if (-not $guid) { throw "ProjectGuid がありません: $($_.FullName)" }
    [pscustomobject]@{
        Name = $_.BaseName
        Path = [IO.Path]::GetRelativePath($root, $_.FullName)
        Guid = $guid.ToUpper()
    }
}

$sb = [Text.StringBuilder]::new()
$nl = "`r`n"
[void]$sb.Append($nl + 'Microsoft Visual Studio Solution File, Format Version 12.00' + $nl)
[void]$sb.Append('# Visual Studio Version 17' + $nl)
[void]$sb.Append('VisualStudioVersion = 17.0.31903.59' + $nl)
[void]$sb.Append('MinimumVisualStudioVersion = 10.0.40219.1' + $nl)
[void]$sb.Append("Project(`"$folderType`") = `"plugins`", `"plugins`", `"$folderGuid`"$nl")
[void]$sb.Append('EndProject' + $nl)
foreach ($p in $projects) {
    [void]$sb.Append("Project(`"$vcxprojType`") = `"$($p.Name)`", `"$($p.Path)`", `"$($p.Guid)`"$nl")
    [void]$sb.Append('EndProject' + $nl)
}
[void]$sb.Append('Global' + $nl)
[void]$sb.Append("`tGlobalSection(SolutionConfigurationPlatforms) = preSolution$nl")
foreach ($c in $configs) { [void]$sb.Append("`t`t$($c.Sln) = $($c.Sln)$nl") }
[void]$sb.Append("`tEndGlobalSection$nl")
[void]$sb.Append("`tGlobalSection(ProjectConfigurationPlatforms) = postSolution$nl")
foreach ($p in $projects) {
    foreach ($c in $configs) {
        [void]$sb.Append("`t`t$($p.Guid).$($c.Sln).ActiveCfg = $($c.Proj)$nl")
        [void]$sb.Append("`t`t$($p.Guid).$($c.Sln).Build.0 = $($c.Proj)$nl")
    }
}
[void]$sb.Append("`tEndGlobalSection$nl")
[void]$sb.Append("`tGlobalSection(SolutionProperties) = preSolution$nl")
[void]$sb.Append("`t`tHideSolutionNode = FALSE$nl")
[void]$sb.Append("`tEndGlobalSection$nl")
[void]$sb.Append("`tGlobalSection(NestedProjects) = preSolution$nl")
foreach ($p in $projects) { [void]$sb.Append("`t`t$($p.Guid) = $folderGuid$nl") }
[void]$sb.Append("`tEndGlobalSection$nl")
[void]$sb.Append('EndGlobal' + $nl)

# VS の .sln は UTF-8 BOM 付きが慣例
[IO.File]::WriteAllText((Join-Path $root 'EmEditorPlugins.sln'), $sb.ToString(), [Text.UTF8Encoding]::new($true))
Write-Host "EmEditorPlugins.sln を更新しました（$($projects.Count) プロジェクト）" -ForegroundColor Green
