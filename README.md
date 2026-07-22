# GitWatcher

GitWatcher is a lightweight native Windows application that monitors a Git working tree and shows changed files as a live, side-by-side diff against `HEAD`.

Version: **0.1**

## Features

- Watches the selected repository and all of its subdirectories in real time.
- Lists modified, added, deleted, renamed, copied, conflicted, and untracked files.
- Compares the current working-tree file with the corresponding blob in `HEAD`.
- Displays additions in green and deletions in red, with old and new line numbers.
- Collapses long unchanged sections while keeping three context lines around a change.
- Preserves the selected file, expanded folders, and diff scroll position when possible.
- Handles UTF-8 (with or without a BOM), UTF-16 LE/BE, and the active Windows ANSI code page.
- Avoids rendering a diff for binary files and files larger than 16 MiB.
- Uses a built-in Myers line-diff implementation; no external diff library is required.

## Requirements

- Windows 10 or Windows 11, x64
- [Git for Windows](https://git-scm.com/download/win)

GitWatcher looks for `git.exe` on `PATH` and in common Git for Windows and Visual Studio installation locations.

## Download

Download `GitWatcher.exe` or the packaged ZIP from the [latest GitHub release](https://github.com/tklee000/gitwatcher/releases/latest). No installer is required.

## Usage

Start GitWatcher and choose a folder that belongs to a Git repository with at least one commit. The application refreshes automatically when files change; the **Refresh** button is also available for a manual scan.

You can optionally pass the repository path on the command line:

```powershell
.\GitWatcher.exe D:\work\my-repository
```

The left pane shows files reported by `git status --porcelain`. Selecting a file displays the `HEAD` version on the left and the working-tree version on the right.

> [!NOTE]
> Version 0.1 compares `HEAD` directly with the working tree. Staged and unstaged changes are not shown as separate comparison modes.

## Build from source

Install Visual Studio 2019 or later with the **Desktop development with C++** workload, plus CMake 3.16 or later.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The application is generated at `build\Release\GitWatcher.exe`.

## How the diff engine works

GitWatcher contains an independent implementation of Eugene W. Myers's [O(ND) difference algorithm](https://doi.org/10.1007/BF01840446). The implementation compares complete lines using exact equality and produces a shortest edit script made of equal, delete, and insert operations.

### Original source and attribution

The O(ND) algorithm described in this README originates from the following publication:

> Eugene W. Myers, "An O(ND) Difference Algorithm and Its Variations," *Algorithmica*, volume 1, pages 251–266, 1986. DOI: [10.1007/BF01840446](https://doi.org/10.1007/BF01840446).

The paper develops the edit-graph formulation, the furthest-reaching diagonal search, and its time- and space-efficient variations. GitWatcher's `DiffEngine` uses the algorithmic method described by Myers but is independently written project code. It does not copy or incorporate source code from the paper, another GitHub repository, or an external diff library. The attribution above identifies the algorithm's academic origin; the GitWatcher implementation itself is licensed as described in [Diff engine license](#diff-engine-license).

### Myers algorithm

Assume that the old file is a sequence `A` containing `N` lines and the new file is a sequence `B` containing `M` lines. The algorithm models the comparison as a path through an edit graph:

- `(x, y)` means that `x` lines from `A` and `y` lines from `B` have been consumed.
- Moving right from `(x, y)` to `(x + 1, y)` deletes one line from `A`.
- Moving down from `(x, y)` to `(x, y + 1)` inserts one line from `B`.
- Moving diagonally to `(x + 1, y + 1)` consumes two equal lines and has no edit cost.

A path from `(0, 0)` to `(N, M)` therefore describes how to transform the old file into the new one. Myers's algorithm finds a path with the smallest number of insertions and deletions.

#### Edit depth and diagonals

`D` is the number of edits used so far. The algorithm examines `D = 0, 1, 2, ...` in increasing order, so the first path that reaches `(N, M)` is a shortest edit script.

Graph positions are grouped by diagonal `k = x - y`. For every edit depth, `V[k]` stores the largest old-file position `x` reachable on diagonal `k`. Keeping only the furthest-reaching position is sufficient: another path ending earlier on the same diagonal cannot lead to a better result.

For each reachable diagonal `k`, the next starting position is selected as follows:

```text
if k == -D or (k != D and V[k - 1] < V[k + 1]):
    x = V[k + 1]       # insertion: move down from diagonal k + 1
else:
    x = V[k - 1] + 1   # deletion: move right from diagonal k - 1

y = x - k
```

After the insertion or deletion, the algorithm follows a diagonal run of equal lines as far as possible. This zero-cost diagonal run is commonly called a **snake**:

```text
while x < N and y < M and A[x] == B[y]:
    x += 1
    y += 1

V[k] = x
```

When both `x >= N` and `y >= M`, the forward search is complete. GitWatcher's tie rule chooses the deletion branch when `V[k - 1]` and `V[k + 1]` reach the same `x` position; this makes ambiguous but equally short edit scripts deterministic.

#### Reconstructing the edit script

The furthest-reaching frontier alone gives the minimum edit count but not the individual operations. GitWatcher saves a copy of the frontier before processing each depth in `trace`.

Backtracking begins at `(N, M)` and the final edit depth:

1. Calculate the current diagonal as `k = x - y`.
2. Use the saved frontier to determine whether the path came from `k + 1` (an insertion) or `k - 1` (a deletion).
3. Walk backward over equal lines until the previous edit position is reached.
4. Record the insertion or deletion that preceded that equal run.
5. Repeat until depth zero, then reverse the collected operations.

This produces the ordered `Equal`, `Delete`, and `Insert` sequence used by the rest of the engine.

#### Complexity

Let `S = N + M` and let `D` be the number of edits in the shortest script.

- The forward search takes `O(SD)` time and degrades to `O(S²)` for completely different inputs.
- This implementation stores a frontier of size `O(S)` for every depth, so backtracking uses `O(SD)` memory.
- Files with relatively few changed lines have a small `D`, which is the case where Myers's algorithm performs especially well.

### Mapping the algorithm to `DiffEngine`

The complete pipeline is:

```text
old/new text
    -> SplitLines
    -> MyersDiff
    -> AlignEdits
    -> CollapseContext
    -> DiffDocument
```

| Stage | Implementation | Responsibility |
| --- | --- | --- |
| Line preparation | `SplitLines` | Splits decoded text on LF and removes a CR immediately before LF. Internal blank lines are preserved. |
| Shortest edit script | `MyersDiff` | Runs the frontier search and trace backtracking described above. |
| Side-by-side rows | `AlignEdits` | Converts the edit script into rows with old/new line numbers and addition/deletion totals. |
| Context folding | `CollapseContext` | Keeps a configurable number of equal lines around every change and replaces hidden runs with collapsed rows. |
| Public entry point | `BuildSideBySideDiff` | Connects all stages and returns the final `DiffDocument`. |

#### 1. Splitting text into lines

`SplitLines` converts each `std::wstring` into a vector of lines. CRLF and LF input therefore compare as the same line content. An empty input produces an empty vector, and a final newline is not represented as an additional empty line. As a result, version 0.1 does not report a difference that consists only of adding or removing the final newline.

Text encoding is handled before the diff engine is called. `GitRepository::DecodeText` converts UTF-8, UTF-16 LE/BE, or the current Windows ANSI code page into `std::wstring`; binary data is rejected by the caller.

#### 2. Building edit operations

`MyersDiff` uses the following concrete data structures:

- `frontier` is the `V` array. An offset is added to each diagonal because `k` can be negative.
- `trace` stores the frontier snapshots needed for backtracking.
- `finalDepth` records the first depth that reaches the lower-right corner of the edit graph.
- `Edit` stores one `Equal`, `Delete`, or `Insert` operation and its complete line text.

The forward pass saves `frontier` before processing a depth. During the reverse pass, `trace[depth]` therefore represents the frontier from which that depth's edit was taken. Equal lines are emitted while walking backward along a snake, followed by the insertion or deletion that entered it. The reversed result is finally restored to file order with `std::reverse`.

#### 3. Producing side-by-side rows

Myers emits only insertions and deletions; it has no native "changed line" operation. `AlignEdits` supplies that presentation layer:

1. Equal operations become rows containing the same old and new text.
2. Consecutive non-equal operations are collected into `removed` and `inserted` groups.
3. The first removed line is paired with the first inserted line, the second with the second, and so on.
4. A pair becomes `Changed`; an unpaired removal becomes `Deleted`; an unpaired insertion becomes `Added`.
5. Old and new line counters advance only when that side contains a line.

For example:

```text
Old                         New
1  alpha                    1  alpha
2  beta                     2  beta edited
3  gamma                    3  gamma
                             4  delta
```

Myers produces `Equal(alpha), Delete(beta), Insert(beta edited), Equal(gamma), Insert(delta)`. `AlignEdits` displays that sequence as:

| Kind | Old side | New side |
| --- | --- | --- |
| `Equal` | `1: alpha` | `1: alpha` |
| `Changed` | `2: beta` | `2: beta edited` |
| `Equal` | `3: gamma` | `3: gamma` |
| `Added` | | `4: delta` |

The `Changed` classification is purely a side-by-side alignment decision. Version 0.1 does not calculate character-level similarity between the paired lines.

#### 4. Collapsing unchanged context

`CollapseContext` first marks every changed row and up to `contextLines` equal rows before and after it as visible. Each remaining consecutive hidden run becomes one `Collapsed` row whose `hiddenCount` records how many unchanged lines were omitted. GitWatcher passes a context size of three. If the documents are identical, the whole document becomes a single collapsed row.

### Current diff-engine limitations

- Comparison is line based and uses exact line equality; there is no word- or character-level diff.
- Replacement alignment pairs deleted and inserted lines by position, not by textual similarity.
- A change to only the final newline is not detected.
- Move detection is not performed; moved text appears as deletions and insertions.
- Saving the full frontier trace favors simple backtracking over the linear-space variant of Myers's algorithm.
- Files larger than 16 MiB and binary files are rejected by the caller before line diffing.

The unit test in [`tests/DiffEngineTests.cpp`](tests/DiffEngineTests.cpp) covers replacement alignment, appended lines, collapsed context, and an empty-to-one-line comparison.

## Implementation notes

- The UI is implemented with the Win32 API and common controls; MFC, Qt, and .NET are not used.
- `ReadDirectoryChangesW` monitors the repository recursively.
- Bursts of filesystem notifications are debounced for 250 ms before Git status is read again.
- GitWatcher invokes `git rev-parse`, `git status`, and `git cat-file` as child processes.

## Diff engine license

The diff engine in [`src/DiffEngine.cpp`](src/DiffEngine.cpp) and [`src/DiffEngine.h`](src/DiffEngine.h) is GitWatcher project code: an independent implementation of Eugene W. Myers's O(ND) difference algorithm. It does not incorporate source code from an external diff library.

Those two files are licensed under the [MIT License](LICENSES/MIT.txt). This MIT grant applies only to files carrying the `SPDX-License-Identifier: MIT` notice; it does not automatically apply to the rest of the repository.

## Third-party software and licensing

GitWatcher does not bundle third-party source code or binary libraries. It links only to Windows system libraries and uses the C++ standard library supplied by the selected compiler toolchain.

Git is an external runtime requirement and is not redistributed with GitWatcher. Git is licensed separately under the [GNU General Public License version 2](https://github.com/git/git/blob/master/COPYING). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for details.
