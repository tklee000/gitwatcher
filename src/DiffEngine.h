// SPDX-License-Identifier: MIT
// Copyright (c) 2026 tklee000
//
// This file is part of GitWatcher's independently implemented Myers diff
// engine. It is licensed under the MIT License; see LICENSES/MIT.txt.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace sw {

enum class DiffRowKind {
    Equal,
    Added,
    Deleted,
    Changed,
    Collapsed,
};

struct DiffRow {
    DiffRowKind kind = DiffRowKind::Equal;
    int oldLine = 0;
    int newLine = 0;
    std::wstring oldText;
    std::wstring newText;
    std::size_t hiddenCount = 0;
};

struct DiffDocument {
    std::vector<DiffRow> rows;
    std::size_t additions = 0;
    std::size_t deletions = 0;
};

std::vector<std::wstring> SplitLines(const std::wstring& text);
DiffDocument BuildSideBySideDiff(const std::wstring& oldText,
                                const std::wstring& newText,
                                std::size_t contextLines = 3);

}  // namespace sw
