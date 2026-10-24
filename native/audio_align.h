#pragma once
// 音频自动对齐：WASAPI loopback 采集 → onset 包络 → NCC 歌头匹配
// 移植自已验证的 Python 方案（2026-10-18 实测：1.2s 内锁定、误差约 20ms）。
#include <vector>
#include <cstdint>
#include <windows.h>

// ---- 纯函数（测试可直接调用）----
// OGG 字节 → 16k 单声道 float + 时长
bool aa_decode_mono16k(const std::vector<unsigned char>& ogg,
                       std::vector<float>& mono, double& duration);
// 波形 → onset 包络（双通道 z-score，与 Python onset.envelope 一致）
bool aa_envelope(const float* x, size_t n, std::vector<float>& env);
// 归一化互相关：live(L 帧) 在 ref[lo..hi] 帧范围找最佳起点
bool aa_ncc(const std::vector<float>& ref, const float* live, int L,
            int lo, int hi, float* score, int* best_k);

// ---- 运行时（游戏内）----
enum AaState { AA_OFF = 0, AA_ANALYZE, AA_SEARCH, AA_LOCK, AA_PAUSE };
struct AaInfo {
    int state = AA_OFF;
    bool anchored = false;
    double song_time = 0.0;   // 锚定/冻结后的谱面时间
    double stamp = 0.0;       // 对应 GetTickCount64()/1000.0
    float last_score = 0.0f;
    double lock_pos = 0.0;
    char status[96] = {0};
};

void aa_start();                                        // 启动采集线程（幂等）
void aa_set_reference_ogg(std::vector<unsigned char> ogg);  // 后台解码+建参考
void aa_enable(bool on);                                // on → SEARCH
void aa_manual_anchor(double song_time, double stamp);  // 手动锚定后接管冻结
bool aa_poll(AaInfo& out);                              // Present 线程每帧调
void aa_shutdown();
