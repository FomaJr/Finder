// Finder.cpp
// Сборка (MinGW):
//   g++ -std=c++17 -O2 -static -s -mwindows -municode Finder.cpp -o Finder.exe -lcomctl32 -lole32 -lshell32
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
#include <mutex>

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
#define ID_BTN_SAVE      110
#define ID_PROGRESS      111
#define ID_STATUS        112

static HWND hPath, hBrowse, hPattern, hMask, hIcase, hExt,
            hSearch, hResults, hClear, hSave, hProgress, hStatus;
static HFONT g_font;
static std::atomic<bool> g_running{false};
static std::atomic<bool> g_stop{false};
static std::mutex g_appendMutex;

// ---------- Структура результатов (для двойного клика) ----------
struct ResultLine {
    int lineIndex;        // индекс строки в EDIT (0-based)
    std::wstring filePath;
    int fileLine;         // номер строки в файле
};
static std::vector<ResultLine> g_resultLines;
static std::mutex g_resultMutex;

// ---------- Путь к .ini с настройками ----------
static std::wstring GetIniPath() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    fs::path p(buf);
    p.replace_filename(L"settings.ini");
    return p.wstring();
}

static void SaveSettings() {
    std::wstring ini = GetIniPath();
    wchar_t path[MAX_PATH] = {}, pat[512] = {}, ext[512] = {};
    GetWindowTextW(hPath, path, MAX_PATH);
    GetWindowTextW(hPattern, pat, 512);
    GetWindowTextW(hExt, ext, 512);
    WritePrivateProfileStringW(L"general", L"path", path, ini.c_str());
    WritePrivateProfileStringW(L"general", L"pattern", pat, ini.c_str());
    WritePrivateProfileStringW(L"general", L"ext", ext, ini.c_str());
    WritePrivateProfileStringW(L"general", L"mask",
        SendMessageW(hMask, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"general", L"icase",
        SendMessageW(hIcase, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0", ini.c_str());
}

static void LoadSettings() {
    std::wstring ini = GetIniPath();
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    wchar_t buf[1024];
    GetPrivateProfileStringW(L"general", L"path", L"G:\\PAN\\Finder\\Finder", buf, 1024, ini.c_str());
    SetWindowTextW(hPath, buf);
    GetPrivateProfileStringW(L"general", L"pattern", L"", buf, 1024, ini.c_str());
    SetWindowTextW(hPattern, buf);
    GetPrivateProfileStringW(L"general", L"ext", L"", buf, 1024, ini.c_str());
    SetWindowTextW(hExt, buf);
    int m = GetPrivateProfileIntW(L"general", L"mask", 0, ini.c_str());
    SendMessageW(hMask, BM_SETCHECK, m ? BST_CHECKED : BST_UNCHECKED, 0);
    int ic = GetPrivateProfileIntW(L"general", L"icase", 0, ini.c_str());
    SendMessageW(hIcase, BM_SETCHECK, ic ? BST_CHECKED : BST_UNCHECKED, 0);
}

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
    } catch (...) { return false; }
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

// ---------- Вывод результатов (с сохранением карты строк) ----------
static void AppendResults(const std::wstring& text) {
    std::lock_guard<std::mutex> lk(g_appendMutex);
    int len = GetWindowTextLengthW(hResults);
    SendMessageW(hResults, EM_SETSEL, len, len);
    SendMessageW(hResults, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
}

static void AppendResultFileLine(const std::wstring& filePath, int fileLine,
                                 const std::wstring& lineText) {
    std::lock_guard<std::mutex> lk(g_appendMutex);
    int len = GetWindowTextLengthW(hResults);
    // Запоминаем, на какой визуальной строке начнётся эта запись
    int lineIndex = (int)SendMessageW(hResults, EM_LINEFROMCHAR, len, 0);
    // Добавляем перевод строки, если в конце текста не было \r\n
    std::wstring toAdd;
    if (len > 0) toAdd += L"\r\n";
    toAdd += std::to_wstring(fileLine) + L": " + lineText;
    SendMessageW(hResults, EM_SETSEL, len, len);
    SendMessageW(hResults, EM_REPLACESEL, FALSE, (LPARAM)toAdd.c_str());

    ResultLine rl;
    rl.lineIndex = lineIndex;
    rl.filePath  = filePath;
    rl.fileLine  = fileLine;
    std::lock_guard<std::mutex> lk2(g_resultMutex);
    g_resultLines.push_back(rl);
}

static void UpdateStatus(const std::wstring& s) {
    SetWindowTextW(hStatus, s.c_str());
}

// ---------- Фоновый поиск ----------
static void RunSearch(std::wstring wpath, std::wstring wpattern,
                      bool useMask, bool icase, std::vector<std::string> exts)
{
    std::string path    = WideToUtf8(wpath);
    std::string pattern = WideToUtf8(wpattern);

    std::error_code ec;
    if (!fs::exists(path, ec)) {
        UpdateStatus(L"Путь не найден.");
        g_running = false;
        EnableWindow(hSearch, TRUE);
        return;
    }

    std::string reStr = useMask ? globToRegex(pattern) : "";
    auto files = collectFiles(path, exts);

    // Прогресс-бар
    SendMessageW(hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, (int)files.size()));
    SendMessageW(hProgress, PBM_SETPOS, 0, 0);

    int matchedFiles = 0;
    int current = 0;

    // Очищаем карту строк результатов
    {
        std::lock_guard<std::mutex> lk(g_resultMutex);
        g_resultLines.clear();
    }

    for (const auto& f : files) {
        if (g_stop) break;
        ++current;

        // обновляем прогресс каждые 50 файлов или на последнем
        if (current % 50 == 0 || current == (int)files.size()) {
            SendMessageW(hProgress, PBM_SETPOS, current, 0);
            UpdateStatus(L"Поиск: " + std::to_wstring(current) +
                         L" из " + std::to_wstring(files.size()) + L" файлов");
        }

        std::ifstream in(f, std::ios::binary);
        if (!in) continue;

        std::string line;
        int lineno = 0;
        bool headerPrinted = false;
        std::wstring fname = Utf8ToWide(f.string());

        while (std::getline(in, line)) {
            if (g_stop) break;
            ++lineno;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            bool hit = useMask ? matchesMask(line, reStr, icase)
                               : contains(line, pattern, icase);
            if (hit) {
                if (!headerPrinted) {
                    AppendResults(L"\r\n=== " + fname + L" ===\r\n");
                    headerPrinted = true;
                }
                AppendResultFileLine(fname, lineno, Utf8ToWide(line));
            }
        }
        if (headerPrinted) ++matchedFiles;
    }

    SendMessageW(hProgress, PBM_SETPOS, (int)files.size(), 0);
    AppendResults(L"\r\nФайлов с совпадениями: " +
                  std::to_wstring(matchedFiles) + L"\r\n");
    UpdateStatus(L"Готово. Найдено файлов: " + std::to_wstring(matchedFiles));

    g_running = false;
    EnableWindow(hSearch, TRUE);
}

// ---------- Сохранить результаты в .txt ----------
static void SaveResultsToFile(HWND hwnd) {
    int len = GetWindowTextLengthW(hResults);
    if (len == 0) {
        MessageBoxW(hwnd, L"Результатов нет.", L"Сохранить", MB_ICONINFORMATION);
        return;
    }

    wchar_t file[MAX_PATH] = L"results.txt";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd;
    ofn.lpstrFilter = L"Текстовые файлы (*.txt)\0*.txt\0Все файлы (*.*)\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (!GetSaveFileNameW(&ofn)) return;

    std::wstring content(len + 1, L'\0');
    GetWindowTextW(hResults, content.data(), len + 1);
    content.resize(len);

    std::ofstream out(file, std::ios::binary);
    if (!out) {
        MessageBoxW(hwnd, L"Не удалось сохранить файл.", L"Ошибка", MB_ICONERROR);
        return;
    }
    std::string utf8 = WideToUtf8(content);
    // BOM UTF-8, чтобы Блокнот понял кодировку
    out.write("\xEF\xBB\xBF", 3);
    out.write(utf8.data(), (std::streamsize)utf8.size());
    MessageBoxW(hwnd, L"Сохранено.", L"OK", MB_ICONINFORMATION);
}

// ---------- Двойной клик: открыть файл ----------
static void OpenResultLine(HWND hwnd) {
    DWORD sel = (DWORD)SendMessageW(hResults, EM_GETSEL, 0, 0);
    int clickedLine = (int)SendMessageW(hResults, EM_LINEFROMCHAR, (WPARAM)LOWORD(sel), 0);

    std::wstring filePath;
    int fileLine = 0;
    {
        std::lock_guard<std::mutex> lk(g_resultMutex);
        for (const auto& rl : g_resultLines) {
            if (rl.lineIndex <= clickedLine) {
                filePath = rl.filePath;
                fileLine = rl.fileLine;
            } else break;
        }
    }
    if (filePath.empty()) return;

    // Пробуем VS Code
    std::wstring cmd = L"code -g \"" + filePath + L":" + std::to_wstring(fileLine) + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::wstring cmdMutable = cmd;
    if (CreateProcessW(nullptr, cmdMutable.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return;
    }

    // Если VS Code нет — пробуем Notepad
    std::wstring cmd2 = L"notepad \"" + filePath + L"\"";
    std::wstring cmd2Mutable = cmd2;
    STARTUPINFOW si2 = { sizeof(si2) };
    PROCESS_INFORMATION pi2 = {};
    CreateProcessW(nullptr, cmd2Mutable.data(), nullptr, nullptr, FALSE,
                   CREATE_NO_WINDOW, nullptr, nullptr, &si2, &pi2);
    if (pi2.hProcess) CloseHandle(pi2.hProcess);
    if (pi2.hThread)  CloseHandle(pi2.hThread);
}

// ---------- Правый клик: контекстное меню ----------
static void ShowResultsContextMenu(HWND hwnd, int x, int y) {
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, 1, L"Копировать");
    AppendMenuW(hMenu, MF_STRING, 2, L"Открыть файл (двойной клик)");
    int cmd = (int)TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                  x, y, 0, hwnd, nullptr);
    DestroyMenu(hMenu);

    if (cmd == 1) {
        SendMessageW(hResults, WM_COPY, 0, 0);
    } else if (cmd == 2) {
        OpenResultLine(hwnd);
    }
}

// ---------- Окно ----------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        CreateWindowW(L"STATIC", L"Папка или файл:",
                      WS_CHILD | WS_VISIBLE, 10, 10, 120, 20,
                      hwnd, nullptr, nullptr, nullptr);
        hPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
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
                      WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 130, 132, 100, 28,
                      hwnd, (HMENU)ID_BTN_SEARCH, nullptr, nullptr);
        hClear  = CreateWindowW(L"BUTTON", L"Очистить",
                      WS_CHILD | WS_VISIBLE, 240, 132, 100, 28,
                      hwnd, (HMENU)ID_BTN_CLEAR, nullptr, nullptr);
        hSave   = CreateWindowW(L"BUTTON", L"Сохранить в .txt",
                      WS_CHILD | WS_VISIBLE, 350, 132, 140, 28,
                      hwnd, (HMENU)ID_BTN_SAVE, nullptr, nullptr);

        hProgress = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
                      WS_CHILD | WS_VISIBLE, 10, 168, 670, 18,
                      hwnd, (HMENU)ID_PROGRESS, nullptr, nullptr);

        hStatus = CreateWindowW(L"STATIC", L"Готов.",
                      WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 190, 670, 20,
                      hwnd, (HMENU)ID_STATUS, nullptr, nullptr);

        hResults = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                      WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                      ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                      10, 214, 670, 300,
                      hwnd, (HMENU)ID_EDIT_RESULTS, nullptr, nullptr);

        EnumChildWindows(hwnd, [](HWND h, LPARAM p) -> BOOL {
            SendMessageW(h, WM_SETFONT, (WPARAM)p, TRUE);
            return TRUE;
        }, (LPARAM)g_font);

        LoadSettings();
        return 0;
    }

    case WM_SIZE: {
        RECT r; GetClientRect(hwnd, &r);
        int w = r.right - r.left;
        int h = r.bottom - r.top;
        MoveWindow(hProgress, 10, 168, w - 20, 18, TRUE);
        MoveWindow(hStatus,   10, 190, w - 20, 20, TRUE);
        MoveWindow(hResults,  10, 214, w - 20, h - 224, TRUE);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        // фон статиков как у окна
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);

        if (id == ID_EDIT_RESULTS && code == EN_ERRSPACE) return 0;

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
            {
                std::lock_guard<std::mutex> lk(g_resultMutex);
                g_resultLines.clear();
            }
            UpdateStatus(L"Готов.");
        }
        else if (id == ID_BTN_SAVE) {
            SaveResultsToFile(hwnd);
        }
        else if (id == ID_BTN_SEARCH) {
            if (g_running) return 0;

            std::wstring path = GetText(hPath);
            std::wstring pat  = GetText(hPattern);
            std::wstring extS = GetText(hExt);

            if (path.empty() || pat.empty()) {
                MessageBoxW(hwnd, L"Заполните путь и что искать.",
                            L"Ошибка", MB_ICONWARNING);
                return 0;
            }

            bool useMask = SendMessageW(hMask, BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool icase   = SendMessageW(hIcase, BM_GETCHECK, 0, 0) == BST_CHECKED;

            std::vector<std::string> exts;
            std::string extUtf8 = WideToUtf8(extS);
            size_t start = 0;
            while (start <= extUtf8.size()) {
                size_t comma = extUtf8.find(',', start);
                std::string e = extUtf8.substr(start, comma - start);
                while (!e.empty() && (e.front()==' '||e.front()=='\t')) e.erase(e.begin());
                while (!e.empty() && (e.back() ==' '||e.back() =='\t')) e.pop_back();
                if (!e.empty()) {
                    if (e[0] != '.') e = "." + e;
                    exts.push_back(e);
                }
                if (comma == std::string::npos) break;
                start = comma + 1;
            }

            SetWindowTextW(hResults, L"");
            {
                std::lock_guard<std::mutex> lk(g_resultMutex);
                g_resultLines.clear();
            }
            AppendResults(L"Поиск...\r\n");
            UpdateStatus(L"Поиск...");

            g_running = true;
            g_stop = false;
            EnableWindow(hSearch, FALSE);
            SaveSettings();

            std::thread(RunSearch, path, pat, useMask, icase, exts).detach();
        }
        return 0;
    }

    case WM_CLOSE:
        // Если поиск идёт — просим остановиться и ждём
        if (g_running) {
            int ans = MessageBoxW(hwnd,
                L"Поиск ещё идёт. Прервать и закрыть?",
                L"Подтверждение", MB_ICONQUESTION | MB_YESNO);
            if (ans != IDYES) return 0;
            g_stop = true;
            // дать потоку время аккуратно выйти
            for (int i = 0; i < 50 && g_running; ++i) Sleep(100);
        }
        SaveSettings();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    const wchar_t* cls = L"FinderGuiClass";
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = cls;
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowW(cls, L"Finder — поиск по файлам",
                              WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 730, 620,
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