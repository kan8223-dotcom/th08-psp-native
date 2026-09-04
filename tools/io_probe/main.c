/* TH08 PSP I/O probe: reproduces the PSP Go ef0 30-second read stall outside
 * the game.  Runs a series of access patterns against the game data next to
 * the port (../TH08PSP/) and reports per-operation latency on screen and in
 * TH08_IO_PROBE.LOG.  CIRCLE aborts, START skips the current phase. */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <pspsysmem.h>
#include <psppower.h>
#include <pspaudio.h>
#include <pspgu.h>
#include <pspge.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <malloc.h>
#include <fcntl.h>
#include <unistd.h>

PSP_MODULE_INFO("TH08IOPROBE", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(12288);

#define printf pspDebugScreenPrintf
#define CHUNK (64 * 1024)
#define MAX_SLOW 32

static char gDataDir[128];   /* ef0:/PSP/GAME/TH08PSP/ */
static char gProbeDir[128];  /* ef0:/PSP/GAME/TH08IOPROBE/ */
static char gLogPath[160];
static char gScratchPath[160];
static unsigned char *gBuf;      /* CHUNK */
static unsigned char *gBuf2;     /* CHUNK, helper thread */
static volatile int gAbort = 0;
static volatile int gSkip = 0;
static volatile int gHelperRun = 0;
static volatile unsigned long gHelperOps = 0;
static volatile unsigned long gHelperMaxMs = 0;
static volatile unsigned long gSpinCount = 0;

typedef struct { char phase[8]; char op[8]; unsigned long offset; unsigned long ms; } SlowEvent;
static SlowEvent gSlow[MAX_SLOW];
static int gSlowCount = 0;
static char gReport[4096];
static int gReportLen = 0;

static unsigned long long NowUs(void) { return (unsigned long long)sceKernelGetSystemTimeWide(); }

static void Report(const char *fmt, ...)
{
    va_list ap; char line[200];
    va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
    printf("%s", line);
    int n = strlen(line);
    if (gReportLen + n < (int)sizeof(gReport) - 1) { memcpy(gReport + gReportLen, line, n); gReportLen += n; gReport[gReportLen] = 0; }
}

static int gFlushed = 0;
static void FlushReport(void)
{
    SceUID fd = sceIoOpen(gLogPath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) { sceIoWrite(fd, gReport + gFlushed, gReportLen - gFlushed); sceIoClose(fd); gFlushed = gReportLen; }
}

static void NoteSlow(const char *phase, const char *op, unsigned long offset, unsigned long ms)
{
    if (gSlowCount < MAX_SLOW) {
        strncpy(gSlow[gSlowCount].phase, phase, 7); gSlow[gSlowCount].phase[7] = 0;
        strncpy(gSlow[gSlowCount].op, op, 7); gSlow[gSlowCount].op[7] = 0;
        gSlow[gSlowCount].offset = offset; gSlow[gSlowCount].ms = ms; gSlowCount++;
    }
}

static void PollButtons(void)
{
    SceCtrlData pad; sceCtrlPeekBufferPositive(&pad, 1);
    if (pad.Buttons & PSP_CTRL_CIRCLE) gAbort = 1;
    if (pad.Buttons & PSP_CTRL_START) gSkip = 1;
}

/* Sequential read of th08.dat in CHUNK reads.  Returns max ms. */
static unsigned long SeqReadInto(const char *phase, unsigned long limitBytes, unsigned char *buf)
{
    char path[160]; snprintf(path, sizeof(path), "%sth08.dat", gDataDir);
    unsigned long long t0 = NowUs();
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    unsigned long openMs = (unsigned long)((NowUs() - t0) / 1000);
    if (fd < 0) { Report("%s: open failed %08x\n", phase, (unsigned)fd); return 0; }
    if (openMs > 500) NoteSlow(phase, "open", 0, openMs);
    unsigned long total = 0, maxMs = 0, slow1s = 0, ops = 0; unsigned long long sumUs = 0;
    gSkip = 0;
    for (;;) {
        unsigned long long a = NowUs();
        int n = sceIoRead(fd, buf, CHUNK);
        unsigned long long d = NowUs() - a; sumUs += d;
        unsigned long ms = (unsigned long)(d / 1000);
        if (n <= 0) { if (n < 0) Report("%s: read error %08x at %lu\n", phase, (unsigned)n, total); break; }
        if (ms > maxMs) maxMs = ms;
        if (ms >= 1000) { slow1s++; NoteSlow(phase, "read", total, ms); }
        total += n; ops++;
        if ((ops & 31) == 0) { pspDebugScreenSetXY(0, 30); printf("%s %lu KiB max %lu ms slow>=1s %lu helper ops %lu max %lu ms   ", phase, total / 1024, maxMs, slow1s, gHelperOps, gHelperMaxMs); PollButtons(); if (gAbort || gSkip) break; }
        if (limitBytes && total >= limitBytes) break;
    }
    sceIoClose(fd);
    Report("%s: %lu KiB in %lu ms, %lu reads, avg %lu us, max %lu ms, slow>=1s %lu, open %lu ms\n",
           phase, total / 1024, (unsigned long)(sumUs / 1000), ops, ops ? (unsigned long)(sumUs / ops) : 0, maxMs, slow1s, openMs);
    return maxMs;
}

static unsigned long SeqRead(const char *phase, unsigned long limitBytes) { return SeqReadInto(phase, limitBytes, gBuf); }

/* Helper thread: 4 KiB append to scratch every 150 ms (BOOT.LOG-like). */
static int AppendThread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    while (gHelperRun) {
        unsigned long long a = NowUs();
        SceUID fd = sceIoOpen(gScratchPath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
        if (fd >= 0) { sceIoWrite(fd, gBuf2, 4096); sceIoClose(fd); }
        unsigned long ms = (unsigned long)((NowUs() - a) / 1000);
        if (ms > gHelperMaxMs) gHelperMaxMs = ms;
        if (ms >= 1000) NoteSlow("helper", "append", gHelperOps, ms);
        gHelperOps++;
        sceKernelDelayThread(150 * 1000);
    }
    return 0;
}

/* Helper thread: BGM-like sequential 32 KiB reads of thbgm.dat every 40 ms. */
static int BgmThread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    char path[160]; snprintf(path, sizeof(path), "%sthbgm.dat", gDataDir);
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0) { gHelperMaxMs = 99999; return 0; }
    sceIoLseek32(fd, 64 * 1024 * 1024, PSP_SEEK_SET);
    while (gHelperRun) {
        unsigned long long a = NowUs();
        int n = sceIoRead(fd, gBuf2, 32 * 1024);
        unsigned long ms = (unsigned long)((NowUs() - a) / 1000);
        if (ms > gHelperMaxMs) gHelperMaxMs = ms;
        if (ms >= 1000) NoteSlow("helper", "bgmread", gHelperOps, ms);
        if (n <= 0) sceIoLseek32(fd, 0, PSP_SEEK_SET);
        gHelperOps++;
        sceKernelDelayThread(40 * 1000);
    }
    sceIoClose(fd);
    return 0;
}

/* Helper thread: pure spin at the given priority. */
static int SpinThread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    while (gHelperRun) { gSpinCount++; }
    return 0;
}

/* Background load: audio output of silence on channel 0 (game-like DMA/IRQ). */
static volatile int gLoadRun = 0;
static int AudioLoadThread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    static short silence[1024 * 2] __attribute__((aligned(64)));
    memset(silence, 0, sizeof(silence));
    int ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, 1024, PSP_AUDIO_FORMAT_STEREO);
    while (gLoadRun) { if (ch >= 0) sceAudioOutputBlocking(ch, 0x4000, silence); else sceKernelDelayThread(20000); }
    if (ch >= 0) sceAudioChRelease(ch);
    return 0;
}
/* Background load: GE block copies inside VRAM every VBlank (does not touch the debug screen). */
static unsigned int __attribute__((aligned(64))) gGeList[1024];
static int GeLoadThread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    sceGuInit();
    while (gLoadRun) {
        sceGuStart(GU_DIRECT, gGeList);
        for (int i = 0; i < 8; i++)
            sceGuCopyImage(GU_PSM_8888, 0, 0, 256, 128, 512, (void *)(0x44000000 + 0x00180000),
                           0, 0, 512, (void *)(0x44000000 + 0x001C0000));
        sceGuFinish();
        sceGuSync(0, 0);
        sceDisplayWaitVblankStart();
    }
    sceGuTerm();
    return 0;
}

/* Bus saturation: GE copies a 1 MiB main-RAM texture into VRAM continuously
 * (game-like texture streaming) while a CPU thread memcpys 1 MiB buffers. */
static volatile int gBusRun = 0;
static volatile unsigned long gBusGeIters = 0, gBusCpuIters = 0;
static unsigned int __attribute__((aligned(64))) gBusList[2048];
static int GeBusThread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    unsigned char *tex = memalign(64, 1024 * 1024);
    if (!tex) return 0;
    memset(tex, 0x3c, 1024 * 1024);
    sceKernelDcacheWritebackAll();
    sceGuInit();
    while (gBusRun) {
        sceGuStart(GU_DIRECT, gBusList);
        for (int i = 0; i < 4; i++)
            sceGuCopyImage(GU_PSM_8888, 0, 0, 512, 128, 512, tex + i * 262144,
                           0, 0, 512, (void *)(0x44000000 + 0x00180000));
        sceGuFinish();
        sceGuSync(0, 0);
        gBusGeIters++;
    }
    sceGuTerm();
    free(tex);
    return 0;
}
static int CpuBusThread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    unsigned char *a = memalign(64, 1024 * 1024), *b = memalign(64, 1024 * 1024);
    if (!a || !b) return 0;
    memset(a, 1, 1024 * 1024);
    while (gBusRun) { memcpy(b, a, 1024 * 1024); memcpy(a, b, 1024 * 1024); gBusCpuIters++; }
    free(a); free(b);
    return 0;
}

static SceUID StartHelper(const char *name, SceKernelThreadEntry entry, int prio)
{
    gHelperRun = 1; gHelperOps = 0; gHelperMaxMs = 0;
    SceUID th = sceKernelCreateThread(name, entry, prio, 0x4000, THREAD_ATTR_USER, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
    return th;
}

static void StopHelper(SceUID th)
{
    gHelperRun = 0;
    if (th >= 0) { sceKernelWaitThreadEnd(th, NULL); sceKernelDeleteThread(th); }
}

static void PhaseFontRandom(void)
{
    char path[160]; snprintf(path, sizeof(path), "%smsgothic-subset.ttf", gDataDir);
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0) { snprintf(path, sizeof(path), "%sNotoSansJP-Regular.ttf", gDataDir); fd = sceIoOpen(path, PSP_O_RDONLY, 0777); }
    if (fd < 0) { Report("P2: font open failed\n"); return; }
    int size = sceIoLseek32(fd, 0, PSP_SEEK_END);
    unsigned long maxMs = 0, slow = 0, ops = 0; unsigned long long sumUs = 0; unsigned seed = 12345;
    gSkip = 0;
    for (int i = 0; i < 3000; i++) {
        seed = seed * 1103515245u + 12345u;
        int off = (int)((seed >> 8) % (unsigned)(size > 512 ? size - 512 : 1));
        unsigned long long a = NowUs();
        sceIoLseek32(fd, off, PSP_SEEK_SET);
        int n = sceIoRead(fd, gBuf, 512);
        unsigned long long d = NowUs() - a; sumUs += d; ops++;
        unsigned long ms = (unsigned long)(d / 1000);
        if (ms > maxMs) maxMs = ms;
        if (ms >= 1000) { slow++; NoteSlow("P2", "seekrd", (unsigned long)off, ms); }
        if (n < 0) { Report("P2: read error %08x\n", (unsigned)n); break; }
        if ((i & 63) == 0) { pspDebugScreenSetXY(0, 30); printf("P2 font random %d/3000 max %lu ms slow %lu            ", i, maxMs, slow); PollButtons(); if (gAbort || gSkip) break; }
    }
    sceIoClose(fd);
    Report("P2: font random 512B reads %lu, avg %lu us, max %lu ms, slow>=1s %lu\n", ops, ops ? (unsigned long)(sumUs / ops) : 0, maxMs, slow);
}

static void PhaseReopen(void)
{
    char path[160]; snprintf(path, sizeof(path), "%sth08.dat", gDataDir);
    SceUID keep = sceIoOpen(path, PSP_O_RDONLY, 0777);
    unsigned long maxMs = 0, slow = 0; unsigned long long sumUs = 0; int ops = 0;
    gSkip = 0;
    for (int i = 0; i < 100; i++) {
        unsigned long long a = NowUs();
        SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
        if (fd >= 0) { sceIoLseek32(fd, (i * 331) % 40000 * 1024, PSP_SEEK_SET); sceIoRead(fd, gBuf, 4096); sceIoClose(fd); }
        unsigned long long d = NowUs() - a; sumUs += d; ops++;
        unsigned long ms = (unsigned long)(d / 1000);
        if (ms > maxMs) maxMs = ms;
        if (ms >= 1000) { slow++; NoteSlow("P6", "reopen", (unsigned long)i, ms); }
        if ((i & 7) == 0) { pspDebugScreenSetXY(0, 30); printf("P6 reopen %d/100 max %lu ms slow %lu            ", i, maxMs, slow); PollButtons(); if (gAbort || gSkip) break; }
    }
    if (keep >= 0) sceIoClose(keep);
    Report("P6: reopen+4KiB read x%d, avg %lu us, max %lu ms, slow>=1s %lu\n", ops, ops ? (unsigned long)(sumUs / ops) : 0, maxMs, slow);
}

static void PhaseWriteThenRead(void)
{
    char path[160]; snprintf(path, sizeof(path), "%sth08.dat", gDataDir);
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0) { Report("P7: open failed\n"); return; }
    unsigned long maxW = 0, maxR = 0, slow = 0; int ops = 0;
    gSkip = 0;
    for (int i = 0; i < 40; i++) {
        unsigned long long a = NowUs();
        SceUID w = sceIoOpen(gScratchPath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
        if (w >= 0) { sceIoWrite(w, gBuf2, CHUNK); sceIoClose(w); }
        unsigned long wms = (unsigned long)((NowUs() - a) / 1000);
        if (wms > maxW) maxW = wms;
        if (wms >= 1000) { slow++; NoteSlow("P7", "write", (unsigned long)i, wms); }
        unsigned long long b = NowUs();
        sceIoLseek32(fd, (i * 977) % 600 * CHUNK, PSP_SEEK_SET);
        sceIoRead(fd, gBuf, CHUNK);
        unsigned long rms = (unsigned long)((NowUs() - b) / 1000);
        if (rms > maxR) maxR = rms;
        if (rms >= 1000) { slow++; NoteSlow("P7", "readaw", (unsigned long)i, rms); }
        ops++;
        pspDebugScreenSetXY(0, 30); printf("P7 write->read %d/40 maxW %lu ms maxR %lu ms slow %lu     ", i, maxW, maxR, slow); PollButtons(); if (gAbort || gSkip) break;
    }
    sceIoClose(fd);
    Report("P7: 64KiB append then 64KiB read x%d, max write %lu ms, max read-after-write %lu ms, slow>=1s %lu\n", ops, maxW, maxR, slow);
}

static void PhaseBgmLike(const char *phase, unsigned iterations)
{
    char path[160]; snprintf(path, sizeof(path), "%sthbgm.dat", gDataDir);
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0) { Report("%s: thbgm open failed\n", phase); return; }
    unsigned long maxMs = 0, slow = 0, ops = 0; unsigned long long sumUs = 0;
    sceIoLseek32(fd, 100000003, PSP_SEEK_SET);
    gSkip = 0;
    for (unsigned i = 0; i < iterations; i++) {
        unsigned long long a = NowUs();
        int n = sceIoRead(fd, gBuf, 44100);
        unsigned long long d = NowUs() - a; sumUs += d; ops++;
        unsigned long ms = (unsigned long)(d / 1000);
        if (ms > maxMs) maxMs = ms;
        if (ms >= 1000) { slow++; NoteSlow(phase, "bgm44k", i, ms); }
        if (n <= 0) sceIoLseek32(fd, 100000003, PSP_SEEK_SET);
        if ((i % 20) == 0) { pspDebugScreenSetXY(0, 30); printf("%s bgm-like %u/%u max %lu ms slow %lu       ", phase, i, iterations, maxMs, slow); PollButtons(); if (gAbort || gSkip) break; }
        sceKernelDelayThread(40 * 1000);
    }
    sceIoClose(fd);
    Report("%s: bgm-like 44100B reads x%lu avg %lu us max %lu ms slow>=1s %lu\n", phase, ops, ops ? (unsigned long)(sumUs / ops) : 0, maxMs, slow);
}

static void PhaseArchiveLike(const char *phase, unsigned iterations)
{
    char path[160]; snprintf(path, sizeof(path), "%sth08.dat", gDataDir);
    unsigned char *big = memalign(64, 1024 * 1024);
    if (!big) { Report("%s: alloc failed\n", phase); return; }
    unsigned long maxMs = 0, slow = 0, ops = 0; unsigned long long sumUs = 0; unsigned seed = 777;
    gSkip = 0;
    for (unsigned i = 0; i < iterations; i++) {
        seed = seed * 1103515245u + 12345u;
        int off = (int)((seed >> 4) % 45000000u) | 1;
        seed = seed * 1103515245u + 12345u;
        int size = (int)((seed >> 6) % 900000u) + 1001;
        unsigned long long a = NowUs();
        SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
        int n = -1;
        if (fd >= 0) { sceIoLseek32(fd, off, PSP_SEEK_SET); n = sceIoRead(fd, big, size); sceIoClose(fd); }
        unsigned long long d = NowUs() - a; sumUs += d; ops++;
        unsigned long ms = (unsigned long)(d / 1000);
        if (ms > maxMs) maxMs = ms;
        if (ms >= 1000) { slow++; NoteSlow(phase, "arch", (unsigned long)off, ms); }
        if (n < 0) Report("%s: read error at %d\n", phase, off);
        if ((i % 10) == 0) { pspDebugScreenSetXY(0, 30); printf("%s archive-like %u/%u max %lu ms slow %lu       ", phase, i, iterations, maxMs, slow); PollButtons(); if (gAbort || gSkip) break; }
    }
    free(big);
    Report("%s: archive-like open/seek/read/close x%lu avg %lu us max %lu ms slow>=1s %lu\n", phase, ops, ops ? (unsigned long)(sumUs / ops) : 0, maxMs, slow);
}

/* FreeType-like burst: (lseek 0 CUR, lseek pos SET, read tiny) x12, 80 ms gap,
 * then one 44100-byte BGM read on another descriptor.  newlib variant. */
static void PhaseTinyBurstNewlib(const char *phase, unsigned bursts)
{
    char fpath[160], bpath[160];
    snprintf(fpath, sizeof(fpath), "%smsgothic-subset.ttf", gDataDir);
    snprintf(bpath, sizeof(bpath), "%sthbgm.dat", gDataDir);
    int ff = open(fpath, O_RDONLY, 0777);
    int bf = open(bpath, O_RDONLY, 0777);
    if (ff < 0 || bf < 0) { Report("%s: open failed ff=%d bf=%d\n", phase, ff, bf); if (ff >= 0) close(ff); if (bf >= 0) close(bf); return; }
    int size = (int)lseek(ff, 0, SEEK_END); lseek(ff, 0, SEEK_SET);
    lseek(bf, 100000003, SEEK_SET);
    static const int sizes[8] = { 2, 4, 8, 16, 32, 64, 128, 300 };
    unsigned long maxTiny = 0, maxBgm = 0, slow = 0; unsigned seed = 4242; unsigned long ops = 0;
    gSkip = 0;
    for (unsigned b = 0; b < bursts; b++) {
        for (int i = 0; i < 12; i++) {
            seed = seed * 1103515245u + 12345u;
            int off = (int)((seed >> 8) % (unsigned)(size > 1024 ? size - 1024 : 1));
            int n = sizes[(seed >> 3) & 7];
            unsigned long long a = NowUs();
            lseek(ff, 0, SEEK_CUR);
            lseek(ff, off, SEEK_SET);
            int r = read(ff, gBuf, n);
            unsigned long ms = (unsigned long)((NowUs() - a) / 1000); ops++;
            if (ms > maxTiny) maxTiny = ms;
            if (ms >= 1000) { slow++; NoteSlow(phase, "tiny", (unsigned long)off, ms); }
            if (r < 0) Report("%s: tiny read error\n", phase);
        }
        sceKernelDelayThread(80 * 1000);
        unsigned long long a = NowUs();
        int r = read(bf, gBuf, 44100);
        unsigned long ms = (unsigned long)((NowUs() - a) / 1000);
        if (ms > maxBgm) maxBgm = ms;
        if (ms >= 1000) { slow++; NoteSlow(phase, "bgmnext", b, ms); }
        if (r <= 0) lseek(bf, 100000003, SEEK_SET);
        if ((b % 5) == 0) { pspDebugScreenSetXY(0, 30); printf("%s burst %u/%u tiny max %lu ms bgm max %lu ms slow %lu   ", phase, b, bursts, maxTiny, maxBgm, slow); PollButtons(); if (gAbort || gSkip) break; }
    }
    close(ff); close(bf);
    Report("%s: newlib tiny bursts x%lu ops, tiny max %lu ms, bgm-after-burst max %lu ms, slow>=1s %lu\n", phase, ops, maxTiny, maxBgm, slow);
}

/* Same pattern with raw sceIo (64-bit sceIoLseek). */
static void PhaseTinyBurstSceIo(const char *phase, unsigned bursts)
{
    char fpath[160], bpath[160];
    snprintf(fpath, sizeof(fpath), "%smsgothic-subset.ttf", gDataDir);
    snprintf(bpath, sizeof(bpath), "%sthbgm.dat", gDataDir);
    SceUID ff = sceIoOpen(fpath, PSP_O_RDONLY, 0777);
    SceUID bf = sceIoOpen(bpath, PSP_O_RDONLY, 0777);
    if (ff < 0 || bf < 0) { Report("%s: open failed\n", phase); if (ff >= 0) sceIoClose(ff); if (bf >= 0) sceIoClose(bf); return; }
    int size = (int)sceIoLseek(ff, 0, PSP_SEEK_END); sceIoLseek(ff, 0, PSP_SEEK_SET);
    sceIoLseek(bf, 100000003, PSP_SEEK_SET);
    static const int sizes[8] = { 2, 4, 8, 16, 32, 64, 128, 300 };
    unsigned long maxTiny = 0, maxBgm = 0, slow = 0; unsigned seed = 9191; unsigned long ops = 0;
    gSkip = 0;
    for (unsigned b = 0; b < bursts; b++) {
        for (int i = 0; i < 12; i++) {
            seed = seed * 1103515245u + 12345u;
            int off = (int)((seed >> 8) % (unsigned)(size > 1024 ? size - 1024 : 1));
            int n = sizes[(seed >> 3) & 7];
            unsigned long long a = NowUs();
            sceIoLseek(ff, 0, PSP_SEEK_CUR);
            sceIoLseek(ff, off, PSP_SEEK_SET);
            int r = sceIoRead(ff, gBuf, n);
            unsigned long ms = (unsigned long)((NowUs() - a) / 1000); ops++;
            if (ms > maxTiny) maxTiny = ms;
            if (ms >= 1000) { slow++; NoteSlow(phase, "tiny", (unsigned long)off, ms); }
            if (r < 0) Report("%s: tiny read error\n", phase);
        }
        sceKernelDelayThread(80 * 1000);
        unsigned long long a = NowUs();
        int r = sceIoRead(bf, gBuf, 44100);
        unsigned long ms = (unsigned long)((NowUs() - a) / 1000);
        if (ms > maxBgm) maxBgm = ms;
        if (ms >= 1000) { slow++; NoteSlow(phase, "bgmnext", b, ms); }
        if (r <= 0) sceIoLseek(bf, 100000003, PSP_SEEK_SET);
        if ((b % 5) == 0) { pspDebugScreenSetXY(0, 30); printf("%s burst %u/%u tiny max %lu ms bgm max %lu ms slow %lu   ", phase, b, bursts, maxTiny, maxBgm, slow); PollButtons(); if (gAbort || gSkip) break; }
    }
    sceIoClose(ff); sceIoClose(bf);
    Report("%s: sceIo tiny bursts x%lu ops, tiny max %lu ms, bgm-after-burst max %lu ms, slow>=1s %lu\n", phase, ops, maxTiny, maxBgm, slow);
}

int main(int argc, char **argv)
{
    pspDebugScreenInit();
    sceCtrlSetSamplingCycle(0); sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    const char *self = argc > 0 ? argv[0] : "ms0:/PSP/GAME/TH08IOPROBE/EBOOT.PBP";
    const char *slash = strrchr(self, '/');
    int len = slash ? (int)(slash - self + 1) : 0;
    if (len <= 0 || len >= (int)sizeof(gProbeDir)) { strcpy(gProbeDir, "ms0:/PSP/GAME/TH08IOPROBE/"); len = strlen(gProbeDir); }
    else { memcpy(gProbeDir, self, len); gProbeDir[len] = 0; }
    /* data dir = <device>:/PSP/GAME/TH08PSP/ */
    const char *colon = strchr(gProbeDir, ':');
    snprintf(gDataDir, sizeof(gDataDir), "%.*s/PSP/GAME/TH08PSP/", colon ? (int)(colon - gProbeDir + 1) : 4, gProbeDir);
    {
        char probe[160]; snprintf(probe, sizeof(probe), "%sth08.dat", gDataDir);
        SceUID t = sceIoOpen(probe, PSP_O_RDONLY, 0777);
        if (t < 0) {
            const char *cands[] = { "ef0:/PSP/GAME/TH08PSP/", "ms0:/PSP/GAME/TH08PSP/" };
            for (int i = 0; i < 2; i++) {
                snprintf(probe, sizeof(probe), "%sth08.dat", cands[i]);
                t = sceIoOpen(probe, PSP_O_RDONLY, 0777);
                if (t >= 0) { strcpy(gDataDir, cands[i]); const char *c2 = strchr(cands[i], ':'); snprintf(gProbeDir, sizeof(gProbeDir), "%.*s/PSP/GAME/TH08IOPROBE/", (int)(c2 - cands[i] + 1), cands[i]); break; }
            }
        }
        if (t >= 0) sceIoClose(t);
    }
    snprintf(gLogPath, sizeof(gLogPath), "%sTH08_IO_PROBE.LOG", gProbeDir);
    sceIoRemove(gLogPath);
    snprintf(gScratchPath, sizeof(gScratchPath), "%sprobe_append.bin", gProbeDir);
    gBuf = memalign(64, CHUNK); gBuf2 = memalign(64, CHUNK);
    if (!gBuf || !gBuf2) { printf("alloc failed\n"); sceKernelDelayThread(3000000); sceKernelExitGame(); }
    memset(gBuf2, 0x5a, CHUNK);
    sceIoRemove(gScratchPath);
    const int cpuBefore = scePowerGetCpuClockFrequencyInt(), busBefore = scePowerGetBusClockFrequencyInt();
    scePowerSetClockFrequency(333, 333, 166);
    Report("clock before %d/%d -> set(333,333,166) -> now %d/%d\n", cpuBefore, busBefore, scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt());
    Report("TH08 IO PROBE v6  cpu=%d bus=%d\nprobe=%s\ndata=%s\nCIRCLE=abort START=skip phase\n", scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt(), gProbeDir, gDataDir);

    Report("argv0=%s\n", self);
    FlushReport();
    if (!gAbort) { PhaseTinyBurstNewlib("P17", 150); FlushReport(); }
    if (!gAbort) { PhaseTinyBurstSceIo("P18", 150); FlushReport(); }
    if (!gAbort) { SeqRead("P1", 0); FlushReport(); }
    if (!gAbort) { PhaseFontRandom(); FlushReport(); }
    if (!gAbort) { SceUID th = StartHelper("appender", AppendThread, 32); SeqRead("P3", 0); StopHelper(th); Report("   P3 helper: %lu appends, max %lu ms\n", gHelperOps, gHelperMaxMs); FlushReport(); }
    if (!gAbort) { SceUID th = StartHelper("bgm", BgmThread, 16); SeqRead("P4", 0); StopHelper(th); Report("   P4 helper: %lu bgm reads, max %lu ms\n", gHelperOps, gHelperMaxMs); FlushReport(); }
    if (!gAbort) { SceUID th = StartHelper("spin33", SpinThread, 33); SeqRead("P5", 16 * 1024 * 1024); StopHelper(th); Report("   P5 spinner(prio33) count %lu\n", gSpinCount); FlushReport(); }
    if (!gAbort) {
        SceUID hi = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "hibuf", PSP_SMEM_High, CHUNK + 64, NULL);
        SceUID lo = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "lobuf", PSP_SMEM_Low, CHUNK + 64, NULL);
        unsigned char *hb = hi >= 0 ? (unsigned char *)(((unsigned)sceKernelGetBlockHeadAddr(hi) + 63) & ~63u) : NULL;
        unsigned char *lb = lo >= 0 ? (unsigned char *)(((unsigned)sceKernelGetBlockHeadAddr(lo) + 63) & ~63u) : NULL;
        Report("P8/P9 buffers: high=%p low=%p memalign=%p (user partition max free %u KiB)\n", (void *)hb, (void *)lb, (void *)gBuf, (unsigned)(sceKernelMaxFreeMemSize() / 1024));
        if (hb) { SeqReadInto("P8hi", 24 * 1024 * 1024, hb); FlushReport(); }
        if (!gAbort && lb) { SeqReadInto("P9lo", 24 * 1024 * 1024, lb); FlushReport(); }
        if (hi >= 0) sceKernelFreePartitionMemory(hi);
        if (lo >= 0) sceKernelFreePartitionMemory(lo);
    }
    if (!gAbort) { PhaseReopen(); FlushReport(); }
    if (!gAbort) { PhaseWriteThenRead(); FlushReport(); }
    if (!gAbort) { PhaseBgmLike("P10", 600); FlushReport(); }
    if (!gAbort) { PhaseArchiveLike("P11", 120); FlushReport(); }
    if (!gAbort) {
        gLoadRun = 1;
        SceUID au = sceKernelCreateThread("audioload", AudioLoadThread, 16, 0x4000, THREAD_ATTR_USER, NULL);
        SceUID ge = sceKernelCreateThread("geload", GeLoadThread, 40, 0x8000, THREAD_ATTR_USER, NULL);
        if (au >= 0) sceKernelStartThread(au, 0, NULL);
        if (ge >= 0) sceKernelStartThread(ge, 0, NULL);
        SceUID bg = StartHelper("bgm2", BgmThread, 16);
        PhaseArchiveLike("P12", 150);
        if (!gAbort) PhaseBgmLike("P13", 600);
        StopHelper(bg);
        Report("   P12/13 bgm helper: %lu reads max %lu ms (audio+GE load running)\n", gHelperOps, gHelperMaxMs);
        gLoadRun = 0;
        if (au >= 0) { sceKernelWaitThreadEnd(au, NULL); sceKernelDeleteThread(au); }
        if (ge >= 0) { sceKernelWaitThreadEnd(ge, NULL); sceKernelDeleteThread(ge); }
        FlushReport();
    }
    if (!gAbort) {
        gBusRun = 1;
        SceUID gb = sceKernelCreateThread("gebus", GeBusThread, 40, 0x8000, THREAD_ATTR_USER, NULL);
        SceUID cb = sceKernelCreateThread("cpubus", CpuBusThread, 33, 0x4000, THREAD_ATTR_USER, NULL);
        if (gb >= 0) sceKernelStartThread(gb, 0, NULL);
        if (cb >= 0) sceKernelStartThread(cb, 0, NULL);
        SceUID bg = StartHelper("bgm3", BgmThread, 16);
        SeqRead("P14", 0);
        if (!gAbort) PhaseBgmLike("P15", 600);
        if (!gAbort) PhaseArchiveLike("P16", 150);
        if (!gAbort) PhaseFontRandom();
        StopHelper(bg);
        Report("   P14-16 under bus load: GE iters %lu (x1MiB main RAM->VRAM), CPU memcpy iters %lu (x2MiB), bgm helper %lu reads max %lu ms\n", gBusGeIters, gBusCpuIters, gHelperOps, gHelperMaxMs);
        gBusRun = 0;
        if (gb >= 0) { sceKernelWaitThreadEnd(gb, NULL); sceKernelDeleteThread(gb); }
        if (cb >= 0) { sceKernelWaitThreadEnd(cb, NULL); sceKernelDeleteThread(cb); }
        FlushReport();
    }
    Report("slow events (>=1s): %d\n", gSlowCount);
    for (int i = 0; i < gSlowCount; i++) Report("  %s %s off=%lu %lu ms\n", gSlow[i].phase, gSlow[i].op, gSlow[i].offset, gSlow[i].ms);
    Report(gAbort ? "ABORTED\n" : "DONE\n");
    FlushReport();
    printf("\nlog: %s\nPress CIRCLE to exit\n", gLogPath);
    for (;;) { SceCtrlData pad; sceCtrlReadBufferPositive(&pad, 1); if (pad.Buttons & PSP_CTRL_CIRCLE) break; sceKernelDelayThread(50000); }
    sceKernelExitGame();
    return 0;
}
