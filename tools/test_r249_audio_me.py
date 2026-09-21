#!/usr/bin/env python3
"""Run the actual old Mix against snapshot/kernel/commit, plus callback gates."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import re
from test_psp_audio_fixed_cursor import function_body

ROOT = Path(__file__).resolve().parents[1]
OLD = (ROOT / 'tools/fixtures/r247_mix_reference.cpp').read_text()
NEW = (ROOT / 'src/modern/linux/linux_compat.cpp').read_text()


class AudioMe(unittest.TestCase):
    def test_actual_transport_timeout_and_stop(self):
        worker = (ROOT / 'psp/me_audio.cpp').read_text()
        signatures = ('void MeAudioRequestStop(', 'bool MeAudioCanExit(', 'void MeAudioShutdown(',
                      'MeAudioJob *MeAudioPrepare(', 'bool MeAudioAdd(', 'bool MeAudioRun(', 'void MeAudioRelease(')
        functions = '\n'.join(function_body(worker, s) for s in signatures)
        functions = functions.replace('__asm__ volatile("cfc1 %0, $31" : "=r"(packet.fcsr));', 'packet.fcsr=0;')
        source = r'''
#include "psp/me_audio_math.hpp"
#include <atomic>
#include <cassert>
#include <cstring>
using namespace th08::psp;
enum {Booting,Ready,Stopped,Fault};
struct Mailbox {unsigned command=0,state=Ready,stop=0,done=0,producer=0;} box;
Mailbox*Box(){return &box;}MeAudioJob packet{};
std::atomic<unsigned>active{1},ownsCore{1};bool powerLocked=true;
unsigned callbacks=0,completed=0,fallbacks=0,timeouts=0,buildUs=0,waitUs=0,waitMaxUs=0,scUs=0,scCalls=0,buildStart=0;
unsigned now=0,restores=0,unlocks=0,serviceMode=0,published=0;
unsigned sceKernelGetSystemTimeLow(){return now;}
void Service();void sceKernelDelayThread(unsigned us){now+=us;Service();}
void Sync(){}bool StackGuards(){return true;}unsigned Lines(unsigned n){return (n+63)&~63U;}
void sceKernelDcacheWritebackInvalidateRange(const void*,unsigned){assert(!box.command);++published;}
void sceKernelDcacheWritebackRange(const void*,unsigned){assert(!box.command);++published;}
void sceKernelDcacheInvalidateRange(const void*,unsigned){assert(box.done);}
int scePowerUnlock(int){assert(restores&&box.state==Stopped);++unlocks;return 0;}
int kinit(void*){assert(box.state==Stopped&&!box.command&&!box.producer);++restores;return 0;}
template<class...A>void BootLog(const char*,A...){}void FlushBootLogHard(){}
void MeAudioRelease();
''' + functions + r'''
void Service(){
 if(serviceMode==1)return;
 if(box.command){assert(published>=3);MeAudioKernel(packet);box.command=0;box.done=1;}
 if(box.stop&&!box.producer&&!box.command){box.state=Stopped;box.done=1;}
}
int main(){
 unsigned char pcm[]={0x30,0x75,0x30,0x75};MeAudioVoice v{};
 v.sourceFrames=1;v.frameBytes=4;v.channels=2;v.bits=16;v.looping=1;v.leftGain=v.rightGain=1;
 std::int16_t output[2048]={};auto*j=MeAudioPrepare(1024);assert(j&&box.producer);
 assert(MeAudioAdd(j,v,pcm));assert(MeAudioRun(j,output));assert(output[0]==30000&&completed==1);
 MeAudioRelease();assert(!box.producer&&!MeAudioCanExit());
 serviceMode=1;j=MeAudioPrepare(1024);assert(j);assert(MeAudioAdd(j,v,pcm));
 std::memset(output,0x33,sizeof(output));assert(!MeAudioRun(j,output));
 assert(timeouts==1&&box.stop&&!active&&box.command&&output[0]==0x3333);
 MeAudioRelease();std::memset(pcm,0,sizeof(pcm)); // caller can overwrite/free now
 serviceMode=0;Service();assert(packet.output[0]==30000&&box.state==Stopped);
 const MeAudioJob retired=packet;assert(!MeAudioPrepare(1024));
 assert(!std::memcmp(&retired,&packet,sizeof(packet)));assert(!MeAudioCanExit());
 MeAudioShutdown();assert(restores==1&&unlocks==1&&MeAudioCanExit());
 // Stop while a callback is building must not ACK before publication releases.
 box={};active=1;ownsCore=1;powerLocked=true;j=MeAudioPrepare(1024);assert(j);
 MeAudioRequestStop();Service();assert(box.state!=Stopped);
 assert(!MeAudioRun(j,output));MeAudioRelease();Service();assert(box.state==Stopped);
 MeAudioShutdown();assert(restores==2&&unlocks==2&&MeAudioCanExit());
}
'''
        with tempfile.TemporaryDirectory(prefix='th08-me-transport-') as d:
            src = Path(d) / 'test.cpp'; src.write_text(source)
            binary = Path(d) / 'test'
            subprocess.run(['g++', '-std=c++17', '-O2', '-I', str(ROOT), str(src), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_kernel_callback_restore(self):
        kernel = (ROOT / 'psp/third_party/me-custom-core/kernel/main.c').read_text()
        owned = kernel[kernel.index('static PspSysEventHandler* ownedEvent;'):kernel.index('int kinit(')]
        source = r'''
#include <cassert>
#include <cstddef>
using PspSysEventHandlerFunc=void(*)();
struct PspSysEventHandler{const char*name;PspSysEventHandlerFunc handler;PspSysEventHandler*next;};
PspSysEventHandler*head;int held=0;
PspSysEventHandler* sceKernelReferSysEventHandler(){return head;}
int sceKernelCpuSuspendIntr(){assert(!held);held=1;return 17;}
void sceKernelCpuResumeIntr(int old){assert(held&&old==17);held=0;}
void original(){}void application(){}void other(){}
''' + owned + function_body(kernel, 'int kinit(') + r'''
int main(){PspSysEventHandler event{"sceMeRpc",original,nullptr};head=&event;
 assert(kinit((void*)application)==0);assert(event.handler==application);
 assert(kinit((void*)application)==-2);assert(kinit(nullptr)==0);assert(event.handler==original);
 assert(kinit(nullptr)==0);assert(!held);
 assert(kinit((void*)application)==0);event.handler=other;
 assert(kinit(nullptr)==-2);assert(event.handler==other);
 event.handler=application;assert(kinit(nullptr)==0);assert(event.handler==original);
 head=nullptr;assert(kinit((void*)application)==-1);assert(kinit(nullptr)==0);
}
'''
        with tempfile.TemporaryDirectory(prefix='th08-kcall-test-') as d:
            src = Path(d) / 'test.cpp'; src.write_text(source)
            binary = Path(d) / 'test'
            subprocess.run(['g++', '-std=c++17', '-O2', str(src), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_actual_mixer_and_callback(self):
        # FinishMix starts inside an existing #if which closes just after its
        # opening brace. Preserve that guard when compiling the extracted code.
        methods = '#if TH08_PSP_ME_AUDIO_ENABLED\n' + function_body(NEW, 'void FinishMix(')
        methods += '\n' + '\n'.join(function_body(NEW, s) for s in (
            'bool ExportMeVoice(', 'const BYTE *MeVoicePcm(', 'void CommitMeVoice('))
        source = r'''
#define PSP 1
#define TH08_PSP_ME_AUDIO_ENABLED 1
#define TH08_PSP_AUDIO_FIXED_CURSOR_ENABLED 1
#define TH08_PSP_AUDIO_FIXED_CURSOR_AUDIT_ENABLED 0
#define TH08_PSP_RUNTIME_TELEMETRY 0
#include "psp/me_audio_math.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
#include <algorithm>
using DWORD=std::uint32_t;using BYTE=unsigned char;using Sint16=std::int16_t;
using INT16=std::int16_t;using Uint8=unsigned char;using LONG=int;
constexpr int DSBVOLUME_MIN=-10000,WAVE_FORMAT_PCM=1;
struct Format{unsigned wFormatTag=1,nSamplesPerSec=44100,nChannels=2,wBitsPerSample=16,nBlockAlign=4;};
struct Notification{DWORD dwOffset;unsigned hEventNotify;};
std::vector<unsigned> events;
void SetEvent(unsigned e){events.push_back(e);}
namespace th08::psp {void AudioCursorProductNoteMix(unsigned,bool){} void AudioTelemetryRecordBgmNotify(){} }
struct Canon {
 std::vector<BYTE> bytes;bool playing=true,looping=true,hasFormat=true;
 DWORD position=0,laps=0;double cursorFrame=0;LONG volume=0,pan=0;Format format;
 std::vector<Notification> notifications;
 DWORD FrameBytes()const{return hasFormat?format.nBlockAlign:0;}
 bool IsPlayingForTelemetry()const{return playing;}
''' + function_body(OLD, 'void Mix(') + r'''
};
struct LinuxSoundBuffer:Canon {
''' + methods + r'''
};
std::vector<LinuxSoundBuffer*> g_soundBuffers;
unsigned mode=0,runCalls=0,releaseCalls=0,scCalls=0;
th08::psp::MeAudioJob packet;
unsigned sceKernelGetSystemTimeLow(){return 1;}
namespace th08::psp {
MeAudioJob* MeAudioPrepare(unsigned f){if(mode==1||!f||f>1024)return nullptr;packet.frames=f;packet.count=0;return &packet;}
bool MeAudioAdd(MeAudioJob*j,const MeAudioVoice&v,const BYTE*p){if(j->count>=32)return false;j->voices[j->count]=v;if(!MeAudioSnapshot(j->voices[j->count],p,j->frames,j->pcm[j->count]))return false;++j->count;return true;}
bool MeAudioRun(MeAudioJob*j,Sint16*out){++runCalls;if(mode==2)return false;bool ok=MeAudioKernel(*j);if(ok)memcpy(out,j->output,j->frames*4);return ok;}
void MeAudioRelease(){++releaseCalls;}void MeAudioNoteSc(unsigned){++scCalls;}
}
''' + function_body(NEW, 'void AudioCallback(') + r'''
void EqualState(const Canon&a,const Canon&b){assert(a.cursorFrame==b.cursorFrame);assert(a.position==b.position);assert(a.laps==b.laps);assert(a.playing==b.playing);}
int main(){using namespace th08::psp;std::mt19937 rng(249);
 for(unsigned trial=0;trial<900;++trial){
  unsigned frames=trial%3?1024:1+rng()%1024,count=1+rng()%32;
  std::vector<Canon> refs(count);std::vector<LinuxSoundBuffer> bufs(count);
  MeAudioJob job{};job.frames=frames;job.count=count;
  for(unsigned n=0;n<count;++n){auto&b=bufs[n];
   unsigned shift=trial%3,sourceFrames=trial<120?1+trial%3:1+rng()%2000;
   b.format.nSamplesPerSec=44100>>shift;b.format.nChannels=1+rng()%2;b.format.wBitsPerSample=rng()%2?8:16;
   b.format.nBlockAlign=b.format.nChannels*(b.format.wBitsPerSample/8);b.looping=trial%4!=0;
   b.volume=trial%7?-(int)(rng()%7000):0;b.pan=(int)(rng()%40001)-20000;
   b.cursorFrame=double(rng()%(sourceFrames*(1U<<shift)+1))/(1U<<shift);
   b.position=unsigned(b.cursorFrame)*b.FrameBytes();b.laps=3;
   b.bytes.resize(sourceFrames*b.FrameBytes());for(auto&v:b.bytes)v=rng();
   for(unsigned p=0;p<sourceFrames;p+=1+sourceFrames/9)b.notifications.push_back({p*b.FrameBytes(),p+n*2000});
   refs[n]=b;assert(b.ExportMeVoice(job.voices[n]));assert(MeAudioSnapshot(job.voices[n],b.bytes.data(),frames,job.pcm[n]));
  }
  std::vector<Sint16> expected(frames*2);
  events.clear();for(auto&b:refs)b.Mix(expected.data(),frames);const auto expectedEvents=events;
  // No live PCM may be read by a late worker after the callback timed out.
  for(auto&b:bufs){b.bytes.clear();b.bytes.shrink_to_fit();}
  assert(MeAudioKernel(job));assert(!memcmp(expected.data(),job.output,frames*4));
  events.clear();for(unsigned n=0;n<count;++n){bufs[n].CommitMeVoice(job.voices[n]);EqualState(refs[n],bufs[n]);}
  assert(events==expectedEvents);
 }
 for(unsigned scenario=0;scenario<8;++scenario){
  std::vector<LinuxSoundBuffer> bufs(scenario==6?33:85);std::vector<Canon> refs(bufs.size());g_soundBuffers.clear();
  for(unsigned n=0;n<bufs.size();++n){auto&b=bufs[n];b.bytes={0x30,0x75,0x30,0x75};b.playing=scenario==6||n<3;
   if(n==2)b.bytes={0xd0,0x8a,0xd0,0x8a}; // +30000,+30000,-30000 -> 2767
   if(scenario==4&&n==1)b.format.nSamplesPerSec=32000;
   if(scenario==5&&n==1)b.format.wBitsPerSample=24;
   if(scenario==7&&n==1)b.cursorFrame=.1;
   refs[n]=b;g_soundBuffers.push_back(&b);
  }
  mode=scenario==1?1:scenario==2?2:0;runCalls=releaseCalls=scCalls=0;
  const unsigned frames=scenario==3?1025:1024;
  std::vector<Sint16>a(frames*2),b(frames*2);
  for(auto&r:refs)r.Mix(a.data(),frames);
  AudioCallback(nullptr,(Uint8*)b.data(),frames*4);assert(a==b);
  for(unsigned n=0;n<bufs.size();++n)EqualState(refs[n],bufs[n]);
  if(scenario==0){assert(b[0]==2767);assert(runCalls==1&&scCalls==0&&releaseCalls==1);}
  else assert(scCalls==1);
  if(scenario>=4)assert(runCalls==0);
 }
 puts("PASS 900 actual Mix/kernel/commit cases + 8 actual callback cases");
}
'''
        with tempfile.TemporaryDirectory(prefix='th08-audio-oracle-') as d:
            src = Path(d) / 'test.cpp'
            src.write_text(source)
            binary = Path(d) / 'test'
            subprocess.run(['g++', '-std=c++17', '-O2', '-ffp-contract=off', '-I', str(ROOT),
                            str(src), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_audio_service_isolation(self):
        worker = (ROOT / 'psp/me_audio.cpp').read_text()
        self.assertIn('#define DISABLE_VME_MINIMAL_CONFIG 1', worker)
        self.assertIn('#define TH08_ME_MAIN_RAM_STACK 1', worker)
        self.assertNotIn('samplesAddr', worker)
        self.assertNotIn('th08_me_core_submit', worker)
        self.assertIn('job->pcm[i]', worker)
        self.assertIn('Box()->producer = 1', worker)
        self.assertIn('Box()->producer = 0', worker)
        self.assertIn('kinit(nullptr)', worker)
        recipe = (ROOT / 'tools/build_go_r249_audio_me_20260922.sh').read_text()
        self.assertIn('TH08_PSP_ME_CORE=0 TH08_PSP_ME_AUDIO_MIX=1', recipe)
        platform = (ROOT / 'psp/platform.cpp').read_text()
        self.assertIn('if (!MeAudioCanExit())', platform)


if __name__ == '__main__':
    unittest.main()
