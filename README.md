# depth recorder

records a color and depth pass simultaneously for graphical applications. similar to fraps/dxtory, but primarily intended for depth maps.

## Get Started

1. Download and extract a portable .zip from [releases](https://github.com/xa1on/depth_recorder/releases/) and run `depth_recorder.exe`.
2. Select a process and click **Attach**. The recorder selects the largest viewport. Use **Target window** to pick a different one.
3. Check the depth preview, set FPS, and click **Start Recording**. Click **Stop Recording** when finished. (or use the convinently assigned keybinds)
4. After **Saved** appears, click **Open folder**. A paired clip contains a color MP4 and a depth MP4 or MKV with the same clip name.

## How does it work?

It hooks into the target application's graphical API (currently support OpenGL, DirectX11, DirectX12, and Vulkan) and extracts the depth buffer.

### Settings you should try out

* Z-near & Z-far: **Very important for most use-cases.** The closer the Z-near and the further the Z-far, the more depth information you will capture. **The same near and far values don't work for all applications, so experiment with this if you are getting a black/white depth preview**
* FPS: self explanitory, frames per second
* Cycle Depth Buffer: cycles through the depth buffers of the application, useful since sometimes there is more than one detected depth buffer.
* Depth mode: specifies the encoding and bit depth of the recorded depth buffer.
    * `Grey8` (default) - Grey-scaled depth with h264 encoding (Standard 256 possible depth values).
    * `ColorPhase` - More information dense h264 utilizing more colors, requires custom decoder (~ 7000 - 8000 depths)
    * `RGB24` - Lossless Utvideo MKV with on 8-bit color. can use all 16,777,216 colors to represent a depth value. requires a custom decoder.
* Drop Dead Frames: drops frames where the depth and color buffer is identical to the previous frame. This will make the video no longer real-time, however can be helpful for video editing since it gets rid of any choppyness. 
* Vulkan Implicit Layer (Attach): Attaches to the Vulkan Implicit Layer to allow for depth recording on a application not started within depth recorder. (**REQUIRED FOR MOST VULKAN APPLICATIONS**)


## Building the Windows release

The installation itself is portable, but build tools are required for development.

- Visual Studio 2026 with C++, CMake Windows SDK `fxc`, Vulkan SDK `glslangValidator`
- Make sure to to set `VULKAN_SDK` to the location of the SDK.
- In MSYS2 UCRT64, install `mingw-w64-ucrt-x86_64-gcc`, `mingw-w64-ucrt-x86_64-pkgconf`, `mingw-w64-ucrt-x86_64-nasm`, `make`, and `diffutils`. From the extracted source ZIP, run `bash tools/build-ffmpeg.sh`. The build uses the archived FFmpeg, x264, nv-codec-headers, AMD AMF headers, and MinHook sources without fetching dependencies. A Git checkout uses Git to fetch the same pinned revisions. See `third-party/ffmpeg-build.txt` in the source ZIP for the encoder toolchain versions.
- In PowerShell at the repository root, build the recorder and run the package script.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
Copy-Item build/ffmpeg/dist/ffmpeg.exe build/Release/ffmpeg.exe
ctest --test-dir build -C Release --output-on-failure
pwsh -File tools/package.ps1
```

## License

The recorder’s own code is licensed under GPL-2.0-or-later. See [LICENSE](LICENSE). MinHook, FFmpeg, x264, nv-codec-headers, and AMD AMF headers retain their respective licenses. The packaged FFmpeg uses GPL components, including the x264 software fallback. AMD AMF uses the runtime from the installed AMD graphics driver. Before public distribution, verify contributor and asset provenance, applicable H.264 distribution obligations, and that both archives match the tested build.