#pragma once
#include "common/config.h"

#include <cstdarg>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <windows.h>

namespace dr {

class RecorderLog {
public:
    void init(const std::wstring& log_path) {
        std::lock_guard lock(mutex_);
        log_path_ = log_path;
        std::error_code ec;
        const auto p = std::filesystem::path(log_path_);
        if (std::filesystem::is_regular_file(p, ec) && std::filesystem::file_size(p, ec) > 2u * 1024u * 1024u) {
            auto prev = p;
            prev.replace_extension(L".prev.log");
            std::filesystem::rename(p, prev, ec);
        }
        file_.open(p, std::ios::out | std::ios::app);
        append_unlocked("REC", "=== Depth Recorder session started ===");
    }

    void write(const char* tag, const char* fmt, ...) {
        char buf[512]{};
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        std::lock_guard lock(mutex_);
        append_unlocked(tag, buf);
    }

    void write_w(const char* tag, const std::wstring& msg) {
        if (msg.empty()) return;
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, msg.data(), static_cast<int>(msg.size()),
                                              nullptr, 0, nullptr, nullptr);
        if (bytes <= 0) return;
        std::string utf8(static_cast<size_t>(bytes), '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, msg.data(), static_cast<int>(msg.size()),
                                utf8.data(), bytes, nullptr, nullptr) > 0) {
            std::lock_guard lock(mutex_);
            append_unlocked(tag, utf8.c_str());
        }
    }

    void drain_shared(const SharedConfig* cfg) {
        if (!cfg) return;
        const LONG head = InterlockedCompareExchange(const_cast<volatile LONG*>(&cfg->log_write_seq), 0, 0);
        if (head <= read_seq_) return;
        std::lock_guard lock(mutex_);
        if (head - read_seq_ > static_cast<LONG>(kLogRingSize)) {
            read_seq_ = head - static_cast<LONG>(kLogRingSize);
        }
        std::string entry;
        while (read_seq_ < head) {
            if (!read_log_entry(cfg, read_seq_ + 1, entry)) break;
            ++read_seq_;
            append_raw_unlocked(entry);
        }
    }

    void reset_shared_cursor() {
        std::lock_guard lock(mutex_);
        read_seq_ = 0;
    }

    bool snapshot(uint64_t& in_out_version, std::wstring& out_text) {
        std::lock_guard lock(mutex_);
        if (version_ == in_out_version) return false;
        in_out_version = version_;
        std::string joined;
        for (const auto& line : lines_) {
            joined += line;
            joined += "\r\n";
        }
        if (joined.empty()) {
            out_text.clear();
            return true;
        }
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, joined.data(), static_cast<int>(joined.size()), nullptr, 0);
        if (wlen > 0) {
            out_text.assign(static_cast<size_t>(wlen), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, joined.data(), static_cast<int>(joined.size()), out_text.data(), wlen);
        } else {
            out_text.clear();
        }
        return true;
    }

    void clear_display() {
        std::lock_guard lock(mutex_);
        lines_.clear();
        ++version_;
    }

    std::wstring path() const {
        std::lock_guard lock(mutex_);
        return log_path_;
    }

private:
    void append_unlocked(const char* tag, const char* msg) {
        char line[600]{};
        std::snprintf(line, sizeof(line), "[%s] %s", tag ? tag : "LOG", msg ? msg : "");
        append_raw_unlocked(line);
    }

    void append_raw_unlocked(const std::string& body) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char stamped[640]{};
        std::snprintf(stamped, sizeof(stamped), "[%02u:%02u:%02u.%03u] %s",
                      st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body.c_str());
        if (file_.is_open()) {
            file_ << stamped << '\n';
            file_.flush();
        }
        lines_.emplace_back(stamped);
        while (lines_.size() > 400) lines_.pop_front();
        ++version_;
    }

    mutable std::mutex mutex_;
    std::wstring log_path_;
    std::ofstream file_;
    std::deque<std::string> lines_;
    uint64_t version_ = 0;
    LONG read_seq_ = 0;
};

} // namespace dr
