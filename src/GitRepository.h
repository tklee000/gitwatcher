#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sw {

enum class ChangeKind { Modified, Added, Deleted, Renamed, Conflicted };

struct ChangedFile {
    std::wstring path;
    std::wstring oldPath;
    ChangeKind kind = ChangeKind::Modified;
    wchar_t indexStatus = L' ';
    wchar_t workTreeStatus = L' ';
};

struct RepositorySnapshot {
    bool isRepository = false;
    std::filesystem::path root;
    std::wstring head;
    std::wstring error;
    std::vector<ChangedFile> files;
};

struct BlobResult {
    bool ok = false;
    std::vector<std::uint8_t> bytes;
    std::wstring error;
};

class GitRepository {
public:
    RepositorySnapshot Scan(const std::filesystem::path& startDirectory) const;
    BlobResult ReadHeadBlob(const std::filesystem::path& repositoryRoot,
                            const std::wstring& repositoryPath) const;
    BlobResult ReadWorkingFile(const std::filesystem::path& repositoryRoot,
                               const std::wstring& repositoryPath) const;
};

std::wstring ChangeKindLabel(ChangeKind kind);
bool DecodeText(const std::vector<std::uint8_t>& bytes, std::wstring& text);

}  // namespace sw
