// Finder.cpp
// Сборка (MinGW):
//   g++ -std=c++17 -O2 -static -mwindows Finder.cpp -o Finder.exe -lcomctl32 -lole32 -lshell32
//
// Сборка (MSVC):
//   cl /std:c++17 /EHsc /O2 /utf-8 Finder.cpp /Fe:Finder.exe /link user32.lib gdi32.lib comctl32.lib ole32.lib shell32.lib

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

namespace fs = std::filesystem;

#define ID_EDIT_PATH     101
#define ID_BTN_BROWSE    102
#define ID_EDIT_PATTERN  103
#define ID_CHK_MASK      104
#define ID_CHK_ICASE     105
#define ID_EDIT_EXT      106
#define ID_BTN_SEARCH    107
#define ID_EDIT_RESULTS  108
#define ID_BTN_CLEAR     109

static HWND hPath, hBrowse, hPattern, hMask, hIcase, hExt,
hSearch, hResults, hClear;
static std::atomic<bool> g_running{ false };

// ---------- Утилиты ----------

static std::string toLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static std::string globToRegex(const std::string& mask) {
    std::string re;
    for (char c : mask) {
        switch (c) {
        case '*': re += ".*"; break;
        case '?': re += ".";  break;
        case '.': case '^': case '$': case '+':
        case '(': case ')': case '[': case ']':
        case '{': case '}': case '|': case '\\':
            re += '\\'; re += c; break;
        default: re += c;
        }
    }
    return re;
}

static bool contains(const std::string& hay, const std::string& needle, bool icase) {
    if (!icase) return hay.find(needle) != std::string::npos;
    return toLower(hay).find(toLower(needle)) != std::string::npos;
}

static bool matchesMask(const std::string& line, const std::string& reStr, bool icase) {
    try {
        auto flags = std::regex::ECMAScript;
        if (icase) flags |= std::regex::icase;
        std::regex re(reStr, flags);
        return std::regex_search(line, re);
    }
    catch (...) { return false; }
}

static bool hasAllowedExt(const fs::path& p, const std::vector<std::string>& exts) {
    if (exts.empty()) return true;
    std::string e = toLower(p.extension().string());
    for (auto& x : exts) if (e == toLower(x)) return true;
    return false;
}

static std::vector<fs::path> collectFiles(const fs::path& root,
    const std::vector<std::string>& exts) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (fs::is_regular_file(root, ec)) { out.push_back(root); return out; }
    if (!fs::is_directory(root, ec)) return out;

    for (auto it = fs::recursive_directory_iterator(
        root, fs::directory_options::skip_permission_denied, ec);
        it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (it->is_regular_file(ec) && hasAllowedExt(it->path(), exts))
            out.push_back(it->path());
    }
    return out;
}

// ---------- UTF-8 <-> wide ----------

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
        nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
        s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring GetText(HWND h) {
    int len = GetWindowTextLengthW(h);
    std::wstring s(len, L'\0');
    GetWindowTextW(h, s.data(), len + 1);
    return s;
}

static void AppendResults(const std::wstring& text) {
    int len = GetWindowTextLengthW(hResults);
    SendMessageW(hResults, EM_SETSEL, len, len);
    SendMessageW(hResults, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
}

// ---------- Фоновый поиск ----------

static void RunSearch(std::wstring wpath, std::wstring wpattern,
    bool useMask, bool icase, std::vector<std::string> exts)
{
    std::string path = WideToUtf8(wpath);
    std::string pattern = WideToUtf8(wpattern);

    std::error_code ec;
    if (!fs::exists(path, ec)) {
        AppendResults(L"Путь не найден.\r\n");
        g_running = false;
        EnableWindow(hSearch, TRUE);
        return;
    }

    std::string reStr = useMask ? globToRegex(pattern) : "";
    auto files = collectFiles(path, exts);

    int matchedFiles = 0;
    for (const auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        if (!in) continue;

        std::string line;
        int lineno = 0;
        bool headerPrinted = false;
        std::wstring buffer;

        while (std::getline(in, line)) {
            ++lineno;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            bool hit = useMask ? matchesMask(line, reStr, icase)
                : contains(line, pattern, icase);
            if (hit) {
                if (!headerPrinted) {
                    buffer += L"\r\n=== ";
                    buffer += Utf8ToWide(f.string());
                    buffer += L" ===\r\n";
                    headerPrinted = true;
                }
                buffer += L"  ";
                buffer += std::to_wstring(lineno);
                buffer += L": ";
                buffer += Utf8ToWide(line);
                buffer += L"\r\n";

                if (buffer.size() > 4000) {
                    AppendResults(buffer);
                    buffer.clear();
                }
            }
        }
        if (!buffer.empty()) AppendResults(buffer);
        if (headerPrinted) ++matchedFiles;
    }

    AppendResults(L"\r\nФайлов с совпадениями: " +
        std::to_wstring(matchedFiles) + L"\r\n");

    g_running = false;
    EnableWindow(hSearch, TRUE);
}

// ---------- Окно ----------

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        CreateWindowW(L"STATIC", L"Папка или файл:",
            WS_CHILD | WS_VISIBLE, 10, 10, 120, 20,
            hwnd, nullptr, nullptr, nullptr);
        hPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            L"G:\\PAN\\Finder\\Finder",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 130, 8, 450, 24,
            hwnd, (HMENU)ID_EDIT_PATH, nullptr, nullptr);
        hBrowse = CreateWindowW(L"BUTTON", L"Обзор…",
            WS_CHILD | WS_VISIBLE, 590, 8, 90, 24,
            hwnd, (HMENU)ID_BTN_BROWSE, nullptr, nullptr);

        CreateWindowW(L"STATIC", L"Что искать:",
            WS_CHILD | WS_VISIBLE, 10, 44, 120, 20,
            hwnd, nullptr, nullptr, nullptr);
        hPattern = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 130, 42, 450, 24,
            hwnd, (HMENU)ID_EDIT_PATTERN, nullptr, nullptr);

        hMask = CreateWindowW(L"BUTTON", L"Маска (*, ?)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 130, 72, 140, 22,
            hwnd, (HMENU)ID_CHK_MASK, nullptr, nullptr);
        hIcase = CreateWindowW(L"BUTTON", L"Без учёта регистра",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 280, 72, 180, 22,
            hwnd, (HMENU)ID_CHK_ICASE, nullptr, nullptr);

        CreateWindowW(L"STATIC", L"Расширения (.txt,.py):",
            WS_CHILD | WS_VISIBLE, 10, 100, 160, 20,
            hwnd, nullptr, nullptr, nullptr);
        hExt = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 175, 98, 405, 24,
            hwnd, (HMENU)ID_EDIT_EXT, nullptr, nullptr);

        hSearch = CreateWindowW(L"BUTTON", L"Найти",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 130, 132, 120, 28,
            hwnd, (HMENU)ID_BTN_SEARCH, nullptr, nullptr);
        hClear = CreateWindowW(L"BUTTON", L"Очистить",
            WS_CHILD | WS_VISIBLE, 260, 132, 120, 28,
            hwnd, (HMENU)ID_BTN_CLEAR, nullptr, nullptr);

        CreateWindowW(L"STATIC", L"Результаты:",
            WS_CHILD | WS_VISIBLE, 10, 168, 120, 20,
            hwnd, nullptr, nullptr, nullptr);
        hResults = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            10, 188, 670, 330,
            hwnd, (HMENU)ID_EDIT_RESULTS, nullptr, nullptr);

        EnumChildWindows(hwnd, [](HWND h, LPARAM p) -> BOOL {
            SendMessageW(h, WM_SETFONT, (WPARAM)p, TRUE);
            return TRUE;
            }, (LPARAM)hFont);
        return 0;
    }

    case WM_SIZE: {
        RECT r; GetClientRect(hwnd, &r);
        int w = r.right - r.left;
        int h = r.bottom - r.top;
        MoveWindow(hResults, 10, 188, w - 20, h - 198, TRUE);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);

        if (id == ID_BTN_BROWSE) {
            BROWSEINFOW bi = {};
            bi.hwndOwner = hwnd;
            bi.lpszTitle = L"Выберите папку";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                wchar_t buf[MAX_PATH];
                if (SHGetPathFromIDListW(pidl, buf))
                    SetWindowTextW(hPath, buf);
                CoTaskMemFree(pidl);
            }
        }
        else if (id == ID_BTN_CLEAR) {
            SetWindowTextW(hResults, L"");
        }
        else if (id == ID_BTN_SEARCH) {
            if (g_running) return 0;

            std::wstring path = GetText(hPath);
            std::wstring pat = GetText(hPattern);
            std::wstring extS = GetText(hExt);

            if (path.empty() || pat.empty()) {
                MessageBoxW(hwnd, L"Заполните путь и что искать.",
                    L"Ошибка", MB_ICONWARNING);
                return 0;
            }

            bool useMask = SendMessageW(hMask, BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool icase = SendMessageW(hIcase, BM_GETCHECK, 0, 0) == BST_CHECKED;

            std::vector<std::string> exts;
            std::string extUtf8 = WideToUtf8(extS);
            size_t start = 0;
            while (start <= extUtf8.size()) {
                size_t comma = extUtf8.find(',', start);
                std::string e = extUtf8.substr(start, comma - start);
                while (!e.empty() && (e.front() == ' ' || e.front() == '\t')) e.erase(e.begin());
                while (!e.empty() && (e.back() == ' ' || e.back() == '\t')) e.pop_back();
                if (!e.empty()) {
                    if (e[0] != '.') e = "." + e;
                    exts.push_back(e);
                }
                if (comma == std::string::npos) break;
                start = comma + 1;
            }

            SetWindowTextW(hResults, L"");
            AppendResults(L"Поиск...\r\n");
            g_running = true;
            EnableWindow(hSearch, FALSE);

            std::thread(RunSearch, path, pat, useMask, icase, exts).detach();
        }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    const wchar_t* cls = L"FinderGuiClass";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = cls;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowW(cls, L"Finder — поиск по файлам",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 730, 590,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}