#include "DiffEngine.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace sw {
namespace {

enum class EditKind { Equal, Delete, Insert };

struct Edit {
    EditKind kind;
    std::wstring text;
};

std::vector<Edit> MyersDiff(const std::vector<std::wstring>& left,
                            const std::vector<std::wstring>& right) {
    const int n = static_cast<int>(left.size());
    const int m = static_cast<int>(right.size());
    const int maximum = n + m;
    const int offset = maximum + 1;

    std::vector<int> frontier(static_cast<std::size_t>(maximum * 2 + 3), 0);
    std::vector<std::vector<int>> trace;
    trace.reserve(static_cast<std::size_t>(maximum + 1));

    int finalDepth = 0;
    bool finished = false;
    for (int depth = 0; depth <= maximum && !finished; ++depth) {
        trace.push_back(frontier);
        for (int diagonal = -depth; diagonal <= depth; diagonal += 2) {
            int x = 0;
            if (diagonal == -depth ||
                (diagonal != depth &&
                 frontier[static_cast<std::size_t>(offset + diagonal - 1)] <
                     frontier[static_cast<std::size_t>(offset + diagonal + 1)])) {
                x = frontier[static_cast<std::size_t>(offset + diagonal + 1)];
            } else {
                x = frontier[static_cast<std::size_t>(offset + diagonal - 1)] + 1;
            }
            int y = x - diagonal;
            while (x < n && y < m && left[static_cast<std::size_t>(x)] ==
                                          right[static_cast<std::size_t>(y)]) {
                ++x;
                ++y;
            }
            frontier[static_cast<std::size_t>(offset + diagonal)] = x;
            if (x >= n && y >= m) {
                finalDepth = depth;
                finished = true;
                break;
            }
        }
    }

    std::vector<Edit> reversed;
    int x = n;
    int y = m;
    for (int depth = finalDepth; depth >= 0; --depth) {
        const auto& previous = trace[static_cast<std::size_t>(depth)];
        const int diagonal = x - y;
        int previousDiagonal = 0;
        if (diagonal == -depth ||
            (diagonal != depth &&
             previous[static_cast<std::size_t>(offset + diagonal - 1)] <
                 previous[static_cast<std::size_t>(offset + diagonal + 1)])) {
            previousDiagonal = diagonal + 1;
        } else {
            previousDiagonal = diagonal - 1;
        }

        const int previousX = previous[static_cast<std::size_t>(offset + previousDiagonal)];
        const int previousY = previousX - previousDiagonal;
        while (x > previousX && y > previousY) {
            reversed.push_back({EditKind::Equal, left[static_cast<std::size_t>(x - 1)]});
            --x;
            --y;
        }

        if (depth == 0) {
            break;
        }
        if (x == previousX) {
            reversed.push_back({EditKind::Insert, right[static_cast<std::size_t>(y - 1)]});
            --y;
        } else {
            reversed.push_back({EditKind::Delete, left[static_cast<std::size_t>(x - 1)]});
            --x;
        }
    }

    std::reverse(reversed.begin(), reversed.end());
    return reversed;
}

std::vector<DiffRow> AlignEdits(const std::vector<Edit>& edits,
                                std::size_t& additions,
                                std::size_t& deletions) {
    std::vector<DiffRow> rows;
    int oldLine = 1;
    int newLine = 1;
    std::size_t index = 0;

    while (index < edits.size()) {
        if (edits[index].kind == EditKind::Equal) {
            rows.push_back({DiffRowKind::Equal, oldLine++, newLine++, edits[index].text,
                            edits[index].text, 0});
            ++index;
            continue;
        }

        std::vector<std::wstring> removed;
        std::vector<std::wstring> inserted;
        while (index < edits.size() && edits[index].kind != EditKind::Equal) {
            if (edits[index].kind == EditKind::Delete) {
                removed.push_back(edits[index].text);
            } else {
                inserted.push_back(edits[index].text);
            }
            ++index;
        }

        additions += inserted.size();
        deletions += removed.size();
        const std::size_t count = (std::max)(removed.size(), inserted.size());
        for (std::size_t item = 0; item < count; ++item) {
            DiffRow row;
            const bool hasOld = item < removed.size();
            const bool hasNew = item < inserted.size();
            if (hasOld && hasNew) {
                row.kind = DiffRowKind::Changed;
            } else if (hasOld) {
                row.kind = DiffRowKind::Deleted;
            } else {
                row.kind = DiffRowKind::Added;
            }
            if (hasOld) {
                row.oldLine = oldLine++;
                row.oldText = removed[item];
            }
            if (hasNew) {
                row.newLine = newLine++;
                row.newText = inserted[item];
            }
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

std::vector<DiffRow> CollapseContext(const std::vector<DiffRow>& rows,
                                     const std::size_t contextLines) {
    if (rows.empty()) {
        return {};
    }

    std::vector<bool> visible(rows.size(), false);
    bool hasChange = false;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (rows[index].kind == DiffRowKind::Equal) {
            continue;
        }
        hasChange = true;
        const std::size_t first = index > contextLines ? index - contextLines : 0;
        const std::size_t last = (std::min)(rows.size() - 1, index + contextLines);
        for (std::size_t nearby = first; nearby <= last; ++nearby) {
            visible[nearby] = true;
        }
    }

    if (!hasChange) {
        DiffRow collapsed;
        collapsed.kind = DiffRowKind::Collapsed;
        collapsed.hiddenCount = rows.size();
        return {collapsed};
    }

    std::vector<DiffRow> result;
    std::size_t index = 0;
    while (index < rows.size()) {
        if (visible[index]) {
            result.push_back(rows[index]);
            ++index;
            continue;
        }
        const std::size_t firstHidden = index;
        while (index < rows.size() && !visible[index]) {
            ++index;
        }
        DiffRow collapsed;
        collapsed.kind = DiffRowKind::Collapsed;
        collapsed.hiddenCount = index - firstHidden;
        result.push_back(std::move(collapsed));
    }
    return result;
}

}  // namespace

std::vector<std::wstring> SplitLines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t newline = text.find(L'\n', start);
        const std::size_t end = newline == std::wstring::npos ? text.size() : newline;
        std::size_t contentEnd = end;
        if (contentEnd > start && text[contentEnd - 1] == L'\r') {
            --contentEnd;
        }
        lines.push_back(text.substr(start, contentEnd - start));
        if (newline == std::wstring::npos) {
            break;
        }
        start = newline + 1;
    }
    return lines;
}

DiffDocument BuildSideBySideDiff(const std::wstring& oldText,
                                 const std::wstring& newText,
                                 const std::size_t contextLines) {
    DiffDocument document;
    const auto edits = MyersDiff(SplitLines(oldText), SplitLines(newText));
    auto aligned = AlignEdits(edits, document.additions, document.deletions);
    document.rows = CollapseContext(aligned, contextLines);
    return document;
}

}  // namespace sw
