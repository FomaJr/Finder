// Finder.cpp

//   g++ -std=c++17 -O2 -static -s -mwindows -municode Finder.cpp -o Finder.exe -lcomctl32 -lole32 -lshell32 -lz


#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <zlib.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>

namespace fs = std::filesystem;

#define ID_EDIT_PATH     101
#define ID_BTN_BROWSE    102
#define ID_EDIT_PATTERN  103
#define ID_CHK_MASK      104
#define ID_CHK_ICASE     105
#define ID_BTN_SEARCH    106
#define ID_LIST_RESULTS  107
#define ID_BTN_CLEAR     108
#define ID_BTN_SAVE      109
#define ID_PROGRESS      110
#define ID_STATUS        111

static HWND hPath, hBrowse, hPattern, hMask, hIcase,
            hSearch, hResults, hClear, hSave, hProgress, hStatus;
static HFONT g_font;
static std::atomic<bool> g_running{false};
static std::atomic<bool> g_stop{false};

static const uint64_t kMaxFileSize = 200ull * 1024 * 1024;

static const std::vector<std::string> kExtensions = {
    ".txt", ".log", ".md", ".ini", ".cfg", ".conf", ".json", ".xml",
    ".yml", ".yaml", ".csv", ".tsv", ".tex", ".rtf",
    ".cpp", ".c", ".h", ".hpp", ".cc", ".cxx", ".cs", ".java",
    ".py", ".js", ".ts", ".jsx", ".tsx", ".php", ".rb", ".go",
    ".rs", ".lua", ".pl", ".sh", ".bat", ".cmd", ".ps1",
    ".html", ".htm", ".css", ".scss", ".less", ".sql", ".vue",
    ".kt", ".swift", ".m", ".mm", ".asm", ".s",
    ".gradle", ".properties",
    ".doc", ".xls", ".ppt",
    ".docx", ".xlsx", ".pptx", ".docm", ".xlsm", ".pptm",
    ".odt", ".ods", ".odp",
    ".pdf"
};

static bool isZipOffice(const std::string& ext) {
    return ext == ".docx" || ext == ".xlsx" || ext == ".pptx"
        || ext == ".docm" || ext == ".xlsm" || ext == ".pptm"
        || ext == ".odt"  || ext == ".ods"  || ext == ".odp";
}

struct ResultRow {
    std::wstring filePath;
    int fileLine;
    std::wstring lineText;
};
static std::vector<ResultRow> g_rows;
static std::mutex g_rowsMutex;
static std::mutex g_listMutex;

static std::string PathToUtf8(const fs::path& p) {
    auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

static fs::path ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

static uint32_t ReadU32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t ReadU16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static bool InflateRaw(const uint8_t* in, size_t inLen, std::string& out) {
    if (inLen == 0) return true;
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
    zs.next_in = const_cast<Bytef*>(in);
    zs.avail_in = (uInt)inLen;
    const size_t chunk = 65536;
    std::vector<char> buf(chunk);
    int ret;
    do {
        zs.next_out = (Bytef*)buf.data();
        zs.avail_out = (uInt)chunk;
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
            inflateEnd(&zs); return false;
        }
        size_t have = chunk - zs.avail_out;
        out.append(buf.data(), have);
        if (out.size() > 500ull * 1024 * 1024) { inflateEnd(&zs); return false; }
    } while (ret != Z_STREAM_END && zs.avail_in > 0);
    inflateEnd(&zs);
    return true;
}

static bool ReadFileAll(const fs::path& path, std::string& data) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    std::streamoff sz = in.tellg();
    if (sz <= 0) return false;
    in.seekg(0, std::ios::beg);
    data.resize((size_t)sz);
    in.read(data.data(), sz);
    return in.good() || in.eof();
}

static bool ZipExtract(const fs::path& zipPath, const std::string& wantName,
                       std::string& out)
{
    std::string data;
    if (!ReadFileAll(zipPath, data)) return false;
    const uint8_t* dp = (const uint8_t*)data.data();
    if (data.size() < 22) return false;

    size_t eocdPos = std::string::npos;
    for (size_t i = data.size() - 22; ; --i) {
        if (data[i] == 0x50 && data[i+1] == 0x4b &&
            data[i+2] == 0x05 && data[i+3] == 0x06) { eocdPos = i; break; }
        if (i == 0) break;
    }
    if (eocdPos == std::string::npos) return false;

    uint32_t cdOffset = ReadU32(&dp[eocdPos + 16]);
    uint16_t cdCount  = ReadU16(&dp[eocdPos + 10]);

    size_t p = cdOffset;
    for (uint16_t n = 0; n < cdCount && p + 46 <= data.size(); ++n) {
        if (!(data[p] == 0x50 && data[p+1] == 0x4b &&
              data[p+2] == 0x01 && data[p+3] == 0x02)) break;

        uint16_t method   = ReadU16(&dp[p + 10]);
        uint32_t compSize = ReadU32(&dp[p + 20]);
        uint16_t nameLen  = ReadU16(&dp[p + 28]);
        uint16_t extraLen = ReadU16(&dp[p + 30]);
        uint16_t commLen  = ReadU16(&dp[p + 32]);
        uint32_t localOff = ReadU32(&dp[p + 42]);

        std::string name((char*)&dp[p + 46], nameLen);

        if (name == wantName) {
            size_t lp = localOff;
            if (lp + 30 > data.size()) return false;
            uint16_t lNameLen  = ReadU16(&dp[lp + 26]);
            uint16_t lExtraLen = ReadU16(&dp[lp + 28]);
            size_t dataStart = lp + 30 + lNameLen + lExtraLen;
            if (dataStart + compSize > data.size()) return false;
            if (method == 0) {
                out.assign((char*)&dp[dataStart], compSize);
                return true;
            } else if (method == 8) {
                return InflateRaw(&dp[dataStart], compSize, out);
            } else {
                return false;
            }
        }
        p += 46 + nameLen + extraLen + commLen;
    }
    return false;
}

static void DecodeXmlEntities(std::string& s) {
    struct { const char* from; const char* to; } map[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
        {"&quot;", "\""}, {"&apos;", "'"}, {"&nbsp;", " "},
    };
    for (auto& m : map) {
        size_t pos = 0;
        while ((pos = s.find(m.from, pos)) != std::string::npos) {
            s.replace(pos, std::strlen(m.from), m.to);
            pos += std::strlen(m.to);
        }
    }
    static const std::regex numEnt(R"(&#(x?[0-9A-Fa-f]+);)");
    std::string result;
    result.reserve(s.size());
    auto begin = std::sregex_iterator(s.begin(), s.end(), numEnt);
    auto end   = std::sregex_iterator();
    size_t last = 0;
    for (auto it = begin; it != end; ++it) {
        result.append(s, last, it->position() - last);
        std::string num = (*it)[1].str();
        long code = 0;
        try {
            if (!num.empty() && (num[0]=='x' || num[0]=='X'))
                code = std::stol(num.substr(1), nullptr, 16);
            else
                code = std::stol(num, nullptr, 10);
        } catch (...) { code = '?'; }
        if (code < 0x80) {
            result += (char)code;
        } else if (code < 0x800) {
            result += (char)(0xC0 | (code >> 6));
            result += (char)(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            result += (char)(0xE0 | (code >> 12));
            result += (char)(0x80 | ((code >> 6) & 0x3F));
            result += (char)(0x80 | (code & 0x3F));
        } else {
            result += (char)(0xF0 | (code >> 18));
            result += (char)(0x80 | ((code >> 12) & 0x3F));
            result += (char)(0x80 | ((code >> 6) & 0x3F));
            result += (char)(0x80 | (code & 0x3F));
        }
        last = it->position() + it->length();
    }
    result.append(s, last, std::string::npos);
    s = std::move(result);
}

static std::string XmlToText(const std::string& xml) {
    std::string s;
    s.reserve(xml.size());
    bool inTag = false;
    for (size_t i = 0; i < xml.size(); ++i) {
        char c = xml[i];
        if (c == '<') { inTag = true; s += ' '; continue; }
        if (c == '>') { inTag = false; s += ' '; continue; }
        if (!inTag) s += c;
    }
    DecodeXmlEntities(s);
    return s;
}

static bool ExtractOfficeText(const fs::path& path, const std::string& ext,
                              std::string& out)
{
    out.clear();
    if (ext == ".docx" || ext == ".docm") {
        std::string xml;
        if (ZipExtract(path, "word/document.xml", xml))
            out = XmlToText(xml);
        return !out.empty();
    }
    if (ext == ".xlsx" || ext == ".xlsm") {
        std::string a;
        std::string shared;
        if (ZipExtract(path, "xl/sharedStrings.xml", a))
            shared = XmlToText(a);
        std::string allSheets;
        for (int i = 1; i <= 20; ++i) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "xl/worksheets/sheet%d.xml", i);
            std::string s;
            if (!ZipExtract(path, buf, s)) break;
            allSheets += XmlToText(s);
            allSheets += "\n";
        }
        out = shared + "\n" + allSheets;
        return !out.empty();
    }
    if (ext == ".pptx" || ext == ".pptm") {
        std::string all;
        for (int i = 1; i <= 100; ++i) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "ppt/slides/slide%d.xml", i);
            std::string s;
            if (!ZipExtract(path, buf, s)) break;
            all += XmlToText(s);
            all += "\n";
        }
        out = all;
        return !out.empty();
    }
    if (ext == ".odt" || ext == ".ods" || ext == ".odp") {
        std::string xml;
        if (ZipExtract(path, "content.xml", xml))
            out = XmlToText(xml);
        return !out.empty();
    }
    return false;
}

static std::string ExtractTextFromContentStream(const std::string& content) {
    std::string out;
    out.reserve(content.size() / 2);
    bool inText = false;
    size_t i = 0;
    const size_t n = content.size();

    while (i < n) {
        if (i + 1 < n && content[i] == 'B' && content[i+1] == 'T') {
            inText = true; i += 2; continue;
        }
        if (i + 1 < n && content[i] == 'E' && content[i+1] == 'T') {
            inText = false; out += ' '; i += 2; continue;
        }
        if (!inText) { ++i; continue; }
        char c = content[i];
        if (c == '(') {
            ++i;
            int depth = 1;
            std::string s;
            while (i < n && depth > 0) {
                char ch = content[i++];
                if (ch == '\\' && i < n) {
                    char esc = content[i++];
                    switch (esc) {
                        case 'n': s += '\n'; break;
                        case 'r': s += '\r'; break;
                        case 't': s += '\t'; break;
                        case '(': s += '('; break;
                        case ')': s += ')'; break;
                        case '\\': s += '\\'; break;
                        default:
                            if (esc >= '0' && esc <= '7') {
                                int val = esc - '0';
                                for (int k = 0; k < 2 && i < n &&
                                                content[i] >= '0' && content[i] <= '7'; ++k)
                                    val = val * 8 + (content[i++] - '0');
                                s += (char)val;
                            } else s += esc;
                    }
                } else if (ch == '(') { ++depth; s += ch; }
                else if (ch == ')') { --depth; if (depth > 0) s += ch; }
                else s += ch;
            }
            out += s;
            out += ' ';
            continue;
        }
        if (c == '<' && i + 1 < n && content[i+1] != '<') {
            ++i;
            while (i < n && content[i] != '>') ++i;
            if (i < n) ++i;
            out += ' ';
            continue;
        }
        ++i;
    }
    return out;
}

static bool ExtractPdfTextFallback(const fs::path& path, std::string& out) {
    std::string data;
    if (!ReadFileAll(path, data)) return false;
    if (data.size() < 8 || data.compare(0, 5, "%PDF-") != 0) return false;
    out.clear();
    size_t pos = 0;
    const size_t n = data.size();
    while (pos < n) {
        size_t sPos = data.find("stream", pos);
        if (sPos == std::string::npos) break;
        size_t dataStart = sPos + 6;
        if (dataStart < n && data[dataStart] == '\r') ++dataStart;
        if (dataStart < n && data[dataStart] == '\n') ++dataStart;
        size_t ePos = data.find("endstream", dataStart);
        if (ePos == std::string::npos) break;
        size_t dataEnd = ePos;
        while (dataEnd > dataStart &&
               (data[dataEnd-1]=='\r' || data[dataEnd-1]=='\n')) --dataEnd;
        size_t lookBack = (sPos > 200) ? sPos - 200 : 0;
        std::string dict(data, lookBack, sPos - lookBack);
        bool isFlate = dict.find("FlateDecode") != std::string::npos;
        std::string decoded;
        if (isFlate) {
            if (!InflateRaw((const uint8_t*)data.data() + dataStart,
                            dataEnd - dataStart, decoded)) {
                pos = ePos + 9; continue;
            }
        } else {
            decoded.assign(data, dataStart, dataEnd - dataStart);
        }
        if (decoded.find("BT") != std::string::npos &&
            decoded.find("ET") != std::string::npos) {
            out += ExtractTextFromContentStream(decoded);
            out += '\n';
        }
        pos = ePos + 9;
    }
    return !out.empty();
}

static bool RunPdftotext(const fs::path& pdftotextExe,
                         const fs::path& pdfPath,
                         std::string& out)
{
    out.clear();
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return false;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = L"\"" + pdftotextExe.wstring() + L"\" -enc UTF-8 -layout \""
                     + pdfPath.wstring() + L"\" -";

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWrite);
    if (!ok) { CloseHandle(hRead); return false; }

    char buf[8192];
    DWORD read = 0;
    while (ReadFile(hRead, buf, sizeof(buf), &read, nullptr) && read > 0) {
        out.append(buf, read);
    }

    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hRead);
    return !out.empty();
}

static bool ExtractPdfText(const fs::path& path, std::string& out) {
    fs::path exeDir = ExeDir();
    fs::path candidates[] = {
        exeDir / L"pdftotext.exe",
        exeDir / L"bin" / L"pdftotext.exe",
    };
    for (auto& cand : candidates) {
        std::error_code ec;
        if (fs::exists(cand, ec)) {
            if (RunPdftotext(cand, path, out) && !out.empty()) return true;
        }
    }
    return ExtractPdfTextFallback(path, out);
}

static std::string ExtractFileText(const fs::path& path,
                                   const std::string& extLower)
{
    if (isZipOffice(extLower)) {
        std::string txt;
        ExtractOfficeText(path, extLower, txt);
        return txt;
    }
    if (extLower == ".pdf") {
        std::string txt;
        ExtractPdfText(path, txt);
        return txt;
    }
    std::string data;
    if (!ReadFileAll(path, data)) return "";
    return data;
}

static std::wstring GetIniPath() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    fs::path p(buf);
    p.replace_filename(L"settings.ini");
    return p.wstring();
}

static void SaveSettings() {
    std::wstring ini = GetIniPath();
    wchar_t path[MAX_PATH] = {}, pat[512] = {};
    GetWindowTextW(hPath, path, MAX_PATH);
    GetWindowTextW(hPattern, pat, 512);
    WritePrivateProfileStringW(L"general", L"path", path, ini.c_str());
    WritePrivateProfileStringW(L"general", L"pattern", pat, ini.c_str());
    WritePrivateProfileStringW(L"general", L"mask",
        SendMessageW(hMask, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"general", L"icase",
        SendMessageW(hIcase, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0", ini.c_str());
}

static std::wstring GetHomeDir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return buf;
    return L"C:\\";
}

static void LoadSettings() {
    std::wstring ini = GetIniPath();
    std::wstring defaultPath = GetHomeDir();

    auto pathExists = [](const std::wstring& p) -> bool {
        if (p.empty()) return false;
        DWORD attr = GetFileAttributesW(p.c_str());
        return attr != INVALID_FILE_ATTRIBUTES;
    };

    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        SetWindowTextW(hPath, defaultPath.c_str());
        return;
    }
    wchar_t buf[1024];
    GetPrivateProfileStringW(L"general", L"path", defaultPath.c_str(),
                             buf, 1024, ini.c_str());
    if (!pathExists(buf)) {
        SetWindowTextW(hPath, defaultPath.c_str());
    } else {
        SetWindowTextW(hPath, buf);
    }

    GetPrivateProfileStringW(L"general", L"pattern", L"", buf, 1024, ini.c_str());
    SetWindowTextW(hPattern, buf);
    int m = GetPrivateProfileIntW(L"general", L"mask", 0, ini.c_str());
    SendMessageW(hMask, BM_SETCHECK, m ? BST_CHECKED : BST_UNCHECKED, 0);
    int ic = GetPrivateProfileIntW(L"general", L"icase", 0, ini.c_str());
    SendMessageW(hIcase, BM_SETCHECK, ic ? BST_CHECKED : BST_UNCHECKED, 0);
}

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

static bool hasAllowedExt(const fs::path& p) {
    std::string e = toLower(PathToUtf8(p.extension()));
    for (auto& x : kExtensions) if (e == x) return true;
    return false;
}

static std::vector<fs::path> collectFiles(const fs::path& root) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (fs::is_regular_file(root, ec)) {
        if (hasAllowedExt(root)) out.push_back(root);
        return out;
    }
    if (!fs::is_directory(root, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (it->is_regular_file(ec) && hasAllowedExt(it->path()))
            out.push_back(it->path());
    }
    return out;
}

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

static void AppendResultRow(const std::wstring& filePath, int fileLine,
                            const std::wstring& lineText)
{
    std::lock_guard<std::mutex> lk(g_listMutex);

    int idx = ListView_GetItemCount(hResults);
    LVITEMW item = {};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = idx;
    item.iSubItem = 0;
    item.pszText = (LPWSTR)filePath.c_str();
    item.lParam = (LPARAM)idx;

    int actual = (int)SendMessageW(hResults, LVM_INSERTITEMW, 0, (LPARAM)&item);
    if (actual == -1) return;

    ListView_SetItemText(hResults, actual, 1, (LPWSTR)std::to_wstring(fileLine).c_str());
    ListView_SetItemText(hResults, actual, 2, (LPWSTR)lineText.c_str());

    SendMessageW(hResults, LVM_ENSUREVISIBLE, actual, FALSE);

    {
        std::lock_guard<std::mutex> lk2(g_rowsMutex);
        ResultRow r;
        r.filePath = filePath;
        r.fileLine = fileLine;
        r.lineText = lineText;
        g_rows.push_back(r);
    }
}

static void ClearResults() {
    {
        std::lock_guard<std::mutex> lk(g_listMutex);
        ListView_DeleteAllItems(hResults);
    }
    {
        std::lock_guard<std::mutex> lk(g_rowsMutex);
        g_rows.clear();
    }
}

static void UpdateStatus(const std::wstring& s) {
    SetWindowTextW(hStatus, s.c_str());
}

static void SearchInText(const std::string& content,
                         const std::wstring& fname,
                         const std::string& pattern,
                         const std::string& reStr,
                         bool useMask, bool icase,
                         int& matchedThisFile)
{
    std::istringstream iss(content);
    std::string line;
    int lineno = 0;
    while (std::getline(iss, line)) {
        if (g_stop) break;
        ++lineno;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        bool hit = useMask ? matchesMask(line, reStr, icase)
                           : contains(line, pattern, icase);
        if (hit) {
            AppendResultRow(fname, lineno, Utf8ToWide(line));
            matchedThisFile = 1;
        }
    }
}

static void RunSearch(std::wstring wpath, std::wstring wpattern,
                      bool useMask, bool icase)
{
    fs::path root = wpath;
    std::string pattern = WideToUtf8(wpattern);

    std::error_code ec;
    if (!fs::exists(root, ec)) {
        UpdateStatus(L"Путь не найден.");
        g_running = false;
        EnableWindow(hSearch, TRUE);
        return;
    }

    std::string reStr = useMask ? globToRegex(pattern) : "";
    auto files = collectFiles(root);

    PostMessageW(hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, (int)files.size()));

    int matchedFiles = 0;
    int current = 0;

    for (const auto& f : files) {
        if (g_stop) break;
        ++current;

        if (current % 25 == 0 || current == (int)files.size()) {
            PostMessageW(hProgress, PBM_SETPOS, current, 0);
            std::wstring s = L"Поиск: " + std::to_wstring(current) +
                             L" из " + std::to_wstring(files.size()) + L" файлов";
            UpdateStatus(s);
        }

        std::error_code ec2;
        auto sz = fs::file_size(f, ec2);
        if (!ec2 && sz > kMaxFileSize) continue;

        std::string extLower = toLower(PathToUtf8(f.extension()));
        std::string content = ExtractFileText(f, extLower);
        if (content.empty()) continue;

        std::wstring fname = Utf8ToWide(PathToUtf8(f));
        int matchedThisFile = 0;

        SearchInText(content, fname, pattern, reStr,
                     useMask, icase, matchedThisFile);

        if (matchedThisFile) ++matchedFiles;
    }

    PostMessageW(hProgress, PBM_SETPOS, (int)files.size(), 0);
    UpdateStatus(L"Готово. Найдено файлов: " + std::to_wstring(matchedFiles));

    g_running = false;
    EnableWindow(hSearch, TRUE);
}

static void SaveResultsToFile(HWND hwnd) {
    int count = ListView_GetItemCount(hResults);
    if (count == 0) {
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

    std::ofstream out(fs::path(file), std::ios::binary);
    if (!out) {
        MessageBoxW(hwnd, L"Не удалось сохранить файл.", L"Ошибка", MB_ICONERROR);
        return;
    }
    out.write("\xEF\xBB\xBF", 3);

    std::lock_guard<std::mutex> lk(g_rowsMutex);
    for (const auto& r : g_rows) {
        std::string utf8 = WideToUtf8(r.filePath + L":" +
                                      std::to_wstring(r.fileLine) + L": " +
                                      r.lineText) + "\r\n";
        out.write(utf8.data(), (std::streamsize)utf8.size());
    }
    out.close();
    MessageBoxW(hwnd, L"Сохранено.", L"OK", MB_ICONINFORMATION);
}

static void OpenRow(int rowIndex) {
    ResultRow r;
    {
        std::lock_guard<std::mutex> lk(g_rowsMutex);
        if (rowIndex < 0 || rowIndex >= (int)g_rows.size()) return;
        r = g_rows[rowIndex];
    }

    std::wstring ext;
    size_t dot = r.filePath.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        ext = r.filePath.substr(dot);
        for (auto& c : ext) c = (wchar_t)towlower(c);
    }

    bool isDocument =
        ext == L".docx" || ext == L".doc"  || ext == L".rtf" ||
        ext == L".odt"  || ext == L".xlsx" || ext == L".xls" ||
        ext == L".ods"  || ext == L".pptx" || ext == L".ppt" ||
        ext == L".odp"  || ext == L".pdf";

    if (isDocument) {
        ShellExecuteW(nullptr, L"open", r.filePath.c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }

    std::wstring cmd = L"notepad.exe \"" + r.filePath + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

static int GetSelectedRow() {
    return ListView_GetNextItem(hResults, -1, LVNI_SELECTED);
}

static void ShowResultsContextMenu(HWND hwnd, int x, int y) {
    int sel = GetSelectedRow();
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, 1, L"Копировать путь");
    AppendMenuW(hMenu, MF_STRING, 2, L"Открыть файл");
    if (sel < 0) {
        EnableMenuItem(hMenu, 1, MF_GRAYED);
        EnableMenuItem(hMenu, 2, MF_GRAYED);
    }
    int cmd = (int)TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                  x, y, 0, hwnd, nullptr);
    DestroyMenu(hMenu);

    if (cmd == 1 && sel >= 0) {
        std::lock_guard<std::mutex> lk(g_rowsMutex);
        if (sel < (int)g_rows.size()) {
            std::wstring text = g_rows[sel].filePath + L":" +
                                std::to_wstring(g_rows[sel].fileLine);
            if (OpenClipboard(hwnd)) {
                EmptyClipboard();
                size_t bytes = (text.size() + 1) * sizeof(wchar_t);
                HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (h) {
                    memcpy(GlobalLock(h), text.c_str(), bytes);
                    GlobalUnlock(h);
                    SetClipboardData(CF_UNICODETEXT, h);
                }
                CloseClipboard();
            }
        }
    } else if (cmd == 2 && sel >= 0) {
        OpenRow(sel);
    }
}

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

        hSearch = CreateWindowW(L"BUTTON", L"Найти",
                      WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 130, 102, 100, 28,
                      hwnd, (HMENU)ID_BTN_SEARCH, nullptr, nullptr);
        hClear  = CreateWindowW(L"BUTTON", L"Очистить",
                      WS_CHILD | WS_VISIBLE, 240, 102, 100, 28,
                      hwnd, (HMENU)ID_BTN_CLEAR, nullptr, nullptr);
        hSave   = CreateWindowW(L"BUTTON", L"Сохранить в .txt",
                      WS_CHILD | WS_VISIBLE, 350, 102, 140, 28,
                      hwnd, (HMENU)ID_BTN_SAVE, nullptr, nullptr);

        hProgress = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
                      WS_CHILD | WS_VISIBLE, 10, 138, 670, 18,
                      hwnd, (HMENU)ID_PROGRESS, nullptr, nullptr);

        hStatus = CreateWindowW(L"STATIC", L"Готов.",
                      WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 160, 670, 20,
                      hwnd, (HMENU)ID_STATUS, nullptr, nullptr);

        hResults = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                      WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS |
                      LVS_SINGLESEL,
                      10, 184, 670, 300,
                      hwnd, (HMENU)ID_LIST_RESULTS, nullptr, nullptr);

        ListView_SetExtendedListViewStyle(hResults,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER |
            LVS_EX_INFOTIP);

        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

        col.iSubItem = 0;
        col.pszText = (LPWSTR)L"Файл";
        col.cx = 300;
        ListView_InsertColumn(hResults, 0, &col);

        col.iSubItem = 1;
        col.pszText = (LPWSTR)L"Стр.";
        col.cx = 50;
        ListView_InsertColumn(hResults, 1, &col);

        col.iSubItem = 2;
        col.pszText = (LPWSTR)L"Текст";
        col.cx = 320;
        ListView_InsertColumn(hResults, 2, &col);

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
        MoveWindow(hProgress, 10, 138, w - 20, 18, TRUE);
        MoveWindow(hStatus,   10, 160, w - 20, 20, TRUE);
        MoveWindow(hResults,  10, 184, w - 20, h - 194, TRUE);

        int listW = w - 20;
        int col0 = 300;
        int col1 = 50;
        int col2 = listW - col0 - col1 - 4;
        if (col2 < 100) col2 = 100;
        ListView_SetColumnWidth(hResults, 0, col0);
        ListView_SetColumnWidth(hResults, 1, col1);
        ListView_SetColumnWidth(hResults, 2, col2);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }

    case WM_NOTIFY: {
        LPNMHDR nh = (LPNMHDR)lp;
        if (nh->idFrom == ID_LIST_RESULTS) {
            if (nh->code == NM_DBLCLK) {
                int sel = GetSelectedRow();
                if (sel >= 0) OpenRow(sel);
                return 0;
            }
            if (nh->code == NM_RCLICK) {
                POINT pt;
                GetCursorPos(&pt);
                ShowResultsContextMenu(hwnd, pt.x, pt.y);
                return 0;
            }
            if (nh->code == LVN_KEYDOWN) {
                LPNMLVKEYDOWN kd = (LPNMLVKEYDOWN)lp;
                if (kd->wVKey == VK_RETURN) {
                    int sel = GetSelectedRow();
                    if (sel >= 0) OpenRow(sel);
                    return 0;
                }
            }
        }
        break;
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
            ClearResults();
            UpdateStatus(L"Готов.");
        }
        else if (id == ID_BTN_SAVE) {
            SaveResultsToFile(hwnd);
        }
        else if (id == ID_BTN_SEARCH) {
            if (g_running) return 0;
            std::wstring path = GetText(hPath);
            std::wstring pat  = GetText(hPattern);
            if (path.empty() || pat.empty()) {
                MessageBoxW(hwnd, L"Заполните путь и что искать.",
                            L"Ошибка", MB_ICONWARNING);
                return 0;
            }
            bool useMask = SendMessageW(hMask, BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool icase   = SendMessageW(hIcase, BM_GETCHECK, 0, 0) == BST_CHECKED;

            ClearResults();
            UpdateStatus(L"Поиск...");

            g_running = true;
            g_stop = false;
            EnableWindow(hSearch, FALSE);
            SaveSettings();

            std::thread(RunSearch, path, pat, useMask, icase).detach();
        }
        return 0;
    }

    case WM_CLOSE:
        if (g_running) {
            int ans = MessageBoxW(hwnd,
                L"Поиск ещё идёт. Прервать и закрыть?",
                L"Подтверждение", MB_ICONQUESTION | MB_YESNO);
            if (ans != IDYES) return 0;
            g_stop = true;
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
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES;
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
                              CW_USEDEFAULT, CW_USEDEFAULT, 740, 600,
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