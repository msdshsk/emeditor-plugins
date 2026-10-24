<#
.SYNOPSIS
  git に入れていない依存物を third_party/ に取得する。
.DESCRIPTION
  サイズが大きい依存物（WebView2 SDK、Mermaid など）はリポジトリに入れず、版とハッシュを固定して取得する。
  build.ps1 から毎回呼ばれ、取得済みのものは飛ばす。-Force で取り直す。

  - WebView2 SDK: NuGet。SHA-256 を照合
  - npm パッケージ: レジストリの integrity（sha512）を照合し、必要なファイルだけ取り出す
#>
param([switch]$Force)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$tp = Join-Path $root 'third_party'

function New-TempDir {
    $d = Join-Path $env:TEMP "fetch-deps-$(Get-Random)"
    New-Item -ItemType Directory $d | Out-Null
    return $d
}

# ---- WebView2 SDK -----------------------------------------------------------
function Get-WebView2Sdk {
    $version = '1.0.4314.35'
    $sha256 = '90778DEB1B77E36292828B4B5833C9255C27533F40E0DDD55891AE718944EDC4'
    $dest = Join-Path $tp 'webview2'
    if ((Test-Path "$dest\include\WebView2.h") -and -not $Force) { return }

    $tmp = New-TempDir
    try {
        $pkg = Join-Path $tmp 'webview2.zip'
        Write-Host "WebView2 SDK $version を取得中..." -ForegroundColor Cyan
        Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$version/microsoft.web.webview2.$version.nupkg" -OutFile $pkg
        $actual = (Get-FileHash $pkg -Algorithm SHA256).Hash
        if ($actual -ne $sha256) { throw "WebView2 SDK のハッシュが一致しません: $actual（期待値 $sha256）" }
        Expand-Archive $pkg "$tmp\pkg"
        if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
        New-Item -ItemType Directory "$dest\include", "$dest\x64", "$dest\x86" | Out-Null
        Copy-Item "$tmp\pkg\build\native\include\*.h" "$dest\include\"
        Copy-Item "$tmp\pkg\build\native\x64\WebView2LoaderStatic.lib" "$dest\x64\"
        Copy-Item "$tmp\pkg\build\native\x86\WebView2LoaderStatic.lib" "$dest\x86\"
        Copy-Item "$tmp\pkg\LICENSE.txt", "$tmp\pkg\NOTICE.txt" "$dest\"
        Set-Content "$dest\VERSION.txt" "Microsoft.Web.WebView2 $version (SHA-256 $sha256)"
    }
    finally { Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue }
}

# ---- npm パッケージ ----------------------------------------------------------
# files: パッケージ内のパス → third_party/<dest>/ 以下の名前
$npmPackages = @(
    @{ Name = '@highlightjs/cdn-assets'; Version = '11.12.0'; Dest = 'highlight'
       Integrity = 'sha512-KvOKXODaiFmId9xaq3xc5xCL66wVLUuOngDbO9B/kewbFTqdGbn2nJxNhN3H5R1cgDTVj6R8vH0zgiNDEGjpDw=='
       Files = [ordered]@{ 'highlight.min.js' = 'highlight.min.js'; 'styles/github.min.css' = 'github.min.css'; 'styles/github-dark.min.css' = 'github-dark.min.css'; 'LICENSE' = 'LICENSE' } }
    @{ Name = 'katex'; Version = '0.19.1'; Dest = 'katex'
       Integrity = 'sha512-jG8uKntjSEcm2EpQOh0XturpPuuC9r3Sq5HLHdsFdWZT27E672SF19QEi+of0/YeOy15lDgRhKvr5614uRrEfA=='
       Files = [ordered]@{ 'dist/katex.min.js' = 'katex.min.js'; 'dist/katex.min.css' = 'katex.min.css'; 'dist/fonts' = 'fonts'; 'LICENSE' = 'LICENSE' } }
    @{ Name = 'mermaid'; Version = '12.2.0'; Dest = 'mermaid'
       Integrity = 'sha512-w7nr/SFrLO7BaVTs3dQTvYSV6nYSBj0Yy9DjBhsIJtAcHqRzXSq4Pg32ujHZ/AdWsAMNEvvhbm5dt9E2tOiHzw=='
       Files = [ordered]@{ 'dist/mermaid.min.js' = 'mermaid.min.js'; 'LICENSE' = 'LICENSE' } }
    @{ Name = 'marked-footnote'; Version = '1.4.0'; Dest = 'marked-footnote'
       Integrity = 'sha512-fZTxAhI1TcLEs5UOjCfYfTHpyKGaWQevbxaGTEA68B51l7i87SctPFtHETYqPkEN0ka5opvy4Dy1l/yXVC+hmg=='
       Files = [ordered]@{ 'dist/index.umd.js' = 'marked-footnote.umd.js'; 'package.json' = 'package.json' } }   # LICENSE ファイルは無い（package.json に MIT）
)

function Get-NpmPackage($p) {
    $dest = Join-Path $tp $p.Dest
    $stamp = Join-Path $dest 'VERSION.txt'
    $want = "$($p.Name) $($p.Version) ($($p.Integrity))"
    if ((Test-Path $stamp) -and ((Get-Content $stamp -Raw).Trim() -eq $want) -and -not $Force) { return $false }

    $tmp = New-TempDir
    try {
        Write-Host "$($p.Name)@$($p.Version) を取得中..." -ForegroundColor Cyan
        $short = $p.Name.Split('/')[-1]
        $tgz = Join-Path $tmp 'pkg.tgz'
        Invoke-WebRequest "https://registry.npmjs.org/$($p.Name)/-/$short-$($p.Version).tgz" -OutFile $tgz
        $sri = 'sha512-' + [Convert]::ToBase64String([Security.Cryptography.SHA512]::Create().ComputeHash([IO.File]::ReadAllBytes($tgz)))
        if ($sri -ne $p.Integrity) { throw "$($p.Name) の integrity が一致しません: $sri" }

        New-Item -ItemType Directory "$tmp\x" | Out-Null
        tar -xzf $tgz -C "$tmp\x"
        if ($LASTEXITCODE -ne 0) { throw "$($p.Name) を展開できません" }
        if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
        New-Item -ItemType Directory $dest | Out-Null
        foreach ($src in $p.Files.Keys) {
            Copy-Item (Join-Path "$tmp\x\package" $src) (Join-Path $dest $p.Files[$src]) -Recurse
        }
        Set-Content $stamp $want
        return $true
    }
    finally { Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue }
}

# KaTeX の CSS はフォントを相対パス（fonts/*.woff2）で参照する。プラグインは DLL 1 つで配るので、
# woff2 を data: URI として埋め込んだ CSS（katex.inline.css）を作る。woff / ttf の代替指定は削る
function New-KatexInlineCss {
    $dir = Join-Path $tp 'katex'
    $css = Get-Content "$dir\katex.min.css" -Raw
    $css = [regex]::Replace($css, 'url\(fonts/([^)]+?\.woff2)\)', {
        param($m)
        $b64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path "$dir\fonts" $m.Groups[1].Value)))
        "url(data:font/woff2;base64,$b64)"
    })
    $css = [regex]::Replace($css, ',url\(fonts/[^)]+?\.(woff|ttf)\) format\("[^"]+"\)', '')
    [IO.File]::WriteAllText("$dir\katex.inline.css", $css, [Text.UTF8Encoding]::new($false))
}

Get-WebView2Sdk
foreach ($p in $npmPackages) {
    $updated = Get-NpmPackage $p
    if ($p.Name -eq 'katex' -and ($updated -or -not (Test-Path (Join-Path $tp 'katex\katex.inline.css')))) { New-KatexInlineCss }
}
