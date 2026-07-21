#include "FileWatcher.h"

#include <array>

namespace sw {

FileWatcher::~FileWatcher() {
    Stop();
}

bool FileWatcher::Start(const std::filesystem::path& directory,
                        const HWND notifyWindow,
                        const UINT message) {
    Stop();
    directory_ = directory;
    notifyWindow_ = notifyWindow;
    message_ = message;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) {
        return false;
    }
    active_ = true;
    worker_ = std::thread(&FileWatcher::Run, this);
    return true;
}

void FileWatcher::Stop() {
    active_ = false;
    if (stopEvent_) {
        SetEvent(stopEvent_);
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
}

void FileWatcher::Run() {
    HANDLE directoryHandle = CreateFileW(
        directory_.c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (directoryHandle == INVALID_HANDLE_VALUE) {
        return;
    }

    HANDLE changedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!changedEvent) {
        CloseHandle(directoryHandle);
        return;
    }

    std::array<std::uint8_t, 64 * 1024> buffer{};
    HANDLE waits[] = {stopEvent_, changedEvent};
    while (active_) {
        ResetEvent(changedEvent);
        OVERLAPPED overlapped{};
        overlapped.hEvent = changedEvent;
        const BOOL watching = ReadDirectoryChangesW(
            directoryHandle, buffer.data(), static_cast<DWORD>(buffer.size()), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
            nullptr, &overlapped, nullptr);
        if (!watching) {
            break;
        }

        const DWORD waitResult = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (waitResult == WAIT_OBJECT_0) {
            CancelIoEx(directoryHandle, &overlapped);
            break;
        }
        if (waitResult != WAIT_OBJECT_0 + 1) {
            CancelIoEx(directoryHandle, &overlapped);
            break;
        }

        DWORD transferred = 0;
        if (GetOverlappedResult(directoryHandle, &overlapped, &transferred, FALSE) &&
            transferred > 0 && active_ && IsWindow(notifyWindow_)) {
            PostMessageW(notifyWindow_, message_, 0, 0);
        }
    }

    CloseHandle(changedEvent);
    CloseHandle(directoryHandle);
}

}  // namespace sw
