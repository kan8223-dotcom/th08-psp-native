#!/usr/bin/env python3
"""Run real platform/stream lifetime code with deterministic SDK/GE stubs."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_psp_trig_df_fastpath import function_body

ROOT = Path(__file__).resolve().parents[1]
SDK = r'''
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <cassert>
using SceSize=unsigned; using SceUID=int;
using Entry=int(*)(SceSize,void*);
struct SceCtrlData {unsigned Buttons=0;};
struct SceKernelThreadInfo {unsigned size; int status=0,waitType=0,waitId=0;};
const unsigned PSP_THREAD_ATTR_USER=1,PSP_POWER_CB_SUSPENDING=1,PSP_POWER_CB_RESUME_COMPLETE=2,
 PSP_POWER_TICK_ALL=0,PSP_CTRL_MODE_ANALOG=1,PSP_CTRL_SELECT=1;
struct Thread {std::string name;Entry fn;unsigned stack;bool started=false;};
inline std::vector<Thread> threads;
inline unsigned nowUs=0,delays=0,resets=0,logs=0;
inline unsigned exits=0;
inline bool blockLog=false,returnFromExit=false;
struct Idle {}; struct Blocked {}; struct XmbExit {};
inline int sceKernelCreateThread(const char*n,Entry f,int,unsigned s,unsigned,void*) {
 threads.push_back({n,f,s});return int(threads.size());}
inline int sceKernelStartThread(int id,unsigned,void*) {threads.at(id-1).started=true;return 0;}
inline int sceKernelDelayThread(unsigned us) {if(++delays>300)throw Idle{};nowUs+=us;return 0;}
inline int sceKernelGetThreadId(){return 88;}
inline unsigned sceKernelGetSystemTimeLow(){return nowUs;}
inline int scePowerRequestColdReset(int){++resets;return 0;}
// ColdReset reports success but does NOT reset while HOME holds its power lock.
// Only starting OS exit is a successful escape in this harness.
inline void sceKernelExitGame(){++exits;if(!returnFromExit)throw XmbExit{};}
extern "C" inline void th08_me_core_request_stop(){}
inline int sceKernelReferThreadStatus(int,SceKernelThreadInfo*){return 0;}
inline int sceKernelGetThreadStackFreeSize(int){return 12000;}
inline int sceKernelCreateCallback(const char*,int(*)(int,int,void*),void*){return 1;}
inline int sceKernelRegisterExitCallback(int){return 0;}
inline int scePowerRegisterCallback(int,int){return 0;}
inline int sceKernelSleepThreadCB(){throw Idle{};}
inline int scePowerTick(int){return 0;}
inline int scePowerSetClockFrequency(int,int,int){return 0;}
inline void pspSdkDisableFPUExceptions(){}
inline int sceCtrlSetSamplingCycle(int){return 0;}
inline int sceCtrlSetSamplingMode(int){return 0;}
inline int sceCtrlPeekBufferPositive(SceCtrlData*,int){return 0;}
inline unsigned sceKernelTotalFreeMemSize(){return 0;}
inline unsigned sceKernelMaxFreeMemSize(){return 0;}
inline void *sceGeEdramGetAddr(){return nullptr;}
inline unsigned sceGeEdramGetSize(){return 0;}
inline unsigned sceKernelDevkitVersion(){return 0;}
inline int scePowerGetCpuClockFrequencyInt(){return 333;}
inline int scePowerGetBusClockFrequencyInt(){return 166;}
namespace th08::psp {
inline void BootLog(const char*,...){++logs;if(blockLog)throw Blocked{};}
inline bool FlushBootLogHard(){return true;}
}
'''
PLATFORM_TEST = r'''
#include "sdk.hpp"
#include "platform.cpp"
#include <cstdio>
int main(){
 using namespace th08::psp;
 PlatformInitialize();
 assert(threads.size()==3 && threads[0].name=="th08_exit_watchdog" && threads[0].started);
 assert(threads[1].name=="th08_psp_callbacks" && threads[1].stack>=16384);
 try {ExitWatchdogThread(0,nullptr);}catch(const Idle&){}
 assert(resets==0&&exits==0); // running for >4s is NOT an exit request
 // Freeze logging/main after HOME: arming must already be complete.
 nowUs=0xffff0000U;delays=0;blockLog=true;
 try {ExitCallback(1,0,nullptr);assert(false);}catch(const Blocked&){}
 assert(!PlatformRunning() && gExitWatchdogState.load()==2);
 unsigned armed=gExitArmedAt;
 nowUs+=2000000U;PlatformArmExitWatchdog();assert(gExitArmedAt==armed);
 unsigned oldLogs=logs;
 try {ExitWatchdogThread(0,nullptr);assert(false);}catch(const XmbExit&){}
 assert(resets==0 && exits==1 && unsigned(nowUs-armed)>=4000000U && unsigned(nowUs-armed)<4050000U);
 assert(gExitAttemptState.load()==1 && gExitAttempts.load()==1);
 assert(logs==oldLogs); // watchdog never depends on blocked logging
 // An unexpected return must not kill the watcher or create a busy loop.
 returnFromExit=true;delays=0;
 try {ExitWatchdogThread(0,nullptr);assert(false);}catch(const Idle&){}
 assert(exits>1 && exits<20 && gExitAttemptState.load()==2);
 assert(gExitReturnAt.load()!=0 && logs==oldLogs && resets==0);
 // Programmatic quit is protected before its first log, too.
 gExitWatchdogState.store(0);gRunning=1;
 try {PlatformRequestExit();assert(false);}catch(const Blocked&){}
 assert(!PlatformRunning() && gExitWatchdogState.load()==2);
 blockLog=false;PowerCallback(0,PSP_POWER_CB_SUSPENDING,nullptr);assert(PlatformSuspended());
 PowerCallback(0,PSP_POWER_CB_RESUME_COMPLETE,nullptr);assert(!PlatformSuspended());
 std::puts("platform: stack/headless watchdog/blocked logging/idempotence/time-wrap/power PASS");
}
'''
STREAM_STUB = r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstdio>
constexpr unsigned kPspglStreamArenaLeaseBytes=262144,kPspglStreamArenaHalfBytes=262144;
static int backing;
static bool held=false,pending=false,installed=false;
static unsigned frees=0,fences=0;
int __pspgl_th08_stream_arena_install(void*p,unsigned){installed=p!=nullptr;return 1;}
void __pspgl_th08_stream_arena_begin_frame(unsigned){}
void __pspgl_th08_stream_arena_stats(unsigned long*a,unsigned long*b,unsigned long*c){
 assert(installed);*a=5;*b=0;*c=128;}
namespace th08::psp {
enum class RenderResourceArenaFreeResult {Freed,NotOwned};
void *RenderResourceArenaAllocate(unsigned,unsigned,const char*){assert(!held);held=true;return &backing;}
RenderResourceArenaFreeResult RenderResourceArenaTryFree(void*p){
 assert(p==&backing && held && !pending);held=false;++frees;return RenderResourceArenaFreeResult::Freed;}
void SwapTripleWaitPendingDone(){++fences;pending=false;}
void BootLog(const char*,...){}
}
struct Device {
 void *pspStreamArenaLease=nullptr;
 bool pspStreamArenaDeferred=false,pspStreamArenaFaulted=false;
 unsigned long pspStreamArenaPresent=~0UL,presentCount=0,pspStreamArenaLeaseFrames=0,
 pspStreamArenaAllocationFailures=0,pspStreamArenaReleaseFailures=0,pspStreamArenaAllocs=0,
 pspStreamArenaOverflows=0,pspStreamArenaPeakBytes=0,pspStreamArenaOrphanAddress=0;
'''
STREAM_TEST = r'''
};
int main(){
 Device d;d.BeginPspglStreamArenaFrame();assert(held&&installed&&frees==0);
 pending=true;++d.presentCount;d.ReleasePspglStreamArenaFrame(true,true);
 assert(held&&!installed&&pending&&d.pspStreamArenaDeferred&&frees==0);
 // Calc-phase allocator cannot acquire this live GE block: it is still held.
 unsigned priorFences=fences;
 d.BeginPspglStreamArenaFrame();
 assert(fences==priorFences+1&&frees==1&&held&&installed&&!pending);
 assert(!d.pspStreamArenaDeferred&&d.pspStreamArenaAllocs==5);
 d.BeginPspglStreamArenaFrame();assert(frees==1); // same-frame no-op
 // Synchronous Present releases immediately, accounting this frame once.
 ++d.presentCount;d.ReleasePspglStreamArenaFrame(true,false);
 assert(!held&&!installed&&frees==2&&d.pspStreamArenaAllocs==10);
 d.BeginPspglStreamArenaFrame();pending=true;++d.presentCount;
 d.ReleasePspglStreamArenaFrame(false,true);
 pending=false; // Reset/destructor glFinish contract
 d.ReleasePspglStreamArenaFrame(false);
 assert(!held&&frees==3&&d.pspStreamArenaAllocs==15);
 std::puts("stream: retained through calc/fence before free/stats once/sync/reset PASS");
}
'''

class SafetyTests(unittest.TestCase):
    def run_cpp(self, d, args=()):
        exe=d/'test'
        subprocess.run(['g++','-std=c++17','-O2','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                        '-I',str(d),str(d/'test.cpp'),'-o',str(exe),*args],check=True,capture_output=True,text=True)
        result=subprocess.run([str(exe)],check=True,capture_output=True,text=True)
        print(result.stdout.strip())

    def test_platform(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp)
            (d/'sdk.hpp').write_text(SDK)
            for name in ['pspctrl.h','pspge.h','pspkernel.h','psploadexec.h','psppower.h','pspsdk.h','pspsysmem.h','fileio.hpp']:
                (d/name).write_text('#include "sdk.hpp"\n')
            for name in ['platform.cpp','platform.hpp','me_core.hpp','wait_probe.hpp']:
                (d/name).write_text((ROOT/'psp'/name).read_text())
            (d/'test.cpp').write_text(PLATFORM_TEST)
            self.run_cpp(d)

    def test_stream_lifetime(self):
        source=(ROOT/'src/modern/linux/d3d8_compat.cpp').read_text()
        begin=function_body(source,'    void BeginPspglStreamArenaFrame()')
        release=function_body(source,'    void ReleasePspglStreamArenaFrame(')
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp);(d/'test.cpp').write_text(STREAM_STUB+begin+release+STREAM_TEST)
            self.run_cpp(d,['-DTH08_PSP_PSPGL_STREAM_ARENA_ENABLED=1','-DTH08_PSP_SWAP_TRIPLE_ENABLED=1'])
        present=function_body(source,'    HRESULT Present(')
        self.assertIn('th08::psp::SwapTripleActive()',present)
        self.assertIn('ReleasePspglStreamArenaFrame(',present)

    def test_popup_ownership(self):
        source=(ROOT/'psp/me_popup.cpp').read_text()
        capture=function_body(source,'extern "C" void th08_psp_me_popup_capture(void)')
        self.assertNotIn('gSlotQid',source)
        self.assertNotIn('sceGeListSync',source)
        guard='if (gOnMe && th08_me_core_job_done(&gJob) == 0)'
        self.assertIn(guard,capture)
        self.assertLess(capture.index(guard),capture.index('gSprites[i].widthPx'))
        self.assertLess(capture.index('SwapTripleWaitPendingDone()'),capture.index('gOutSlot ^= 1U'))
        self.assertIn('#if TH08_PSP_SWAP_TRIPLE_ENABLED',capture)
        actual_guard=function_body(capture,guard)
        stub='''
#include <cassert>
bool gLive=false,gOnMe=false,done=false; int gJob=0,gFbBusy=0,writes=0;
int th08_me_core_job_done(int*){return done?1:0;}
void capture(){
'''+actual_guard+'''
++writes;
}
int main(){
 gOnMe=true; capture(); assert(writes==0&&gFbBusy==1);
 gLive=true; capture(); assert(writes==0&&gFbBusy==2);
 gLive=false;done=true;capture();assert(writes==1);
 gOnMe=false;done=false;capture();assert(writes==2);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp);(d/'test.cpp').write_text(stub);self.run_cpp(d)
        make=(ROOT/'Makefile.psp').read_text()
        line=make[:make.index('\t$(SWAP_TRIPLE_CONFIG_STAMP)',make.index('# The triple buffer spans'))]
        self.assertIn('psp/me_popup.o',line.split('# The triple buffer spans')[-1])

if __name__=='__main__':unittest.main()
