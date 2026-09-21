#!/usr/bin/env python3
"""Execute native copy/index and Prepare-cache code with explicit host mocks."""
import re
import unittest
from pathlib import Path
from test_native_ge_stream_contract import body
from test_r246_render_text_contract import compile_run

ROOT = Path(__file__).resolve().parents[1]
GE = (ROOT/'psp/native_ge.cpp').read_text()
D3D = (ROOT/'src/modern/linux/d3d8_psp_native.cpp').read_text()
OLD = (ROOT.parent/'artifacts/go-items1-6-20260922/before/src/modern/linux/d3d8_psp_native.cpp').read_text()

class Contracts(unittest.TestCase):
    def test_copy_direct_and_fixed_index_lifetimes(self):
        signatures = ['bool Draw(const State &s, unsigned primitive, const Vertex *vertices, unsigned count,\n'
                      '          const unsigned short *indices, unsigned indexCount)',
                      'void CancelVertices(const Vertex *vertices)',
                      'bool DrawReserved(const State &s, unsigned primitive, const Vertex *vertices, unsigned count,\n'
                      '                  const unsigned short *indices, unsigned indexCount, bool immutableIndices)']
        functions = '\n'.join(s+body(GE,s) for s in signatures)
        for direct in (0, 1):
            compile_run('#define TH08_PSP_NATIVE_VERTEX_DIRECT '+str(direct)+r'''
#include <cassert>
#include <cstring>
#include <cstdint>
#include <stdexcept>
struct Vertex{float u,v;std::uint32_t color;float x,y,z;};struct State{bool through;};
constexpr unsigned kListBytes=4096+120;
Vertex *vertexReservation;unsigned vertexReserved,vertexPage=0,vertexCursor=0;
int vertexFences[2]={-1,-1},openFence=7;unsigned vertexDraws=0,vertexBytes=0;
unsigned allocated=0;alignas(64) unsigned char commands[4096];
const void *drawn=nullptr,*drawnIx=nullptr;
enum{GU_TEXTURE_32BITF=1,GU_COLOR_8888=2,GU_VERTEX_32BITF=4,GU_TRANSFORM_2D=8,GU_TRANSFORM_3D=16,GU_INDEX_16BIT=32};
void Fatal(const char *s){throw std::runtime_error(s);}
void Reserve(unsigned){}void Apply(const State &){}void Kick(){}
void *sceGuGetMemory(unsigned n){auto*p=commands+allocated;allocated+=n;return p;}
void sceKernelDcacheWritebackRange(const void *,unsigned){}
void sceGuDrawArray(unsigned,unsigned,unsigned,const void *ix,const void *v){drawn=v;drawnIx=ix;}
''' + functions + r'''
int main(){State s{true};Vertex v[6]{};unsigned short ix[6]={0,1,2,1,2,3};
 for(bool fixed:{false,true}){
  allocated=0;vertexReservation=v;vertexReserved=6;v[0].color=123;
  assert(DrawReserved(s,3,v,4,ix,6,fixed));assert(!vertexReservation);
  if(TH08_PSP_NATIVE_VERTEX_DIRECT){
   assert(drawn==v);assert(vertexFences[0]==openFence);
   assert((drawnIx==ix)==fixed);assert(allocated==(fixed?0:12));
  }else{
   assert(drawn!=v&&drawnIx!=ix&&allocated==108);
   assert(vertexCursor==0&&vertexFences[0]==-1);
   v[0].color=456;assert(static_cast<const Vertex*>(drawn)[0].color==123);
  }
 }
 vertexReservation=v;vertexReserved=6;
 assert(!DrawReserved(s,7,v,4,ix,6,false));assert(vertexReservation==v);
 CancelVertices(v);assert(!vertexReservation);
 if(!TH08_PSP_NATIVE_VERTEX_DIRECT){
  vertexReservation=v;vertexReserved=6;
  assert(!DrawReserved(s,3,v,6,ix,6,false)); // Draw's command-capacity failure.
  assert(vertexReservation==v);CancelVertices(v);
 }
}
''')

    def test_prepare_cache_matches_old_and_keeps_texture_live(self):
        sigs = ['void InvalidatePrepared(unsigned slot=3)',
                'bool Prepare(ng::State &s,bool through,bool worldFolded)',
                'bool BuildPrepared(ng::State &s,bool through,bool worldFolded)']
        methods = '\n'.join(s+body(D3D,s) for s in sigs)
        oracle = 'bool Oracle(ng::State &s,bool through,bool worldFolded)'+body(OLD,sigs[1])
        names = sorted(set(re.findall(r'\bD3D\w+', methods+oracle)))
        names = [s for s in names if s not in ('D3D',)]
        enum = 'enum {'+','.join(f'{s}={i+1}' for i,s in enumerate(names))+'};'
        # Use the original operation predicate, with its complete signature.
        match = re.search(r'bool TextureOperationUsesTexture\([^)]*\)',D3D)
        predicate = match.group()+body(D3D,match.group())
        extra = sorted((set(re.findall(r'\bD3D\w+',predicate))|{'D3DTOP_MODULATE'})-set(names))
        enum += 'enum {'+','.join(f'{s}={255 if s == "D3DTA_SELECTMASK" else i+80}' for i,s in enumerate(extra))+'};' if extra else ''
        compile_run(r'''
#define TH08_PSP_NATIVE_PREPARE_CACHE 1
#include "psp/native_ge.hpp"
#include <cassert>
#include <cstring>
#include <random>
using DWORD=unsigned;
namespace ng=th08::psp::native;
struct Matrix{float x[16];};
struct Viewport{unsigned X,Y,Width,Height;};
struct Buffer{unsigned width=640,height=480;};
struct Surface{ng::Texture *gpu=nullptr;};
struct Texture{Surface store,*surface=&store;unsigned uploads=0;bool fail=false;
 bool Upload(){++uploads;return !fail;}};
int ScaleFloor(unsigned x,unsigned n,unsigned d){return x*n/d;}
int ScaleCeil(unsigned x,unsigned n,unsigned d){return (x*n+d-1)/d;}
''' + enum + predicate + r'''
struct Device{
 unsigned renderStates[256]{},textureStates[128]{};
 Matrix world{},view{},projection{};Viewport viewport{0,0,640,480};
 Buffer buffer,*backbuffer=&buffer;Texture *texture=nullptr;
 ng::State prepared[3];bool preparedValid[3]{};
''' + methods + oracle + r'''
};
int main(){Device d;Texture t;ng::Texture gpu1{},gpu2{};std::mt19937 rng(248);
 for(unsigned trial=0;trial<400;++trial){
  for(auto &v:d.renderStates)v=rng()%100;
  for(auto &v:d.textureStates)v=rng()%100;
  for(unsigned i=0;i<16;++i){d.world.x[i]=(int(rng()%100)-50)*0.01f;d.view.x[i]=(int(rng()%100)-50)*0.01f;d.projection.x[i]=(int(rng()%100)-50)*0.01f;}
  d.viewport={unsigned(rng()%100),unsigned(rng()%100),320,240};d.InvalidatePrepared();
  for(bool through:{false,true})for(bool folded:{false,true}){
   ng::State a,b;assert(d.Oracle(a,through,folded));assert(d.Prepare(b,through,folded));assert(!memcmp(&a,&b,sizeof(a)));
   b.screenSpace=true;b.fog=999;b.projection[0]=999;
   assert(d.Prepare(b,through,folded));assert(!memcmp(&a,&b,sizeof(a)));
  }
 }
 d.texture=&t;t.store.gpu=&gpu1;
 d.textureStates[D3DTSS_COLOROP]=D3DTOP_MODULATE;
 d.textureStates[D3DTSS_COLORARG1]=D3DTA_TEXTURE;
 d.InvalidatePrepared();ng::State a,b;
 assert(d.Prepare(a,true,false)&&a.texture==&gpu1);
 t.store.gpu=&gpu2;unsigned calls=t.uploads;
 assert(d.Prepare(b,true,false)&&b.texture==&gpu2&&t.uploads==calls+1);
 t.fail=true;assert(!d.Prepare(b,true,false));t.fail=false;
 assert(d.Prepare(b,true,false)&&b.texture==&gpu2);
 d.Prepare(a,false,true);d.Prepare(b,false,false);
 d.world.x[0]+=4;d.InvalidatePrepared(2);
 assert(d.preparedValid[0]&&d.preparedValid[1]&&!d.preparedValid[2]);
 d.Prepare(a,false,false);d.Oracle(b,false,false);assert(!memcmp(&a,&b,sizeof(a)));
 d.texture=nullptr;d.InvalidatePrepared();assert(d.Prepare(a,true,false)&&!a.texture);
}
''')

if __name__ == '__main__':
    unittest.main()
