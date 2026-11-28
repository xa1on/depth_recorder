[CmdletBinding()]
param(
    [string]$X64Build = 'build',
    [string]$Win32Build = 'build32',
    [string]$EncoderDistribution = 'build/ffmpeg/dist',
    [string]$Output = 'out',
    [switch]$IncludeWin32
)

$ErrorActionPreference = 'Stop'

function Write-ReleaseZip([string]$Directory, [string]$Destination) {
    [string[]]$files = Get-ChildItem -LiteralPath $Directory -Recurse -File -Force |
        ForEach-Object { [IO.Path]::GetRelativePath($Directory, $_.FullName) }
    [Array]::Sort($files, [StringComparer]::Ordinal)
    $base = Split-Path -Leaf $Directory
    $epoch = [DateTimeOffset]::new(1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
    $temporary = $Destination + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    try {
        $zip = [IO.Compression.ZipFile]::Open($temporary, [IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($file in $files) {
                $entry = $zip.CreateEntry($base + '/' + $file.Replace('\', '/'), [IO.Compression.CompressionLevel]::Optimal)
                $entry.LastWriteTime = $epoch
                $inputStream = [IO.File]::OpenRead((Join-Path $Directory $file))
                try {
                    $entryStream = $entry.Open()
                    try { $inputStream.CopyTo($entryStream) } finally { $entryStream.Dispose() }
                } finally { $inputStream.Dispose() }
            }
        } finally { $zip.Dispose() }
        [IO.File]::Move($temporary, $Destination, $true)
    } finally {
        if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
    }
}

$root = Split-Path -Parent $PSScriptRoot
Push-Location -LiteralPath $root
try {
    $x64 = (Resolve-Path -LiteralPath $X64Build).Path
    $builds = @{ x64 = $x64 }
    if ($IncludeWin32) { $builds.x86 = (Resolve-Path -LiteralPath $Win32Build).Path }
    $encoder = (Resolve-Path -LiteralPath $EncoderDistribution).Path
    $ffmpeg = Join-Path $encoder 'ffmpeg.exe'
    $expectedHash = ((Get-Content -LiteralPath (Join-Path $encoder 'ffmpeg.sha256') -Raw).Trim() -split '\s+')[0]
    if ((Get-FileHash -LiteralPath $ffmpeg -Algorithm SHA256).Hash -ine $expectedHash) {
        throw 'FFmpeg did not match the checksum from the pinned build.'
    }
    $buildConfig = & $ffmpeg -hide_banner -buildconf 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0 -or $buildConfig -match '--enable-nonfree' -or $buildConfig -notmatch '--enable-gpl') {
        throw 'FFmpeg must use the redistributable GPL build from tools/build-ffmpeg.sh.'
    }
    $encoders = & $ffmpeg -hide_banner -encoders 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0 -or $encoders -notmatch '\blibx264\b' -or $encoders -notmatch '\butvideo\b' -or
        $encoders -notmatch '\bh264_amf\b') {
        throw 'The package requires AMD AMF, the x264 software fallback, and UtVideo.'
    }
    $gpuInclude = Select-String -LiteralPath (Join-Path $x64 'CMakeCache.txt') -Pattern '^DEPTH_RECORDER_NVENC_INCLUDE:PATH=.+$' -Quiet
    if ($gpuInclude) {
        $demuxers = & $ffmpeg -hide_banner -demuxers 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0 -or $demuxers -notmatch '(?m)^\s*D\s+h264\s') {
            throw 'GPU recording requires the H.264 demuxer from the current tools/build-ffmpeg.sh.'
        }
    }
    foreach ($component in 'ffmpeg', 'x264', 'nv-codec-headers', 'amf') {
        if (!(Test-Path -LiteralPath (Join-Path $encoder "source/$component.tar.gz") -PathType Leaf)) {
            throw "Corresponding source for $component is missing."
        }
    }
    $bundledMinHook = Join-Path $root 'third-party/minhook.tar.gz'
    $hasBundledMinHook = Test-Path -LiteralPath $bundledMinHook -PathType Leaf
    $minhook = Join-Path $x64 $(if ($hasBundledMinHook) { '_deps/minhook' } else { '_deps/minhook-src' })
    $null = New-Item -ItemType Directory -Path $Output -Force
    $out = (Resolve-Path -LiteralPath $Output).Path
    $staging = Join-Path $out ("staging-" + [Guid]::NewGuid().ToString('N'))
    $portable = Join-Path $staging 'DepthRecorder'
    $source = Join-Path $staging 'DepthRecorder-source'
    $null = New-Item -ItemType Directory -Path $portable,$source -Force
    $licenses = Join-Path $portable 'licenses'
    $null = New-Item -ItemType Directory -Path $licenses

    foreach ($name in 'depth_recorder.exe', 'depth_hook.dll') {
        Copy-Item -LiteralPath (Join-Path $x64 "Release/$name") -Destination $portable
    }
    if ($IncludeWin32) {
        foreach ($name in 'inject32.exe', 'depth_hook32.dll') {
            Copy-Item -LiteralPath (Join-Path $builds.x86 "Release/$name") -Destination $portable
        }
    }
    foreach ($arch in $builds.Keys) {
        $layer = Join-Path $portable "vulkan/$arch"
        $null = New-Item -ItemType Directory -Path $layer -Force
        foreach ($name in 'VkLayer_depth_recorder.dll', 'VkLayer_depth_recorder.json') {
            Copy-Item -LiteralPath (Join-Path $builds[$arch] "Release/$name") -Destination $layer
        }
    }
    Copy-Item -LiteralPath $ffmpeg -Destination $portable
    Copy-Item -LiteralPath 'LICENSE','README.md' -Destination $portable
    Copy-Item -LiteralPath (Join-Path $minhook 'LICENSE.txt') -Destination (Join-Path $licenses 'MinHook.txt')
    foreach ($name in 'FFmpeg-GPL-2.0.txt','x264-license.txt','AMF-license.txt','nv-codec-headers') {
        Copy-Item -LiteralPath (Join-Path $encoder $name) -Destination $licenses -Recurse
    }

    # Only archive the recorder's source paths.
    $sourceRoots = 'CMakeLists.txt','LICENSE','README.md','.gitignore','src','tests','tools'
    if ($hasBundledMinHook) {
        $sourceFiles = Get-ChildItem -LiteralPath $sourceRoots -File -Recurse -Force |
            ForEach-Object { $_.FullName.Substring($root.Length + 1) }
    } else {
        $sourceFiles = & git -c core.quotepath=false ls-files --cached --others --exclude-standard -- @sourceRoots
        if ($LASTEXITCODE -ne 0) { throw 'Could not enumerate recorder sources.' }
    }
    foreach ($path in ($sourceFiles | Sort-Object -Unique)) {
        $destination = Join-Path $source $path
        $null = New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force
        Copy-Item -LiteralPath $path -Destination $destination
    }
    $thirdParty = Join-Path $source 'third-party'
    Copy-Item -LiteralPath (Join-Path $encoder 'source') -Destination $thirdParty -Recurse
    if ($hasBundledMinHook) {
        Copy-Item -LiteralPath $bundledMinHook -Destination $thirdParty
    } else {
        & git -C $minhook archive --format=tar.gz --prefix=minhook/ -o (Join-Path $thirdParty 'minhook.tar.gz') HEAD
        if ($LASTEXITCODE -ne 0) { throw 'Could not archive the exact MinHook sources.' }
    }

    $portableName = if ($IncludeWin32) { 'DepthRecorder-Windows-x64-with-x86-targets.zip' } else { 'DepthRecorder-Windows-x64.zip' }
    $portableZip = Join-Path $out $portableName
    $sourceZip = Join-Path $out 'DepthRecorder-source.zip'
    Write-ReleaseZip $portable $portableZip
    Write-ReleaseZip $source $sourceZip
    Get-FileHash -LiteralPath $portableZip,$sourceZip -Algorithm SHA256
} finally {
    Pop-Location
}
