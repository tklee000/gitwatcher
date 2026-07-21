#include "GitRepository.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>

namespace sw {
namespace {

struct ProcessResult {
    DWORD exitCode = static_cast<DWORD>(-1);
    DWORD launchError = ERROR_SUCCESS;
    std::vector<std::uint8_t> output;
};

bool IsFile(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

std::filesystem::path EnvironmentPath(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        return {};
    }

    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
    if (written == 0 || written >= required) {
        return {};
    }
    value.resize(written);
    return std::filesystem::path(value);
}

std::filesystem::path FindGitExecutable() {
    std::vector<wchar_t> searchResult(32768);
    const DWORD found = SearchPathW(nullptr, L"git.exe", nullptr,
                                    static_cast<DWORD>(searchResult.size()),
                                    searchResult.data(), nullptr);
    if (found > 0 && found < searchResult.size()) {
        return std::filesystem::path(searchResult.data());
    }

    const auto programFiles = EnvironmentPath(L"ProgramFiles");
    const auto programFilesX86 = EnvironmentPath(L"ProgramFiles(x86)");
    const auto localAppData = EnvironmentPath(L"LOCALAPPDATA");

    for (const auto& base : {programFiles, programFilesX86}) {
        if (base.empty()) {
            continue;
        }
        const auto candidate = base / L"Git" / L"cmd" / L"git.exe";
        if (IsFile(candidate)) {
            return candidate;
        }
    }

    if (!localAppData.empty()) {
        const auto candidate = localAppData / L"Programs" / L"Git" / L"cmd" / L"git.exe";
        if (IsFile(candidate)) {
            return candidate;
        }
    }

    for (const auto& base : {programFiles, programFilesX86}) {
        if (base.empty()) {
            continue;
        }
        for (const wchar_t* version : {L"2022", L"2019", L"2017"}) {
            for (const wchar_t* edition : {L"Community", L"Professional", L"Enterprise",
                                           L"BuildTools"}) {
                const auto candidate =
                    base / L"Microsoft Visual Studio" / version / edition / L"Common7" /
                    L"IDE" / L"CommonExtensions" / L"Microsoft" / L"TeamFoundation" /
                    L"Team Explorer" / L"Git" / L"cmd" / L"git.exe";
                if (IsFile(candidate)) {
                    return candidate;
                }
            }
        }
    }

    return L"git.exe";
}

std::wstring QuoteArgument(const std::wstring& argument) {
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

ProcessResult RunGit(const std::filesystem::path& workingDirectory,
                     const std::vector<std::wstring>& arguments) {
    ProcessResult result;
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &attributes, 0)) {
        return result;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    static const std::filesystem::path gitExecutable = FindGitExecutable();
    std::wstring command = QuoteArgument(gitExecutable.wstring());
    for (const auto& argument : arguments) {
        command.push_back(L' ');
        command += QuoteArgument(argument);
    }
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};

    const BOOL created = CreateProcessW(
        nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
        workingDirectory.c_str(), &startup, &process);
    CloseHandle(writePipe);
    writePipe = nullptr;

    if (!created) {
        result.launchError = GetLastError();
        CloseHandle(readPipe);
        return result;
    }

    std::uint8_t buffer[8192];
    DWORD bytesRead = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
        result.output.insert(result.output.end(), buffer, buffer + bytesRead);
    }
    CloseHandle(readPipe);
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &result.exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}

std::wstring Utf8ToWide(const std::uint8_t* bytes, const std::size_t size) {
    if (size == 0) {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, 0,
                                              reinterpret_cast<const char*>(bytes),
                                              static_cast<int>(size), nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(bytes),
                        static_cast<int>(size), result.data(), required);
    return result;
}

std::wstring TrimOutput(const std::vector<std::uint8_t>& output) {
    std::wstring value = Utf8ToWide(output.data(), output.size());
    while (!value.empty() && (value.back() == L'\r' || value.back() == L'\n' ||
                              value.back() == L' ' || value.back() == L'\t')) {
        value.pop_back();
    }
    return value;
}

ChangeKind Classify(const wchar_t index, const wchar_t workTree) {
    if (index == L'U' || workTree == L'U' || (index == L'A' && workTree == L'A') ||
        (index == L'D' && workTree == L'D')) {
        return ChangeKind::Conflicted;
    }
    if (index == L'R' || workTree == L'R') {
        return ChangeKind::Renamed;
    }
    if (index == L'D' || workTree == L'D') {
        return ChangeKind::Deleted;
    }
    if (index == L'A' || workTree == L'A' || index == L'?') {
        return ChangeKind::Added;
    }
    return ChangeKind::Modified;
}

}  // namespace

RepositorySnapshot GitRepository::Scan(const std::filesystem::path& startDirectory) const {
    RepositorySnapshot snapshot;
    const auto rootResult = RunGit(startDirectory, {L"rev-parse", L"--show-toplevel"});
    if (rootResult.exitCode != 0) {
        if (rootResult.launchError == ERROR_FILE_NOT_FOUND ||
            rootResult.launchError == ERROR_PATH_NOT_FOUND) {
            snapshot.error = L"Git 실행 파일을 찾을 수 없습니다. Git for Windows를 설치하거나 PATH에 추가하세요.";
        } else {
            snapshot.error = L"선택한 폴더는 Git 저장소가 아닙니다.";
        }
        return snapshot;
    }

    snapshot.root = std::filesystem::path(TrimOutput(rootResult.output));
    snapshot.isRepository = true;

    const auto headResult = RunGit(snapshot.root, {L"rev-parse", L"--short=10", L"HEAD"});
    if (headResult.exitCode != 0) {
        snapshot.isRepository = false;
        snapshot.error = L"저장소에 아직 커밋(HEAD)이 없습니다.";
        return snapshot;
    }
    snapshot.head = TrimOutput(headResult.output);

    const auto statusResult = RunGit(snapshot.root,
                                     {L"-c", L"core.quotepath=false", L"status",
                                      L"--porcelain=v1", L"-z", L"--untracked-files=all",
                                      L"--ignored=no"});
    if (statusResult.exitCode != 0) {
        snapshot.error = L"git status 실행에 실패했습니다.";
        return snapshot;
    }

    const auto& data = statusResult.output;
    std::size_t cursor = 0;
    while (cursor + 3 <= data.size()) {
        const wchar_t indexStatus = static_cast<wchar_t>(data[cursor]);
        const wchar_t workTreeStatus = static_cast<wchar_t>(data[cursor + 1]);
        cursor += 3;
        const std::size_t pathStart = cursor;
        while (cursor < data.size() && data[cursor] != 0) {
            ++cursor;
        }
        if (cursor >= data.size()) {
            break;
        }

        ChangedFile file;
        file.indexStatus = indexStatus;
        file.workTreeStatus = workTreeStatus;
        file.path = Utf8ToWide(data.data() + pathStart, cursor - pathStart);
        file.kind = Classify(indexStatus, workTreeStatus);
        ++cursor;

        if (indexStatus == L'R' || indexStatus == L'C' || workTreeStatus == L'R' ||
            workTreeStatus == L'C') {
            const std::size_t oldPathStart = cursor;
            while (cursor < data.size() && data[cursor] != 0) {
                ++cursor;
            }
            file.oldPath = Utf8ToWide(data.data() + oldPathStart, cursor - oldPathStart);
            if (cursor < data.size()) {
                ++cursor;
            }
        }
        snapshot.files.push_back(std::move(file));
    }

    std::sort(snapshot.files.begin(), snapshot.files.end(),
              [](const ChangedFile& left, const ChangedFile& right) {
                  return left.path < right.path;
              });
    return snapshot;
}

BlobResult GitRepository::ReadHeadBlob(const std::filesystem::path& repositoryRoot,
                                       const std::wstring& repositoryPath) const {
    BlobResult result;
    const auto process = RunGit(repositoryRoot, {L"cat-file", L"blob", L"HEAD:" + repositoryPath});
    result.ok = process.exitCode == 0;
    result.bytes = process.output;
    if (!result.ok) {
        result.bytes.clear();
        result.error = L"HEAD에서 파일을 읽을 수 없습니다.";
    }
    return result;
}

BlobResult GitRepository::ReadWorkingFile(const std::filesystem::path& repositoryRoot,
                                          const std::wstring& repositoryPath) const {
    BlobResult result;
    const std::filesystem::path fullPath = repositoryRoot / std::filesystem::path(repositoryPath);
    std::ifstream stream(fullPath, std::ios::binary);
    if (!stream) {
        result.error = L"작업 파일을 읽을 수 없습니다.";
        return result;
    }
    result.bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    result.ok = true;
    return result;
}

std::wstring ChangeKindLabel(const ChangeKind kind) {
    switch (kind) {
        case ChangeKind::Added:
            return L"A";
        case ChangeKind::Deleted:
            return L"D";
        case ChangeKind::Renamed:
            return L"R";
        case ChangeKind::Conflicted:
            return L"!";
        case ChangeKind::Modified:
        default:
            return L"M";
    }
}

bool DecodeText(const std::vector<std::uint8_t>& bytes, std::wstring& text) {
    text.clear();
    if (bytes.empty()) {
        return true;
    }

    if (bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) {
        const std::size_t count = (bytes.size() - 2) / 2;
        text.resize(count);
        for (std::size_t index = 0; index < count; ++index) {
            text[index] = static_cast<wchar_t>(bytes[2 + index * 2] |
                                               (bytes[3 + index * 2] << 8));
        }
        return true;
    }
    if (bytes.size() >= 2 && bytes[0] == 0xfe && bytes[1] == 0xff) {
        const std::size_t count = (bytes.size() - 2) / 2;
        text.resize(count);
        for (std::size_t index = 0; index < count; ++index) {
            text[index] = static_cast<wchar_t>((bytes[2 + index * 2] << 8) |
                                               bytes[3 + index * 2]);
        }
        return true;
    }

    const std::size_t bom = bytes.size() >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb &&
                                    bytes[2] == 0xbf
                                ? 3
                                : 0;
    if (std::find(bytes.begin() + static_cast<std::ptrdiff_t>(bom), bytes.end(), 0) !=
        bytes.end()) {
        return false;
    }

    const int size = static_cast<int>(bytes.size() - bom);
    int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       reinterpret_cast<const char*>(bytes.data() + bom), size,
                                       nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (required <= 0) {
        codePage = CP_ACP;
        flags = 0;
        required = MultiByteToWideChar(codePage, flags,
                                       reinterpret_cast<const char*>(bytes.data() + bom), size,
                                       nullptr, 0);
    }
    if (required <= 0) {
        return false;
    }
    text.resize(static_cast<std::size_t>(required));
    MultiByteToWideChar(codePage, flags, reinterpret_cast<const char*>(bytes.data() + bom), size,
                        text.data(), required);
    return true;
}

}  // namespace sw
