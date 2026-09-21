#!/usr/bin/env python3
"""Execute actual old/new area filters and native band publisher with mocks."""
from pathlib import Path
import unittest
from test_r246_render_text_contract import compile_run
from test_native_ge_stream_contract import body

ROOT = Path(__file__).resolve().parents[1]
OLD = (ROOT.parent / 'artifacts/go-musicroom-20260922/before/src/modern/linux/d3dx8_compat.cpp').read_text()
NEW = (ROOT / 'src/modern/linux/d3dx8_compat.cpp').read_text()
D3D = (ROOT / 'src/modern/linux/d3d8_psp_native.cpp').read_text()


class Contracts(unittest.TestCase):
    def test_actual_old_new_area_filter_pixels(self):
        signatures = ['UINT BytesPerPixel(D3DFORMAT format)',
                      'void DecodePixel(const BYTE *source, D3DFORMAT format, BYTE *rgba)',
                      'void EncodePixel(BYTE *destination, D3DFORMAT format, const BYTE *rgba)',
                      'RECT FullRect(UINT width, UINT height)', 'bool ValidRect(const RECT &rect)']
        support = '\n'.join(s + body(OLD, s) for s in signatures)
        signature = ('HRESULT CopyMemoryAreaAverage(IDirect3DSurface8 *destinationRaw, '
                     'const RECT *destinationRectRaw, const LinuxSurfaceAccess &source, '
                     'const RECT *sourceRectRaw, D3DCOLOR colorKey)')
        functions = signature.replace('CopyMemoryAreaAverage', 'Old') + body(OLD, 'HRESULT CopyMemoryAreaAverage(')
        functions += signature.replace('CopyMemoryAreaAverage', 'New') + body(NEW, 'HRESULT CopyMemoryAreaAverage(')
        compile_run(r'''
#include "psp/text_area_average.hpp"
#include <cassert>
#include <vector>
#include <random>
#include <algorithm>
using UINT=unsigned;using BYTE=unsigned char;using WORD=std::uint16_t;
using DWORD=unsigned;using LONG=int;using HRESULT=int;using D3DCOLOR=unsigned;
enum D3DFORMAT{D3DFMT_R8G8B8,D3DFMT_R5G6B5,D3DFMT_X1R5G5B5,D3DFMT_A1R5G5B5,
 D3DFMT_A4R4G4B4,D3DFMT_X8R8G8B8,D3DFMT_A8R8G8B8};
constexpr int S_OK=0,E_INVALIDARG=-1;
struct RECT{LONG left,top,right,bottom;};
struct LinuxSurfaceAccess{BYTE*pixels;UINT width,height,pitch;D3DFORMAT format;};
struct IDirect3DSurface8{LinuxSurfaceAccess view;};
bool th08_linux_surface_access(IDirect3DSurface8*s,LinuxSurfaceAccess*out,bool){*out=s->view;return true;}
unsigned full=0,bands=0;
void th08_linux_surface_changed(IDirect3DSurface8*){++full;}
bool th08_native_surface_changed_rect(IDirect3DSurface8*,const RECT&){++bands;return true;}
#define TH08_NOTE_SURFACE_OP(x) ((void)0)
#define TH08_PSP_NATIVE_GE 1
''' + support + functions + r'''
int main(){std::mt19937 rng(247);
 for(unsigned trial=0;trial<1100;++trial){
  const auto sf=trial<300?D3DFMT_A1R5G5B5:static_cast<D3DFORMAT>(rng()%7);
  const auto df=trial<300?(trial%2?D3DFMT_A1R5G5B5:D3DFMT_A4R4G4B4):static_cast<D3DFORMAT>(rng()%7);
  unsigned dw=trial<100?512:1+rng()%128,dh=trial<100?15:1+rng()%30;
  unsigned sw=trial<300?dw*2:1+rng()%260,sh=trial<100?38:1+rng()%64;
  const unsigned sp=(sw+4)*BytesPerPixel(sf)+6,dp=(dw+6)*BytesPerPixel(df)+10;
  std::vector<BYTE> source(sp*(sh+4)),a(dp*(dh+5)),b;
  for(auto&v:source)v=rng();
  for(auto&v:a)v=rng();
  b=a;
  LinuxSurfaceAccess src{source.data(),sw+4,sh+4,sp,sf};
  IDirect3DSurface8 before{{a.data(),dw+6,dh+5,dp,df}},after{{b.data(),dw+6,dh+5,dp,df}};
  RECT sr{2,1,static_cast<int>(sw+2),static_cast<int>(sh+1)};
  RECT dr{3,2,static_cast<int>(dw+3),static_cast<int>(dh+2)};
  // Also exercise destination clipping and unknown ratios/formats/color keys.
  if(trial%13==0)dr.right+=7;
  if(trial%17==0)dr.bottom+=7;
  unsigned key=trial>=300&&trial%2?0x123456:0;
  assert(Old(&before,&dr,src,&sr,key)==New(&after,&dr,src,&sr,key));assert(a==b);
 }
 assert(full==1100&&bands==1100);
 BYTE src[32]={},dst[16];std::fill(dst,dst+16,99);
 assert(!th08::psp::AverageText5551_2x(src,8,7,dst,4,2,2));
 assert(std::all_of(dst,dst+16,[](BYTE v){return v==99;}));
}
''')

    def test_actual_native_publisher_fences_and_neighbors(self):
        function = 'bool th08_native_surface_changed_rect(IDirect3DSurface8 *raw, const RECT &r)' + body(D3D, 'bool th08_native_surface_changed_rect(')
        compile_run(r'''
#include <cassert>
#include <cstring>
#include <vector>
using BYTE=unsigned char;using LONG=int;
struct RECT{int left,top,right,bottom;};struct IDirect3DSurface8{};
bool flushed=false,prepared=false,allow=true;unsigned bytes=0,first=0,end=0;
namespace ng{
struct Texture{unsigned format=4;std::vector<BYTE> pixels=std::vector<BYTE>(1024,17);};
bool PrepareTextureWrite(Texture*){assert(flushed);return prepared=allow;}
void*TexturePixel(Texture*t,unsigned x,unsigned y){assert(prepared);return t->pixels.data()+y*64+x*4;}
void TextureRowsChanged(Texture*,unsigned a,unsigned b){assert(prepared);first=a;end=b;}
}
struct LinuxTexture{bool discardCpuCopy=false;};
struct LinuxSurface:IDirect3DSurface8{bool backbuffer=false,dirty=false;LinuxTexture*owner=nullptr;
 unsigned width=16,height=16,pitch=64,format=4;ng::Texture*gpu=nullptr;
 std::vector<BYTE> pixels=std::vector<BYTE>(1024,42);};
void Flush(){flushed=true;}unsigned BytesPerPixel(unsigned){return 4;}
void DecodePixel(const BYTE*p,unsigned,BYTE*out){memcpy(out,p,4);}
void Pack(void*p,unsigned,const BYTE*in){assert(prepared);memcpy(p,in,4);}
namespace th08::psp{void RenderPerfNoteActualUpload(unsigned n){bytes+=n;}}
''' + function + r'''
int main(){LinuxSurface s;LinuxTexture t;ng::Texture g;s.owner=&t;s.gpu=&g;
 RECT r{2,7,14,10};assert(th08_native_surface_changed_rect(&s,r));
 assert(!s.dirty&&first==7&&end==10&&bytes==144);
 for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)for(unsigned c=0;c<4;++c)
  assert(g.pixels[y*64+x*4+c]==(y>=7&&y<10&&x>=2&&x<14?42:17));
 auto old=g.pixels;allow=false;assert(!th08_native_surface_changed_rect(&s,r)&&g.pixels==old);
 allow=true;s.dirty=true;assert(!th08_native_surface_changed_rect(&s,r));s.dirty=false;
 t.discardCpuCopy=true;assert(!th08_native_surface_changed_rect(&s,r));t.discardCpuCopy=false;
 s.backbuffer=true;assert(!th08_native_surface_changed_rect(&s,r));s.backbuffer=false;
 s.owner=nullptr;assert(!th08_native_surface_changed_rect(&s,r));s.owner=&t;
 s.gpu=nullptr;assert(!th08_native_surface_changed_rect(&s,r));s.gpu=&g;
 RECT invalid{-1,0,16,1};assert(!th08_native_surface_changed_rect(&s,invalid));
 assert(g.pixels==old);
}
''')


if __name__ == '__main__':
    unittest.main()
