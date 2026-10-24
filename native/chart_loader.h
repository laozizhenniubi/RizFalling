#pragma once
#include <string>
#include <vector>

// 谱面音符：t/end 单位秒；end < 0 表示 tap
struct ChartNote {
    float t;
    float end;
    int lane;
};

struct ChartData {
    std::string title;
    std::string artist;
    std::string diff;
    std::vector<ChartNote> notes;   // 按 t 升序
    double first_note = 0.0;
};

struct DiffEntry {
    int index = -1;      // zip 内文件序号
    std::string file;    // 包内文件名（UTF-8）
    std::string name;    // 显示用难度名（[..] 内）
};

// 读取 osz 内的 .osu 列表，按 IN/AT > HD > EZ > 其它 排序
bool chart_list_diffs(const std::wstring& osz_path, std::vector<DiffEntry>& out);

// 加载排序后的第 idx 个难度
bool chart_load(const std::wstring& osz_path, int idx, ChartData& out);

// 读取整个文件（Windows 宽字符路径）
bool read_file_w(const std::wstring& path, std::vector<unsigned char>& out);

// 从 osz 里取出音频（第一个 .ogg），供自动对齐分析
bool chart_read_ogg(const std::wstring& osz_path, std::vector<unsigned char>& out);
