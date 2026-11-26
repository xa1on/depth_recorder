#include "common/depth_math.h"
#include "common/ipc_ring.h"
#include "recorder/encoder.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

static std::vector<uint8_t> decode_first_frame_rgb24(const std::wstring& ffmpeg, const std::wstring& video_path, uint32_t w, uint32_t h) {
    const std::wstring tmp_raw = video_path + L".raw";
    std::wstring cmd = L"\"" + ffmpeg + L"\" -y -loglevel error -i \"" + video_path +
                       L"\" -frames:v 1 -f rawvideo -pix_fmt rgb24 \"" + tmp_raw + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 3, 0);
    FILE* f = nullptr;
    _wfopen_s(&f, tmp_raw.c_str(), L"rb");
    if (f) {
        fread(out.data(), 1, out.size(), f);
        fclose(f);
    }
    std::filesystem::remove(tmp_raw);
    return out;
}

int main() {
    // 1. Math round-trip checks
    for (int i = 0; i <= 1000; ++i) {
        const float d = static_cast<float>(i) / 1000.0f;
        uint8_t r = 0, g = 0, b = 0;
        dr::encode_rgb24(d, r, g, b);
        const float d_rec = dr::decode_rgb24(r, g, b);
        assert(std::fabs(d - d_rec) < 1e-6f);

        dr::encode_color_phase(d, r, g, b);
        const float d_ph = dr::decode_color_phase(r, g, b);
        assert(std::fabs(d - d_ph) < 0.005f);
    }

    // 2. IPC ring push/pop check
    dr::IpcRing ring;
    assert(ring.open_or_create(true));
    ring.reset_indices();

    const uint32_t W = 320, H = 240;
    std::vector<uint8_t> color(W * H * 4, 128);
    std::vector<uint8_t> depth_rgb0(W * H * 4, 0);
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            const float d = static_cast<float>(x + y * W) / static_cast<float>(W * H - 1);
            uint8_t r = 0, g = 0, b = 0;
            dr::encode_rgb24(d, r, g, b);
            const size_t idx = (static_cast<size_t>(y) * W + x) * 4;
            depth_rgb0[idx + 0] = r;
            depth_rgb0[idx + 1] = g;
            depth_rgb0[idx + 2] = b;
            depth_rgb0[idx + 3] = 255;
        }
    }

    dr::FrameHeader hdr{};
    hdr.width = W;
    hdr.height = H;
    hdr.color_bytes = static_cast<uint32_t>(color.size());
    hdr.depth_bytes = static_cast<uint32_t>(depth_rgb0.size());
    hdr.depth_mode = static_cast<uint32_t>(dr::DepthMode::Rgb24Lossless);
    hdr.has_depth = 1;
    assert(ring.try_push(hdr, color.data(), depth_rgb0.data()));

    dr::FrameHeader out_hdr{};
    const uint8_t* peeked = ring.try_peek(out_hdr);
    assert(peeked != nullptr && out_hdr.width == W && out_hdr.height == H);
    ring.release_pop();

    // 3. UtVideo lossless MKV + H.264 MP4 round-trip
    const std::wstring ffmpeg = dr::VideoEncoder::find_ffmpeg();
    assert(!ffmpeg.empty());

    const std::wstring out_dir = L".\\test_out";
    std::filesystem::remove_all(out_dir);

    dr::VideoEncoder enc;
    dr::EncoderSettings settings{out_dir, dr::DepthMode::Rgb24Lossless, true, 60};
    assert(enc.start(W, H, settings));
    for (int f = 0; f < 30; ++f) {
        assert(enc.write_frame(color.data(), static_cast<uint32_t>(color.size()),
                               depth_rgb0.data(), static_cast<uint32_t>(depth_rgb0.size())));
    }
    const std::wstring color_path = enc.last_color_path();
    const std::wstring depth_path = enc.last_depth_path();
    enc.stop();

    assert(std::filesystem::exists(color_path));
    assert(std::filesystem::exists(depth_path));

    // Verify bit-exact 24-bit depth recovery from UtVideo MKV
    const auto decoded = decode_first_frame_rgb24(ffmpeg, depth_path, W, H);
    for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i) {
        assert(decoded[i * 3 + 0] == depth_rgb0[i * 4 + 0]);
        assert(decoded[i * 3 + 1] == depth_rgb0[i * 4 + 1]);
        assert(decoded[i * 3 + 2] == depth_rgb0[i * 4 + 2]);
    }

    std::filesystem::remove_all(out_dir);
    std::printf("test_ipc_ffmpeg PASSED (bit-exact UtVideo 24-bit depth verified)\n");
    return 0;
}

