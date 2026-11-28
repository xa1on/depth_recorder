#!/usr/bin/env bash
# Run in an MSYS2 UCRT64 shell from the repository root.
set -euo pipefail

root="$(pwd)"
work="${1:-$root/build/ffmpeg}"
mkdir -p "$work"
work="$(cd "$work" && pwd)"
prefix="$work/prefix"
jobs="${JOBS:-$(nproc)}"
command -v cmp >/dev/null

ffmpeg=894da5ca7d742e4429ffb2af534fcda0103ef593
x264=31e19f92f00c7003fa115047ce50978bc98c3a0d
nvcodec=c69278340ab1d5559c7d7bf0edf615dc33ddbba7
amf=16f7d73e0b45c473e903e46981ed0b91efc4c091
bundled="$root/third-party"
components=(ffmpeg x264 nv-codec-headers amf)

checkout() {
    local name="$1" url="$2" rev="$3"
    shift 3
    if [[ ! -d "$work/$name/.git" ]]; then
        git -c init.defaultBranch=main init "$work/$name"
        git -C "$work/$name" remote add origin "$url"
    fi
    if ! git -C "$work/$name" cat-file -e "$rev^{commit}" 2>/dev/null; then
        if [[ $# -gt 0 ]]; then
            git -C "$work/$name" sparse-checkout set --no-cone "$@"
            git -C "$work/$name" fetch --depth=1 --filter=blob:none origin "$rev"
        else
            git -C "$work/$name" fetch --depth=1 origin "$rev"
        fi
    fi
    git -C "$work/$name" checkout --detach "$rev"
    git -C "$work/$name" diff --quiet HEAD
}

export LC_ALL=C
export SOURCE_DATE_EPOCH
if [[ -f "$bundled/ffmpeg.tar.gz" ]]; then
    for component in "${components[@]}"; do
        archive="$bundled/$component.tar.gz"
        digest="$(sha256sum "$archive" | cut -d ' ' -f 1)"
        stamp="$work/$component.archive-sha256"
        if [[ -d "$work/$component" && -f "$stamp" && "$(cat "$stamp")" == "$digest" ]]; then
            continue
        fi
        if [[ -e "$work/$component" ]]; then
            echo "Bundled source conflicts with the existing $work/$component. Use a new build directory." >&2
            exit 1
        fi
        unpack="$(mktemp -d "$work/source.XXXXXXXX")"
        tar -xzf "$archive" -C "$unpack" "$component/"
        mv "$unpack/$component" "$work/$component"
        rmdir "$unpack"
        printf '%s\n' "$digest" > "$stamp"
    done
    # Commit time of the pinned FFmpeg revision above.
    SOURCE_DATE_EPOCH=1763603031
else
    checkout ffmpeg https://github.com/FFmpeg/FFmpeg.git "$ffmpeg"
    checkout x264 https://github.com/mirror/x264.git "$x264"
    checkout nv-codec-headers https://github.com/FFmpeg/nv-codec-headers.git "$nvcodec"
    checkout amf https://github.com/GPUOpen-LibrariesAndSDKs/AMF.git "$amf" /LICENSE.txt /amf/public/include/
    SOURCE_DATE_EPOCH="$(git -C "$work/ffmpeg" show -s --format=%ct HEAD)"
fi
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"

mkdir -p "$work/x264-build" "$work/ffmpeg-build" "$work/dist/source"
(
    cd "$work/x264-build"
    "$work/x264/configure" --prefix="$prefix" --enable-static --disable-cli --disable-opencl
    make -j"$jobs"
    make install
)
make -C "$work/nv-codec-headers" PREFIX="$prefix" install
mkdir -p "$prefix/include/AMF"
cp -R "$work/amf/amf/public/include/." "$prefix/include/AMF/"

(
    cd "$work/ffmpeg-build"
    "$work/ffmpeg/configure" \
        --arch=x86_64 --target-os=mingw32 --pkg-config-flags=--static \
        --extra-ldflags='-static -Wl,--no-insert-timestamp' --extra-cflags=-I../prefix/include --enable-static --disable-shared \
        --disable-autodetect --disable-network --disable-debug --disable-doc \
        --disable-ffplay --enable-gpl --enable-libx264 --enable-ffnvcodec \
        --enable-nvenc --enable-amf --enable-mediafoundation --enable-d3d11va \
        --disable-everything --enable-avdevice --enable-swscale \
        --enable-encoder=h264_nvenc,h264_amf,h264_mf,libx264,utvideo,rawvideo,wrapped_avframe \
        --enable-decoder=h264,utvideo,rawvideo,wrapped_avframe \
        --enable-demuxer=rawvideo,mov,matroska,h264 --enable-parser=h264 \
        --enable-muxer=mp4,matroska,null,rawvideo \
        --enable-filter=color,pad,format,scale,null,trim,atrim \
        --enable-indev=lavfi --enable-protocol=file,pipe
    # The upstream Makefile sets the version script's revision from REVISION.
    # Use relative source paths so diagnostics do not embed the build-machine directory.
    make -j"$jobs" "REVISION=${ffmpeg:0:7}" SRC_PATH=../ffmpeg SRC_LINK=../ffmpeg ffmpeg.exe ffprobe.exe
    cp ffmpeg.exe ffprobe.exe "$work/dist/"
)

for component in "${components[@]}"; do
    if [[ -f "$bundled/ffmpeg.tar.gz" ]]; then
        cp "$bundled/$component.tar.gz" "$work/dist/source/"
    elif [[ "$component" == amf ]]; then
        git -C "$work/amf" archive --format=tar.gz --prefix=amf/ HEAD LICENSE.txt amf/public/include > "$work/dist/source/amf.tar.gz"
    else
        git -C "$work/$component" archive --format=tar.gz --prefix="$component/" HEAD > "$work/dist/source/$component.tar.gz"
    fi
done
cp "$work/ffmpeg/COPYING.GPLv2" "$work/dist/FFmpeg-GPL-2.0.txt"
cp "$work/x264/COPYING" "$work/dist/x264-license.txt"
cp "$work/amf/LICENSE.txt" "$work/dist/AMF-license.txt"
# The nv-codec-headers license is in the headers in this revision.
mkdir -p "$work/dist/nv-codec-headers"
cp "$work/nv-codec-headers/include/ffnvcodec/"*.h "$work/dist/nv-codec-headers/"
cp "$root/tools/build-ffmpeg.sh" "$work/dist/source/"
(
    cd "$work/dist"
    sha256sum ffmpeg.exe > ffmpeg.sha256
    "$work/dist/ffmpeg.exe" -hide_banner -buildconf 2>&1
    gcc --version | head -n 1
    nasm -v
    pacman -Q mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-nasm make diffutils
) > "$work/dist/source/ffmpeg-build.txt"

echo "FFmpeg and its corresponding source are in $work/dist"
