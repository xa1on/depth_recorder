# Depth Recorder

Depth Recorder captures game color and depth as two videos on one real-time timeline.

## Record

1. Extract the Windows ZIP and run `depth_recorder.exe`.
2. Select a process and click **Attach**. The recorder selects the largest viewport. Use **Target window** to choose another.
3. Check the depth preview, set FPS, and click **Start Recording**. Click **Stop Recording** when finished.
4. After **Saved** appears, click **Open folder**. A paired clip contains a color MP4 and a depth MP4 or MKV with the same clip name.

The FPS setting controls how many frames are written per second of real time. If capture pauses, the recorder shows **Waiting for capture** and repeats the last pair in both videos until fresh pairs arrive or you stop. If the viewport resizes, both videos fit the new image into the original canvas. RGB24 Lossless preserves the packed depth bytes in an UtVideo MKV. Gray8 and ColorPhase use a compressed MP4 suitable for visual use. An encoder failure may leave a folder marked `_partial`.

Enable **Drop dead frames** in Settings to skip slots that have no fresh pair and skip ahead if encoding falls behind. Both videos still use the selected playback FPS and stay synchronized. The clip may play shorter than the time you spent recording.

Use **Launch EXE** for Vulkan so the capture layer can load during Vulkan initialization. If the Vulkan loader cannot discover the layer from a path outside the Windows system code page, extract the ZIP to a path with ASCII characters. Protected or elevated applications and unsupported render formats may be unavailable. The portable ZIP targets 64-bit games on x64 Windows PCs with their normal graphics drivers installed. The source also supports an optional package with 32-bit capture helpers.

## Build the Windows release

Build tools are required on the development machine. Users only need the ZIP.

- Install Visual Studio 2026 with Desktop development with C++, CMake, the Windows SDK with `fxc`, and the Vulkan SDK with `glslangValidator`. Set `VULKAN_SDK` to that SDK’s location.
- In MSYS2 UCRT64, install `mingw-w64-ucrt-x86_64-gcc`, `mingw-w64-ucrt-x86_64-pkgconf`, `mingw-w64-ucrt-x86_64-nasm`, `make`, and `diffutils`. From the extracted source ZIP, run `bash tools/build-ffmpeg.sh`. The build uses the archived FFmpeg, x264, nv-codec-headers, AMD AMF headers, and MinHook sources without fetching dependencies. A Git checkout uses Git to fetch the same pinned revisions. See `third-party/ffmpeg-build.txt` in the source ZIP for the encoder toolchain versions.
- In PowerShell at the repository root, build the recorder and run the package script.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
Copy-Item build/ffmpeg/dist/ffmpeg.exe build/Release/ffmpeg.exe
ctest --test-dir build -C Release --output-on-failure
pwsh -File tools/package.ps1
```

The `out` directory contains the Windows ZIP and a corresponding-source ZIP. Developers with approved 32-bit capture binaries can build with `-A Win32` into `build32` and pass `-IncludeWin32` to the package script. Publish both together for the same version. Recordings themselves contain no sidecar files.

To compare release hashes, build from a fresh extraction and use the same compiler, Windows SDK, Vulkan shader tools, MSYS2 packages, and PowerShell version. FFmpeg’s upstream build requires a source path without spaces. The recorder ZIP can be extracted to folders with spaces.

## Test and benchmark

`ctest` exercises timeline rounding, `IpcRing`, `CaptureSlotTracker`, `FrameInbox`, `FrameFitter`, and decodes paired encoder output. For a GPU smoke test, run the recorder on `sample_dx11_depth.exe` from the build. The recorder accepts `--launch <exe> --record-frames 600 --fps 60 --depth-mode gray8`. Use `--depth-mode rgb24` for packed lossless depth, `--no-depth` for a color-only comparison, `--drop-dead-frames` to skip duplicate frames, `--extra-ffmpeg "<flags>"` for custom FFmpeg output flags, `--warmup-encoder` to preflight the encoder before starting the clip clock, and `--discard` to stop after capture and IPC. Set `DEPTH_RECORDER_FFMPEG` to override the `ffmpeg.exe` path during development.

## License

The recorder’s own code is licensed under GPL-2.0-or-later. See [LICENSE](LICENSE). MinHook, FFmpeg, x264, nv-codec-headers, and AMD AMF headers retain their respective licenses. The packaged FFmpeg uses GPL components, including the x264 software fallback. AMD AMF uses the runtime from the installed AMD graphics driver. Before public distribution, verify contributor and asset provenance, applicable H.264 distribution obligations, and that both archives match the tested build.
