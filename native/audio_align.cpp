// audio_align.cpp — WASAPI loopback 采集 + 歌头匹配状态机（无外部依赖）
// 方案移植自已验证的 Python 版（1.2s 快速锁定、暂停冻结原位恢复）。
#include "audio_align.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#define STB_VORBIS_HEADER_ONLY
#include "stb/stb_vorbis.h"

// ---------------- 常量（与已验证的 Python 版一致）----------------
static const int AA_SR = 16000;
static const int FRAME = 1024;
static const int HOP = 256;
static const double HOP_S = 256.0 / 16000.0;   // 0.016
static const double QUICK_S = 1.2;
static const double FULL_S = 2.5;
static const double HEAD_RANGE = 3.0;
static const float QUICK_SCORE = 0.72f;
static const float FULL_SCORE = 0.55f;
static const float RESUME_QUICK = 0.60f;
static const float RESUME_SCORE = 0.45f;
static const double PAUSE_SIL = 1.5;
static const double RESUME_RANGE = 8.0;
static const double LAT = 0.020;
static const int RING_SAMPLES = 16000 * 8;

static double now_s() { return GetTickCount64() / 1000.0; }
static void aa_log(const char* msg) { OutputDebugStringA(msg); }

// ---------------- FFT（迭代 radix-2）----------------
static void fft_inplace(float* re, float* im, int n, bool inv) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = (float)(2.0 * 3.14159265358979323846 / len * (inv ? 1 : -1));
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cwr = 1, cwi = 0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                float tr = re[b] * cwr - im[b] * cwi;
                float ti = re[b] * cwi + im[b] * cwr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
                float nwr = cwr * wr - cwi * wi;
                cwi = cwr * wi + cwi * wr;
                cwr = nwr;
            }
        }
    }
    if (inv)
        for (int i = 0; i < n; i++) { re[i] /= n; im[i] /= n; }
}

// ---------------- OGG 解码 → 16k 单声道 ----------------
bool aa_decode_mono16k(const std::vector<unsigned char>& ogg,
                       std::vector<float>& mono, double& duration) {
    mono.clear();
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_memory(ogg.data(), (int)ogg.size(), &err,
                                           nullptr);
    if (!v) return false;
    stb_vorbis_info info = stb_vorbis_get_info(v);
    int src_rate = info.sample_rate;
    int ch = info.channels;
    int src_total = (int)stb_vorbis_stream_length_in_samples(v);
    duration = src_rate > 0 ? (double)src_total / src_rate : 0.0;
    if (src_rate <= 0 || ch <= 0) {
        stb_vorbis_close(v);
        return false;
    }
    std::vector<short> pcm(1152 * ch);
    std::vector<int> buf;                 // 本批 mono 样本
    std::vector<float> out16;
    out16.reserve((size_t)((double)src_total * AA_SR / src_rate) + 16);
    double step = (double)src_rate / AA_SR;
    double pos = 0.0;                     // 绝对源位置
    long long consumed = 0;
    int last_s = 0;
    bool have_last = false;
    for (;;) {
        int n = stb_vorbis_get_frame_short_interleaved(v, 2, pcm.data(),
                                                       (int)pcm.size());
        if (n <= 0) break;
        buf.clear();
        buf.reserve(n);
        for (int i = 0; i < n; i++) {
            int acc = 0;
            for (int c = 0; c < ch; c++) acc += pcm[(size_t)i * ch + c];
            buf.push_back(acc / ch);
        }
        // 线性插值产出 16k 样本
        while (pos < consumed + (double)buf.size()) {
            double idx = pos - consumed;
            int i0 = (int)idx;
            double f = idx - i0;
            int a = (i0 == -1 && have_last) ? last_s
                                            : buf[i0];
            int b = (i0 + 1 < (int)buf.size()) ? buf[i0 + 1] : buf[i0];
            out16.push_back((a + (b - a) * (float)f) / 32768.0f);
            pos += step;
        }
        consumed += (long long)buf.size();
        if (!buf.empty()) {
            last_s = buf.back();
            have_last = true;
        }
    }
    stb_vorbis_close(v);
    mono.swap(out16);
    return !mono.empty();
}

// ---------------- onset 包络（与 Python onset.envelope 一致）----------------
bool aa_envelope(const float* x, size_t n, std::vector<float>& env) {
    env.clear();
    if (n < (size_t)FRAME) return false;
    size_t frames = 1 + (n - FRAME) / HOP;
    size_t bins = FRAME / 2 + 1;
    std::vector<float> mag(frames * bins), energy(frames);
    std::vector<float> re(FRAME), im(FRAME), hann(FRAME);
    for (int i = 0; i < FRAME; i++)
        hann[i] = 0.5f - 0.5f * cosf((float)(2.0 * 3.14159265358979 * i /
                                             (FRAME - 1)));  // np.hanning
    for (size_t f = 0; f < frames; f++) {
        const float* src = x + f * HOP;
        for (int i = 0; i < FRAME; i++) { re[i] = src[i] * hann[i]; im[i] = 0; }
        fft_inplace(re.data(), im.data(), FRAME, false);
        float* mrow = &mag[f * bins];
        double esum = 0;
        for (int i = 0; i < FRAME; i++) esum += (double)src[i] * src[i];
        energy[f] = (float)sqrt(esum / FRAME);
        for (size_t b = 0; b < bins; b++)
            mrow[b] = sqrtf(re[b] * re[b] + im[b] * im[b]);
    }
    std::vector<float> chA(frames), chB(frames);
    for (size_t f = 0; f < frames; f++) {
        const float* cur = &mag[f * bins];
        const float* prev = &mag[(f ? f - 1 : f) * bins];
        double fa = 0, fb = 0;
        float e = energy[f] + 1e-8f;
        for (size_t b = 0; b < bins; b++) {
            double d = (double)cur[b] - prev[b];
            if (d > 0) fa += d;
            double sc = cur[b] / e, sp = prev[b] / e;
            if (sc - sp > 0) fb += sc - sp;
        }
        chA[f] = (float)log1p(fa);
        chB[f] = (float)fb;
    }
    auto zscore = [&](std::vector<float>& v) {
        double m = 0;
        for (float q : v) m += q;
        m /= (double)v.size();
        double s = 0;
        for (float q : v) s += (double)(q - m) * (q - m);
        s = sqrt(s / (double)v.size());
        if (s < 1e-8)
            for (float& q : v) q = q - (float)m;
        else
            for (float& q : v) q = (float)((q - m) / s);
    };
    zscore(chA);
    zscore(chB);
    env.resize(frames);
    for (size_t f = 0; f < frames; f++) env[f] = chA[f] + chB[f];
    if (frames >= 3) {
        std::vector<float> t(env);
        for (size_t f = 0; f < frames; f++) {
            float s = t[f];
            int c = 1;
            if (f > 0) { s += t[f - 1]; c++; }
            if (f + 1 < frames) { s += t[f + 1]; c++; }
            env[f] = s / c;
        }
    }
    return true;
}

// ---------------- NCC（局部零均值，FFT 互相关）----------------
bool aa_ncc(const std::vector<float>& ref, const float* live, int Lw,
            int lo, int hi, float* score, int* best_k) {
    *score = -2;
    *best_k = 0;
    int n = (int)ref.size() - Lw + 1;
    if (n <= 0 || Lw < 8) return false;
    if (lo < 0) lo = 0;
    if (hi > n - 1) hi = n - 1;
    if (hi < lo) return false;
    std::vector<double> cs(ref.size() + 1), cs2(ref.size() + 1);
    cs[0] = cs2[0] = 0;
    for (size_t i = 0; i < ref.size(); i++) {
        double v = ref[i];
        cs[i + 1] = cs[i] + v;
        cs2[i + 1] = cs2[i] + v * v;
    }
    std::vector<float> lm(Lw);
    double lmean = 0;
    for (int i = 0; i < Lw; i++) lmean += live[i];
    lmean /= Lw;
    double ln = 0;
    for (int i = 0; i < Lw; i++) {
        lm[i] = live[i] - (float)lmean;
        ln += (double)lm[i] * lm[i];
    }
    ln = sqrt(ln);
    int P = 1;
    while (P < (int)ref.size() + Lw) P <<= 1;
    std::vector<float> ar(P, 0), ai(P, 0), br(P, 0), bi(P, 0);
    for (size_t i = 0; i < ref.size(); i++) ar[i] = ref[i];
    for (int i = 0; i < Lw; i++) br[i] = lm[Lw - 1 - i];
    fft_inplace(ar.data(), ai.data(), P, false);
    fft_inplace(br.data(), bi.data(), P, false);
    for (int i = 0; i < P; i++) {
        float r = ar[i] * br[i] - ai[i] * bi[i];
        float im = ar[i] * bi[i] + ai[i] * br[i];
        ar[i] = r;
        ai[i] = im;
    }
    fft_inplace(ar.data(), ai.data(), P, true);
    double best = -2;
    int bk = 0;
    for (int k = lo; k <= hi; k++) {
        double mean = (cs[k + Lw] - cs[k]) / Lw;
        double var = (cs2[k + Lw] - cs2[k]) / Lw - mean * mean;
        if (var < 1e-12) var = 1e-12;
        double denom = sqrt(var * Lw) * (ln + 1e-9);
        double s = ar[k + Lw - 1] / (denom + 1e-12);
        if (s > best) {
            best = s;
            bk = k;
        }
    }
    *score = (float)best;
    *best_k = bk;
    return true;
}

// ---------------- 运行时 ----------------
struct AaRuntime {
    std::mutex mtx;
    std::vector<float> ref_env;
    double ref_dur = 0;
    bool ref_ready = false;
    bool want_on = false;
    int state = AA_OFF;
    bool anchored = false;
    double song_time = 0, stamp = 0;
    double lock_pos = 0;
    float last_score = 0;
    char status[96] = "off";
    std::vector<float> ring;
    long long wr = 0;
    double last_sound = 0;
    double sound_start = -1;
    bool tried_quick = false, tried_full = false;
    double last_try_q = 0, last_try_f = 0;
    double frozen = 0;
    std::thread cap;
    std::thread worker;
    volatile LONG stop_flag = 0;
    bool cap_started = false;

    void set_status(const char* s) {
        strncpy(status, s, sizeof(status) - 1);
        status[sizeof(status) - 1] = 0;
    }
};
static AaRuntime g_aa;

static void aa_lock(double pos, double stamp_t, float score, const char* tag) {
    g_aa.anchored = true;
    g_aa.song_time = pos;
    g_aa.stamp = stamp_t;
    g_aa.state = AA_LOCK;
    g_aa.lock_pos = pos;
    g_aa.last_score = score;
    char b[96];
    snprintf(b, sizeof(b), "%s %.2fs (score %.2f)", tag, pos, score);
    g_aa.set_status(b);
}

static bool ring_tail(std::vector<float>& out, int back) {
    if (g_aa.wr < back) return false;
    long long s = g_aa.wr - back;
    out.resize(back);
    for (int i = 0; i < back; i++)
        out[i] = g_aa.ring[(size_t)((s + i) % RING_SAMPLES)];
    return true;
}
static bool ring_from(std::vector<float>& out, long long start, int len) {
    if (start < 0) return false;
    if (g_aa.wr < start + len) return false;
    if (g_aa.wr - start > RING_SAMPLES) return false;
    out.resize(len);
    for (int i = 0; i < len; i++)
        out[i] = g_aa.ring[(size_t)((start + i) % RING_SAMPLES)];
    return true;
}

static void run_matcher(double t) {
    const int quick_n = (int)(QUICK_S * AA_SR);
    const int full_n = (int)(FULL_S * AA_SR);
    const int head_hi = (int)(HEAD_RANGE / HOP_S);

    if (g_aa.state == AA_SEARCH && g_aa.ref_ready) {
        std::vector<float> win, env;
        float sc;
        int k;
        if (g_aa.wr >= quick_n && t - g_aa.last_try_q >= 0.15) {
            g_aa.last_try_q = t;
            if (ring_tail(win, quick_n) &&
                aa_envelope(win.data(), win.size(), env) &&
                aa_ncc(g_aa.ref_env, env.data(), (int)env.size(), 0, head_hi,
                       &sc, &k)) {
                g_aa.last_score = sc;
                if (sc >= QUICK_SCORE) {
                    aa_lock(k * HOP_S + QUICK_S + LAT, t, sc, "自动锁定");
                    return;
                }
            }
        }
        if (g_aa.wr >= full_n && t - g_aa.last_try_f >= 0.30) {
            g_aa.last_try_f = t;
            if (ring_tail(win, full_n) &&
                aa_envelope(win.data(), win.size(), env) &&
                aa_ncc(g_aa.ref_env, env.data(), (int)env.size(), 0, head_hi,
                       &sc, &k)) {
                g_aa.last_score = sc;
                if (sc >= FULL_SCORE)
                    aa_lock(k * HOP_S + FULL_S + LAT, t, sc, "自动锁定(完整)");
            }
        }
        if (g_aa.state == AA_SEARCH) g_aa.set_status("搜索歌曲开头…");
        return;
    }

    if (g_aa.state == AA_LOCK) {
        if (t - g_aa.last_sound >= PAUSE_SIL) {
            double sil = g_aa.last_sound;
            g_aa.song_time = g_aa.song_time + (sil - g_aa.stamp);
            g_aa.stamp = t;
            g_aa.frozen = g_aa.song_time;
            g_aa.state = AA_PAUSE;
            g_aa.sound_start = -1;
            g_aa.tried_quick = g_aa.tried_full = false;
            g_aa.set_status("游戏暂停，已冻结");
        }
        return;
    }

    if (g_aa.state == AA_PAUSE) {
        bool sounding = (t - g_aa.last_sound) < 0.05;
        if (!sounding) {
            if (g_aa.sound_start >= 0 && t - g_aa.last_sound >= 0.5) {
                g_aa.sound_start = -1;
                g_aa.tried_quick = g_aa.tried_full = false;
            }
            return;
        }
        if (g_aa.sound_start < 0) {
            g_aa.sound_start = g_aa.last_sound;
            g_aa.tried_quick = g_aa.tried_full = false;
            g_aa.set_status("恢复声音，匹配中…");
        }
        double dur = t - g_aa.sound_start;
        long long start_idx = g_aa.wr - (long long)(dur * AA_SR);
        std::vector<float> win, env;
        float sc;
        int k;
        int lo = (int)(std::max(0.0, g_aa.frozen - RESUME_RANGE) / HOP_S);
        int hi = (int)((g_aa.frozen + RESUME_RANGE) / HOP_S);
        if (!g_aa.tried_quick && dur >= QUICK_S) {
            g_aa.tried_quick = true;
            if (ring_from(win, start_idx, quick_n) &&
                aa_envelope(win.data(), win.size(), env) &&
                aa_ncc(g_aa.ref_env, env.data(), (int)env.size(), lo, hi, &sc,
                       &k) &&
                sc >= RESUME_QUICK) {
                g_aa.anchored = true;
                g_aa.song_time = k * HOP_S + dur;
                g_aa.stamp = t;
                g_aa.state = AA_LOCK;
                g_aa.lock_pos = k * HOP_S;
                g_aa.last_score = sc;
                g_aa.set_status("暂停恢复(快速)");
            }
            return;
        }
        if (!g_aa.tried_full && dur >= FULL_S) {
            g_aa.tried_full = true;
            if (ring_from(win, start_idx, full_n) &&
                aa_envelope(win.data(), win.size(), env)) {
                if (aa_ncc(g_aa.ref_env, env.data(), (int)env.size(), 0,
                           head_hi, &sc, &k) &&
                    sc >= FULL_SCORE && g_aa.frozen > HEAD_RANGE + 2.0) {
                    aa_lock(k * HOP_S + FULL_S + LAT, t, sc, "重开一局");
                    return;
                }
                if (aa_ncc(g_aa.ref_env, env.data(), (int)env.size(), lo, hi,
                           &sc, &k) &&
                    sc >= RESUME_SCORE) {
                    g_aa.anchored = true;
                    g_aa.song_time = k * HOP_S + dur;
                    g_aa.stamp = t;
                    g_aa.state = AA_LOCK;
                    g_aa.lock_pos = k * HOP_S;
                    g_aa.last_score = sc;
                    g_aa.set_status("暂停恢复(完整)");
                } else {
                    g_aa.set_status("恢复匹配失败，继续等待");
                }
            }
        }
    }
}

// ---------------- WASAPI loopback ----------------
static const CLSID MyCLSID_MMDeviceEnumerator =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID MyIID_IMMDeviceEnumerator =
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID MyIID_IAudioClient =
    {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID MyIID_IAudioCaptureClient =
    {0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
static const GUID MySUBTYPE_FLOAT =
    {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

static void aa_push(const float* s, int n, bool sounded, double t) {
    std::lock_guard<std::mutex> lk(g_aa.mtx);
    for (int i = 0; i < n; i++) {
        g_aa.ring[(size_t)(g_aa.wr % RING_SAMPLES)] = s[i];
        g_aa.wr++;
    }
    if (sounded) g_aa.last_sound = t;
    run_matcher(t);
}

static void capture_thread() {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(MyCLSID_MMDeviceEnumerator, nullptr,
                                CLSCTX_ALL, MyIID_IMMDeviceEnumerator,
                                (void**)&en)) ||
        !en) {
        aa_log("aa: enumerator fail");
        goto done;
    }
    {
        IMMDevice* dev = nullptr;
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev)) ||
            !dev) {
            aa_log("aa: no endpoint");
            goto rel_en;
        }
        IAudioClient* cli = nullptr;
        if (FAILED(dev->Activate(MyIID_IAudioClient, CLSCTX_ALL, nullptr,
                                 (void**)&cli)) ||
            !cli) {
            aa_log("aa: activate fail");
            dev->Release();
            goto rel_en;
        }
        {
            WAVEFORMATEX* wfx = nullptr;
            if (FAILED(cli->GetMixFormat(&wfx)) || !wfx) {
                aa_log("aa: mixformat fail");
                cli->Release();
                dev->Release();
                goto rel_en;
            }
            if (FAILED(cli->Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_LOOPBACK |
                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                    10000000, 0, wfx, nullptr))) {
                aa_log("aa: initialize fail");
                CoTaskMemFree(wfx);
                cli->Release();
                dev->Release();
                goto rel_en;
            }
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            cli->SetEventHandle(ev);
            IAudioCaptureClient* cap = nullptr;
            if (FAILED(cli->GetService(MyIID_IAudioCaptureClient,
                                       (void**)&cap)) ||
                !cap) {
                CloseHandle(ev);
                CoTaskMemFree(wfx);
                cli->Release();
                dev->Release();
                goto rel_en;
            }
            int sr = wfx->nSamplesPerSec;
            int ch = wfx->nChannels;
            int bits = wfx->wBitsPerSample;
            bool is_float = (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
            if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                auto* ex = (WAVEFORMATEXTENSIBLE*)wfx;
                is_float = IsEqualGUID(ex->SubFormat, MySUBTYPE_FLOAT);
            }
            CoTaskMemFree(wfx);
            cli->Start();
            aa_log("aa: loopback started");
            double step = (double)sr / AA_SR;
            double pos = 0.0;
            long long consumed = 0;
            float last_v = 0;
            bool have_last = false;
            std::vector<float> mono, block;
            BYTE* data = nullptr;
            UINT frames = 0;
            DWORD flags = 0;
            while (!g_aa.stop_flag) {
                WaitForSingleObject(ev, 100);
                while (cap->GetNextPacketSize(&frames) == S_OK && frames > 0) {
                    if (cap->GetBuffer(&data, &frames, &flags, nullptr,
                                       nullptr) != S_OK)
                        break;
                    bool sounded = !(flags & AUDCLNT_BUFFERFLAGS_SILENT);
                    mono.clear();
                    mono.reserve(frames);
                    if (!sounded) {
                        mono.assign(frames, 0.0f);
                    } else {
                        for (UINT i = 0; i < frames; i++) {
                            double acc = 0;
                            for (int c = 0; c < ch; c++) {
                                BYTE* p =
                                    data + ((size_t)i * ch + c) * (bits / 8);
                                double v = 0;
                                if (is_float && bits == 32) v = *(float*)p;
                                else if (bits == 16)
                                    v = *(short*)p / 32768.0;
                                else if (bits == 32)
                                    v = *(int*)p / 2147483648.0;
                                acc += v;
                            }
                            mono.push_back((float)(acc / ch));
                        }
                    }
                    cap->ReleaseBuffer(frames);
                    block.clear();
                    for (size_t i = 0; i < mono.size(); i++) {
                        while (pos < consumed + (double)i + 1) {
                            double idx = pos - consumed;
                            int i0 = (int)idx;
                            double f = idx - i0;
                            float a = (i0 == -1 && have_last) ? last_v
                                                              : mono[i0];
                            float b = (i0 + 1 < (int)mono.size())
                                          ? mono[i0 + 1]
                                          : mono[i0];
                            block.push_back(a + (b - a) * (float)f);
                            pos += step;
                        }
                    }
                    if (!mono.empty()) {
                        last_v = mono.back();
                        have_last = true;
                    }
                    consumed += (long long)mono.size();
                    aa_push(block.data(), (int)block.size(), sounded, now_s());
                }
            }
            cli->Stop();
            cap->Release();
            CloseHandle(ev);
        }
        cli->Release();
        dev->Release();
    }
rel_en:
    if (en) en->Release();
done:
    if (SUCCEEDED(hrCo)) CoUninitialize();
}

// ---------------- 公共 API ----------------
static void ensure_capture() {
    std::lock_guard<std::mutex> lk(g_aa.mtx);
    if (g_aa.cap_started) return;
    g_aa.cap_started = true;
    g_aa.stop_flag = 0;
    g_aa.ring.assign(RING_SAMPLES, 0.0f);
    g_aa.last_sound = now_s();
    g_aa.cap = std::thread(capture_thread);
}

void aa_start() { ensure_capture(); }

static void worker_analyze(std::vector<unsigned char> ogg) {
    std::vector<float> mono, env;
    double dur = 0;
    bool ok = aa_decode_mono16k(ogg, mono, dur) &&
              aa_envelope(mono.data(), mono.size(), env);
    std::lock_guard<std::mutex> lk(g_aa.mtx);
    if (ok) {
        g_aa.ref_env = std::move(env);
        g_aa.ref_dur = dur;
        g_aa.ref_ready = true;
        if (g_aa.want_on && g_aa.state != AA_LOCK && g_aa.state != AA_PAUSE) {
            g_aa.state = AA_SEARCH;
            g_aa.anchored = false;
            g_aa.set_status("搜索歌曲开头…");
        } else {
            char b[96];
            snprintf(b, sizeof(b), "参考就绪 %.0fs", dur);
            g_aa.set_status(b);
        }
    } else {
        g_aa.ref_ready = false;
        g_aa.set_status("音频分析失败");
    }
}

void aa_set_reference_ogg(std::vector<unsigned char> ogg) {
    {
        std::lock_guard<std::mutex> lk(g_aa.mtx);
        g_aa.ref_ready = false;
        if (g_aa.state == AA_SEARCH) g_aa.state = AA_ANALYZE;
        g_aa.set_status("音频分析中…");
    }
    ensure_capture();
    if (g_aa.worker.joinable()) g_aa.worker.join();
    g_aa.worker = std::thread(worker_analyze, std::move(ogg));
}

void aa_enable(bool on) {
    ensure_capture();
    std::lock_guard<std::mutex> lk(g_aa.mtx);
    g_aa.want_on = on;
    if (!on) {
        g_aa.state = AA_OFF;
        g_aa.anchored = false;
        g_aa.set_status("off");
        return;
    }
    if (g_aa.state == AA_LOCK || g_aa.state == AA_PAUSE) return;
    if (g_aa.ref_ready) {
        g_aa.state = AA_SEARCH;
        g_aa.anchored = false;
        g_aa.set_status("搜索歌曲开头…");
    } else {
        g_aa.state = AA_ANALYZE;
        g_aa.set_status("音频分析中…");
    }
}

void aa_manual_anchor(double song_time, double stamp) {
    std::lock_guard<std::mutex> lk(g_aa.mtx);
    g_aa.anchored = true;
    g_aa.song_time = song_time;
    g_aa.stamp = stamp;
    g_aa.state = AA_LOCK;
    g_aa.frozen = song_time;
    g_aa.set_status("手动锚定");
}

bool aa_poll(AaInfo& out) {
    std::lock_guard<std::mutex> lk(g_aa.mtx);
    out.state = g_aa.state;
    out.anchored = g_aa.anchored;
    out.song_time = g_aa.song_time;
    out.stamp = g_aa.stamp;
    out.last_score = g_aa.last_score;
    out.lock_pos = g_aa.lock_pos;
    strncpy(out.status, g_aa.status, sizeof(out.status) - 1);
    out.status[sizeof(out.status) - 1] = 0;
    return g_aa.state != AA_OFF;
}

void aa_shutdown() {
    g_aa.stop_flag = 1;
    if (g_aa.cap.joinable()) g_aa.cap.join();
    if (g_aa.worker.joinable()) g_aa.worker.join();
}
