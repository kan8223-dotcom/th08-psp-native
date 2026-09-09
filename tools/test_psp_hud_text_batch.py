#!/usr/bin/env python3
"""Execute the actual HUD native builder/run planner against a fake GE arena.

The geometry primitive is stubbed; PPSSPP HUD_STREAM compares real geometry.
This harness exercises ordering, run boundaries and reservation failures that
normal demos do not reliably cover.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        depth += (source[pos] == "{") - (source[pos] == "}")
        if depth == 0:
            return source[start:pos + 1]
    raise ValueError(signature)


PREAMBLE = r'''
#include <cassert>
#include <cstring>
#include <vector>
#include <cstdio>
#define TH08_PSP_HUD_TEXT_STREAM_AUDIT 1
using u8 = unsigned char; using u16 = unsigned short; using u32 = unsigned int;
struct Float2 { float x, y; };
struct Float3 { float x, y, z; };
struct Color { u32 d3dColor; };
struct IDirect3DTexture8 {} textures[2];
struct AnmLoadedSprite { IDirect3DTexture8 *texture; float uv; };
struct AnmVm {
    bool visible = true, flag1 = true, flag17 = false;
    Color color1{}, color2{}; Float3 pos{}; Float2 scale{1,1}, spriteSize{8,16};
    AnmLoadedSprite *loadedSprite = nullptr;
    bool IsVisible() const { return visible; }
};
struct AsciiManagerString {
    char text[64]{}; Float3 position{1,2,0.4f}; u32 color = 0xff123456;
    float scaleX = 1, scaleY = 1; int isSelected = 0, isGui = 0;
};
struct Atlas {
    AnmLoadedSprite sprites[512];
    AnmLoadedSprite *GetSprite(int i) { return i >= 0 && i < 512 ? &sprites[i] : nullptr; }
} atlas;
enum class PspAsciiRenderOwner { Game, FpsCounter };
struct AsciiManager {
    Atlas *asciiAnm = &atlas; AnmVm largeText; AsciiManagerString strings[256];
    int numStrings = 0; float spaceWidth = 8;
    PspAsciiRenderOwner owners[256]{};
};
PspAsciiRenderOwner PspAsciiStringRenderOwner(AsciiManager *am, int i) { return am->owners[i]; }
struct Stats {
    unsigned long hudSkipVm=0,hudSkipIndices=0,hudSkipAlpha=0,hudSkipTexture=0,hudSkipIdentity=0;
    unsigned long hudFallbacks=0,hudStrings=0,hudGlyphs=0,hudSubmits=0,hudMergedStrings=0;
} g_PspAsciiSub;
struct PspHudGlyphVertex { float u,v; u8 r,g,b,a; float x,y,z; };
static_assert(sizeof(PspHudGlyphVertex) == 24);
unsigned int g_PspHudStreamHash=2166136261U, g_PspHudStreamGlyphs=0;
u16 indices[0x600*6];
struct AnmManager {
    bool useMixColor = false; Color color{0x804020ff};
    const u16 *PspBulletUnifiedQuadIndices() { return indices; }
    void FlushVertexBuffer() {}
    void PspMeApplySpriteState(AnmVm *) {}
} manager, *g_AnmManager=&manager;
struct { void *d3dDevice=&manager; } g_Supervisor;
PspHudGlyphVertex arena[0x800*4];
unsigned cursor=0,reserveStart=0,reserveEnd=0,limit=0x800;
bool identity=true,reject=false;
std::vector<PspHudGlyphVertex> submitted;
void th08_psp_bullet_me_reserve_commit(void *) { reserveStart=reserveEnd; }
void *th08_psp_bullet_me_reserve(void *, unsigned quads) {
    if(cursor==reserveEnd && reserveEnd>reserveStart) cursor=reserveStart;
    if(cursor+quads*4>limit*4) return nullptr;
    reserveStart=cursor; cursor+=quads*4; reserveEnd=cursor;
    return arena+reserveStart;
}
int th08_psp_bullet_me_color_identity(void *) { return identity; }
int th08_psp_bullet_me_submit(void *, const void *ptr, unsigned quads, const u16 *) {
    assert(quads>0 && quads<=0x600);
    if(reject) return 0;
    auto p=static_cast<const PspHudGlyphVertex *>(ptr);
    assert(p>=arena && p+quads*4<=arena+cursor);
    submitted.insert(submitted.end(),p,p+quads*4); return 1;
}
void th08_psp_ascii_popup_build_quad(AnmVm *vm,const AnmLoadedSprite *s,float width,float sx,float sy,float *xyz,float *uv) {
    for(int i=0;i<4;i++) {
        xyz[i*3]=vm->pos.x+(i&1)*width*sx;
        xyz[i*3+1]=vm->pos.y+(i/2)*16*sy; xyz[i*3+2]=vm->pos.z;
        uv[i*2]=s->uv+(i&1)*0.01f; uv[i*2+1]=(i/2)*0.01f;
    }
}
'''

TESTS = r'''
void reset() {
    cursor=reserveStart=reserveEnd=0; limit=0x800; reject=false; identity=true;
    submitted.clear(); g_PspAsciiSub={}; g_PspHudStreamHash=2166136261U; g_PspHudStreamGlyphs=0;
    for(int i=0;i<512;i++) atlas.sprites[i]={&textures[0],i/512.0f};
}
void text(AsciiManager &am,int i,const char *s) { std::strcpy(am.strings[i].text,s); }
bool single(AsciiManager &am,int i) {
    auto &s=am.strings[i]; am.largeText.pos=s.position; am.largeText.scale={s.scaleX,s.scaleY};
    return PspHudTextDrawNative(&am,&s,&am.largeText,am.spaceWidth*s.scaleX);
}
int main() {
    reset(); AsciiManager am; am.numStrings=3;
    text(am,0,"12 3\n4"); text(am,1,"Abcd"); text(am,2,"987");
    am.strings[1].isSelected=1; am.strings[1].scaleX=0.75f; am.strings[1].scaleY=1.5f;
    am.strings[2].position={20,30,0.6f}; am.strings[2].color=0x90102030;
    manager.useMixColor=true; am.largeText.flag17=true; am.largeText.color2={0xc0ffffff};
    AsciiManager before=am;
    for(int i=0;i<3;i++) assert(single(am,i));
    auto expected=submitted; auto hash=g_PspHudStreamHash; AnmVm final=am.largeText;
    reset(); am=before;
    assert(PspHudTextDrawRun(&am,0)==3);
    assert(g_PspAsciiSub.hudSubmits==1 && g_PspAsciiSub.hudMergedStrings==2);
    assert(expected.size()==submitted.size());
    assert(std::memcmp(expected.data(),submitted.data(),expected.size()*sizeof(expected[0]))==0);
    assert(hash==g_PspHudStreamHash);
    assert(std::memcmp(&final.pos,&am.largeText.pos,sizeof(final.pos))==0);
    assert(std::memcmp(&final.scale,&am.largeText.scale,sizeof(final.scale))==0);
    assert(final.color1.d3dColor==am.largeText.color1.d3dColor && final.loadedSprite==am.largeText.loadedSprite);

    // Each boundary leaves the third string for its own state/owner path.
    for(int kind=0;kind<5;kind++) {
        reset(); am=before;
        if(kind==0) am.strings[2].isGui=1;
        if(kind==1) am.owners[2]=PspAsciiRenderOwner::FpsCounter;
        if(kind==2) text(am,2," \n ");
        if(kind==3) am.strings[2].color=0;
        if(kind==4) { text(am,2,"Z"); atlas.sprites['Z'+31-' '].texture=&textures[1]; }
        assert(PspHudTextDrawRun(&am,0)==2);
    }
    reset(); am=before; am.largeText.visible=false;
    assert(PspHudTextDrawRun(&am,0)==0 && submitted.empty());
    reset(); am=before; identity=false;
    assert(PspHudTextDrawRun(&am,0)==0 && submitted.empty());
    assert(g_PspAsciiSub.hudStrings==0 && g_PspHudStreamHash==2166136261U);

    // Failed large reservation still permits the old first-string builder.
    reset(); am=before; limit=5;
    assert(PspHudTextDrawRun(&am,0)==0 && submitted.empty());
    assert(single(am,0) && g_PspAsciiSub.hudGlyphs==4);
    reset(); am=before; reject=true;
    assert(PspHudTextDrawRun(&am,0)==0 && submitted.empty());
    assert(g_PspAsciiSub.hudStrings==0 && g_PspHudStreamHash==2166136261U);
    assert(g_PspHudStreamGlyphs==0);
    reject=false; assert(single(am,0));

    // Hard index cap splits only at a whole-string boundary.
    reset(); am=AsciiManager{}; am.numStrings=25;
    for(int i=0;i<25;i++) { std::memset(am.strings[i].text,'a',63); am.strings[i].text[63]=0; }
    assert(PspHudTextDrawRun(&am,0)==24 && g_PspAsciiSub.hudGlyphs==1512);

    // A preceding ME reservation and submitted text must both survive the
    // next reservation's automatic rewind semantics.
    reset(); am=before;
    auto earlier=static_cast<PspHudGlyphVertex *>(th08_psp_bullet_me_reserve(nullptr,8));
    std::memset(earlier,0x5a,8*4*sizeof(*earlier));
    assert(PspHudTextDrawRun(&am,0)==3);
    for(unsigned i=0;i<8*4*sizeof(*earlier);i++) assert(reinterpret_cast<u8 *>(earlier)[i]==0x5a);
    unsigned end=cursor; auto later=th08_psp_bullet_me_reserve(nullptr,1);
    assert(later==arena+end);
    std::puts("HUD batch: packed stream, VM state, 5 boundaries, capacity, reject rollback, arena lifetime PASS");
}
'''


def main():
    source = (ROOT / "src/AsciiManager.cpp").read_text()
    bodies = "\n".join(function(source, signature) for signature in (
        "void PspHudStreamBytes(", "inline unsigned int PspHudMix(",
        "bool PspHudTextDrawNative(", "int PspHudTextDrawRun("))
    with tempfile.TemporaryDirectory(prefix="th08-hud-batch-") as tmp:
        cpp, binary = Path(tmp) / "test.cpp", Path(tmp) / "test"
        cpp.write_text(PREAMBLE + bodies + TESTS)
        subprocess.run(["g++", "-std=c++17", "-O1", "-g", "-fsanitize=address,undefined",
                        "-fno-omit-frame-pointer", str(cpp), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
