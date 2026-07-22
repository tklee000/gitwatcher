# Third-Party Notices

GitWatcher 0.1 does not incorporate or redistribute any third-party source code or binary libraries.

The built-in diff engine is GitWatcher project code and is an independent implementation of the Myers O(ND) difference algorithm. It is not a third-party component. The engine source files are separately licensed under the MIT License as documented in `LICENSES/MIT.txt`.

## External runtime dependency

GitWatcher invokes a separately installed copy of Git (`git.exe`) to inspect repositories. Git is not included in GitWatcher source archives or release packages.

Git is distributed under the GNU General Public License version 2. Its license text and source are available from the upstream Git project:

- License: <https://github.com/git/git/blob/master/COPYING>
- Source: <https://github.com/git/git>

## Platform and toolchain components

The application uses Windows APIs and links to Windows system libraries (`comctl32`, `shell32`, and `ole32`). It also uses the C++ standard library and runtime supplied by the compiler toolchain. These are platform or toolchain components and are not bundled in this repository.
