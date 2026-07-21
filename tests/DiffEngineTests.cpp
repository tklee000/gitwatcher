#include "DiffEngine.h"

#include <cstdlib>
#include <iostream>

namespace {

void Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
}  // namespace

int main() {
    std::wstring oldText;
    std::wstring newText;
    for (int line = 1; line <= 14; ++line) {
        oldText += L"line " + std::to_wstring(line) + L"\n";
        newText += (line == 8 ? L"changed 8\n" : L"line " + std::to_wstring(line) + L"\n");
    }
    newText += L"added tail\n";

    const auto document = sw::BuildSideBySideDiff(oldText, newText, 3);
    Require(document.additions == 2, "expected replacement plus appended addition");
    Require(document.deletions == 1, "expected one replaced line");

    bool hasChanged = false;
    bool hasAdded = false;
    bool hasCollapse = false;
    for (const auto& row : document.rows) {
        hasChanged = hasChanged || row.kind == sw::DiffRowKind::Changed;
        hasAdded = hasAdded || row.kind == sw::DiffRowKind::Added;
        hasCollapse = hasCollapse || row.kind == sw::DiffRowKind::Collapsed;
    }
    Require(hasChanged, "replacement should align as a changed row");
    Require(hasAdded, "append should appear as an added row");
    Require(hasCollapse, "distant equal lines should collapse");

    const auto emptyToOne = sw::BuildSideBySideDiff(L"", L"hello\n", 3);
    Require(emptyToOne.additions == 1 && emptyToOne.deletions == 0,
            "empty-to-one diff is incorrect");
    return 0;
}
