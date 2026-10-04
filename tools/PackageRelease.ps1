#requires -Version 7.0
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
    & cmake --preset windows-x64
    if ($LASTEXITCODE) { throw 'Release configuration failed' }
    & cmake --build --preset release --parallel
    if ($LASTEXITCODE) { throw 'Release build failed' }
    & ctest --preset release --output-on-failure
    if ($LASTEXITCODE) { throw 'Release tests failed' }

    $build = Join-Path $repo 'build/windows-x64-gl32'
    $cache = Get-Content (Join-Path $build 'CMakeCache.txt')
    $package = ($cache | Where-Object { $_ -match '^GAMESPEED_PACKAGE_DIR:STRING=' }) -replace '^GAMESPEED_PACKAGE_DIR:STRING=', ''
    if (-not $package) { throw 'Missing package directory in CMake cache' }
    $package = $package.Replace('$<CONFIG>', 'Release')
    $manifest = Get-Content (Join-Path $package 'plugin.json') -Raw | ConvertFrom-Json
    $version = $manifest.version
    if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Unexpected release version' }
    $dist = Join-Path $repo 'dist'
    New-Item -ItemType Directory -Path $dist -Force | Out-Null
    $archive = Join-Path $dist "MCDK-GameSpeed-$version-windows-x64.zip"
    if (Test-Path -LiteralPath $archive) {
        $archive = Join-Path $dist "MCDK-GameSpeed-$version-windows-x64-$(Get-Date -Format yyyyMMdd-HHmmss).zip"
    }
    # A fresh directory and an explicit allowlist keep diagnostics, test DLLs,
    # local configuration and stale build outputs out of distributable files.
    $stage = Join-Path $dist ('.staging-' + [guid]::NewGuid().ToString('N'))
    $root = Join-Path $stage 'game-speed'
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    foreach ($file in @('game_speed_plugin.dll', 'gamespeed_hook.dll', 'plugin.json', 'assets', 'licenses')) {
        Copy-Item -LiteralPath (Join-Path $package $file) -Destination $root -Recurse
    }
    Copy-Item -LiteralPath (Join-Path $build 'bin/Release/gamespeed.exe') -Destination $root
    Copy-Item -LiteralPath (Join-Path $repo 'docs/Release-README.md') -Destination (Join-Path $root 'README.md')
    Copy-Item -LiteralPath (Join-Path $repo 'docs/THIRD-PARTY-NOTICES.txt') -Destination $root
    Copy-Item -LiteralPath (Join-Path $repo 'docs/NLOHMANN-JSON-MIT.txt') -Destination (Join-Path $root 'licenses')
    $example = Get-Content 'examples/mcdev.plugins.json' -Raw | ConvertFrom-Json
    $example.plugins[0].path = './plugins/game-speed'
    $example | ConvertTo-Json -Depth 10 | Set-Content (Join-Path $root 'mcdev.plugins.example.json') -Encoding utf8
    # Preserve asset provenance without publishing a developer's machine path.
    $provenancePath = Join-Path $root 'assets/oreui/provenance.json'
    $provenance = Get-Content $provenancePath -Raw | ConvertFrom-Json
    $provenance.source = 'User-provided oreui-unpacked/test/hbui'
    $provenance | ConvertTo-Json -Depth 20 | Set-Content $provenancePath -Encoding utf8
    $fontLicenses = Get-Content 'external/RmlUi/Samples/assets/LICENSE.txt' -Raw
    $ofl = [regex]::Match($fontLicenses, '(?s)SIL OPEN FONT LICENSE Version 1\.1.*?OTHER DEALINGS IN THE FONT SOFTWARE\.')
    if (-not $ofl.Success) { throw 'Cannot locate the bundled SIL OFL text' }
    $ofl.Value | Set-Content (Join-Path $root 'licenses/SIL-OFL-1.1.txt') -Encoding utf8

    # Verify the shipped PE architecture and external runtime dependencies.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $dumpbin = Get-ChildItem (Join-Path $vs 'VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe') | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $dumpbin) { throw 'Cannot find dumpbin for release dependency validation' }
    $dependencies = @{}
    foreach ($name in @('game_speed_plugin.dll', 'gamespeed_hook.dll', 'gamespeed.exe')) {
        $file = Join-Path $root $name
        $bytes = [IO.File]::ReadAllBytes($file)
        $pe = [BitConverter]::ToInt32($bytes, 0x3c)
        if ([BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550 -or
            [BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664 -or
            [BitConverter]::ToUInt16($bytes, $pe + 24) -ne 0x20b) { throw "$name is not PE32+ AMD64" }
        $imports = & $dumpbin.FullName /dependents $file
        if ($LASTEXITCODE) { throw "Cannot inspect $name" }
        $dlls = @($imports | Where-Object { $_ -match '^\s+[\w.-]+\.dll\s*$' } | ForEach-Object { $_.Trim() })
        foreach ($dll in $dlls) {
            if ($dll -notmatch '^(KERNEL32|USER32|GDI32|OPENGL32|IMM32|ADVAPI32|COMCTL32)\.dll$') {
                throw "Review unexpected runtime dependency: $name -> $dll"
            }
        }
        $dependencies[$name] = $dlls
    }
    # Exercise relocated DLLs, not merely their original build-tree copies.
    & (Join-Path $build 'Release/gamespeed_host_plugin_tests.exe') (Join-Path $root 'game_speed_plugin.dll')
    if ($LASTEXITCODE) { throw 'Packaged host plugin validation failed' }
    & (Join-Path $build 'Release/gamespeed_integration_tests.exe') (Join-Path $root 'gamespeed_hook.dll')
    if ($LASTEXITCODE) { throw 'Packaged injection/UI validation failed' }
    & (Join-Path $root 'gamespeed.exe')
    if ($LASTEXITCODE) { throw 'Packaged controller validation failed' }

    $commit = (& git rev-parse HEAD).Trim()
    $dirty = [bool](& git status --porcelain)
    $submodules = @(& git submodule status)
    [ordered]@{
        version = $version; platform = 'windows-x64'; configuration = 'Release'
        builtAtUtc = [DateTime]::UtcNow.ToString('o'); sourceCommit = $commit
        includesUncommittedChanges = $dirty; submodules = $submodules
        supportedGameVersion = '3.10.0.420447'; runtime = 'static MSVC CRT'
        dependencies = $dependencies
        validation = @('CTest: 6 passed', 'Relocated host plugin ABI passed', 'Relocated injection and OpenGL UI passed', 'Controller smoke test passed')
    } | ConvertTo-Json -Depth 10 | Set-Content (Join-Path $root 'BUILD-INFO.json') -Encoding utf8

    $hashes = [ordered]@{}
    foreach ($file in Get-ChildItem $root -File -Recurse | Sort-Object FullName) {
        $relative = [IO.Path]::GetRelativePath($root, $file.FullName).Replace('\', '/')
        $hashes[$relative] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    @($hashes.GetEnumerator() | ForEach-Object { "$($_.Value)  $($_.Key)" }) |
        Set-Content (Join-Path $root 'SHA256SUMS.txt') -Encoding utf8
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($root, $archive, [IO.Compression.CompressionLevel]::Optimal, $true)
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        foreach ($entry in $zip.Entries) {
            if ($entry.FullName.EndsWith('/')) { continue }
            $relative = $entry.FullName.Substring('game-speed/'.Length)
            if ($relative -eq 'SHA256SUMS.txt') { continue }
            if (-not $hashes.Contains($relative)) { throw "Unexpected ZIP entry: $relative" }
            $stream = $entry.Open()
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $hash = [Convert]::ToHexString($sha.ComputeHash($stream)).ToLowerInvariant() }
            finally { $sha.Dispose(); $stream.Dispose() }
            if ($hash -ne $hashes[$relative]) { throw "ZIP checksum mismatch: $relative" }
            $hashes.Remove($relative)
        }
        if ($hashes.Count) { throw 'Missing files in release ZIP' }
    } finally { $zip.Dispose() }
    $archiveHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    "$archiveHash  $([IO.Path]::GetFileName($archive))" | Set-Content "$archive.sha256" -Encoding ascii
    Write-Host "Release ready: $archive"
    Write-Host "SHA256: $archiveHash"
} finally { Pop-Location }
