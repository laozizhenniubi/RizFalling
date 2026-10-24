#include "chart_loader.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "miniz/miniz.h"

bool read_file_w(const std::wstring& path, std::vector<unsigned char>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (256LL << 20)) {
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    BOOL ok = ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr);
    CloseHandle(h);
    if (!ok || got != out.size()) {
        out.clear();
        return false;
    }
    return true;
}

static bool ends_with_ci(const std::string& s, const char* suffix) {
    size_t n = strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)s[s.size() - n + i]) !=
            tolower((unsigned char)suffix[i]))
            return false;
    }
    return true;
}

static std::string bracket_name(const std::string& f) {
    size_t e = f.rfind(']');
    if (e == std::string::npos) return f;
    size_t b = f.rfind('[', e);
    if (b == std::string::npos || b >= e) return f;
    return f.substr(b + 1, e - b - 1);
}

static int diff_rank(const std::string& name) {
    std::string u;
    u.reserve(name.size());
    for (char c : name) u.push_back((char)toupper((unsigned char)c));
    if (u == "IN" || u == "AT") return 0;
    if (u == "HD") return 1;
    if (u == "EZ") return 2;
    return 5;
}

bool chart_list_diffs(const std::wstring& osz_path, std::vector<DiffEntry>& out) {
    out.clear();
    std::vector<unsigned char> buf;
    if (!read_file_w(osz_path, buf)) return false;
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, buf.data(), buf.size(), 0)) return false;
    mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        std::string name = st.m_filename ? st.m_filename : "";
        if (!ends_with_ci(name, ".osu")) continue;
        DiffEntry e;
        e.index = (int)i;
        e.file = name;
        e.name = bracket_name(name);
        out.push_back(e);
    }
    mz_zip_reader_end(&zip);
    std::sort(out.begin(), out.end(), [](const DiffEntry& a, const DiffEntry& b) {
        int ra = diff_rank(a.name), rb = diff_rank(b.name);
        if (ra != rb) return ra < rb;
        return a.name < b.name;
    });
    return !out.empty();
}

static void trim_ws(std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) e--;
    s = s.substr(b, e - b);
}

static void parse_osu(const char* data, size_t size, ChartData& c) {
    c.title.clear();
    c.artist.clear();
    c.diff.clear();
    c.notes.clear();
    std::string text(data, size);
    bool in_hit = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos
                                                                   : nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        trim_ws(line);
        if (line.empty() || line.rfind("//", 0) == 0) continue;
        if (line.front() == '[' && line.back() == ']') {
            in_hit = (line == "[HitObjects]");
            continue;
        }
        if (line.rfind("Title:", 0) == 0) { c.title = line.substr(6); trim_ws(c.title); continue; }
        if (line.rfind("Artist:", 0) == 0) { c.artist = line.substr(7); trim_ws(c.artist); continue; }
        if (line.rfind("Version:", 0) == 0) { c.diff = line.substr(8); trim_ws(c.diff); continue; }
        if (!in_hit) continue;
        // x,y,time,type,hitSound,endTime:...
        std::vector<std::string> parts;
        size_t p = 0;
        while (p <= line.size()) {
            size_t comma = line.find(',', p);
            parts.push_back(line.substr(p, comma == std::string::npos ? std::string::npos
                                                                      : comma - p));
            if (comma == std::string::npos) break;
            p = comma + 1;
        }
        if (parts.size() < 4) continue;
        int x = atoi(parts[0].c_str());
        int t_ms = atoi(parts[2].c_str());
        int type = atoi(parts[3].c_str());
        int lane = x * 4 / 512;
        if (lane < 0) lane = 0;
        if (lane > 3) lane = 3;
        float t = (float)(t_ms / 1000.0);
        if ((type & 128) && parts.size() >= 6) {
            double end_ms = atof(parts[5].c_str());
            float e = (float)(end_ms / 1000.0);
            if (e > t) {
                c.notes.push_back({t, e, lane});
                continue;
            }
        }
        c.notes.push_back({t, -1.0f, lane});
    }
    std::sort(c.notes.begin(), c.notes.end(),
              [](const ChartNote& a, const ChartNote& b) { return a.t < b.t; });
    c.first_note = c.notes.empty() ? 0.0 : (double)c.notes[0].t;
}

bool chart_load(const std::wstring& osz_path, int idx, ChartData& out) {
    std::vector<DiffEntry> diffs;
    if (!chart_list_diffs(osz_path, diffs)) return false;
    if (idx < 0 || idx >= (int)diffs.size()) return false;
    std::vector<unsigned char> buf;
    if (!read_file_w(osz_path, buf)) return false;
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, buf.data(), buf.size(), 0)) return false;
    size_t size = 0;
    void* p = mz_zip_reader_extract_to_heap(&zip, (mz_uint)diffs[idx].index, &size, 0);
    if (!p) {
        mz_zip_reader_end(&zip);
        return false;
    }
    parse_osu((const char*)p, size, out);
    mz_free(p);
    mz_zip_reader_end(&zip);
    if (out.diff.empty()) out.diff = diffs[idx].name;
    return !out.notes.empty();
}

bool chart_read_ogg(const std::wstring& osz_path, std::vector<unsigned char>& out) {
    out.clear();
    std::vector<unsigned char> buf;
    if (!read_file_w(osz_path, buf)) return false;
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, buf.data(), buf.size(), 0)) return false;
    mz_uint n = mz_zip_reader_get_num_files(&zip);
    bool found = false;
    for (mz_uint i = 0; i < n && !found; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        std::string name = st.m_filename ? st.m_filename : "";
        if (!ends_with_ci(name, ".ogg")) continue;
        size_t size = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (p && size > 0) {
            out.assign((unsigned char*)p, (unsigned char*)p + size);
            found = true;
        }
        if (p) mz_free(p);
    }
    mz_zip_reader_end(&zip);
    return found;
}
