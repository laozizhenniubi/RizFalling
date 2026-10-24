// test_align.cpp — 自动对齐核心离线测试 + WASAPI 端到端测试
#include "audio_align.h"
#include "chart_loader.h"
#include <cstdio>
#include <cstring>
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

static std::vector<unsigned char> load_ogg(const wchar_t* osz) {
    std::vector<unsigned char> ogg;
    if (!chart_read_ogg(osz, ogg)) { printf("ogg read fail\n"); }
    return ogg;
}

static bool write_wav(const char* path, const std::vector<float>& mono16k) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    int n = (int)mono16k.size();
    int data_bytes = n * 2;
    int sr = 16000;
    unsigned char hdr[44] = {'R','I','F','F'};
    *(int*)(hdr+4) = 36 + data_bytes;
    memcpy(hdr+8, "WAVEfmt ", 8);
    *(int*)(hdr+16) = 16;
    *(short*)(hdr+20) = 1;
    *(short*)(hdr+22) = 1;
    *(int*)(hdr+24) = sr;
    *(int*)(hdr+28) = sr * 2;
    *(short*)(hdr+32) = 2;
    *(short*)(hdr+34) = 16;
    memcpy(hdr+36, "data", 4);
    *(int*)(hdr+40) = data_bytes;
    fwrite(hdr, 1, 44, f);
    for (int i = 0; i < n; i++) {
        float v = mono16k[i]; if (v > 1) v = 1; if (v < -1) v = -1;
        short s16 = (short)(v * 32000);
        fwrite(&s16, 2, 1, f);
    }
    fclose(f);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { printf("usage: test_align <osz>\n"); return 1; }
    setvbuf(stdout, nullptr, _IONBF, 0);
    auto ogg = load_ogg(argv[1]);
    printf("ogg bytes=%zu\n", ogg.size());
    if (ogg.empty()) return 2;

    // 1) 解码 + 参考
    std::vector<float> mono;
    double dur = 0;
    if (!aa_decode_mono16k(ogg, mono, dur)) { printf("decode FAIL\n"); return 3; }
    printf("decoded %.1fs samples=%zu\n", dur, mono.size());
    std::vector<float> ref;
    if (!aa_envelope(mono.data(), mono.size(), ref)) { printf("env FAIL\n"); return 4; }
    printf("ref env frames=%zu\n", ref.size());

    // 2) NCC：开头段应高分、30s 中段应低分
    std::vector<float> env;
    auto seg = [&](double t0, double len) {
        std::vector<float> s(mono.begin() + (size_t)(t0*16000),
                             mono.begin() + (size_t)((t0+len)*16000));
        return s;
    };
    float sc; int k;
    aa_envelope(seg(0, 1.2).data(), (size_t)(1.2*16000), env);
    aa_ncc(ref, env.data(), (int)env.size(), 0, 187, &sc, &k);
    printf("head 1.2s  -> score=%.3f pos=%.2fs (expect >=0.72 pos~0)\n", sc, k*0.016);
    aa_envelope(seg(0.3, 1.2).data(), (size_t)(1.2*16000), env);
    aa_ncc(ref, env.data(), (int)env.size(), 0, 187, &sc, &k);
    printf("head 0.3+  -> score=%.3f pos=%.2fs (expect >=0.72 pos~0.3)\n", sc, k*0.016);
    aa_envelope(seg(30.0, 1.2).data(), (size_t)(1.2*16000), env);
    aa_ncc(ref, env.data(), (int)env.size(), 0, 187, &sc, &k);
    printf("mid  30s   -> score=%.3f (expect <0.72)\n", sc);

    // 3) WASAPI 端到端：扬声器放前 8 秒，等待锁定
    write_wav("aa_test.wav", std::vector<float>(mono.begin(),
               mono.begin() + std::min<size_t>(mono.size(), 8*16000)));
    aa_start();
    aa_set_reference_ogg(ogg);
    aa_enable(true);
    MCIERROR e = mciSendStringW(L"open aa_test.wav alias aatest", nullptr, 0, nullptr);
    if (e) { printf("mci open err %lu\n", (unsigned long)e); return 5; }
    mciSendStringW(L"play aatest", nullptr, 0, nullptr);
    printf("playing head 8s, waiting for lock...\n");
    AaInfo info;
    for (int i = 0; i < 120; i++) {
        Sleep(100);
        aa_poll(info);
        if (i % 10 == 0) printf("  t=%4.1fs state=%d %s\n", i*0.1, info.state, info.status);
        if (info.anchored) break;
    }
    mciSendStringW(L"close aatest", nullptr, 0, nullptr);
    aa_poll(info);
    printf("final: state=%d anchored=%d pos=%.2fs score=%.2f status=%s\n",
           info.state, (int)info.anchored, info.lock_pos, info.last_score, info.status);
    aa_shutdown();
    bool ok = info.anchored && info.lock_pos < 2.0;
    printf(ok ? "E2E PASS\n" : "E2E FAIL\n");
    return ok ? 0 : 9;
}
