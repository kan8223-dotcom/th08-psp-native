#!/usr/bin/env python3
"""Exercise real r232 helpers with sanitizers, plus PSP-only call-site guards."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_psp_trig_df_fastpath import function_body, STUBS, STUB_CPP

ROOT = Path(__file__).resolve().parents[1]

LASER_TEST = r'''
#include "laser_trig_cache.hpp"
#include <cstdio>
#include <random>
#include <limits>
#include <cstdlib>
using namespace th08;
extern void other_tu(float);
static unsigned long checks;
static void check(float a, float x, float y) {
    float s, c;
    psp::LaserSinCos(a, &s, &c);
    if (psp::LaserTrigBits(s) != psp::LaserTrigBits(X87CompatibleSin(a)) ||
        psp::LaserTrigBits(c) != psp::LaserTrigBits(X87CompatibleCos(a))) std::abort();
    Float3 point, got, want;
    point.x=x; point.y=y; point.z=13;
    got.z=71;
    psp::LaserRotate(&got, &point, a);
    Rotate(&want, &point, a);
    if (psp::LaserTrigBits(got.x) != psp::LaserTrigBits(want.x) ||
        psp::LaserTrigBits(got.y) != psp::LaserTrigBits(want.y) || got.z != 71) std::abort();
    ++checks;
}
int main() {
    std::mt19937 rng(23208);
    std::uniform_real_distribution<float> angle(-16,16), coord(-1024,1024);
    for (unsigned i=0; i<200000; ++i) {
        const float a=angle(rng), x=coord(rng), y=coord(rng);
        check(a,x,y); check(a,x,y); check(-a,y,x); check(a,x,y);
    }
    const float edge[]={0.0f,-0.0f,1e-40f,-1e-40f,1e-30f,-1e-30f,65536.0f,-65536.0f,
        3.4e38f,-3.4e38f,std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN()};
    for(float a:edge) for(float x:edge) { check(a,x,0); check(a,0,x); }
    // Deliberately find two finite, unequal keys mapping to one cache entry.
    float first=0.25f, second=0;
    unsigned index=psp::LaserTrigIndex(psp::LaserTrigBits(first));
    for(unsigned i=1; i<100000; ++i) {
        float a=static_cast<float>(i)/4096;
        if(psp::LaserTrigBits(a)!=psp::LaserTrigBits(first) && psp::LaserTrigIndex(psp::LaserTrigBits(a))==index) {
            second=a; break;
        }
    }
    if(second==0) std::abort();
    check(first,10,20); check(second,10,20); check(first,10,20);
    const auto calls=psp::gLaserTrigStats.calls;
    other_tu(first);
    if(psp::gLaserTrigStats.calls!=calls+1) std::abort();
    if(psp::gLaserTrigStats.mismatch || psp::gLaserTrigStats.rotationMismatch) std::abort();
    std::printf("laser checks=%lu hits=%lu misses=%lu mismatch=0 rotation_mismatch=0 shared_TU=PASS\n",
        checks,psp::gLaserTrigStats.hits,psp::gLaserTrigStats.misses);
}
'''

RECOVERY_STUB = r'''
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdarg>
const int ZUN_SUCCESS=0;
struct TitleScreen {};
namespace psp { void BootLog(const char *,...) {} }
struct Memory {
    unsigned freed=0;
    void Free(void *p) { assert(p); ++freed; delete[] static_cast<char *>(p); }
} g_ZunMemory;
struct Anm {
    void *surfaceData[1]={NULL}; int surfaceDataSizes[1]={0};
    void *surfaces[1]={reinterpret_cast<void *>(123)}; void *surfacesBis[1]={NULL};
    unsigned remaining=0, calls=0;
    int LoadSurface(int,const char *) {
        assert(surfaceData[0]==NULL); // retry must re-read, not reuse failed source
        ++calls;
        if(remaining) { --remaining; surfaceData[0]=new char[16]; surfaceDataSizes[0]=16; return 1; }
        return 0;
    }
} anm;
Anm *g_AnmManager=&anm;
'''

RECOVERY_TEST = r'''
int main() {
    TitleScreen a,b;
    assert(PspPrepareReplaySurface(&a)); assert(anm.calls==1);
    anm.remaining=1;
    assert(!PspPrepareReplaySurface(&a)); assert(g_PspReplaySurfaceAttempts==1);
    assert(PspPrepareReplaySurface(&a)); assert(g_PspReplaySurfaceAttempts==0);
    assert(g_ZunMemory.freed==1);
    anm.remaining=100;
    assert(!PspPrepareReplaySurface(&a)); assert(!PspPrepareReplaySurface(&a));
    assert(PspPrepareReplaySurface(&a)); assert(g_PspReplaySurfaceAttempts==0);
    assert(anm.surfaceData[0]==NULL && anm.surfaceDataSizes[0]==0);
    assert(anm.surfaces[0]==reinterpret_cast<void *>(123));
    assert(g_ZunMemory.freed==4);
    // New entry after fallback gets a fresh bounded retry budget.
    assert(!PspPrepareReplaySurface(&a));
    assert(!PspPrepareReplaySurface(&b)); assert(g_PspReplaySurfaceAttempts==1);
    anm.remaining=0; assert(PspPrepareReplaySurface(&b));
    // No old surface must not delete the menu or leak the failed input either.
    anm.surfaces[0]=NULL; anm.remaining=100;
    assert(!PspPrepareReplaySurface(&b)); assert(!PspPrepareReplaySurface(&b));
    assert(PspPrepareReplaySurface(&b)); assert(anm.surfaceData[0]==NULL);
    std::puts("recovery success/one_failure/permanent/reentry/owner_change/no_old_surface PASS");
}
'''

class R232Tests(unittest.TestCase):
    def compile_run(self, directory, sources, args=()):
        exe=directory / "test"
        command=["g++","-std=c++17","-O2","-ffp-contract=off","-fno-omit-frame-pointer",
                 "-fsanitize=address,undefined","-I",str(directory),"-I",str(ROOT/"psp"),"-I",str(ROOT)]
        subprocess.run(command+list(args)+[str(s) for s in sources]+["-o",str(exe)],check=True,capture_output=True,text=True)
        result=subprocess.run([str(exe)],check=True,capture_output=True,text=True)
        print(result.stdout.strip())

    def test_real_laser_helpers(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp)
            for name,content in STUBS.items(): (d/name).write_text(content)
            (d/"ZunMath.hpp").write_text((ROOT/"src/ZunMath.hpp").read_text())
            rotate=function_body((ROOT/"src/Global.cpp").read_text(),"void Rotate(Float3 *outVector")
            (d/"stub.cpp").write_text(STUB_CPP+'\n#include "ZunMath.hpp"\nnamespace th08 {\n'+rotate+'\n}\n')
            (d/"test.cpp").write_text(LASER_TEST)
            (d/"other.cpp").write_text('#include "laser_trig_cache.hpp"\nvoid other_tu(float a) {float s,c; th08::psp::LaserSinCos(a,&s,&c);}\n')
            self.compile_run(d,[d/"test.cpp",d/"stub.cpp",d/"other.cpp",ROOT/"psp/trig_df_fastpath.cpp"],
                ["-DPSP=1","-DTH08_MODERN_PORT=1","-DTH08_PSP_TRIG_DF_FASTPATH=1",
                 "-DTH08_PSP_LASER_TRIG_CACHE=1","-DTH08_PSP_LASER_TRIG_AUDIT=1"])

    def test_real_recovery_helper(self):
        source=(ROOT/"src/TitleScreen.cpp").read_text()
        helper=function_body(source,"bool PspPrepareReplaySurface(TitleScreen *owner)")
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp)
            (d/"test.cpp").write_text(RECOVERY_STUB+'\nTitleScreen *g_PspReplaySurfaceOwner=NULL;\nunsigned g_PspReplaySurfaceAttempts=0;\n'+helper+RECOVERY_TEST)
            self.compile_run(d,[d/"test.cpp"])
        self.assertIn('if (!PspPrepareReplaySurface(this))\n                    return CHAIN_CALLBACK_RESULT_CONTINUE;',source)
        self.assertNotIn('ReleaseSurface(',helper)
        self.assertEqual(source.count('g_PspReplaySurfaceOwner = NULL;'),3)

if __name__=="__main__": unittest.main()
