#include "d3d8_internal.hpp"
#include "psp/native_ge.hpp"
#include "psp/anm_scratch.hpp"
#include "psp/dialogue_text_cache.hpp"
#include "psp/backbuffer_shadow.hpp"
#include "psp/boot_checkpoint.hpp"
#include "psp/fileio.hpp"
#include "psp/perf_attribution.hpp"
#include "psp/render_resource_arena.hpp"
#include "psp/render_perf_telemetry.hpp"
#include "psp/memory_telemetry.hpp"
#include "psp/draw_priority_subprofile.hpp"
#include "psp/usage_meter.hpp"
#include "psp/me_bullet_adopt.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <malloc.h>
#include <new>
#include <vector>

extern "C" int sceKernelDcacheWritebackInvalidateRange(const void *, unsigned int);
extern "C" void sceKernelDcacheWritebackRange(const void *, unsigned int);
namespace ng = th08::psp::native;
namespace
{
class LinuxTexture;
class LinuxDevice;
LinuxDevice *device;
void Flush();
char textureOwner[32];
const char *surfaceOperation = "none";
UINT BytesPerPixel(D3DFORMAT format)
{
    switch (format)
    {
    case D3DFMT_R8G8B8: return 3;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4: return 2;
    default: return 4;
    }
}

void DecodePixel(const BYTE *source, D3DFORMAT format, BYTE *rgba)
{
    WORD pixel;
    switch (format)
    {
    case D3DFMT_R8G8B8:
        rgba[0] = source[2]; rgba[1] = source[1]; rgba[2] = source[0]; rgba[3] = 255; break;
    case D3DFMT_R5G6B5:
        memcpy(&pixel, source, sizeof(pixel));
        rgba[0] = static_cast<BYTE>(((pixel >> 11) & 31) * 255 / 31);
        rgba[1] = static_cast<BYTE>(((pixel >> 5) & 63) * 255 / 63);
        rgba[2] = static_cast<BYTE>((pixel & 31) * 255 / 31); rgba[3] = 255; break;
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
        memcpy(&pixel, source, sizeof(pixel));
        rgba[0] = static_cast<BYTE>(((pixel >> 10) & 31) * 255 / 31);
        rgba[1] = static_cast<BYTE>(((pixel >> 5) & 31) * 255 / 31);
        rgba[2] = static_cast<BYTE>((pixel & 31) * 255 / 31);
        rgba[3] = format == D3DFMT_A1R5G5B5 && !(pixel & 0x8000) ? 0 : 255; break;
    case D3DFMT_A4R4G4B4:
        memcpy(&pixel, source, sizeof(pixel));
        rgba[0] = static_cast<BYTE>(((pixel >> 8) & 15) * 17);
        rgba[1] = static_cast<BYTE>(((pixel >> 4) & 15) * 17);
        rgba[2] = static_cast<BYTE>((pixel & 15) * 17);
        rgba[3] = static_cast<BYTE>(((pixel >> 12) & 15) * 17); break;
    default:
        rgba[0] = source[2]; rgba[1] = source[1]; rgba[2] = source[0];
        rgba[3] = format == D3DFMT_X8R8G8B8 ? 255 : source[3]; break;
    }
}

void EncodePixel(BYTE *destination, D3DFORMAT format, const BYTE *rgba)
{
    WORD pixel;
    switch (format)
    {
    case D3DFMT_R8G8B8:
        destination[0] = rgba[2]; destination[1] = rgba[1]; destination[2] = rgba[0]; break;
    case D3DFMT_R5G6B5:
        pixel = static_cast<WORD>(((rgba[0] * 31 / 255) << 11) |
                                  ((rgba[1] * 63 / 255) << 5) | (rgba[2] * 31 / 255));
        memcpy(destination, &pixel, sizeof(pixel)); break;
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
        pixel = static_cast<WORD>(((format == D3DFMT_X1R5G5B5 || rgba[3] >= 128) ? 0x8000 : 0) |
                                  ((rgba[0] * 31 / 255) << 10) |
                                  ((rgba[1] * 31 / 255) << 5) | (rgba[2] * 31 / 255));
        memcpy(destination, &pixel, sizeof(pixel)); break;
    case D3DFMT_A4R4G4B4:
        pixel = static_cast<WORD>(((rgba[3] >> 4) << 12) | ((rgba[0] >> 4) << 8) |
                                  ((rgba[1] >> 4) << 4) | (rgba[2] >> 4));
        memcpy(destination, &pixel, sizeof(pixel)); break;
    default:
        destination[0] = rgba[2]; destination[1] = rgba[1]; destination[2] = rgba[0];
        destination[3] = format == D3DFMT_X8R8G8B8 ? 255 : rgba[3]; break;
    }
}

void Identity(D3DMATRIX *matrix)
{
    memset(matrix, 0, sizeof(*matrix));
    matrix->_11 = matrix->_22 = matrix->_33 = matrix->_44 = 1.0f;
}


UINT VertexCount(D3DPRIMITIVETYPE type, UINT primitiveCount)
{
    if (type == D3DPT_POINTLIST) return primitiveCount;
    if (type == D3DPT_LINELIST) return primitiveCount * 2;
    if (type == D3DPT_LINESTRIP) return primitiveCount + 1;
    if (type == D3DPT_TRIANGLELIST) return primitiveCount * 3;
    return primitiveCount + 2;
}


bool TextureOperationUsesTexture(DWORD operation, DWORD argument1, DWORD argument2)
{
    if (operation == D3DTOP_DISABLE) return false;
    if (operation == D3DTOP_SELECTARG1)
        return (argument1 & D3DTA_SELECTMASK) == D3DTA_TEXTURE;
    return (argument1 & D3DTA_SELECTMASK) == D3DTA_TEXTURE ||
           (argument2 & D3DTA_SELECTMASK) == D3DTA_TEXTURE;
}


unsigned Rgba(unsigned c)
{ return (c & 0xff00ff00U) | ((c & 255U) << 16) | ((c >> 16) & 255U); }
unsigned PrimitiveMode(D3DPRIMITIVETYPE type)
{
    switch (type) {
    case D3DPT_POINTLIST: return 0; case D3DPT_LINELIST: return 1;
    case D3DPT_LINESTRIP: return 2; case D3DPT_TRIANGLESTRIP: return 4;
    case D3DPT_TRIANGLEFAN: return 5; default: return 3;
    }
}
bool HotOwner(const char *s)
{
    return s && (!strncmp(s,"etama",5) || !strncmp(s,"eff",3) ||
           !strncmp(s,"front",5) || !strncmp(s,"title",5) ||
           (!strncmp(s,"stg",3) && strstr(s,"bg")));
}
void Pack(void *out, unsigned format, const BYTE *c)
{
    unsigned value;
    if (format == 0) value = (c[0] >> 3) | ((c[1] >> 2) << 5) | ((c[2] >> 3) << 11);
    else if (format == 1)
    {
        value = (c[0] >> 3) | ((c[1] >> 3) << 5) | ((c[2] >> 3) << 10) | ((c[3] >> 7) << 15);
        // Preserve accepted PSPGL de4260ad swap_5551's 0x0036 blue mask.
        // It drops this blue bit; fixing that historical color bug is a
        // separate change, not part of removing the rendering wrapper.
        value &= ~0x1000U;
    }
    else if (format == 2) value = (c[0] >> 4) | ((c[1] >> 4) << 4) | ((c[2] >> 4) << 8) | ((c[3] >> 4) << 12);
    else { memcpy(out,c,4); return; }
    const unsigned short packed = value; memcpy(out, &packed, 2);
}
void Unpack(const void *in, unsigned format, BYTE *c)
{
    unsigned short p; memcpy(&p,in,2);
    if (format == 3) { memcpy(c,in,4); return; }
    if (format == 2)
    { c[0]=(p&15)*17; c[1]=((p>>4)&15)*17; c[2]=((p>>8)&15)*17; c[3]=((p>>12)&15)*17; }
    else if (format == 1)
    { c[0]=(p&31)*255/31; c[1]=((p>>5)&31)*255/31; c[2]=((p>>10)&31)*255/31; c[3]=(p&0x8000)?255:0; }
    else
    { c[0]=(p&31)*255/31; c[1]=((p>>5)&63)*255/63; c[2]=((p>>11)&31)*255/31; c[3]=255; }
}
int ScaleFloor(int x,int physical,int logical) { return x*physical/logical; }
int ScaleCeil(int x,int physical,int logical) { return (x*physical+logical-1)/logical; }

class LinuxSurface : public IDirect3DSurface8
{
public:
    LinuxSurface(UINT w, UINT h, D3DFORMAT f, bool back, LinuxTexture *own, bool allocate = true)
        : refs(1), width(w), height(h), pitch(w*BytesPerPixel(f)), format(f),
          backbuffer(back), owner(own), dirty(!back), discard(false), gpu(nullptr),
          contentWidth(0), contentHeight(0), readOnlyLock(false)
    { if (allocate && !own) EnsurePixels(); }
    ~LinuxSurface() { Flush(); ng::DestroyTexture(gpu); }
    ULONG AddRef() { return ++refs; }
    ULONG Release() { ULONG n=--refs; if (!n) delete this; return n; }
    HRESULT GetDesc(D3DSURFACE_DESC *d)
    {
        if (!d) return E_INVALIDARG;
        memset(d,0,sizeof(*d)); d->Format=format; d->Type=D3DRTYPE_SURFACE;
        d->Size=pitch*height; d->Width=width; d->Height=height; return S_OK;
    }
    bool EnsurePixels();
    bool ReadBackbuffer();
    bool BuildCache();
    bool Capture(bool displayed);
    bool Blit(const RECT &, const POINT &);
    void FlushBackbuffer();
    HRESULT LockRect(D3DLOCKED_RECT *lock, const RECT *rect, DWORD flags)
    {
        if (!lock || (rect && (rect->left<0 || rect->top<0 || rect->right>static_cast<LONG>(width) ||
            rect->bottom>static_cast<LONG>(height) || rect->right<=rect->left || rect->bottom<=rect->top))) return E_INVALIDARG;
        Flush();
        if (!EnsurePixels() || (backbuffer && (flags&D3DLOCK_READONLY) && !ReadBackbuffer())) return E_OUTOFMEMORY;
        readOnlyLock=(flags&D3DLOCK_READONLY)!=0;
        lock->Pitch=pitch; lock->pBits=pixels.data()+(rect?rect->top*pitch+rect->left*BytesPerPixel(format):0);
        return S_OK;
    }
    HRESULT UnlockRect()
    {
        if (!readOnlyLock) dirty=true;
        else if (backbuffer && !pixels.empty() && pixels.data()==th08::psp::BackbufferShadowBase())
            std::vector<BYTE>().swap(pixels);
        readOnlyLock=false; return S_OK;
    }
    HRESULT GetDC(HDC *dc)
    { if (!dc) return E_INVALIDARG; *dc=CreateCompatibleDC(nullptr); return *dc?S_OK:E_FAIL; }
    HRESULT ReleaseDC(HDC dc) { return DeleteDC(dc)?S_OK:E_FAIL; }
    ULONG refs;
    UINT width,height,pitch;
    D3DFORMAT format;
    bool backbuffer;
    LinuxTexture *owner;
    bool dirty, discard;
    ng::Texture *gpu;
    unsigned contentWidth,contentHeight;
    bool readOnlyLock;
    std::vector<BYTE> pixels;
};

class LinuxTexture : public IDirect3DTexture8
{
public:
    LinuxTexture(UINT w,UINT h,D3DFORMAT format)
        : refs(1), priority(0), discardCpuCopy(false), surface(new(std::nothrow) LinuxSurface(w,h,format,false,this,false)) {}
    ~LinuxTexture() { if(surface) { surface->owner=nullptr; surface->Release(); } }
    ULONG AddRef() { return ++refs; }
    ULONG Release() { ULONG n=--refs; if(!n) delete this; return n; }
    DWORD SetPriority(DWORD p) { DWORD old=priority; priority=p; return old; }
    void PreLoad() { Upload(); }
    HRESULT GetLevelDesc(UINT level,D3DSURFACE_DESC *d) { return level||!surface?E_INVALIDARG:surface->GetDesc(d); }
    HRESULT GetSurfaceLevel(UINT level,IDirect3DSurface8 **out)
    { if(level||!out||!surface) return E_INVALIDARG; *out=surface; surface->AddRef(); return S_OK; }
    HRESULT LockRect(UINT level,D3DLOCKED_RECT *lock,const RECT *r,DWORD f)
    { return level||!surface?E_INVALIDARG:surface->LockRect(lock,r,f); }
    HRESULT UnlockRect(UINT level) { return level||!surface?E_INVALIDARG:surface->UnlockRect(); }
    bool Upload();
    bool UploadStatic(const void *,UINT,D3DFORMAT);
    ULONG refs; DWORD priority; bool discardCpuCopy; LinuxSurface *surface;
};
class LinuxVertexBuffer : public IDirect3DVertexBuffer8
{
  public:
    explicit LinuxVertexBuffer(UINT size) : refs(1), bytes(size) {}
    ULONG AddRef() { return ++refs; }
    ULONG Release() { ULONG value = --refs; if (value == 0) delete this; return value; }
    HRESULT Lock(UINT offset, UINT size, BYTE **data, DWORD)
    {
        if (data == NULL || offset > bytes.size()) return E_INVALIDARG;
        if (size == 0) size = static_cast<UINT>(bytes.size() - offset);
        if (offset + size > bytes.size()) return E_INVALIDARG;
        *data = bytes.empty() ? NULL : &bytes[offset]; return S_OK;
    }
    HRESULT Unlock() { return S_OK; }
    ULONG refs;
    std::vector<BYTE> bytes;
};


bool LinuxSurface::EnsurePixels()
{
    if (!pixels.empty()) return true;
    const size_t needed=static_cast<size_t>(pitch)*height;
    if (!width || !height || needed/pitch!=height) return false;
    if (backbuffer)
    {
        if (!th08::psp::BackbufferShadowAvailable() && !th08::psp::RenderResourceArenaCanAllocate(needed)) return false;
    }
    else if (!th08::psp::RenderResourceArenaCanAllocate(needed))
    {
        th08::psp::BootLog("NATIVE_SURFACE allocation_failed op=%s bytes=%lu\n",surfaceOperation,static_cast<unsigned long>(needed));
        return false;
    }
    th08::psp::RenderResourceAllocationScope scope(backbuffer?"backbuffer shadow":"native surface pixels");
    pixels.resize(needed);
    if (gpu)
    {
        if (!ng::PrepareTextureWrite(gpu)) { std::vector<BYTE>().swap(pixels); return false; }
        const unsigned cw=contentWidth?contentWidth:width, ch=contentHeight?contentHeight:height;
        for(UINT y=0;y<height;++y) for(UINT x=0;x<width;++x)
        {
            const unsigned sx=std::min<UINT>(cw-1,((2*x+1)*cw)/(2*width));
            const unsigned sy=std::min<UINT>(ch-1,((2*y+1)*ch)/(2*height));
            const void *source=ng::TexturePixel(gpu,sx,sy);
            BYTE *destination=pixels.data()+y*pitch+x*BytesPerPixel(format);
            const bool same16=(gpu->format==0&&format==D3DFMT_R5G6B5)||
                (gpu->format==1&&(format==D3DFMT_A1R5G5B5||format==D3DFMT_X1R5G5B5))||
                (gpu->format==2&&format==D3DFMT_A4R4G4B4);
            if(same16)
            {
                unsigned short p;memcpy(&p,source,2);
                if(gpu->format==0) p=((p&31)<<11)|(p&0x7e0)|((p>>11)&31);
                else if(gpu->format==1) p=(p&0x83e0)|((p&31)<<10)|((p>>10)&31);
                else p=(p&0xf0f0)|((p&15)<<8)|((p>>8)&15);
                if(format==D3DFMT_X1R5G5B5)p|=0x8000;
                memcpy(destination,&p,2);
            }
            else
            { BYTE rgba[4];Unpack(source,gpu->format,rgba);EncodePixel(destination,format,rgba); }
        }
        ng::ReleaseTextureRead(gpu);
    }
    return true;
}
bool LinuxTexture::UploadStatic(const void *source,UINT sourcePitch,D3DFORMAT sourceFormat)
{
    if (!surface || !source || sourcePitch<surface->width*BytesPerPixel(sourceFormat)) return false;
    Flush();
    unsigned format=3;
    if(surface->format==D3DFMT_R5G6B5) format=0;
    else if(surface->format==D3DFMT_A1R5G5B5 || surface->format==D3DFMT_X1R5G5B5) format=1;
    else if(surface->format==D3DFMT_A4R4G4B4) format=2;
#if defined(TH08_PSP_ANM_TEXTURE_16BIT) && TH08_PSP_ANM_TEXTURE_16BIT
    if (discardCpuCopy && surface->format==D3DFMT_A8R8G8B8 && sourceFormat==D3DFMT_A8R8G8B8 &&
        sourcePitch==surface->width*4)
    {
        bool binary=true;
        for (UINT y=0;y<surface->height&&binary;++y) for(UINT x=0;x<surface->width;++x)
        { BYTE a=static_cast<const BYTE *>(source)[y*sourcePitch+x*4+3]; if(a!=0&&a!=255) {binary=false;break;} }
        format=binary?1:2;
    }
#endif
    ng::Texture *next=surface->gpu;
    const bool replacement=!next || next->format!=format;
    if(replacement)
        next=ng::CreateTexture(surface->width,surface->height,format,discardCpuCopy,
                               discardCpuCopy&&HotOwner(textureOwner),textureOwner[0]?textureOwner:"native texture");
    if(!next) return false;
    if(!replacement && !ng::PrepareTextureWrite(next)) return false;
    for(UINT y=0;y<surface->height;++y) for(UINT x=0;x<surface->width;++x)
    {
        BYTE rgba[4]; DecodePixel(static_cast<const BYTE *>(source)+y*sourcePitch+x*BytesPerPixel(sourceFormat),sourceFormat,rgba);
        Pack(ng::TexturePixel(next,x,y),format,rgba);
    }
    ng::TextureChanged(next);
    if(replacement) { ng::DestroyTexture(surface->gpu); surface->gpu=next; }
    surface->dirty=false; surface->contentWidth=surface->width; surface->contentHeight=surface->height;
    if(discardCpuCopy) std::vector<BYTE>().swap(surface->pixels);
    th08::psp::RenderPerfNoteActualUpload(next->bytes);
    return true;
}
bool LinuxTexture::Upload()
{
    if(!surface) return false;
    if(surface->gpu&&!surface->dirty) return true;
    th08::psp::RenderPerfNoteUploadAttempt();
    if(!surface->pixels.empty()) return UploadStatic(surface->pixels.data(),surface->pitch,surface->format);
    if(!surface->gpu)
    {
        unsigned format=surface->format==D3DFMT_R5G6B5?0:
            surface->format==D3DFMT_A1R5G5B5||surface->format==D3DFMT_X1R5G5B5?1:
            surface->format==D3DFMT_A4R4G4B4?2:3;
        surface->gpu=ng::CreateTexture(surface->width,surface->height,format,false,false,"native empty texture");
        if(!surface->gpu) return false;
        ng::TextureChanged(surface->gpu);
    }
    surface->dirty=false;
    return true;
}

alignas(64) ng::Vertex producerVertices[16384];
alignas(64) unsigned short quadIndices[3072];

class LinuxDevice : public IDirect3DDevice8
{
public:
    LinuxDevice(const D3DPRESENT_PARAMETERS &p)
        : refs(1), backbuffer(nullptr), texture(nullptr), vertexBuffer(nullptr), fvf(0), streamStride(0),
          quadCount(0), quadThrough(false), quadVertices(nullptr), presentCount(0), producerPresent(~0UL),
          reserveCursor(0), reserveStart(0), reserveEnd(0), ready(false), renderFailed(false)
    {
        memset(renderStates,0,sizeof(renderStates)); memset(textureStates,0,sizeof(textureStates));
        InvalidatePrepared();
        Identity(&world); Identity(&view); Identity(&projection); Identity(&textureTransform);
        renderStates[D3DRS_TEXTUREFACTOR]=0xffffffff;
        renderStates[D3DRS_SRCBLEND]=D3DBLEND_SRCALPHA;
        renderStates[D3DRS_DESTBLEND]=D3DBLEND_INVSRCALPHA;
        renderStates[D3DRS_ZWRITEENABLE]=1; renderStates[D3DRS_ZFUNC]=D3DCMP_LESSEQUAL;
        renderStates[D3DRS_ALPHAFUNC]=D3DCMP_ALWAYS;
        textureStates[D3DTSS_COLOROP]=textureStates[D3DTSS_ALPHAOP]=D3DTOP_MODULATE;
        textureStates[D3DTSS_COLORARG1]=textureStates[D3DTSS_ALPHAARG1]=D3DTA_TEXTURE;
        textureStates[D3DTSS_COLORARG2]=textureStates[D3DTSS_ALPHAARG2]=D3DTA_DIFFUSE;
        textureStates[D3DTSS_ADDRESSU]=textureStates[D3DTSS_ADDRESSV]=D3DTADDRESS_WRAP;
        textureStates[D3DTSS_MINFILTER]=textureStates[D3DTSS_MAGFILTER]=D3DTEXF_POINT;
        for(unsigned q=0;q<512;++q)
        {
            const unsigned v=q*4, i=q*6;
            quadIndices[i]=v; quadIndices[i+1]=v+1; quadIndices[i+2]=v+2;
            quadIndices[i+3]=v+1; quadIndices[i+4]=v+2; quadIndices[i+5]=v+3;
        }
#if TH08_PSP_NATIVE_FIXED_INDICES
        sceKernelDcacheWritebackRange(quadIndices,sizeof(quadIndices));
#endif
        sceKernelDcacheWritebackInvalidateRange(producerVertices,sizeof(producerVertices));
        if(!ng::Initialize()) return;
        device=this;
        ready=ResetInternal(p);
    }
    ~LinuxDevice()
    {
        FlushBatch(); ng::Drain();
        if(texture) texture->Release();
        if(vertexBuffer) vertexBuffer->Release();
        if(backbuffer) backbuffer->Release();
        device=nullptr; ng::Shutdown();
    }
    ULONG AddRef() { return ++refs; }
    ULONG Release() { ULONG n=--refs; if(!n) delete this; return n; }
    HRESULT TestCooperativeLevel() { return renderFailed?D3DERR_DEVICENOTRESET:ready?S_OK:E_FAIL; }
    bool ResetInternal(const D3DPRESENT_PARAMETERS &p)
    {
        if(backbuffer) backbuffer->Release();
        const UINT w=p.BackBufferWidth?p.BackBufferWidth:640, h=p.BackBufferHeight?p.BackBufferHeight:480;
        backbuffer=new(std::nothrow) LinuxSurface(w,h,p.BackBufferFormat==D3DFMT_UNKNOWN?D3DFMT_A8R8G8B8:p.BackBufferFormat,true,nullptr,false);
        viewport={0,0,w,h,0.0f,1.0f}; InvalidatePrepared(); return backbuffer!=nullptr;
    }
    HRESULT Reset(D3DPRESENT_PARAMETERS *p)
    {
        if(!p)return E_INVALIDARG;
        FlushBatch();ng::Drain();
        // The caller exits on a failed recovery. Never continue a run after
        // losing accepted deferred geometry, nor spin as a "lost" device.
        if(renderFailed)return E_OUTOFMEMORY;
        ready=ResetInternal(*p);return ready?S_OK:E_FAIL;
    }
    struct UsageMeterVertex
    {
        float x, y, z, rhw;
        DWORD color;
    };
    void UsageMeterQuad(float x0, float y0, float x1, float y1, DWORD color)
    {
        UsageMeterVertex q[4] = {{x0, y0, 0.5f, 1.0f, color}, {x1, y0, 0.5f, 1.0f, color},
                                 {x0, y1, 0.5f, 1.0f, color}, {x1, y1, 0.5f, 1.0f, color}};
        Draw(D3DPT_TRIANGLESTRIP, 2, reinterpret_cast<const BYTE *>(q), sizeof(UsageMeterVertex));
    }
    void UsageMeterPanel(float px, float py, float w, float h, const std::uint8_t *history, unsigned head)
    {
        const DWORD kFrame = 0xff99a8acu, kPanel = 0xff000000u, kGrid = 0xff006600u;
        const DWORD kLine = 0xff00ff21u, kOver = 0xffe63c3cu;
        UsageMeterQuad(px - 1.0f, py - 1.0f, px + w + 1.0f, py + h + 1.0f, kFrame);
        UsageMeterQuad(px, py, px + w, py + h, kPanel);
        UsageMeterVertex grid[6];
        for (int i = 0; i < 3; ++i)
        {
            const float gy = py + h - (h * 25.0f * static_cast<float>(i + 1)) / 100.0f;
            grid[i * 2] = {px, gy, 0.5f, 1.0f, kGrid};
            grid[i * 2 + 1] = {px + w, gy, 0.5f, 1.0f, kGrid};
        }
        Draw(D3DPT_LINELIST, 3, reinterpret_cast<const BYTE *>(grid), sizeof(UsageMeterVertex));
        UsageMeterVertex strip[th08::psp::kUsageMeterHistory];
        for (unsigned i = 0; i < th08::psp::kUsageMeterHistory; ++i)
        {
            const unsigned idx = (head + 1U + i) % th08::psp::kUsageMeterHistory;
            const unsigned pct = history[idx];
            const unsigned clipped = pct > 100U ? 100U : pct;
            strip[i] = {px + (static_cast<float>(i) * w) / static_cast<float>(th08::psp::kUsageMeterHistory - 1U),
                        py + h - (h * static_cast<float>(clipped)) / 100.0f, 0.5f, 1.0f,
                        pct > 100U ? kOver : kLine};
        }
        Draw(D3DPT_LINESTRIP, th08::psp::kUsageMeterHistory - 1U, reinterpret_cast<const BYTE *>(strip),
             sizeof(UsageMeterVertex));
    }
    void DrawUsageMeterOverlay()
    {
        const DWORD savedFvf = fvf;
        IDirect3DTexture8 *const savedTexture = texture;
        const DWORD savedBlend = renderStates[D3DRS_ALPHABLENDENABLE];
        const DWORD savedZ = renderStates[D3DRS_ZENABLE];
        const DWORD savedFog = renderStates[D3DRS_FOGENABLE];
        const DWORD savedAlphaTest = renderStates[D3DRS_ALPHATESTENABLE];
        if (savedTexture != NULL)
            savedTexture->AddRef();
        SetTexture(0, NULL);
        SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        SetRenderState(D3DRS_ZENABLE, FALSE);
        SetRenderState(D3DRS_FOGENABLE, FALSE);
        SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        const D3DVIEWPORT8 savedViewport = viewport;
        const DWORD savedColorOp = textureStates[D3DTSS_COLOROP], savedColorArg2 = textureStates[D3DTSS_COLORARG1];
        const DWORD savedAlphaOp = textureStates[D3DTSS_ALPHAOP], savedAlphaArg2 = textureStates[D3DTSS_ALPHAARG1];
        D3DVIEWPORT8 fullViewport = savedViewport;
        fullViewport.X = 0; fullViewport.Y = 0; fullViewport.Width = 640; fullViewport.Height = 480;
        SetViewport(&fullViewport);
        SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        // Free strip left of the logo (logical 640x480): top = SC, bottom = ME.
        const float x = 436.0f, w = 48.0f, h = 28.0f;
        UsageMeterPanel(x, 296.0f, w, h, th08::psp::UsageMeterScHistory(), th08::psp::UsageMeterHead());
        UsageMeterPanel(x, 332.0f, w, h, th08::psp::UsageMeterMeHistory(), th08::psp::UsageMeterHead());
        SetVertexShader(savedFvf);
        SetTextureStageState(0, D3DTSS_COLOROP, savedColorOp);
        SetTextureStageState(0, D3DTSS_COLORARG1, savedColorArg2);
        SetTextureStageState(0, D3DTSS_ALPHAOP, savedAlphaOp);
        SetTextureStageState(0, D3DTSS_ALPHAARG1, savedAlphaArg2);
        SetViewport(&savedViewport);
        SetRenderState(D3DRS_ALPHABLENDENABLE, savedBlend);
        SetRenderState(D3DRS_ZENABLE, savedZ);
        SetRenderState(D3DRS_FOGENABLE, savedFog);
        SetRenderState(D3DRS_ALPHATESTENABLE, savedAlphaTest);
        SetTexture(0, savedTexture);
        if (savedTexture != NULL)
            savedTexture->Release();
    }
    HRESULT Present(const RECT *,const RECT *,HWND,const RGNDATA *)
    {
        {
#if TH08_PSP_PERF_ATTRIBUTION_ENABLED
            th08::psp::PerfAttributionScope scope(th08::psp::PerfAttributionPhase::PresentPreSwap);
#endif
            FlushBatch();
#if TH08_PSP_USAGE_METER_ENABLED
            DrawUsageMeterOverlay();
#endif
            FlushBatch(); backbuffer->FlushBackbuffer();
            if(renderFailed)return E_FAIL;
            th08::psp::UsageMeterEndFrame(ng::Now());
        }
        {
#if TH08_PSP_PERF_ATTRIBUTION_ENABLED
            th08::psp::PerfAttributionScope scope(th08::psp::PerfAttributionPhase::PresentSwap);
            th08::psp::PerfAttributionWaitContextScope wait(th08::psp::PerfAttributionWaitContext::Swap);
#endif
            ng::Present();
        }
        ++presentCount;
        th08::psp::RenderPerfTelemetryEndFrame();
        th08::psp::MemoryTelemetryAfterPresent();
        // Borrowed producer vertices remain live after an asynchronous
        // Present. Recycle only at the next reservation's completion fence.
        return S_OK;
    }
    HRESULT GetBackBuffer(UINT index,D3DBACKBUFFER_TYPE,IDirect3DSurface8 **out)
    { if(index||!out||!backbuffer) return E_INVALIDARG; *out=backbuffer; backbuffer->AddRef(); return S_OK; }
    HRESULT CreateTexture(UINT w,UINT h,UINT,DWORD,D3DFORMAT f,D3DPOOL,IDirect3DTexture8 **out)
    {
        if(!out||!w||!h||w>512||h>512) return E_INVALIDARG;
        auto *t=new(std::nothrow) LinuxTexture(w,h,f);
        if(!t||!t->surface) { delete t; return E_OUTOFMEMORY; } *out=t; return S_OK;
    }
    HRESULT CreateVertexBuffer(UINT n,DWORD,DWORD,D3DPOOL,IDirect3DVertexBuffer8 **out)
    { if(!out) return E_INVALIDARG; *out=new(std::nothrow) LinuxVertexBuffer(n); return *out?S_OK:E_OUTOFMEMORY; }
    HRESULT CreateSurface(UINT w,UINT h,D3DFORMAT f,IDirect3DSurface8 **out)
    {
        if(!out||!w||!h) return E_INVALIDARG;
        auto *s=new(std::nothrow) LinuxSurface(w,h,f,false,nullptr);
        if(!s||s->pixels.empty()) { delete s; return E_OUTOFMEMORY; } *out=s; return S_OK;
    }
    HRESULT CreateRenderTarget(UINT w,UINT h,D3DFORMAT f,D3DMULTISAMPLE_TYPE,BOOL,IDirect3DSurface8 **out)
    { return CreateSurface(w,h,f,out); }
    HRESULT CreateImageSurface(UINT w,UINT h,D3DFORMAT f,IDirect3DSurface8 **out) { return CreateSurface(w,h,f,out); }
    HRESULT CopyRects(IDirect3DSurface8 *src,const RECT *rects,UINT count,IDirect3DSurface8 *dst,const POINT *points)
    {
        if(!src||!dst) return E_INVALIDARG;
        FlushBatch();
        auto *s=static_cast<LinuxSurface *>(src), *d=static_cast<LinuxSurface *>(dst);
        if(!rects) count=1;
        for(UINT i=0;i<count;++i)
        {
            RECT r=rects?rects[i]:RECT{0,0,static_cast<LONG>(s->width),static_cast<LONG>(s->height)};
            POINT p=points?points[i]:POINT{0,0};
            if(r.left<0||r.top<0||r.right>static_cast<LONG>(s->width)||r.bottom>static_cast<LONG>(s->height)||
               r.left>=r.right||r.top>=r.bottom||p.x<0||p.y<0) return E_INVALIDARG;
            if(d->backbuffer) { if(!s->Blit(r,p)) return E_FAIL; d->dirty=false; continue; }
            if(!s->EnsurePixels()||!d->EnsurePixels()||(s->backbuffer&&!s->ReadBackbuffer())) return E_OUTOFMEMORY;
            const UINT w=std::min<UINT>(r.right-r.left,d->width-std::min<UINT>(p.x,d->width));
            const UINT h=std::min<UINT>(r.bottom-r.top,d->height-std::min<UINT>(p.y,d->height));
            if(s->format==d->format)
            {
                for(UINT y=0;y<h;++y)
                    memmove(d->pixels.data()+(p.y+y)*d->pitch+p.x*BytesPerPixel(d->format),
                            s->pixels.data()+(r.top+y)*s->pitch+r.left*BytesPerPixel(s->format),w*BytesPerPixel(s->format));
            }
            else for(UINT y=0;y<h;++y) for(UINT x=0;x<w;++x)
            {
                BYTE c[4]; DecodePixel(s->pixels.data()+(r.top+y)*s->pitch+(r.left+x)*BytesPerPixel(s->format),s->format,c);
                EncodePixel(d->pixels.data()+(p.y+y)*d->pitch+(p.x+x)*BytesPerPixel(d->format),d->format,c);
            }
            d->dirty=true;
        }
        return S_OK;
    }
    HRESULT BeginScene()
    {
        FlushBatch(); ng::BeginFrame();
        const int w=backbuffer->width,h=backbuffer->height;
        const int bands[4][4]={{0,0,640,16},{0,464,640,480},{0,16,32,464},{416,16,640,464}};
        for(const auto &r:bands)
            ng::Clear(ScaleFloor(r[0],480,w),ScaleFloor(r[1],272,h),
                      ScaleCeil(r[2],480,w),ScaleCeil(r[3],272,h),D3DCLEAR_TARGET,0,1.0f,0);
        return S_OK;
    }
    HRESULT EndScene() { FlushBatch(); return S_OK; }
    HRESULT Clear(DWORD,const D3DRECT *,DWORD flags,D3DCOLOR color,float depth,DWORD stencil)
    {
        FlushBatch();
        ng::Clear(ScaleFloor(viewport.X,480,backbuffer->width),ScaleFloor(viewport.Y,272,backbuffer->height),
                  ScaleCeil(viewport.X+viewport.Width,480,backbuffer->width),
                  ScaleCeil(viewport.Y+viewport.Height,272,backbuffer->height),flags,Rgba(color),depth,stencil);
        return S_OK;
    }
    HRESULT SetTransform(D3DTRANSFORMSTATETYPE type,const D3DMATRIX *m)
    {
        if(!m) return E_INVALIDARG;
        D3DMATRIX *target=type==D3DTS_WORLD?&world:type==D3DTS_VIEW?&view:
            type==D3DTS_PROJECTION?&projection:type==D3DTS_TEXTURE0?&textureTransform:nullptr;
        if(target&&memcmp(target,m,sizeof(*m)))
        {
            if(type==D3DTS_VIEW||type==D3DTS_PROJECTION) FlushBatch();
            *target=*m;
            if(type==D3DTS_WORLD) InvalidatePrepared(2);
            else if(type==D3DTS_VIEW||type==D3DTS_PROJECTION) InvalidatePrepared();
        }
        return S_OK;
    }
    HRESULT SetViewport(const D3DVIEWPORT8 *v)
    { if(!v) return E_INVALIDARG; if(memcmp(v,&viewport,sizeof(*v))) {FlushBatch();viewport=*v;InvalidatePrepared();} return S_OK; }
    HRESULT GetViewport(D3DVIEWPORT8 *v) { if(!v) return E_INVALIDARG; *v=viewport; return S_OK; }
    HRESULT SetRenderState(D3DRENDERSTATETYPE key,DWORD v)
    { if(static_cast<unsigned>(key)<256&&renderStates[key]!=v) {FlushBatch();renderStates[key]=v;if(key!=D3DRS_TEXTUREFACTOR)InvalidatePrepared();} return S_OK; }
    HRESULT SetTexture(DWORD stage,IDirect3DTexture8 *v)
    {
        if(stage||v==texture) return S_OK;
        FlushBatch(); if(v) v->AddRef(); if(texture) texture->Release(); texture=static_cast<LinuxTexture *>(v); InvalidatePrepared(); return S_OK;
    }
    HRESULT SetTextureStageState(DWORD stage,D3DTEXTURESTAGESTATETYPE key,DWORD v)
    { if(!stage&&static_cast<unsigned>(key)<32&&textureStates[key]!=v) {FlushBatch();textureStates[key]=v;InvalidatePrepared();} return S_OK; }
    HRESULT SetVertexShader(DWORD v) { fvf=v; return S_OK; }
    HRESULT SetStreamSource(UINT stream,IDirect3DVertexBuffer8 *v,UINT stride)
    {
        if(stream) return E_INVALIDARG;
        if(v) v->AddRef(); if(vertexBuffer) vertexBuffer->Release();
        vertexBuffer=static_cast<LinuxVertexBuffer *>(v);streamStride=stride;return S_OK;
    }
    HRESULT DrawPrimitive(D3DPRIMITIVETYPE type,UINT start,UINT primitives)
    {
        if(!vertexBuffer||!streamStride) return E_FAIL;
        const size_t offset=static_cast<size_t>(start)*streamStride,bytes=static_cast<size_t>(VertexCount(type,primitives))*streamStride;
        if(offset>vertexBuffer->bytes.size()||bytes>vertexBuffer->bytes.size()-offset) return E_INVALIDARG;
        return Draw(type,primitives,vertexBuffer->bytes.data()+offset,streamStride);
    }
    HRESULT DrawPrimitiveUP(D3DPRIMITIVETYPE type,UINT count,const void *v,UINT stride)
    { return Draw(type,count,static_cast<const BYTE *>(v),stride); }
    HRESULT DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type,UINT minIndex,UINT count,UINT primitives,
                                  const void *ix,D3DFORMAT format,const void *v,UINT stride)
    {
        FlushBatch();
        if(!v||!ix||format!=D3DFMT_INDEX16||!stride||minIndex>8192||count>8192-minIndex) return E_INVALIDARG;
        const UINT indexCount=VertexCount(type,primitives);
        if(indexCount>65535) return E_INVALIDARG;
        const auto *indices=static_cast<const unsigned short *>(ix);
        for(UINT i=0;i<indexCount;++i) if(indices[i]<minIndex||indices[i]>=minIndex+count) return E_INVALIDARG;
        const bool through=(fvf&D3DFVF_XYZRHW)!=0;
        ng::State state;
        if(!Prepare(state,through,false)) return E_OUTOFMEMORY;
        ng::Vertex *converted=ng::ReserveVertices(minIndex+count);
        if(!converted) return E_OUTOFMEMORY;
        for(UINT i=0;i<minIndex+count;++i)
            if(!Convert(static_cast<const BYTE *>(v)+i*stride,stride,converted[i],through,false,false))
            { ng::CancelVertices(converted); return E_INVALIDARG; }
        return Submit(state,PrimitiveMode(type),converted,minIndex+count,indices,indexCount)?S_OK:E_FAIL;
    }
    HRESULT GetDeviceCaps(D3DCAPS8 *caps)
    {
        if (caps == NULL) return E_INVALIDARG;
        memset(caps, 0, sizeof(*caps)); caps->DeviceType = D3DDEVTYPE_HAL;
        caps->Caps2 = D3DCAPS2_CANRENDERWINDOWED;
        caps->PresentationIntervals = D3DPRESENT_INTERVAL_ONE | D3DPRESENT_INTERVAL_IMMEDIATE;
        caps->DevCaps = D3DDEVCAPS_HWTRANSFORMANDLIGHT | D3DDEVCAPS_HWRASTERIZATION |
                        D3DDEVCAPS_TEXTURESYSTEMMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY |
                        D3DDEVCAPS_TLVERTEXSYSTEMMEMORY | D3DDEVCAPS_TLVERTEXVIDEOMEMORY;
        caps->MaxTextureWidth = caps->MaxTextureHeight = 4096;
        caps->MaxTextureBlendStages = 1; caps->MaxSimultaneousTextures = 1;
        caps->MaxPrimitiveCount = 0x100000; caps->MaxStreams = 1; caps->MaxStreamStride = 256;
        caps->TextureOpCaps = D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_MODULATE | D3DTEXOPCAPS_SELECTARG1;
        return S_OK;
    }
    HRESULT ResourceManagerDiscardBytes(DWORD) { return S_OK; }


    D3DCOLOR EffectiveColor(D3DCOLOR diffuse,
                            bool forceDiffuseArg2 = false)
    {
        // Treat the texture operand as white. GE MODULATE supplies the real
        // texture sample after this value becomes the primary vertex color.
        // COLOROP and ALPHAOP are evaluated separately to preserve D3D8's
        // per-component SELECTARG1/MODULATE behavior.
        const DWORD factor = renderStates[D3DRS_TEXTUREFACTOR];
        const DWORD colorArg1 = textureStates[D3DTSS_COLORARG1] & D3DTA_SELECTMASK;
        const DWORD colorArg2 = forceDiffuseArg2
            ? D3DTA_DIFFUSE
            : textureStates[D3DTSS_COLORARG2] & D3DTA_SELECTMASK;
        const DWORD alphaArg1 = textureStates[D3DTSS_ALPHAARG1] & D3DTA_SELECTMASK;
        const DWORD alphaArg2 = forceDiffuseArg2
            ? D3DTA_DIFFUSE
            : textureStates[D3DTSS_ALPHAARG2] & D3DTA_SELECTMASK;
        const DWORD color1 = colorArg1 == D3DTA_TEXTURE ? 0xffffffffu
                             : colorArg1 == D3DTA_TFACTOR ? factor : diffuse;
        const DWORD color2 = colorArg2 == D3DTA_TEXTURE ? 0xffffffffu
                             : colorArg2 == D3DTA_TFACTOR ? factor : diffuse;
        const DWORD alpha1 = alphaArg1 == D3DTA_TEXTURE ? 0xffffffffu
                             : alphaArg1 == D3DTA_TFACTOR ? factor : diffuse;
        const DWORD alpha2 = alphaArg2 == D3DTA_TEXTURE ? 0xffffffffu
                             : alphaArg2 == D3DTA_TFACTOR ? factor : diffuse;

        DWORD rgb;
        if (textureStates[D3DTSS_COLOROP] == D3DTOP_MODULATE)
        {
            const DWORD r = (((color1 >> 16) & 255) * ((color2 >> 16) & 255) + 127) / 255;
            const DWORD g = (((color1 >> 8) & 255) * ((color2 >> 8) & 255) + 127) / 255;
            const DWORD b = ((color1 & 255) * (color2 & 255) + 127) / 255;
            rgb = (r << 16) | (g << 8) | b;
        }
        else
        {
            rgb = color1 & 0x00ffffffu;
        }

        DWORD alpha;
        if (textureStates[D3DTSS_ALPHAOP] == D3DTOP_MODULATE)
        {
            const DWORD a = (((alpha1 >> 24) & 255) * ((alpha2 >> 24) & 255) + 127) / 255;
            alpha = a << 24;
        }
        else
        {
            alpha = alpha1 & 0xff000000u;
        }
        return alpha | rgb;

    }

    void InvalidatePrepared(unsigned slot=3)
    {
#if TH08_PSP_NATIVE_PREPARE_CACHE
        if(slot<3) preparedValid[slot]=false;
        else for(bool &valid:preparedValid) valid=false;
#else
        (void)slot;
#endif
    }
    bool Prepare(ng::State &s,bool through,bool worldFolded)
    {
#if TH08_PSP_NATIVE_PREPARE_CACHE
        const unsigned slot=through?0:worldFolded?1:2;
        if(!preparedValid[slot])
        {
            if(!BuildPrepared(prepared[slot],through,worldFolded))return false;
            preparedValid[slot]=true;
        }
        s=prepared[slot];
#else
        if(!BuildPrepared(s,through,worldFolded))return false;
#endif
        // Mutable texture contents can change without SetTexture. Never
        // cache upload readiness or an allocation's GPU pointer.
        const bool textured=texture&&(TextureOperationUsesTexture(textureStates[D3DTSS_COLOROP],textureStates[D3DTSS_COLORARG1],textureStates[D3DTSS_COLORARG2])||
            TextureOperationUsesTexture(textureStates[D3DTSS_ALPHAOP],textureStates[D3DTSS_ALPHAARG1],textureStates[D3DTSS_ALPHAARG2]));
        if(textured) { if(!texture->Upload()) return false; s.texture=texture->surface->gpu; }
        return true;
    }
    bool BuildPrepared(ng::State &s,bool through,bool worldFolded)
    {
        memset(&s,0,sizeof(s)); s.through=through;
        s.blend=renderStates[D3DRS_ALPHABLENDENABLE]; s.sourceBlend=renderStates[D3DRS_SRCBLEND];
        s.destinationBlend=renderStates[D3DRS_DESTBLEND];
        s.alphaTest=renderStates[D3DRS_ALPHATESTENABLE];s.alphaFunction=renderStates[D3DRS_ALPHAFUNC];
        s.alphaReference=renderStates[D3DRS_ALPHAREF];s.depthTest=renderStates[D3DRS_ZENABLE];
        s.depthFunction=renderStates[D3DRS_ZFUNC];s.depthWrite=renderStates[D3DRS_ZWRITEENABLE];
        s.fog=renderStates[D3DRS_FOGENABLE];s.fogColor=renderStates[D3DRS_FOGCOLOR];
        memcpy(&s.fogStart,&renderStates[D3DRS_FOGSTART],4);memcpy(&s.fogEnd,&renderStates[D3DRS_FOGEND],4);
        s.fog=s.fog&&renderStates[D3DRS_FOGVERTEXMODE]==D3DFOG_LINEAR&&s.fogEnd>s.fogStart;
        s.minLinear=textureStates[D3DTSS_MINFILTER]==D3DTEXF_LINEAR;
        s.magLinear=textureStates[D3DTSS_MAGFILTER]==D3DTEXF_LINEAR;
        s.clampU=textureStates[D3DTSS_ADDRESSU]==D3DTADDRESS_CLAMP;
        s.clampV=textureStates[D3DTSS_ADDRESSV]==D3DTADDRESS_CLAMP;
        s.left=ScaleFloor(viewport.X,480,backbuffer->width);s.top=ScaleFloor(viewport.Y,272,backbuffer->height);
        s.width=ScaleCeil(viewport.X+viewport.Width,480,backbuffer->width)-s.left;
        s.height=ScaleCeil(viewport.Y+viewport.Height,272,backbuffer->height)-s.top;
        if(through)
        { for(unsigned i=0;i<4;++i) s.model[i*5]=s.projection[i*5]=1.0f; }
        else
        {
            if(worldFolded) memcpy(s.model,&view,64);
            else
            {
                const float *w=reinterpret_cast<const float *>(&world), *v=reinterpret_cast<const float *>(&view);
                for(unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c)
                { float value=0;for(unsigned k=0;k<4;++k)value+=w[r*4+k]*v[k*4+c];s.model[r*4+c]=value; }
            }
            memcpy(s.projection,&projection,64);
            for(unsigned r=0;r<4;++r) s.projection[r*4+2]=s.projection[r*4+2]*2.0f-s.projection[r*4+3];
        }
        return true;
    }
    bool Convert(const BYTE *v,UINT stride,ng::Vertex &out,bool through,bool foldWorld,bool forceDiffuse)
    {
        UINT offset=through?16:12;
        if(fvf&D3DFVF_NORMAL) offset+=12;
        if(fvf&D3DFVF_PSIZE) offset+=4;
        const UINT colorOffset=offset;
        if(fvf&D3DFVF_DIFFUSE) offset+=4;
        if(fvf&D3DFVF_SPECULAR) offset+=4;
        const UINT texOffset=offset;
        const bool hasTex=(fvf&D3DFVF_TEXCOUNT_MASK)!=0;
        if(stride<offset+(hasTex?8:0)) return false;
        float pos[3];memcpy(pos,v,12);
        if(through)
        {
            out.x=(pos[0]+0.5f)*480.0f/backbuffer->width;
            out.y=(pos[1]+0.5f)*272.0f/backbuffer->height;
            out.z=(1.0f-2.0f*pos[2])*32767.5f+32767.5f;
        }
        else if(foldWorld)
        {
            const float *w=reinterpret_cast<const float *>(&world);
            out.x=pos[0]*w[0]+pos[1]*w[4]+pos[2]*w[8]+w[12];
            out.y=pos[0]*w[1]+pos[1]*w[5]+pos[2]*w[9]+w[13];
            out.z=pos[0]*w[2]+pos[1]*w[6]+pos[2]*w[10]+w[14];
        }
        else {out.x=pos[0];out.y=pos[1];out.z=pos[2];}
        D3DCOLOR color=0xffffffff; if(fvf&D3DFVF_DIFFUSE) memcpy(&color,v+colorOffset,4);
        out.color=Rgba(EffectiveColor(color,forceDiffuse));out.u=out.v=0;
        if(hasTex)
        {
            float uv[2];memcpy(uv,v+texOffset,8);out.u=uv[0];out.v=uv[1];
            if(!through)
            {
                out.u=uv[0]*textureTransform._11+uv[1]*textureTransform._21+textureTransform._31;
                out.v=uv[0]*textureTransform._12+uv[1]*textureTransform._22+textureTransform._32;
            }
            else if(texture) {out.u*=texture->surface->width;out.v*=texture->surface->height;}
        }
        return true;
    }
    bool Submit(const ng::State &state,unsigned primitive,const ng::Vertex *v,unsigned count,
                const unsigned short *ix=nullptr,unsigned indexCount=0,bool immutableIndices=false)
    {
        const bool ok=ng::DrawReserved(state,primitive,v,count,ix,indexCount,immutableIndices);
        if(!ok) {ng::CancelVertices(v);renderFailed=true;th08::psp::BootLog("NATIVE_GE draw_rejected vertices=%u indices=%u\n",count,indexCount);th08::psp::FlushBootLog();}
        th08::psp::RenderPerfNoteDraw(ix?indexCount:count);
#if TH08_PSP_DRAW_PRIORITY_SUBPROFILE_ENABLED
        th08::psp::DrawPrioritySubprofileNoteDraw(ix?indexCount:count);
#endif
        return ok;
    }
    void FlushBatch()
    {
        if(!quadCount) return;
        const unsigned quads=quadCount;quadCount=0;
        ng::Vertex *vertices=quadVertices;quadVertices=nullptr;
        ng::State s;
        if(!Prepare(s,quadThrough,true))
        { ng::CancelVertices(vertices);renderFailed=true;th08::psp::BootLog("NATIVE_GE quad_prepare_failed quads=%u\n",quads);return; }
        if(!Submit(s,3,vertices,quads*4,quadIndices,quads*6,TH08_PSP_NATIVE_FIXED_INDICES!=0))
        { renderFailed=true;th08::psp::BootLog("NATIVE_GE quad_failed quads=%u\n",quads);th08::psp::FlushBootLog(); }
    }
    HRESULT Draw(D3DPRIMITIVETYPE type,UINT primitives,const BYTE *v,UINT stride)
    {
        if(!v||!stride) return E_INVALIDARG;
        const unsigned count=VertexCount(type,primitives);
        if(!count) return S_OK;
        const bool through=(fvf&D3DFVF_XYZRHW)!=0;
        if(type==D3DPT_TRIANGLESTRIP&&primitives==2)
        {
            if(quadCount&&(quadThrough!=through||quadCount==512)) FlushBatch();
            if(!quadCount)
            {
                quadVertices=ng::ReserveVertices(2048);
                if(!quadVertices) return E_OUTOFMEMORY;
            }
            quadThrough=through;
            for(unsigned i=0;i<4;++i)
                if(!Convert(v+i*stride,stride,quadVertices[quadCount*4+i],through,!through,false))
                {
                    if(!quadCount) {ng::CancelVertices(quadVertices);quadVertices=nullptr;}
                    return E_INVALIDARG;
                }
            ++quadCount; return S_OK;
        }
        FlushBatch();ng::State s;
        if(!Prepare(s,through,false)) return E_OUTOFMEMORY;
        // Canonical Anm staging holds up to 0x18000 vertices. Keep the GE
        // scratch bounded without silently dropping a large fallback batch.
        unsigned start=0;
        while(start<count)
        {
            const unsigned capacity=type==D3DPT_TRIANGLELIST?8190:8192;
            const bool fanContinuation=type==D3DPT_TRIANGLEFAN&&start!=0;
            const unsigned prefix=fanContinuation?2:0;
            const unsigned added=std::min(capacity-prefix,count-start);
            const unsigned batch=added+prefix;
            ng::Vertex *converted=ng::ReserveVertices(batch);
            if(!converted) return E_OUTOFMEMORY;
            for(unsigned i=0;i<batch;++i)
            {
                const unsigned index=fanContinuation?(i==0?0:i==1?start-1:start+i-2):start+i;
                if(!Convert(v+index*stride,stride,converted[i],through,false,false))
                {ng::CancelVertices(converted);return E_INVALIDARG;}
            }
            if(!Submit(s,PrimitiveMode(type),converted,batch)) return E_FAIL;
            start+=added;
            if(start<count)
            {
                if(type==D3DPT_LINESTRIP) --start;
                else if(type==D3DPT_TRIANGLESTRIP) start-=2;
            }
        }
        return S_OK;
    }
    void *ReserveProducer(unsigned quads)
    {
        if(!quads||quads>2048) return nullptr;
        if(producerPresent!=presentCount)
        {
            // Bullet generation runs before BeginScene; waiting there would
            // be too late. HUD reservations share this same ownership fence.
            ng::WaitPresentedFrame();
            reserveCursor=reserveStart=reserveEnd=0;
            producerPresent=presentCount;
        }
        if(reserveCursor==reserveEnd&&reserveEnd>reserveStart) reserveCursor=reserveStart;
        const unsigned count=quads*4;
        if(reserveCursor>16384-count) return nullptr;
        reserveStart=reserveCursor;reserveCursor+=count;reserveEnd=reserveCursor;
        return reinterpret_cast<void *>(reinterpret_cast<unsigned>(producerVertices+reserveStart)|0x40000000U);
    }
    void CommitProducer() { reserveStart=reserveEnd; }
    bool SubmitProducer(const void *raw,unsigned quads,const unsigned short *indices)
    {
        if(!raw||!indices||!quads||quads>2048) return false;
        FlushBatch(); ng::State s;if(!Prepare(s,true,false)) return false;
        // Restore the accepted producer contract: packed 24-byte vertices,
        // already +0.5 pixel centered with z=1-2*D3Dz and normalized UV.
        // GE, not the CPU, applies glOrtho(0,w,h,0,-1,1) and texture scaling.
        s.through=false;s.screenSpace=true;s.fog=0;
        s.projection[0]=2.0f/backbuffer->width;
        s.projection[5]=-2.0f/backbuffer->height;
        s.projection[10]=-1.0f;s.projection[12]=-1.0f;s.projection[13]=1.0f;
        const bool ok=ng::DrawBorrowed(s,static_cast<const ng::Vertex *>(raw),quads*4,indices,quads*6);
        if(ok) CommitProducer();
        else {renderFailed=true;th08::psp::BootLog("NATIVE_GE borrowed_rejected quads=%u\n",quads);th08::psp::FlushBootLog();}
        th08::psp::RenderPerfNoteDraw(quads*6);
#if TH08_PSP_DRAW_PRIORITY_SUBPROFILE_ENABLED
        th08::psp::DrawPrioritySubprofileNoteDraw(quads*6);
#endif
        return ok;
    }
    bool SpritePairs(const void *raw,unsigned count,unsigned stride)
    {
        if(!raw||!texture||count>4096||fvf!=(D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1)||stride<28) return false;
        FlushBatch();ng::State s;if(!Prepare(s,true,false)) return false;
        ng::Vertex *converted=ng::ReserveVertices(count*2);
        if(!converted) return false;
        for(unsigned i=0;i<count*2;++i)
            if(!Convert(static_cast<const BYTE *>(raw)+i*stride,stride,converted[i],true,false,true))
            {ng::CancelVertices(converted);return false;}
        return Submit(s,6,converted,count*2);
    }
    ULONG refs;
    LinuxSurface *backbuffer;
    LinuxTexture *texture;
    LinuxVertexBuffer *vertexBuffer;
    DWORD fvf;UINT streamStride;
    DWORD renderStates[256],textureStates[32];
    D3DMATRIX world,view,projection,textureTransform;
    D3DVIEWPORT8 viewport;
    unsigned quadCount;bool quadThrough;
    ng::Vertex *quadVertices;
    unsigned long presentCount,producerPresent;
    unsigned reserveCursor,reserveStart,reserveEnd;
    bool ready,renderFailed;
#if TH08_PSP_NATIVE_PREPARE_CACHE
    ng::State prepared[3];
    bool preparedValid[3];
#endif
};
void Flush() { if(device) device->FlushBatch(); }

bool LinuxSurface::ReadBackbuffer()
{
    if(!backbuffer||!EnsurePixels()) return false;
    Flush();
    th08::psp::RenderResourceAllocationScope scope("native framebuffer readback");
    auto *raw=static_cast<unsigned short *>(th08::psp::BackbufferShadowNativeBuffer());
    const bool owned=raw==nullptr;
    if(owned)raw=static_cast<unsigned short *>(memalign(64,480*272*2));
    if(!raw) return false;
    const bool ok=ng::ReadFramebuffer(raw,480,false);
    if(ok)
        for(UINT y=0;y<height;++y) for(UINT x=0;x<width;++x)
        {
            const unsigned sx=std::min<UINT>(479,((2*x+1)*480)/(2*width));
            const unsigned sy=std::min<UINT>(271,((2*y+1)*272)/(2*height));
            BYTE c[4];Unpack(raw+sy*480+sx,0,c);
            EncodePixel(pixels.data()+y*pitch+x*BytesPerPixel(format),format,c);
        }
    if(owned)free(raw);
    if(ok)dirty=false;
    return ok;
}
bool LinuxSurface::Capture(bool displayed)
{
    Flush();
    if(!th08::psp::AnmScratchTransitionActive() ||
       !th08::psp::AnmScratchTransitionSetActiveBytes(480*272*2))return false;
    auto *raw=static_cast<unsigned short *>(th08::psp::AnmScratchTransitionBase());
    if(!raw)return false;
    bool ok=ng::ReadFramebuffer(raw,480,displayed);
    ng::Texture *next=ok?ng::CreateTexture(512,512,0,false,false,"native framebuffer capture"):nullptr;
    ok=ok&&next;
    if(ok)
    {
        for(unsigned y=0;y<272;++y) memcpy(ng::TexturePixel(next,0,y),raw+y*480,480*2);
        ng::TextureChanged(next);ng::DestroyTexture(gpu);gpu=next;
        contentWidth=480;contentHeight=272;dirty=false;discard=true;
        std::vector<BYTE>().swap(pixels);
    }
    return ok;
}
bool LinuxSurface::BuildCache()
{
    if(owner) return owner->Upload();
    if(gpu&&!dirty) return true;
    if(!EnsurePixels()) return false;
    const unsigned cw=std::min(512,ScaleCeil(width,480,640));
    const unsigned ch=std::min(512,ScaleCeil(height,272,480));
    if(!cw||!ch) return false;
    const unsigned cacheFormat=backbuffer?3:0;
    ng::Texture *next=ng::CreateTexture(512,512,cacheFormat,false,false,"native surface cache");
    if(!next) return false;
    // Same center-sampled bilinear reduction as the accepted PSP surface path.
    const UINT bytes=BytesPerPixel(format);
    for(unsigned y=0;y<ch;++y)
    {
        float fy=(static_cast<float>(y)+0.5f)*height/ch-0.5f;
        int y0=static_cast<int>(floorf(fy));float ty=fy-y0;
        if(y0<0) {y0=0;ty=0;} unsigned y1=y0+1;if(y1>=height) {y1=height-1;ty=0;}
        for(unsigned x=0;x<cw;++x)
        {
            float fx=(static_cast<float>(x)+0.5f)*width/cw-0.5f;
            int x0=static_cast<int>(floorf(fx));float tx=fx-x0;
            if(x0<0) {x0=0;tx=0;} unsigned x1=x0+1;if(x1>=width) {x1=width-1;tx=0;}
            BYTE p[4][4],c[4];
            DecodePixel(pixels.data()+y0*pitch+x0*bytes,format,p[0]);
            DecodePixel(pixels.data()+y0*pitch+x1*bytes,format,p[1]);
            DecodePixel(pixels.data()+y1*pitch+x0*bytes,format,p[2]);
            DecodePixel(pixels.data()+y1*pitch+x1*bytes,format,p[3]);
            for(unsigned k=0;k<4;++k)
            {
                const float top=p[0][k]+(p[1][k]-p[0][k])*tx;
                const float bottom=p[2][k]+(p[3][k]-p[2][k])*tx;
                c[k]=static_cast<BYTE>(top+(bottom-top)*ty+0.5f);
            }
            Pack(ng::TexturePixel(next,x,y),cacheFormat,c);
        }
    }
    ng::TextureChanged(next);ng::DestroyTexture(gpu);gpu=next;
    contentWidth=cw;contentHeight=ch;dirty=false;
    if(discard&&!backbuffer) std::vector<BYTE>().swap(pixels);
    return true;
}
bool LinuxSurface::Blit(const RECT &r,const POINT &p)
{
    Flush();if(!BuildCache()||!device) return false;
    ng::State s{};s.texture=gpu;s.through=true;s.width=480;s.height=272;
    s.alphaFunction=s.depthFunction=8;s.sourceBlend=5;s.destinationBlend=6;
    s.clampU=s.clampV=s.minLinear=s.magLinear=1;
    for(unsigned i=0;i<4;++i) s.model[i*5]=s.projection[i*5]=1.0f;
    const float halfPixel=backbuffer?0.0f:0.5f;
    const float x0=(p.x+halfPixel)*480.0f/device->backbuffer->width;
    const float y0=(p.y+halfPixel)*272.0f/device->backbuffer->height;
    const float x1=(p.x+r.right-r.left+halfPixel)*480.0f/device->backbuffer->width;
    const float y1=(p.y+r.bottom-r.top+halfPixel)*272.0f/device->backbuffer->height;
    const float u0=r.left*static_cast<float>(contentWidth)/width, v0=r.top*static_cast<float>(contentHeight)/height;
    const float u1=r.right*static_cast<float>(contentWidth)/width,v1=r.bottom*static_cast<float>(contentHeight)/height;
    const ng::Vertex v[4]={{u0,v0,0xffffffff,x0,y0,0},{u1,v0,0xffffffff,x1,y0,0},
                            {u0,v1,0xffffffff,x0,y1,0},{u1,v1,0xffffffff,x1,y1,0}};
    return ng::Draw(s,4,v,4);
}
void LinuxSurface::FlushBackbuffer()
{
    if(!backbuffer||!dirty||pixels.empty()) return;
    const RECT r={0,0,static_cast<LONG>(width),static_cast<LONG>(height)};const POINT p={0,0};
    if(Blit(r,p)) {dirty=false;std::vector<BYTE>().swap(pixels);}
}
class LinuxDirect3D : public IDirect3D8
{
public:
    LinuxDirect3D():refs(1) {}
    ULONG AddRef() { return ++refs; }
    ULONG Release() { ULONG n=--refs;if(!n)delete this;return n; }
    HRESULT GetAdapterDisplayMode(UINT,D3DDISPLAYMODE *m)
    { if(!m)return E_INVALIDARG;*m={640,480,60,D3DFMT_X8R8G8B8};return S_OK; }
    HRESULT CheckDeviceFormat(UINT,D3DDEVTYPE,D3DFORMAT,DWORD,D3DRESOURCETYPE,D3DFORMAT) { return S_OK; }
    HRESULT CreateDevice(UINT,D3DDEVTYPE,HWND window,DWORD,D3DPRESENT_PARAMETERS *p,IDirect3DDevice8 **out)
    {
        if(!window||!p||!out) return E_INVALIDARG;*out=nullptr;
        TH08_PSP_BOOT_CHECKPOINT("native_device","before",0);
        auto *d=new(std::nothrow) LinuxDevice(*p);
        if(!d||!d->ready) {delete d;return E_FAIL;}
        *out=d;TH08_PSP_BOOT_CHECKPOINT("native_device","ready",1);return S_OK;
    }
    ULONG refs;
};
} // namespace

void th08_psp_bullet_direct_ge_set_batch(bool) {}
void th08_psp_item_mixed_ge_set_batch(bool) {}
void th08_psp_item_direct_ge_set_batch(bool) {}
bool th08_psp_item_direct_ge_release_stage(IDirect3DDevice8 *) { ng::Drain(); return true; }
extern "C" void *th08_psp_bullet_me_reserve(void *d,unsigned int quads)
{ return d?static_cast<LinuxDevice *>(static_cast<IDirect3DDevice8 *>(d))->ReserveProducer(quads):nullptr; }
extern "C" void th08_psp_bullet_me_reserve_commit(void *d)
{ if(d)static_cast<LinuxDevice *>(static_cast<IDirect3DDevice8 *>(d))->CommitProducer(); }
extern "C" int th08_psp_bullet_me_submit(void *d,const void *v,unsigned int quads,const unsigned short *indices)
{ return d&&static_cast<LinuxDevice *>(static_cast<IDirect3DDevice8 *>(d))->SubmitProducer(v,quads,indices); }
extern "C" int th08_psp_bullet_me_color_identity(void *d)
{
    if(!d)return 0;auto *dev=static_cast<LinuxDevice *>(static_cast<IDirect3DDevice8 *>(d));
    const D3DCOLOR probes[]={0,0xffffffff,0x80402010,0x7f3f1f0f};
    for(auto p:probes)if(dev->EffectiveColor(p)!=p)return 0;return 1;
}
bool th08_psp_reserve_ascii_popup_sprite_pairs(IDirect3DDevice8 *d,UINT count)
{
    if(!d||!count||count>4096)return false;
    auto *dev=static_cast<LinuxDevice *>(d);
    // Workspace is fixed; upload before the caller mutates its shared VM.
    dev->FlushBatch();ng::State state;return dev->Prepare(state,true,false);
}
bool th08_psp_draw_ascii_popup_sprite_pairs(IDirect3DDevice8 *d,const void *v,UINT count,UINT stride)
{ return d&&static_cast<LinuxDevice *>(d)->SpritePairs(v,count,stride); }

bool th08_linux_surface_access(IDirect3DSurface8 *raw,LinuxSurfaceAccess *out,bool readBackbuffer)
{
    (void)readBackbuffer;
    if(!raw||!out)return false;
    auto *s=static_cast<LinuxSurface *>(raw);Flush();
    // Destination access can be a partial D3DX update. Preserve pixels outside
    // that rectangle just as source access does, rather than starting blank.
    if(!s->EnsurePixels()||(s->backbuffer&&!s->ReadBackbuffer()))return false;
    *out={s->pixels.data(),s->width,s->height,s->pitch,s->format};return true;
}
void th08_linux_surface_access_end(IDirect3DSurface8 *raw)
{
    if(!raw)return;auto *s=static_cast<LinuxSurface *>(raw);
    if(s->backbuffer&&s->pixels.data()==th08::psp::BackbufferShadowBase())
        std::vector<BYTE>().swap(s->pixels);
}
void th08_linux_surface_changed(IDirect3DSurface8 *raw)
{ if(!raw)return;Flush();auto *s=static_cast<LinuxSurface *>(raw);s->dirty=true;s->FlushBackbuffer(); }
bool th08_native_surface_changed_rect(IDirect3DSurface8 *raw, const RECT &r)
{
    if(!raw)return false;
    auto *s=static_cast<LinuxSurface *>(raw);
    // Initial/otherwise dirty uploads still use the complete CPU master.
    // Image surfaces, static/discarded textures and backbuffers are not atlases.
    if(s->backbuffer||!s->owner||s->owner->discardCpuCopy||s->dirty||!s->gpu||
       s->pixels.empty()||r.left<0||r.top<0||r.right<=r.left||r.bottom<=r.top||
       r.right>static_cast<LONG>(s->width)||r.bottom>static_cast<LONG>(s->height))return false;
    Flush();
    if(!ng::PrepareTextureWrite(s->gpu))return false;
    const unsigned bpp=BytesPerPixel(s->format);
    for(LONG y=r.top;y<r.bottom;++y)for(LONG x=r.left;x<r.right;++x)
    {
        // Keep the accepted backend's conversion (including its 5551 blue
        // rounding); do not substitute a raw D3D-to-GE channel swap here.
        BYTE rgba[4];DecodePixel(s->pixels.data()+y*s->pitch+x*bpp,s->format,rgba);
        Pack(ng::TexturePixel(s->gpu,x,y),s->gpu->format,rgba);
    }
    ng::TextureRowsChanged(s->gpu,r.top,r.bottom);
    th08::psp::RenderPerfNoteActualUpload((r.right-r.left)*bpp*(r.bottom-r.top));
    return true;
}
bool th08_native_upload_text_row(IDirect3DTexture8 *raw, const RECT &r, const void *pixels,
                                  unsigned pitch, D3DFORMAT format)
{
    if(!raw||!pixels)return false;
    auto *t=static_cast<LinuxTexture *>(raw);auto *s=t->surface;
    if(!s||s->backbuffer||t->discardCpuCopy||s->format!=format||r.left<0||r.top<0||
       r.right<=r.left||r.bottom<=r.top||r.right>static_cast<LONG>(s->width)||
       r.bottom>static_cast<LONG>(s->height))return false;
    const unsigned bpp=BytesPerPixel(format),bytes=(r.right-r.left)*bpp;
    if(pitch<bytes)return false;
    Flush();if(!s->EnsurePixels())return false;
    for(LONG y=r.top;y<r.bottom;++y)
        memcpy(s->pixels.data()+y*s->pitch+r.left*bpp,
               static_cast<const BYTE *>(pixels)+(y-r.top)*pitch,bytes);
    if(!s->gpu||s->dirty)
    {
        s->dirty=true;
        return t->Upload();
    }
    if(!ng::PrepareTextureWrite(s->gpu)) {s->dirty=true;return false;}
    for(LONG y=r.top;y<r.bottom;++y)for(LONG x=r.left;x<r.right;++x)
    {
        BYTE rgba[4];DecodePixel(s->pixels.data()+y*s->pitch+x*bpp,format,rgba);
        Pack(ng::TexturePixel(s->gpu,x,y),s->gpu->format,rgba);
    }
    ng::TextureRowsChanged(s->gpu,r.top,r.bottom);
    th08::psp::RenderPerfNoteActualUpload(bytes*(r.bottom-r.top));
    return true;
}
void th08_linux_texture_mark_static(IDirect3DTexture8 *raw)
{ if(raw)static_cast<LinuxTexture *>(raw)->discardCpuCopy=true; }
void th08_linux_surface_mark_static(IDirect3DSurface8 *raw)
{ if(raw)static_cast<LinuxSurface *>(raw)->discard=true; }
void th08_linux_surface_discard_readback(IDirect3DSurface8 *raw)
{ if(raw&&static_cast<LinuxSurface *>(raw)->backbuffer)std::vector<BYTE>().swap(static_cast<LinuxSurface *>(raw)->pixels); }
extern "C" void th08_linux_set_texture_upload_owner(const char *name)
{ if(!name)name="";strncpy(textureOwner,name,sizeof(textureOwner)-1);textureOwner[sizeof(textureOwner)-1]=0; }
extern "C" void th08_linux_note_surface_op(const char *name)
{ surfaceOperation=name?name:"none"; }
bool th08_linux_texture_upload_static(IDirect3DTexture8 *raw,const void *source,UINT pitch,D3DFORMAT format)
{
    if(!raw)return false;
    auto *t=static_cast<LinuxTexture *>(raw);t->discardCpuCopy=true;
    const bool ok=t->UploadStatic(source,pitch,format);
    if(ok) th08::psp::BootLog("ANM_TEX_NATIVE owner=%s size=%ux%u d3d=%u ge=%u bytes=%u upper=%u\n",
        textureOwner,t->surface->width,t->surface->height,static_cast<unsigned>(t->surface->format),
        t->surface->gpu->format,t->surface->gpu->bytes,t->surface->gpu->upper);
    return ok;
}
bool th08_linux_capture_direct_to_texture(IDirect3DSurface8 *raw,const RECT *destination,
                                          const LinuxSurfaceAccess &source,const RECT *sourceRect,D3DCOLOR colorKey)
{
    if(!raw||!source.pixels)return false;
    auto *s=static_cast<LinuxSurface *>(raw);
    if(s->backbuffer||!s->owner||!s->pixels.empty()||!s->gpu)return false;
    RECT sr=sourceRect?*sourceRect:RECT{0,0,static_cast<LONG>(source.width),static_cast<LONG>(source.height)};
    RECT dr=destination?*destination:RECT{0,0,static_cast<LONG>(s->width),static_cast<LONG>(s->height)};
    if(sr.left<0||sr.top<0||sr.right>static_cast<LONG>(source.width)||sr.bottom>static_cast<LONG>(source.height)||
       sr.right<=sr.left||sr.bottom<=sr.top||dr.left<0||dr.top<0||dr.right<=dr.left||dr.bottom<=dr.top)return false;
    dr.right=std::min<LONG>(dr.right,s->width);dr.bottom=std::min<LONG>(dr.bottom,s->height);
    if(dr.right<=dr.left||dr.bottom<=dr.top)return false;
    Flush();if(!ng::PrepareTextureWrite(s->gpu))return false;
    const unsigned dw=dr.right-dr.left,dh=dr.bottom-dr.top,sw=sr.right-sr.left,sh=sr.bottom-sr.top;
    for(unsigned y=0;y<dh;++y)for(unsigned x=0;x<dw;++x)
    {
        const unsigned sx=sr.left+(static_cast<unsigned long long>(x)*sw)/dw;
        const unsigned sy=sr.top+(static_cast<unsigned long long>(y)*sh)/dh;
        BYTE c[4];DecodePixel(source.pixels+sy*source.pitch+sx*BytesPerPixel(source.format),source.format,c);
        if(colorKey&&(colorKey&0xffffffU)==((c[0]<<16)|(c[1]<<8)|c[2]))c[3]=0;
        Pack(ng::TexturePixel(s->gpu,dr.left+x,dr.top+y),s->gpu->format,c);
    }
    ng::TextureChanged(s->gpu);s->dirty=false;return true;
}
bool th08_linux_surface_capture_native(UINT width,UINT height,bool displayed,IDirect3DSurface8 **out)
{
    if(!out||!width||!height)return false;*out=nullptr;
    auto *s=new(std::nothrow) LinuxSurface(width,height,D3DFMT_R5G6B5,false,nullptr,false);
    const bool captured=s&&s->Capture(displayed);
    const bool released=th08::psp::AnmScratchReleaseTransition();
    if(!captured||!released) {delete s;return false;}
    *out=s;th08::psp::BootLog("NATIVE_CAPTURE ready=1 backend=ge displayed=%u\n",displayed);return true;
}
void th08_linux_dialogue_snapshot_restore(IDirect3DDevice8 *)
{
    // This backend's recipe keeps the accepted live-background policy.
#if !defined(TH08_PSP_DIALOGUE_LIVE_BACKGROUND) || !TH08_PSP_DIALOGUE_LIVE_BACKGROUND
#error Native GE currently requires the accepted Go live dialogue background policy
#endif
}
bool th08_linux_begin_framebuffer_probe(IDirect3DDevice8 *,int,int,int,int) { return false; }
bool th08_linux_end_framebuffer_probe(IDirect3DDevice8 *,LinuxFramebufferDeltaStats *) { return false; }
extern "C" IDirect3D8 *Direct3DCreate8(UINT version)
{ return version==D3D_SDK_VERSION?new(std::nothrow) LinuxDirect3D():nullptr; }

bool th08_linux_texture_region_stats(IDirect3DTexture8 *textureRaw, float u0, float v0,
                                     float u1, float v1, D3DCOLOR diffuse,
                                     LinuxTextureRegionStats *stats)
{
    if (textureRaw == NULL || stats == NULL)
        return false;
    LinuxTexture *texture = static_cast<LinuxTexture *>(textureRaw);
    LinuxSurface *surface = texture->surface;
    if (surface == NULL || surface->pixels.empty() || surface->width == 0 || surface->height == 0)
        return false;
    if (u0 != u0 || v0 != v0 || u1 != u1 || v1 != v1 ||
        fabsf(u0) > 16.0f || fabsf(v0) > 16.0f ||
        fabsf(u1) > 16.0f || fabsf(v1) > 16.0f)
        return false;

    if (u0 > u1) { const float swap = u0; u0 = u1; u1 = swap; }
    if (v0 > v1) { const float swap = v0; v0 = v1; v1 = swap; }
    int left = static_cast<int>(floorf(u0 * surface->width));
    int top = static_cast<int>(floorf(v0 * surface->height));
    int right = static_cast<int>(ceilf(u1 * surface->width));
    int bottom = static_cast<int>(ceilf(v1 * surface->height));
    if (left >= right || top >= bottom || right - left > static_cast<int>(surface->width) * 4 ||
        bottom - top > static_cast<int>(surface->height) * 4)
        return false;

    memset(stats, 0, sizeof(*stats));
    const UINT bytes = BytesPerPixel(surface->format);
    const BYTE diffuseAlpha = (diffuse >> 24) & 0xff;
    const BYTE diffuseRed = (diffuse >> 16) & 0xff;
    const BYTE diffuseGreen = (diffuse >> 8) & 0xff;
    const BYTE diffuseBlue = diffuse & 0xff;
    for (int y = top; y < bottom; ++y)
    {
        for (int x = left; x < right; ++x)
        {
            const int textureWidth = static_cast<int>(surface->width);
            const int textureHeight = static_cast<int>(surface->height);
            const int wrappedX = ((x % textureWidth) + textureWidth) % textureWidth;
            const int wrappedY = ((y % textureHeight) + textureHeight) % textureHeight;
            BYTE rgba[4];
            DecodePixel(&surface->pixels[wrappedY * surface->pitch + wrappedX * bytes],
                        surface->format, rgba);
            ++stats->sampledPixels;
            if (rgba[3] <= 8)
                continue;
            ++stats->visiblePixels;
            const BYTE maximum = rgba[0] > rgba[1]
                ? (rgba[0] > rgba[2] ? rgba[0] : rgba[2])
                : (rgba[1] > rgba[2] ? rgba[1] : rgba[2]);
            const BYTE minimum = rgba[0] < rgba[1]
                ? (rgba[0] < rgba[2] ? rgba[0] : rgba[2])
                : (rgba[1] < rgba[2] ? rgba[1] : rgba[2]);
            if (maximum - minimum >= 32)
                ++stats->colorfulPixels;
            if (rgba[0] >= 240 && rgba[1] >= 240 && rgba[2] >= 240)
                ++stats->nearWhitePixels;
            if (x == left || x == right - 1 || y == top || y == bottom - 1)
                ++stats->visibleEdgePixels;

            const BYTE modulatedAlpha = static_cast<BYTE>(rgba[3] * diffuseAlpha / 255U);
            if (modulatedAlpha <= 8)
                continue;
            ++stats->modulatedVisiblePixels;
            const BYTE modulatedRed = static_cast<BYTE>(rgba[0] * diffuseRed / 255U);
            const BYTE modulatedGreen = static_cast<BYTE>(rgba[1] * diffuseGreen / 255U);
            const BYTE modulatedBlue = static_cast<BYTE>(rgba[2] * diffuseBlue / 255U);
            const BYTE contributionRed =
                static_cast<BYTE>(modulatedRed * modulatedAlpha / 255U);
            const BYTE contributionGreen =
                static_cast<BYTE>(modulatedGreen * modulatedAlpha / 255U);
            const BYTE contributionBlue =
                static_cast<BYTE>(modulatedBlue * modulatedAlpha / 255U);
            const BYTE contributionMaximum = contributionRed > contributionGreen
                ? (contributionRed > contributionBlue ? contributionRed : contributionBlue)
                : (contributionGreen > contributionBlue ? contributionGreen : contributionBlue);
            const BYTE contributionMinimum = contributionRed < contributionGreen
                ? (contributionRed < contributionBlue ? contributionRed : contributionBlue)
                : (contributionGreen < contributionBlue ? contributionGreen : contributionBlue);
            if (contributionMaximum - contributionMinimum >= 8)
                ++stats->modulatedColorfulPixels;
            if (modulatedRed >= 240 && modulatedGreen >= 240 && modulatedBlue >= 240)
                ++stats->modulatedNearWhitePixels;
        }
    }
    return true;
}
