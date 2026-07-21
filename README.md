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

## Implementation notes

- The UI is implemented with the Win32 API and common controls; MFC, Qt, and .NET are not used.
- `ReadDirectoryChangesW` monitors the repository recursively.
- Bursts of filesystem notifications are debounced for 250 ms before Git status is read again.
- GitWatcher invokes `git rev-parse`, `git status`, and `git cat-file` as child processes.

## Third-party software and licensing

GitWatcher does not bundle third-party source code or binary libraries. It links only to Windows system libraries and uses the C++ standard library supplied by the selected compiler toolchain.

Git is an external runtime requirement and is not redistributed with GitWatcher. Git is licensed separately under the [GNU General Public License version 2](https://github.com/git/git/blob/master/COPYING). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for details.
