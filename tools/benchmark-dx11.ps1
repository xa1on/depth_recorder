[CmdletBinding()]
param(
    [string]$Build = 'build',
    [ValidateRange(64, 4096)][int]$Width = 1920,
    [ValidateRange(64, 2160)][int]$Height = 1080,
    [ValidateRange(1, 240)][int]$SourceFps = 240,
    [ValidateRange(1, 240)][int]$OutputFps = 120,
    [ValidateRange(0, 240)][double]$MinimumFreshLowFps = 0,
    [ValidateRange(0, 65535)][int]$AdapterVendorId = 0,
    [ValidateRange(0, 1000)][int]$LatePresentMs = 0,
    [ValidateRange(1, 3840)][int]$LatePresentEvery = 120,
    [ValidateRange(0, 3840)][int]$DepthPadding = 0,
    [ValidateRange(1, 600)][int]$Seconds = 30,
    [ValidateSet('Capture', 'Pipes', 'Color', 'Gray8', 'Lossless', 'Gpu')][string[]]$Mode = @('Capture', 'Pipes', 'Color', 'Gray8', 'Lossless'),
    [switch]$Overlay,
    [switch]$SoftwareEncoder,
    [switch]$VerifyFrameIds,
    [switch]$Offscreen
)

$ErrorActionPreference = 'Stop'
if (!$MinimumFreshLowFps -and $OutputFps -eq 120) { $MinimumFreshLowFps = 100 }
if ($MinimumFreshLowFps -gt $OutputFps) { throw 'The minimum fresh-pair rate cannot exceed the output rate.' }
$buildDir = (Resolve-Path -LiteralPath $Build).Path
$release = Join-Path $buildDir 'Release'
$recorder = (Resolve-Path -LiteralPath (Join-Path $release 'depth_recorder.exe')).Path
$sample = (Resolve-Path -LiteralPath (Join-Path $release 'sample_dx11_depth.exe')).Path
$ffmpeg = Join-Path $release 'ffmpeg.exe'
$probe = Join-Path $buildDir 'ffmpeg/dist/ffprobe.exe'
if (!(Test-Path -LiteralPath $probe -PathType Leaf)) { $probe = Join-Path $release 'ffprobe.exe' }
$probe = (Resolve-Path -LiteralPath $probe).Path
$run = Join-Path $buildDir ('benchmark-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$null = New-Item -ItemType Directory -Path $run
Write-Host "Benchmark files are in $run"

$sampleOptions = @{
    WIDTH = "$Width"
    HEIGHT = "$Height"
    FPS = "$SourceFps"
    LATE_PRESENT_EVERY = "$LatePresentEvery"
    LATE_PRESENT_MS = "$LatePresentMs"
    DEPTH_PADDING = "$DepthPadding"
    OFFSCREEN = if ($Offscreen) { '1' } else { '0' }
    ADAPTER_VENDOR = "$AdapterVendorId"
    REPORT = (Join-Path $run 'baseline.log')
    REPORT_SECONDS = "$Seconds"
    WINDOWS = $null
    RESIZE_MS = $null
    SECOND_RESIZE_MS = $null
    GAP_MS = $null
    MSAA = $null
    RENDER_PERCENT = $null
    GPU_PROBE = $null
    WARP = $null
}
$saved = @{}

try {
    foreach ($key in $sampleOptions.Keys) {
        $name = "DEPTH_RECORDER_SAMPLE_$key"
        $saved[$name] = [Environment]::GetEnvironmentVariable($name)
        [Environment]::SetEnvironmentVariable($name, $sampleOptions[$key])
    }

    $baseline = Start-Process -FilePath $sample -WindowStyle Hidden -PassThru
    try {
        $deadline = [DateTime]::UtcNow.AddSeconds($Seconds + 10)
        do {
            Start-Sleep -Milliseconds 100
            $baselineLog = Get-Content -LiteralPath $sampleOptions.REPORT -Raw -ErrorAction SilentlyContinue
        } until ($baselineLog -or $baseline.HasExited -or [DateTime]::UtcNow -ge $deadline)
        if ($baselineLog -notmatch 'p95_ms=([\d.]+)') { throw 'The DX11 sample did not report its baseline frame time.' }
        $baselineP95 = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
    } finally {
        if (!$baseline.HasExited) { Stop-Process -InputObject $baseline }
    }
    Write-Host "Baseline game p95 $baselineP95 ms"
    [Environment]::SetEnvironmentVariable('DEPTH_RECORDER_SAMPLE_REPORT', $null)

    $frames = $Seconds * $OutputFps
    foreach ($case in $Mode) {
        $clipDir = Join-Path $run $case
        $log = Join-Path $run "$case.log"
        $options = switch ($case) {
            'Capture' { '--discard' }
            'Pipes' { '--pipe-discard --depth-mode gray8' }
            'Color' { '--no-depth' }
            'Gray8' { '--depth-mode gray8' }
            'Lossless' { '--depth-mode rgb24' }
            'Gpu' { "--gpu-record-seconds $Seconds" }
        }
        $overlayOption = if ($Overlay) { '--overlay' } else { '--no-overlay' }
        $encoderOption = if ($SoftwareEncoder -and $case -ne 'Gpu') { '--software-encoder' } else { '' }
        $depthOption = if ($VerifyFrameIds) { '--no-linearize' } else { '' }
        $arguments = "--launch `"$sample`" --out `"$clipDir`" --fps $OutputFps --record-frames $frames --warmup-encoder $options $overlayOption $encoderOption $depthOption"
        $result = Start-Process -FilePath $recorder -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput $log
        $stats = Get-Content -LiteralPath $log -Raw
        Write-Host $stats.TrimEnd()
        if ($stats -notmatch 'presents=(\d+) published=(\d+) received=(\d+)') { throw "No capture counters for $case." }
        $presents = [int]$Matches[1]
        $published = [int]$Matches[2]
        $received = [int]$Matches[3]
        if ($stats -notmatch 'written=(\d+) unique=(\d+) intervals=(\d+) p95_ms=([\d.]+)') { throw "No telemetry for $case." }
        $written = [int]$Matches[1]
        $unique = [int]$Matches[2]
        $p95 = [double]::Parse($Matches[4], [Globalization.CultureInfo]::InvariantCulture)
        if ($stats -notmatch 'fresh_low_fps=([\d.]+) fresh_p99_fps=([\d.]+)') { throw "No fresh-pair timing for $case." }
        $low = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
        $freshP99 = [double]::Parse($Matches[2], [Globalization.CultureInfo]::InvariantCulture)
        $cadencePassed = if ($MinimumFreshLowFps) { $low -ge $MinimumFreshLowFps } else { $unique -ge 0.99 * $frames }
        $recordsOutput = $case -notin @('Capture', 'Pipes')
        $verifyMarkers = $VerifyFrameIds -and $recordsOutput -and $case -ne 'Color'
        $tracks = @()
        $maxPairSkewMs = $null
        $markerMismatches = $null
        if ($result.ExitCode -eq 0 -and $recordsOutput) {
            $tracks = @(foreach ($video in (Get-ChildItem -LiteralPath $clipDir -File -Recurse | Sort-Object Name)) {
                $media = & $probe -v error -select_streams v:0 -show_entries 'frame=best_effort_timestamp_time:stream=codec_name:stream_tags=encoder:format=duration' -of json $video.FullName | ConvertFrom-Json
                if ($LASTEXITCODE -ne 0) { throw "Could not decode $($video.Name)." }
                $stream = $media.streams[0]
                $times = @($media.frames | ForEach-Object {
                    [double]::Parse($_.best_effort_timestamp_time, [Globalization.CultureInfo]::InvariantCulture)
                })
                $maxClockErrorMs = 0.0
                for ($i = 0; $i -lt $times.Count; ++$i) {
                    $maxClockErrorMs = [math]::Max($maxClockErrorMs, 1000 * [math]::Abs($times[$i] - $i / $OutputFps))
                }
                $samples = if ($verifyMarkers) {
                    # The fixture writes a six-value marker in the clear color and depth outside the triangle.
                    $raw = Join-Path $run ($video.BaseName + '.rgb')
                    & $ffmpeg -nostdin -v error -filter_threads 1 -i $video.FullName -map 0:v:0 -an `
                        -vf 'scale=16:16:flags=neighbor' -c:v rawvideo -threads 1 -pix_fmt rgb24 -f rawvideo $raw
                    if ($LASTEXITCODE -ne 0) { throw "Could not decode frame IDs from $($video.Name)." }
                    ,([IO.File]::ReadAllBytes($raw))
                }
                [pscustomobject]@{
                    Encoder = if ($stream.tags.encoder) { ($stream.tags.encoder -split ' ')[-1] } else { $stream.codec_name }
                    Duration = [double]$media.format.duration
                    Times = $times
                    MaxClockErrorMs = $maxClockErrorMs
                    Samples = $samples
                }
            })
            if ($tracks.Count -eq 2 -and $tracks[0].Times.Count -eq $frames -and $tracks[1].Times.Count -eq $frames) {
                $maxPairSkewMs = 0.0
                for ($i = 0; $i -lt $frames; ++$i) {
                    $maxPairSkewMs = [math]::Max($maxPairSkewMs, 1000 * [math]::Abs($tracks[0].Times[$i] - $tracks[1].Times[$i]))
                }
                $sampleFrameBytes = 16 * 16 * 3
                if ($verifyMarkers -and $tracks[0].Samples.Count -eq $frames * $sampleFrameBytes -and
                    $tracks[1].Samples.Count -eq $frames * $sampleFrameBytes) {
                    $markerMismatches = 0
                    for ($i = 0; $i -lt $frames; ++$i) {
                        $offset = $i * $sampleFrameBytes
                        $colorId = [math]::Round($tracks[0].Samples[$offset] / 32.0)
                        $depthId = [math]::Round($tracks[1].Samples[$offset] / 32.0)
                        if ($colorId -lt 1 -or $colorId -gt 6 -or $colorId -ne $depthId) { ++$markerMismatches }
                    }
                }
            }
        }
        $expectedFiles = if ($case -eq 'Color') { 1 } else { 2 }
        # MP4 and Matroska use different time bases. Allow one millisecond of container rounding.
        $matchingOutput = !$recordsOutput -or ($tracks.Count -eq $expectedFiles -and
            !($tracks | Where-Object { $_.Times.Count -ne $frames -or $_.MaxClockErrorMs -gt 1.001 -or
                [math]::Abs($_.Duration - $Seconds) -gt 1.0 / $OutputFps }) -and
            ($expectedFiles -eq 1 -or ($null -ne $maxPairSkewMs -and $maxPairSkewMs -le 1.001)) -and
            (!$verifyMarkers -or ($null -ne $markerMismatches -and $markerMismatches -eq 0)))
        [pscustomobject]@{
            Mode = $case
            Exit = $result.ExitCode
            Frames = $written
            PresentFps = if ($result.ExitCode -eq 0) { [math]::Round($presents / $Seconds, 2) }
            PublishedFps = if ($result.ExitCode -eq 0) { [math]::Round($published / $Seconds, 2) }
            ReceivedFps = if ($result.ExitCode -eq 0) { [math]::Round($received / $Seconds, 2) }
            UniqueFps = [math]::Round($unique / $Seconds, 2)
            Fresh1PercentLowFps = $low
            FreshP99GapFps = $freshP99
            GameP95Ms = $p95
            GameP95Change = [math]::Round(100 * ($p95 / $baselineP95 - 1), 1)
            DecodedFrames = ($tracks | ForEach-Object { $_.Times.Count }) -join '/'
            Encoders = $tracks.Encoder -join '/'
            PairSkewMs = if ($null -ne $maxPairSkewMs) { [math]::Round($maxPairSkewMs, 3) } else { $null }
            FrameIdMismatches = $markerMismatches
            Passed = $result.ExitCode -eq 0 -and $matchingOutput -and $written -eq $frames -and
                $cadencePassed -and $p95 -le 1.10 * $baselineP95
        }
    }
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
}
