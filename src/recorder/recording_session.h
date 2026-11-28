#pragma once
#include "frame_inbox.h"
#include "ui_win32.h"
#include <cstdint>
#include <string>

namespace dr {

int64_t qpc_now();
int64_t qpc_frequency();

void ring_consumer_loop(RecorderAppState& app, FrameInbox& inbox);
void writer_loop(RecorderAppState& app, FrameInbox& inbox, uint64_t max_frames);
void finish_recording(RecorderAppState& app, FrameInbox& inbox, bool success,
                      std::wstring message, std::wstring folder = {});

} // namespace dr
