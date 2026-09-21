#!/usr/bin/env python3
"""Host tests of actual reservation functions and the lossless text codec.

These test ownership/byte contracts, not PSP cache hardware or performance.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_native_ge_stream_contract import body

ROOT = Path(__file__).resolve().parents[1]
GE = (ROOT / "psp/native_ge.cpp").read_text()
CACHE = (ROOT / "psp/dialogue_text_cache.cpp").read_text()
D3D = (ROOT / "src/modern/linux/d3d8_psp_native.cpp").read_text()


def compile_run(source):
    with tempfile.TemporaryDirectory(prefix="th08-r246-contract-") as tmp:
        path = Path(tmp)
        (path / "test.cpp").write_text(source)
        subprocess.run(["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT), str(path / "test.cpp"), "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)


class Contracts(unittest.TestCase):
    def test_actual_vertex_reservation_functions(self):
        functions = "\n".join(signature + body(GE, signature) for signature in (
            "Vertex *ReserveVertices(unsigned capacity)",
            "void CancelVertices(const Vertex *vertices)",
            "bool DrawReserved(const State &s, unsigned primitive, const Vertex *vertices, unsigned count,\n"
            "                  const unsigned short *indices, unsigned indexCount, bool immutableIndices)"))
        compile_run(r'''
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <cstdint>
struct Vertex { float u,v; std::uint32_t color; float x,y,z; };
struct State { bool through; };
#define TH08_PSP_NATIVE_VERTEX_DIRECT 1
bool DrawReserved(const State &,unsigned,const Vertex *,unsigned,const unsigned short *,unsigned,bool=false);
constexpr unsigned kVertexPages=2,kVerticesPerPage=16;
alignas(64) Vertex vertexPages[2][16];
int vertexFences[2]={-1,-1};
unsigned vertexPage=0,vertexCursor=0,vertexReserved=0;
Vertex *vertexReservation=nullptr;
unsigned long vertexPageTurns=0,vertexDraws=0,vertexBytes=0;
bool listOpen=true,rollList=false;
int openFence=-1,nextFence=1,closed=0,waits=0;
const void *written=nullptr,*drawn=nullptr;
enum {GU_TEXTURE_32BITF=1,GU_COLOR_8888=2,GU_VERTEX_32BITF=4,
      GU_TRANSFORM_2D=8,GU_TRANSFORM_3D=16,GU_INDEX_16BIT=32};
void Fatal(const char *s) {throw std::runtime_error(s);}
void Submit(){assert(listOpen);closed=openFence;openFence=-1;listOpen=false;}
void Wait(int id){if(id<0)return;assert(!listOpen||id!=openFence);assert(id<=closed);++waits;}
void Reserve(unsigned){if(rollList&&listOpen){Submit();rollList=false;}listOpen=true;}
void Apply(const State &){}
void *sceGuGetMemory(unsigned n){static unsigned char indices[1024];assert(n<=sizeof(indices));return indices;}
void sceKernelDcacheWritebackRange(const void *p,unsigned){written=p;}
void sceGuDrawArray(unsigned,unsigned,unsigned,const void *,const void *p){assert(written==p);drawn=p;}
void Kick(){assert(listOpen);if(openFence<0)openFence=nextFence++;}
''' + functions + r'''
void commit(Vertex *p,unsigned count){State s{true};unsigned short ix[3]={0,1,2};
 assert(DrawReserved(s,3,p,count,ix,3));assert(drawn==p);assert(!vertexReservation);}
int main(){
 assert(!ReserveVertices(0));assert(!ReserveVertices(17));
 auto *p=ReserveVertices(8);assert(p);assert(!ReserveVertices(1));p[0].x=42;
 // Drain retires fences but must preserve a pending, unsubmitted reservation.
 Submit();for(auto &f:vertexFences)f=-1;
 assert(vertexReservation==p&&p[0].x==42);commit(p,8);
 // Both pages referenced by one open stream; reuse closes it before waiting.
 p=ReserveVertices(12);assert(vertexPage==1);commit(p,8);
 const int old=openFence;p=ReserveVertices(12);assert(vertexPage==0);
 assert(closed==old&&waits==1);CancelVertices(p);
 p=ReserveVertices(12);assert(p);rollList=true;
 // Index Reserve can switch command lists after vertex reservation.
 listOpen=true;Kick();const int previous=openFence;commit(p,4);
 assert(vertexFences[vertexPage]==openFence&&openFence!=previous);
 assert(vertexCursor%8==0);
 for(unsigned n=0;n<10000;++n){p=ReserveVertices(8);assert(p);p[0].color=n;
  if(n%7==0){CancelVertices(p);p=ReserveVertices(8);assert(p);}
  commit(p,4);}
 assert(vertexDraws==10003&&vertexPageTurns>1000);
 State s{};assert(!DrawReserved(s,3,nullptr,1,nullptr,0));
}
''')

    def test_lossless_codec_and_malformed_streams(self):
        compile_run(r'''
#include "psp/text_row_codec.hpp"
#include <array>
#include <cassert>
#include <random>
int main(){std::mt19937 rng(246);
 std::array<unsigned char,65536> input{},output{};
 std::array<unsigned char,131072> encoded{};
 for(unsigned trial=0;trial<500;++trial){unsigned n=1+rng()%input.size();
  for(unsigned i=0;i<n;++i)input[i]=(rng()%5)?0:static_cast<unsigned char>(rng());
  auto bytes=th08::psp::EncodeTextRow(input.data(),n,encoded.data(),encoded.size());
  assert(bytes&&th08::psp::DecodeTextRow(encoded.data(),bytes,output.data(),n));
  assert(!std::memcmp(input.data(),output.data(),n));
  assert(!th08::psp::DecodeTextRow(encoded.data(),bytes-1,output.data(),n));
  assert(!th08::psp::EncodeTextRow(input.data(),n,encoded.data(),0));
 }
 unsigned char overflow[]={255},truncated[]={127,1};
 assert(!th08::psp::DecodeTextRow(overflow,1,output.data(),1));
 assert(!th08::psp::DecodeTextRow(truncated,2,output.data(),128));
}
''')

    def test_no_probe_simulation_or_input_rewrite(self):
        for forbidden in ("SetAndExecuteScriptIdx", "g_Rng", "g_GuiMessageInput", "RunMsg("):
            self.assertNotIn(forbidden, CACHE)
        gm = (ROOT / "src/GameManager.cpp").read_text()
        self.assertLess(gm.index("psp::PrewarmDialogueText()"), gm.index("g_Supervisor.PlayMusic("))
        self.assertIn("requests == covered", CACHE)
        self.assertIn("if (!ready) { std::free(arena); arena = nullptr; }", CACHE)
        self.assertIn("th08_psp_gdi_text_trim_cache()", CACHE)

    def test_normal_vertices_have_one_final_storage(self):
        self.assertNotIn("ng::Vertex converted[", D3D)
        self.assertNotIn("ng::Vertex quadVertices[", D3D)
        submit = body(D3D, "bool Submit(")
        self.assertIn("ng::DrawReserved", submit)
        reserved = body(GE, "bool DrawReserved(")
        self.assertNotIn("memcpy(v,", reserved)
        self.assertIn("std::memcpy(copy, indices", reserved)
        self.assertLess(reserved.index("Kick()"), reserved.index("vertexFences[vertexPage] = openFence"))
        self.assertNotIn("vertexReservation =", body(GE, "void Drain()"))

    def test_cache_updates_preserve_other_atlas_rows(self):
        upload = body(D3D, "bool th08_native_upload_text_row(")
        self.assertIn("s->EnsurePixels()", upload)
        self.assertIn("s->gpu||s->dirty", upload)
        self.assertIn("ng::TextureRowsChanged(s->gpu,r.top,r.bottom)", upload)
        self.assertIn("Pack(ng::TexturePixel", upload)
        self.assertLess(upload.index("ng::PrepareTextureWrite"), upload.index("Pack(ng::TexturePixel"))

    def test_actual_text_band_upload(self):
        function = "bool th08_native_upload_text_row(IDirect3DTexture8 *raw, const RECT &r, const void *pixels,\n" \
                   "unsigned pitch, D3DFORMAT format)" + body(D3D, "bool th08_native_upload_text_row(")
        compile_run(r'''
#include <cassert>
#include <cstring>
#include <vector>
using BYTE=unsigned char;using LONG=int;using D3DFORMAT=unsigned;
struct RECT{int left,top,right,bottom;};
struct IDirect3DTexture8{};
bool flushed=false,prepared=false,allow=true;
unsigned uploads=0,noted=0,rowFirst=0,rowEnd=0;
namespace ng {
struct Texture{unsigned format=4;std::vector<BYTE> data=std::vector<BYTE>(16*16*4,17);};
bool PrepareTextureWrite(Texture*){assert(flushed);prepared=allow;return allow;}
void *TexturePixel(Texture*t,unsigned x,unsigned y){assert(prepared);return t->data.data()+y*64+x*4;}
void TextureRowsChanged(Texture*,unsigned first,unsigned end){assert(prepared);rowFirst=first;rowEnd=end;}
}
struct LinuxSurface{bool backbuffer=false,dirty=true;unsigned format=4,width=16,height=16,pitch=64;
 std::vector<BYTE> pixels=std::vector<BYTE>(1024,17);ng::Texture store;ng::Texture*gpu=&store;
 bool EnsurePixels(){assert(flushed);return true;}};
struct LinuxTexture:IDirect3DTexture8 {LinuxSurface*surface;bool discardCpuCopy=false;
 bool Upload(){assert(flushed);++uploads;surface->store.data=surface->pixels;surface->dirty=false;return true;}};
unsigned BytesPerPixel(unsigned){return 4;}
void Flush(){flushed=true;}
void DecodePixel(const BYTE *p,unsigned,BYTE*out){memcpy(out,p,4);}
void Pack(void*p,unsigned,const BYTE*in){assert(prepared);memcpy(p,in,4);}
namespace th08::psp {void RenderPerfNoteActualUpload(unsigned n){noted+=n;}}
''' + function + r'''
int main(){LinuxSurface s;LinuxTexture t;t.surface=&s;
 std::vector<BYTE> row(64*3,42);RECT r{0,7,16,10};
 assert(th08_native_upload_text_row(&t,r,row.data(),64,4));assert(uploads==1);
 auto before=s.pixels;std::fill(row.begin(),row.end(),73);flushed=prepared=false;
 assert(th08_native_upload_text_row(&t,r,row.data(),64,4));
 assert(uploads==1&&rowFirst==7&&rowEnd==10&&noted==192);
 for(unsigned i=0;i<1024;++i){const BYTE expected=i>=448&&i<640?73:17;
   assert(s.pixels[i]==expected&&s.store.data[i]==expected);}
 RECT second{2,12,14,13};flushed=prepared=false;
 assert(th08_native_upload_text_row(&t,second,row.data(),64,4));
 assert(rowFirst==12&&rowEnd==13&&noted==240);
 auto intact=s.pixels;assert(!th08_native_upload_text_row(&t,second,row.data(),47,4));
 assert(s.pixels==intact);allow=false;
 assert(!th08_native_upload_text_row(&t,r,row.data(),64,4)&&s.dirty);
 allow=true;assert(th08_native_upload_text_row(&t,r,row.data(),64,4)&&uploads==2);
 t.discardCpuCopy=true;assert(!th08_native_upload_text_row(&t,r,row.data(),64,4));
}
''')


if __name__ == "__main__":
    unittest.main()
