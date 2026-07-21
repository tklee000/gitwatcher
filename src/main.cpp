#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "DiffEngine.h"
#include "FileWatcher.h"
#include "GitRepository.h"
#include "Version.h"

#pragma comment(linker,                                                                    \
                "\"/manifestdependency:type='win32' "                                     \
                "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "             \
                "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr wchar_t kMainWindowClass[] = L"GitWatcher.MainWindow";
constexpr wchar_t kDiffWindowClass[] = L"GitWatcher.DiffPane";
constexpr wchar_t kMainWindowCaption[] = L"GitWatcher " GITWATCHER_VERSION_WSTRING;
constexpr UINT kFileChangedMessage = WM_APP + 17;
constexpr UINT_PTR kRefreshTimer = 1;
constexpr int kChooseButtonId = 101;
constexpr int kRefreshButtonId = 102;
constexpr int kTreeId = 103;
constexpr int kToolbarHeight = 52;
constexpr int kStatusHeight = 28;
constexpr int kExplorerWidth = 300;
constexpr int kDiffHeaderHeight = 38;
constexpr int kDiffRowHeight = 22;

COLORREF Blend(const COLORREF foreground, const COLORREF background, const int alpha) {
    const auto channel = [alpha](const BYTE front, const BYTE back) {
        return static_cast<BYTE>((front * alpha + back * (255 - alpha)) / 255);
    };
    return RGB(channel(GetRValue(foreground), GetRValue(background)),
               channel(GetGValue(foreground), GetGValue(background)),
               channel(GetBValue(foreground), GetBValue(background)));
}

void FillSolid(HDC dc, const RECT& rectangle, const COLORREF color) {
    const HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rectangle, brush);
    DeleteObject(brush);
}

class DiffPane {
    struct ViewportAnchor {
        bool valid = false;
        bool pinnedToBottom = false;
        int previousFirstRow = 0;
        int rowOffset = 0;
        sw::DiffRow row;
    };

public:
    static bool Register(const HINSTANCE instance) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.hInstance = instance;
        windowClass.lpszClassName = kDiffWindowClass;
        windowClass.lpfnWndProc = WindowProcedure;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        return RegisterClassExW(&windowClass) != 0;
    }

    bool Create(const HWND parent, const HINSTANCE instance) {
        window_ = CreateWindowExW(0, kDiffWindowClass, nullptr,
                                  WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPSIBLINGS,
                                  0, 0, 0, 0, parent, nullptr, instance, this);
        return window_ != nullptr;
    }

    HWND Window() const { return window_; }

    void SetDocument(sw::DiffDocument document,
                     std::wstring path,
                     std::wstring head,
                     std::wstring message = {},
                     const bool preserveViewport = false) {
        const bool canPreserve = preserveViewport && !path.empty() && path == path_ &&
                                 message.empty() && message_.empty();
        const ViewportAnchor anchor = canPreserve ? CaptureViewport() : ViewportAnchor{};
        document_ = std::move(document);
        path_ = std::move(path);
        head_ = std::move(head);
        message_ = std::move(message);
        firstRow_ = canPreserve ? RestoreViewport(anchor) : 0;
        UpdateScrollBar();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void ShowMessage(std::wstring message) {
        SetDocument({}, {}, {}, std::move(message));
    }

private:
    int VisibleRowCount() const {
        RECT client{};
        GetClientRect(window_, &client);
        return (std::max)(1,
                          (static_cast<int>(client.bottom) - kDiffHeaderHeight) / kDiffRowHeight);
    }

    int MaximumFirstRow(const std::size_t rowCount) const {
        return (std::max)(0, static_cast<int>(rowCount) - VisibleRowCount());
    }

    ViewportAnchor CaptureViewport() const {
        ViewportAnchor anchor;
        anchor.previousFirstRow = firstRow_;
        const int visibleRows = VisibleRowCount();
        const int maximum = MaximumFirstRow(document_.rows.size());
        anchor.pinnedToBottom = document_.rows.size() > static_cast<std::size_t>(visibleRows) &&
                                firstRow_ >= maximum;
        if (document_.rows.empty() || firstRow_ < 0 ||
            firstRow_ >= static_cast<int>(document_.rows.size())) {
            return anchor;
        }

        int anchorIndex = firstRow_;
        const int lastVisible =
            (std::min)(static_cast<int>(document_.rows.size()), firstRow_ + visibleRows);
        while (anchorIndex < lastVisible &&
               document_.rows[static_cast<std::size_t>(anchorIndex)].kind ==
                   sw::DiffRowKind::Collapsed) {
            ++anchorIndex;
        }
        if (anchorIndex >= lastVisible) {
            anchorIndex = firstRow_;
        }

        anchor.valid = true;
        anchor.rowOffset = anchorIndex - firstRow_;
        anchor.row = document_.rows[static_cast<std::size_t>(anchorIndex)];
        return anchor;
    }

    int FindAnchorRow(const ViewportAnchor& anchor) const {
        int bestIndex = -1;
        int bestRank = 3;
        std::size_t bestDistance = static_cast<std::size_t>(-1);
        const std::size_t expected = static_cast<std::size_t>(
            (std::max)(0, anchor.previousFirstRow + anchor.rowOffset));

        for (std::size_t index = 0; index < document_.rows.size(); ++index) {
            const auto& candidate = document_.rows[index];
            const bool oldIdentity =
                anchor.row.oldLine > 0 && candidate.oldLine == anchor.row.oldLine &&
                candidate.oldText == anchor.row.oldText;
            const bool newIdentity =
                anchor.row.newLine > 0 && candidate.newLine == anchor.row.newLine &&
                candidate.newText == anchor.row.newText;
            const bool sameContents = candidate.kind == anchor.row.kind &&
                                      candidate.oldText == anchor.row.oldText &&
                                      candidate.newText == anchor.row.newText;
            const bool oneSideMatches =
                (!anchor.row.oldText.empty() && candidate.oldText == anchor.row.oldText) ||
                (!anchor.row.newText.empty() && candidate.newText == anchor.row.newText);

            int rank = 3;
            if (oldIdentity || newIdentity) {
                rank = 0;
            } else if (sameContents) {
                rank = 1;
            } else if (oneSideMatches) {
                rank = 2;
            }
            if (rank == 3) {
                continue;
            }

            const std::size_t distance = index > expected ? index - expected : expected - index;
            if (rank < bestRank || (rank == bestRank && distance < bestDistance)) {
                bestRank = rank;
                bestDistance = distance;
                bestIndex = static_cast<int>(index);
            }
        }
        return bestIndex;
    }

    int RestoreViewport(const ViewportAnchor& anchor) const {
        const int maximum = MaximumFirstRow(document_.rows.size());
        if (anchor.pinnedToBottom) {
            return maximum;
        }
        if (!anchor.valid) {
            return std::clamp(anchor.previousFirstRow, 0, maximum);
        }

        const int restoredAnchor = FindAnchorRow(anchor);
        if (restoredAnchor < 0) {
            return std::clamp(anchor.previousFirstRow, 0, maximum);
        }
        return std::clamp(restoredAnchor - anchor.rowOffset, 0, maximum);
    }

    static LRESULT CALLBACK WindowProcedure(HWND window,
                                            UINT message,
                                            WPARAM wParam,
                                            LPARAM lParam) {
        DiffPane* pane = reinterpret_cast<DiffPane*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            pane = static_cast<DiffPane*>(create->lpCreateParams);
            pane->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pane));
        }
        if (!pane) {
            return DefWindowProcW(window, message, wParam, lParam);
        }
        return pane->HandleMessage(message, wParam, lParam);
    }

    LRESULT HandleMessage(const UINT message, const WPARAM wParam, const LPARAM lParam) {
        switch (message) {
            case WM_CREATE:
                uiFont_ = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
                codeFont_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
                return 0;
            case WM_SIZE:
                UpdateScrollBar();
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
                Paint();
                return 0;
            case WM_MOUSEWHEEL: {
                const int notches = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
                ScrollTo(firstRow_ - notches * 3);
                return 0;
            }
            case WM_VSCROLL:
                HandleVerticalScroll(LOWORD(wParam));
                return 0;
            case WM_NCDESTROY:
                if (uiFont_) {
                    DeleteObject(uiFont_);
                    uiFont_ = nullptr;
                }
                if (codeFont_) {
                    DeleteObject(codeFont_);
                    codeFont_ = nullptr;
                }
                SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
                window_ = nullptr;
                return 0;
            default:
                return DefWindowProcW(window_, message, wParam, lParam);
        }
    }

    void UpdateScrollBar() {
        if (!window_) {
            return;
        }
        const int visibleRows = VisibleRowCount();
        const int maximum = document_.rows.empty() ? 0 : static_cast<int>(document_.rows.size()) - 1;
        firstRow_ = (std::min)(firstRow_, (std::max)(0, maximum - visibleRows + 1));

        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_PAGE | SIF_POS | SIF_RANGE;
        info.nMin = 0;
        info.nMax = maximum;
        info.nPage = static_cast<UINT>(visibleRows);
        info.nPos = firstRow_;
        SetScrollInfo(window_, SB_VERT, &info, TRUE);
    }

    void ScrollTo(int position) {
        const int maximum = MaximumFirstRow(document_.rows.size());
        position = std::clamp(position, 0, maximum);
        if (position == firstRow_) {
            return;
        }
        firstRow_ = position;
        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_POS;
        info.nPos = firstRow_;
        SetScrollInfo(window_, SB_VERT, &info, TRUE);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void HandleVerticalScroll(const int request) {
        RECT client{};
        GetClientRect(window_, &client);
        const int page =
            (std::max)(1, (static_cast<int>(client.bottom) - kDiffHeaderHeight) / kDiffRowHeight);
        int target = firstRow_;
        switch (request) {
            case SB_LINEUP:
                --target;
                break;
            case SB_LINEDOWN:
                ++target;
                break;
            case SB_PAGEUP:
                target -= page;
                break;
            case SB_PAGEDOWN:
                target += page;
                break;
            case SB_TOP:
                target = 0;
                break;
            case SB_BOTTOM:
                target = static_cast<int>(document_.rows.size());
                break;
            case SB_THUMBPOSITION:
            case SB_THUMBTRACK: {
                SCROLLINFO info{};
                info.cbSize = sizeof(info);
                info.fMask = SIF_TRACKPOS;
                GetScrollInfo(window_, SB_VERT, &info);
                target = info.nTrackPos;
                break;
            }
            default:
                break;
        }
        ScrollTo(target);
    }

    void DrawTextCell(HDC dc,
                      RECT cell,
                      const int lineNumber,
                      const std::wstring& text,
                      const COLORREF background,
                      const COLORREF gutterBackground) const {
        FillSolid(dc, cell, background);
        RECT gutter = cell;
        gutter.right = (std::min)(cell.right, cell.left + 58);
        FillSolid(dc, gutter, gutterBackground);

        HPEN separator = CreatePen(PS_SOLID, 1, RGB(224, 228, 234));
        const HGDIOBJ oldPen = SelectObject(dc, separator);
        MoveToEx(dc, gutter.right, gutter.top, nullptr);
        LineTo(dc, gutter.right, gutter.bottom);
        SelectObject(dc, oldPen);
        DeleteObject(separator);

        SetBkMode(dc, TRANSPARENT);
        if (lineNumber > 0) {
            SetTextColor(dc, RGB(120, 128, 139));
            RECT numberRect = gutter;
            numberRect.right -= 9;
            DrawTextW(dc, std::to_wstring(lineNumber).c_str(), -1, &numberRect,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        SetTextColor(dc, RGB(35, 40, 48));
        RECT textRect = cell;
        textRect.left = gutter.right + 10;
        textRect.right -= 8;
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &textRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_EXPANDTABS |
                      DT_END_ELLIPSIS);
    }

    void Paint() {
        PAINTSTRUCT paint{};
        const HDC windowDc = BeginPaint(window_, &paint);
        RECT client{};
        GetClientRect(window_, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        const int middle = width / 2;

        const HDC dc = CreateCompatibleDC(windowDc);
        const HBITMAP bitmap = CreateCompatibleBitmap(windowDc, (std::max)(1, width),
                                                       (std::max)(1, height));
        const HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
        FillSolid(dc, client, RGB(255, 255, 255));

        RECT leftHeader{0, 0, middle, kDiffHeaderHeight};
        RECT rightHeader{middle, 0, width, kDiffHeaderHeight};
        FillSolid(dc, leftHeader, RGB(43, 50, 62));
        FillSolid(dc, rightHeader, RGB(49, 57, 70));
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, uiFont_);
        SetTextColor(dc, RGB(245, 247, 250));
        leftHeader.left += 14;
        rightHeader.left += 14;
        const std::wstring leftTitle = head_.empty() ? L"HEAD" : L"HEAD  ·  " + head_;
        const std::wstring rightTitle = path_.empty() ? L"WORKING TREE" : L"WORKING TREE  ·  " + path_;
        DrawTextW(dc, leftTitle.c_str(), -1, &leftHeader,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        DrawTextW(dc, rightTitle.c_str(), -1, &rightHeader,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

        HPEN divider = CreatePen(PS_SOLID, 1, RGB(207, 212, 220));
        HGDIOBJ previousPen = SelectObject(dc, divider);
        MoveToEx(dc, middle, 0, nullptr);
        LineTo(dc, middle, height);
        SelectObject(dc, previousPen);
        DeleteObject(divider);

        if (!message_.empty()) {
            SelectObject(dc, uiFont_);
            SetTextColor(dc, RGB(102, 111, 124));
            RECT messageRect{24, kDiffHeaderHeight + 24, width - 24, height - 24};
            DrawTextW(dc, message_.c_str(), -1, &messageRect,
                      DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
        } else {
            SelectObject(dc, codeFont_);
            const COLORREF red = Blend(RGB(244, 67, 54), RGB(255, 255, 255), 35);
            const COLORREF redGutter = Blend(RGB(244, 67, 54), RGB(247, 248, 250), 55);
            const COLORREF green = Blend(RGB(32, 168, 82), RGB(255, 255, 255), 35);
            const COLORREF greenGutter = Blend(RGB(32, 168, 82), RGB(247, 248, 250), 55);
            const COLORREF empty = RGB(248, 249, 251);
            const COLORREF gutter = RGB(246, 247, 249);

            int top = kDiffHeaderHeight;
            for (std::size_t index = static_cast<std::size_t>(firstRow_);
                 index < document_.rows.size() && top < height; ++index, top += kDiffRowHeight) {
                const auto& row = document_.rows[index];
                RECT whole{0, top, width, (std::min)(height, top + kDiffRowHeight)};
                if (row.kind == sw::DiffRowKind::Collapsed) {
                    FillSolid(dc, whole, RGB(242, 245, 248));
                    SetTextColor(dc, RGB(104, 115, 129));
                    const std::wstring label = L"⋯  " + std::to_wstring(row.hiddenCount) +
                                               L" unchanged lines  ⋯";
                    DrawTextW(dc, label.c_str(), -1, &whole,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                } else {
                    RECT left{0, top, middle, whole.bottom};
                    RECT right{middle + 1, top, width, whole.bottom};
                    COLORREF leftBackground = RGB(255, 255, 255);
                    COLORREF leftGutter = gutter;
                    COLORREF rightBackground = RGB(255, 255, 255);
                    COLORREF rightGutter = gutter;
                    if (row.kind == sw::DiffRowKind::Deleted ||
                        row.kind == sw::DiffRowKind::Changed) {
                        leftBackground = red;
                        leftGutter = redGutter;
                    } else if (row.oldLine == 0) {
                        leftBackground = empty;
                        leftGutter = empty;
                    }
                    if (row.kind == sw::DiffRowKind::Added ||
                        row.kind == sw::DiffRowKind::Changed) {
                        rightBackground = green;
                        rightGutter = greenGutter;
                    } else if (row.newLine == 0) {
                        rightBackground = empty;
                        rightGutter = empty;
                    }
                    DrawTextCell(dc, left, row.oldLine, row.oldText, leftBackground, leftGutter);
                    DrawTextCell(dc, right, row.newLine, row.newText, rightBackground, rightGutter);
                }

                HPEN line = CreatePen(PS_SOLID, 1, RGB(235, 237, 241));
                previousPen = SelectObject(dc, line);
                MoveToEx(dc, 0, whole.bottom - 1, nullptr);
                LineTo(dc, width, whole.bottom - 1);
                SelectObject(dc, previousPen);
                DeleteObject(line);
            }
        }

        BitBlt(windowDc, 0, 0, width, height, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
        EndPaint(window_, &paint);
    }

    HWND window_ = nullptr;
    HFONT uiFont_ = nullptr;
    HFONT codeFont_ = nullptr;
    sw::DiffDocument document_;
    std::wstring path_;
    std::wstring head_;
    std::wstring message_ = L"왼쪽에서 변경된 파일을 선택하세요.";
    int firstRow_ = 0;
};

class GitWatcherApp {
public:
    GitWatcherApp(HINSTANCE instance, std::filesystem::path initialDirectory)
        : instance_(instance), directory_(std::move(initialDirectory)) {}

    static bool Register(const HINSTANCE instance) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.hInstance = instance;
        windowClass.lpszClassName = kMainWindowClass;
        windowClass.lpfnWndProc = WindowProcedure;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        return RegisterClassExW(&windowClass) != 0;
    }

    bool Create() {
        window_ = CreateWindowExW(0, kMainWindowClass, kMainWindowCaption,
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                  CW_USEDEFAULT, 1380, 860, nullptr, nullptr, instance_, this);
        if (!window_) {
            return false;
        }
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);
        return true;
    }

private:
    enum class RefreshReason { Initial, Automatic, Manual, DirectoryChanged };

    struct TreeViewState {
        bool hasTopItem = false;
        int topVisibleIndex = 0;
        std::wstring topItemKey;
        std::set<std::wstring> expandedKeys;
    };

    static LRESULT CALLBACK WindowProcedure(HWND window,
                                            UINT message,
                                            WPARAM wParam,
                                            LPARAM lParam) {
        auto* app = reinterpret_cast<GitWatcherApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            app = static_cast<GitWatcherApp*>(create->lpCreateParams);
            app->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (!app) {
            return DefWindowProcW(window, message, wParam, lParam);
        }
        return app->HandleMessage(message, wParam, lParam);
    }

    LRESULT HandleMessage(const UINT message, const WPARAM wParam, const LPARAM lParam) {
        switch (message) {
            case WM_CREATE:
                return OnCreate() ? 0 : -1;
            case WM_SIZE:
                Layout(LOWORD(lParam), HIWORD(lParam));
                return 0;
            case WM_GETMINMAXINFO: {
                auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
                info->ptMinTrackSize.x = 900;
                info->ptMinTrackSize.y = 560;
                return 0;
            }
            case WM_COMMAND:
                if (LOWORD(wParam) == kChooseButtonId) {
                    ChooseDirectory();
                } else if (LOWORD(wParam) == kRefreshButtonId) {
                    ScanNow(RefreshReason::Manual);
                }
                return 0;
            case WM_NOTIFY:
                return OnNotify(reinterpret_cast<NMHDR*>(lParam));
            case WM_TIMER:
                if (wParam == kRefreshTimer) {
                    KillTimer(window_, kRefreshTimer);
                    ScanNow(RefreshReason::Automatic);
                }
                return 0;
            case kFileChangedMessage:
                KillTimer(window_, kRefreshTimer);
                SetTimer(window_, kRefreshTimer, 250, nullptr);
                return 0;
            case WM_PAINT:
                PaintFrame();
                return 0;
            case WM_CTLCOLORSTATIC: {
                const HDC dc = reinterpret_cast<HDC>(wParam);
                SetBkColor(dc, RGB(255, 255, 255));
                return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
            }
            case WM_DESTROY:
                watcher_.Stop();
                if (uiFont_) {
                    DeleteObject(uiFont_);
                    uiFont_ = nullptr;
                }
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProcW(window_, message, wParam, lParam);
        }
    }

    bool OnCreate() {
        uiFont_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");
        rootLabel_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     16, 16, 0, 0, window_, nullptr, instance_, nullptr);
        chooseButton_ = CreateWindowExW(0, L"BUTTON", L"폴더 선택", WS_CHILD | WS_VISIBLE,
                                        0, 0, 0, 0, window_,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChooseButtonId)),
                                        instance_, nullptr);
        refreshButton_ = CreateWindowExW(0, L"BUTTON", L"새로 고침", WS_CHILD | WS_VISIBLE,
                                         0, 0, 0, 0, window_,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshButtonId)),
                                         instance_, nullptr);
        tree_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS |
                                    TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
                                0, 0, 0, 0, window_,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTreeId)), instance_,
                                nullptr);
        status_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                  0, 0, 0, 0, window_, nullptr, instance_, nullptr);
        if (!rootLabel_ || !chooseButton_ || !refreshButton_ || !tree_ || !status_ ||
            !diffPane_.Create(window_, instance_)) {
            return false;
        }

        for (const HWND control : {rootLabel_, chooseButton_, refreshButton_, tree_, status_}) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
        }
        TreeView_SetBkColor(tree_, RGB(250, 251, 252));
        TreeView_SetTextColor(tree_, RGB(38, 44, 53));
        ScanNow(RefreshReason::Initial);
        return true;
    }

    void Layout(const int width, const int height) const {
        const int buttonWidth = 104;
        const int rightPadding = 12;
        const int buttonHeight = 32;
        MoveWindow(chooseButton_, width - rightPadding - buttonWidth * 2 - 8, 10, buttonWidth,
                   buttonHeight, TRUE);
        MoveWindow(refreshButton_, width - rightPadding - buttonWidth, 10, buttonWidth,
                   buttonHeight, TRUE);
        MoveWindow(rootLabel_, 16, 15,
                   (std::max)(20, width - buttonWidth * 2 - rightPadding - 52), 24, TRUE);

        const int contentHeight = (std::max)(0, height - kToolbarHeight - kStatusHeight);
        const int explorerWidth = (std::min)(kExplorerWidth, width / 3);
        MoveWindow(tree_, 0, kToolbarHeight, explorerWidth, contentHeight, TRUE);
        MoveWindow(diffPane_.Window(), explorerWidth + 1, kToolbarHeight,
                   (std::max)(0, width - explorerWidth - 1), contentHeight, TRUE);
        MoveWindow(status_, 12, height - kStatusHeight + 5, (std::max)(0, width - 24), 20,
                   TRUE);
    }

    void PaintFrame() const {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window_, &paint);
        RECT client{};
        GetClientRect(window_, &client);
        RECT toolbar{0, 0, client.right, kToolbarHeight};
        RECT statusBar{0, client.bottom - kStatusHeight, client.right, client.bottom};
        FillSolid(dc, toolbar, RGB(255, 255, 255));
        FillSolid(dc, statusBar, RGB(255, 255, 255));
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(218, 222, 228));
        const HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, 0, kToolbarHeight - 1, nullptr);
        LineTo(dc, client.right, kToolbarHeight - 1);
        MoveToEx(dc, 0, client.bottom - kStatusHeight, nullptr);
        LineTo(dc, client.right, client.bottom - kStatusHeight);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        EndPaint(window_, &paint);
    }

    void EnsureWatcher(const std::filesystem::path& directory) {
        std::error_code error;
        const auto normalized = std::filesystem::weakly_canonical(directory, error);
        const auto target = error ? directory : normalized;
        if (target == watchedDirectory_) {
            return;
        }
        watcher_.Stop();
        watchedDirectory_ = target;
        watcher_.Start(watchedDirectory_, window_, kFileChangedMessage);
    }

    static std::wstring NormalizeRepositoryPath(std::wstring path) {
        std::replace(path.begin(), path.end(), L'\\', L'/');
        return path;
    }

    static bool SameTreeContents(const sw::RepositorySnapshot& left,
                                 const sw::RepositorySnapshot& right) {
        if (left.isRepository != right.isRepository || left.root != right.root ||
            left.files.size() != right.files.size()) {
            return false;
        }
        for (std::size_t index = 0; index < left.files.size(); ++index) {
            const auto& leftFile = left.files[index];
            const auto& rightFile = right.files[index];
            if (leftFile.path != rightFile.path || leftFile.oldPath != rightFile.oldPath ||
                leftFile.kind != rightFile.kind || leftFile.indexStatus != rightFile.indexStatus ||
                leftFile.workTreeStatus != rightFile.workTreeStatus) {
                return false;
            }
        }
        return true;
    }

    std::wstring TreeItemText(const HTREEITEM item) const {
        wchar_t text[1024]{};
        TVITEMW data{};
        data.mask = TVIF_TEXT;
        data.hItem = item;
        data.pszText = text;
        data.cchTextMax = static_cast<int>(sizeof(text) / sizeof(text[0]));
        return TreeView_GetItem(tree_, &data) ? std::wstring(text) : std::wstring{};
    }

    std::wstring TreeItemKey(const HTREEITEM item,
                             const sw::RepositorySnapshot& sourceSnapshot) const {
        if (!item) {
            return {};
        }

        TVITEMW data{};
        data.mask = TVIF_PARAM;
        data.hItem = item;
        if (TreeView_GetItem(tree_, &data) && data.lParam > 0) {
            const std::size_t index = static_cast<std::size_t>(data.lParam - 1);
            if (index < sourceSnapshot.files.size()) {
                return L"F:" + NormalizeRepositoryPath(sourceSnapshot.files[index].path);
            }
        }

        if (!TreeView_GetParent(tree_, item)) {
            return L"D:";
        }

        std::vector<std::wstring> parts;
        for (HTREEITEM current = item; current && TreeView_GetParent(tree_, current);
             current = TreeView_GetParent(tree_, current)) {
            parts.push_back(TreeItemText(current));
        }
        std::wstring path;
        for (auto part = parts.rbegin(); part != parts.rend(); ++part) {
            path += path.empty() ? *part : L"/" + *part;
        }
        return L"D:" + path;
    }

    void CaptureExpandedItems(const HTREEITEM first,
                              const sw::RepositorySnapshot& sourceSnapshot,
                              TreeViewState& state) const {
        for (HTREEITEM item = first; item; item = TreeView_GetNextSibling(tree_, item)) {
            const HTREEITEM child = TreeView_GetChild(tree_, item);
            if (child) {
                if ((TreeView_GetItemState(tree_, item, TVIS_EXPANDED) & TVIS_EXPANDED) != 0) {
                    state.expandedKeys.insert(TreeItemKey(item, sourceSnapshot));
                }
                CaptureExpandedItems(child, sourceSnapshot, state);
            }
        }
    }

    TreeViewState CaptureTreeViewState(const sw::RepositorySnapshot& sourceSnapshot) const {
        TreeViewState state;
        const HTREEITEM firstVisible = TreeView_GetFirstVisible(tree_);
        if (firstVisible) {
            state.hasTopItem = true;
            state.topItemKey = TreeItemKey(firstVisible, sourceSnapshot);
            HTREEITEM visible = TreeView_GetRoot(tree_);
            while (visible && visible != firstVisible) {
                visible = TreeView_GetNextVisible(tree_, visible);
                ++state.topVisibleIndex;
            }
        }
        CaptureExpandedItems(TreeView_GetRoot(tree_), sourceSnapshot, state);
        return state;
    }

    std::size_t FindFileIndex(const std::wstring& path) const {
        for (std::size_t index = 0; index < snapshot_.files.size(); ++index) {
            if (snapshot_.files[index].path == path) {
                return index;
            }
        }
        return snapshot_.files.size();
    }

    void ScanNow(const RefreshReason reason) {
        const bool preserveViews =
            reason == RefreshReason::Automatic || reason == RefreshReason::Manual;
        const std::wstring previousSelection = selectedPath_;
        const TreeViewState treeState =
            preserveViews ? CaptureTreeViewState(snapshot_) : TreeViewState{};

        auto next = repository_.Scan(directory_);
        const bool treeChanged = !SameTreeContents(snapshot_, next);
        if (next.isRepository) {
            directory_ = next.root;
        }
        EnsureWatcher(directory_);
        snapshot_ = std::move(next);
        SetWindowTextW(rootLabel_, (L"감시 폴더   " + directory_.wstring()).c_str());

        if (!preserveViews || treeChanged) {
            PopulateTree(previousSelection, preserveViews ? &treeState : nullptr);
        }

        if (!snapshot_.isRepository) {
            SetWindowTextW(status_, snapshot_.error.c_str());
            diffPane_.ShowMessage(snapshot_.error + L"\n\n상단의 ‘폴더 선택’에서 HEAD 커밋이 있는 저장소를 선택하세요.");
            return;
        }
        if (snapshot_.files.empty()) {
            const std::wstring status = L"HEAD " + snapshot_.head +
                                        L"  ·  변경 파일 없음  ·  실시간 감시 중";
            SetWindowTextW(status_, status.c_str());
            diffPane_.ShowMessage(L"작업 트리가 깨끗합니다.\n파일이 수정되면 자동으로 여기에 표시됩니다.");
            return;
        }

        const std::size_t selectedIndex = FindFileIndex(selectedPath_);
        if (selectedIndex < snapshot_.files.size()) {
            LoadDiff(selectedIndex, preserveViews && selectedPath_ == previousSelection);
        }
    }

    void PopulateTree(const std::wstring& preferredPath, const TreeViewState* viewState) {
        populatingTree_ = true;
        SendMessageW(tree_, WM_SETREDRAW, FALSE, 0);
        TreeView_DeleteAllItems(tree_);
        selectedPath_.clear();

        const std::wstring rootName = snapshot_.isRepository
                                          ? snapshot_.root.filename().wstring()
                                          : directory_.filename().wstring();
        TVINSERTSTRUCTW rootInsert{};
        rootInsert.hParent = TVI_ROOT;
        rootInsert.hInsertAfter = TVI_LAST;
        rootInsert.item.mask = TVIF_TEXT | TVIF_PARAM;
        rootInsert.item.pszText = const_cast<wchar_t*>(rootName.c_str());
        rootInsert.item.lParam = 0;
        const HTREEITEM rootItem = TreeView_InsertItem(tree_, &rootInsert);
        std::map<std::wstring, HTREEITEM> directories;
        std::map<std::wstring, HTREEITEM> itemsByKey;
        directories[L""] = rootItem;
        itemsByKey[L"D:"] = rootItem;
        HTREEITEM preferredItem = nullptr;
        HTREEITEM firstFile = nullptr;
        std::size_t preferredIndex = snapshot_.files.size();
        std::size_t firstFileIndex = snapshot_.files.size();

        for (std::size_t fileIndex = 0; fileIndex < snapshot_.files.size(); ++fileIndex) {
            const auto& file = snapshot_.files[fileIndex];
            const std::wstring normalized = NormalizeRepositoryPath(file.path);
            std::size_t start = 0;
            std::wstring cumulative;
            HTREEITEM parent = rootItem;
            while (true) {
                const std::size_t slash = normalized.find(L'/', start);
                if (slash == std::wstring::npos) {
                    break;
                }
                const std::wstring part = normalized.substr(start, slash - start);
                cumulative += cumulative.empty() ? part : L"/" + part;
                const auto found = directories.find(cumulative);
                if (found != directories.end()) {
                    parent = found->second;
                } else {
                    TVINSERTSTRUCTW directoryInsert{};
                    directoryInsert.hParent = parent;
                    directoryInsert.hInsertAfter = TVI_SORT;
                    directoryInsert.item.mask = TVIF_TEXT | TVIF_PARAM;
                    directoryInsert.item.pszText = const_cast<wchar_t*>(part.c_str());
                    directoryInsert.item.lParam = 0;
                    parent = TreeView_InsertItem(tree_, &directoryInsert);
                    directories[cumulative] = parent;
                    itemsByKey[L"D:" + cumulative] = parent;
                }
                start = slash + 1;
            }

            const std::wstring filename = normalized.substr(start);
            const std::wstring label = L"[" + sw::ChangeKindLabel(file.kind) + L"]  " + filename;
            TVINSERTSTRUCTW fileInsert{};
            fileInsert.hParent = parent;
            fileInsert.hInsertAfter = TVI_SORT;
            fileInsert.item.mask = TVIF_TEXT | TVIF_PARAM;
            fileInsert.item.pszText = const_cast<wchar_t*>(label.c_str());
            fileInsert.item.lParam = static_cast<LPARAM>(fileIndex + 1);
            const HTREEITEM item = TreeView_InsertItem(tree_, &fileInsert);
            itemsByKey[L"F:" + normalized] = item;
            if (!firstFile) {
                firstFile = item;
                firstFileIndex = fileIndex;
            }
            if (file.path == preferredPath) {
                preferredItem = item;
                preferredIndex = fileIndex;
            }
        }

        for (const auto& entry : directories) {
            const std::wstring key = L"D:" + entry.first;
            const bool expand = !viewState || viewState->expandedKeys.count(key) != 0;
            TreeView_Expand(tree_, entry.second, expand ? TVE_EXPAND : TVE_COLLAPSE);
        }

        const HTREEITEM selectedItem = preferredItem ? preferredItem : firstFile;
        const std::size_t selectedIndex = preferredItem ? preferredIndex : firstFileIndex;
        if (selectedItem && selectedIndex < snapshot_.files.size()) {
            TreeView_SelectItem(tree_, selectedItem);
            selectedPath_ = snapshot_.files[selectedIndex].path;
        }

        if (viewState && viewState->hasTopItem) {
            HTREEITEM topItem = nullptr;
            const auto matchingTop = itemsByKey.find(viewState->topItemKey);
            if (matchingTop != itemsByKey.end()) {
                topItem = matchingTop->second;
            } else {
                topItem = TreeView_GetRoot(tree_);
                for (int index = 0; topItem && index < viewState->topVisibleIndex; ++index) {
                    const HTREEITEM next = TreeView_GetNextVisible(tree_, topItem);
                    if (!next) {
                        break;
                    }
                    topItem = next;
                }
            }
            if (topItem) {
                SendMessageW(tree_, TVM_SELECTITEM, TVGN_FIRSTVISIBLE,
                             reinterpret_cast<LPARAM>(topItem));
            }
        } else if (selectedItem) {
            TreeView_EnsureVisible(tree_, selectedItem);
        }

        SendMessageW(tree_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(tree_, nullptr, TRUE);
        populatingTree_ = false;
    }

    LRESULT OnNotify(const NMHDR* header) {
        if (!header || header->idFrom != kTreeId) {
            return 0;
        }
        if (populatingTree_) {
            return 0;
        }
        if (header->code == TVN_SELCHANGEDW) {
            const auto* treeNotification = reinterpret_cast<const NMTREEVIEWW*>(header);
            const LPARAM value = treeNotification->itemNew.lParam;
            if (value > 0) {
                const std::size_t index = static_cast<std::size_t>(value - 1);
                if (index < snapshot_.files.size()) {
                    LoadDiff(index, false);
                }
            }
        }
        return 0;
    }

    void LoadDiff(const std::size_t index, const bool preserveViewport) {
        const auto& file = snapshot_.files[index];
        selectedPath_ = file.path;
        constexpr std::size_t maximumTextSize = 16 * 1024 * 1024;

        sw::BlobResult oldBlob;
        if (file.kind == sw::ChangeKind::Added) {
            oldBlob.ok = true;
        } else {
            const std::wstring oldPath = file.oldPath.empty() ? file.path : file.oldPath;
            oldBlob = repository_.ReadHeadBlob(snapshot_.root, oldPath);
        }

        sw::BlobResult newBlob;
        if (file.kind == sw::ChangeKind::Deleted) {
            newBlob.ok = true;
        } else {
            newBlob = repository_.ReadWorkingFile(snapshot_.root, file.path);
        }

        if ((!oldBlob.ok && file.kind != sw::ChangeKind::Added) ||
            (!newBlob.ok && file.kind != sw::ChangeKind::Deleted)) {
            diffPane_.SetDocument({}, file.path, snapshot_.head,
                                  L"파일을 읽는 동안 변경되었거나 접근할 수 없습니다.");
            return;
        }
        if (oldBlob.bytes.size() > maximumTextSize || newBlob.bytes.size() > maximumTextSize) {
            diffPane_.SetDocument({}, file.path, snapshot_.head,
                                  L"16MB보다 큰 파일은 이 프로토타입에서 diff를 표시하지 않습니다.");
            return;
        }

        std::wstring oldText;
        std::wstring newText;
        if (!sw::DecodeText(oldBlob.bytes, oldText) || !sw::DecodeText(newBlob.bytes, newText)) {
            diffPane_.SetDocument({}, file.path, snapshot_.head,
                                  L"바이너리 파일은 줄 단위 diff를 표시할 수 없습니다.");
            return;
        }

        auto document = sw::BuildSideBySideDiff(oldText, newText, 3);
        const std::size_t additions = document.additions;
        const std::size_t deletions = document.deletions;
        diffPane_.SetDocument(std::move(document), file.path, snapshot_.head, {},
                              preserveViewport);
        const std::wstring status = L"HEAD " + snapshot_.head + L"  ·  " + file.path +
                                    L"  ·  +" + std::to_wstring(additions) + L"  -" +
                                    std::to_wstring(deletions) + L"  ·  실시간 감시 중";
        SetWindowTextW(status_, status.c_str());
    }

    void ChooseDirectory() {
        IFileDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dialog)))) {
            return;
        }
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dialog->SetTitle(L"감시할 Git 저장소 선택");

        IShellItem* currentFolder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(directory_.c_str(), nullptr,
                                                  IID_PPV_ARGS(&currentFolder)))) {
            dialog->SetFolder(currentFolder);
            currentFolder->Release();
        }

        if (SUCCEEDED(dialog->Show(window_))) {
            IShellItem* selected = nullptr;
            if (SUCCEEDED(dialog->GetResult(&selected))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(selected->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    directory_ = path;
                    selectedPath_.clear();
                    CoTaskMemFree(path);
                    ScanNow(RefreshReason::DirectoryChanged);
                }
                selected->Release();
            }
        }
        dialog->Release();
    }

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND rootLabel_ = nullptr;
    HWND chooseButton_ = nullptr;
    HWND refreshButton_ = nullptr;
    HWND tree_ = nullptr;
    HWND status_ = nullptr;
    HFONT uiFont_ = nullptr;
    DiffPane diffPane_;
    sw::GitRepository repository_;
    sw::FileWatcher watcher_;
    sw::RepositorySnapshot snapshot_;
    std::filesystem::path directory_;
    std::filesystem::path watchedDirectory_;
    std::wstring selectedPath_;
    bool populatingTree_ = false;
};

std::filesystem::path InitialDirectory() {
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    std::filesystem::path directory;
    if (arguments && argumentCount >= 2) {
        directory = arguments[1];
    } else {
        std::error_code error;
        directory = std::filesystem::current_path(error);
        if (error) {
            wchar_t modulePath[MAX_PATH]{};
            GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
            directory = std::filesystem::path(modulePath).parent_path();
        }
    }
    if (arguments) {
        LocalFree(arguments);
    }
    return directory;
}

}  // namespace

int WINAPI wWinMain(const HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    if (!DiffPane::Register(instance) || !GitWatcherApp::Register(instance)) {
        if (SUCCEEDED(comResult)) {
            CoUninitialize();
        }
        return 1;
    }

    GitWatcherApp app(instance, InitialDirectory());
    if (!app.Create()) {
        if (SUCCEEDED(comResult)) {
            CoUninitialize();
        }
        return 1;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (SUCCEEDED(comResult)) {
        CoUninitialize();
    }
    return static_cast<int>(message.wParam);
}
