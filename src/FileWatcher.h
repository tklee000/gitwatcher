#pragma once

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <thread>

namespace sw {

class FileWatcher {
public:
    FileWatcher() = default;
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    bool Start(const std::filesystem::path& directory, HWND notifyWindow, UINT message);
    void Stop();

private:
    void Run();

    std::filesystem::path directory_;
    HWND notifyWindow_ = nullptr;
    UINT message_ = 0;
    HANDLE stopEvent_ = nullptr;
    std::thread worker_;
    std::atomic<bool> active_{false};
};

}  // namespace sw
