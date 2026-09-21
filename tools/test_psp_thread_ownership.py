#!/usr/bin/env python3
"""PSP owner guards plus a host runtime test of the actual compatibility code."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text()


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for i in range(brace, len(source)):
        depth += (source[i] == "{") - (source[i] == "}")
        if depth == 0:
            return source[start:i + 1]
    raise AssertionError(signature)


class ThreadOwnership(unittest.TestCase):
    def test_workers_do_not_discard_psp_owner(self):
        for path, count in (("src/Supervisor.cpp", 2),
                            ("src/TitleScreen.cpp", 1), ("src/GameManager.cpp", 2)):
            source = read(path)
            assignment = "    g_Supervisor.runningSubthreadHandle = NULL;"
            self.assertEqual(source.count(assignment), count)
            self.assertEqual(source.count("#if !defined(PSP)\n" + assignment + "\n#endif"), count)

    def test_poll_precedes_shutdown_and_is_nonblocking(self):
        update = function(read("src/Supervisor.cpp"), "ChainCallbackResult Supervisor::OnUpdate(")
        self.assertIn("WaitForSingleObject(s->runningSubthreadHandle, 0) == WAIT_OBJECT_0", update)
        self.assertLess(update.index("CloseHandle(s->runningSubthreadHandle)"),
                        update.index("s->flags.receivedCloseMsg"))

    def test_bgm_keeps_handle_until_close(self):
        source = read("src/SoundPlayer.cpp")
        release = source[source.index("case SOUNDPLAYER_COMMAND_RELEASE_BGM:"):
                         source.index("case SOUNDPLAYER_COMMAND_STOP_BGM:")]
        self.assertIn("#if !defined(PSP)\n                this->bgmThreadHandle = NULL;\n#endif", release)
        step3 = release[release.index("commandCursor->step == 3"):]
        self.assertLess(step3.index("CloseHandle(this->bgmThreadHandle)"),
                        step3.index("this->bgmThreadHandle = NULL"))

    def test_message_queue_retired_before_id_reuse(self):
        close = function(read("src/modern/linux/linux_compat.cpp"), "BOOL CloseHandle(")
        self.assertLess(close.index("if (!thread->finished)"), close.index("g_threadMessages.erase"))
        self.assertLess(close.index("g_threadMessages.erase"), close.index("pthread_join"))

    def test_real_compatibility_functions_runtime(self):
        compat = read("src/modern/linux/linux_compat.cpp")
        supervisor = read("src/Supervisor.cpp")
        handle = function(compat, "struct ThreadHandle : LinuxHandle") + ";"
        definitions = "\n".join(function(compat, signature) for signature in (
            "void *ThreadTrampoline(", "BOOL CloseHandle(", "HANDLE CreateThread(",
            "DWORD WaitForSingleObject("))
        definitions += "\n" + function(supervisor, "ZunResult Supervisor::ThreadStart(")
        definitions += "\n" + function(supervisor, "void Supervisor::ThreadClose(")
        update = function(supervisor, "ChainCallbackResult Supervisor::OnUpdate(")
        poll = update[update.index("#if defined(PSP)"):update.index("    if (s->flags.receivedCloseMsg")]
        prelude = r'''
#define PSP 1
#include <atomic>
#include <cassert>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <map>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <vector>
using DWORD = unsigned long; using LPDWORD = DWORD *;
using LPVOID = void *; using HANDLE = void *; using BOOL = int;
using LPTHREAD_START_ROUTINE = DWORD (*)(LPVOID);
using ZunResult = int;
constexpr int TRUE=1, FALSE=0, ZUN_SUCCESS=0, ZUN_ERROR=-1;
constexpr DWORD WAIT_OBJECT_0=0, WAIT_TIMEOUT=258, INFINITE=0xffffffffUL;
#define INVALID_HANDLE_VALUE reinterpret_cast<void *>(-1)
enum HandleKind { HANDLE_THREAD, HANDLE_EVENT };
int liveHandles=0, joins=0; bool failCreate=false, failJoin=false;
struct LinuxHandle {
    HandleKind kind;
    explicit LinuxHandle(HandleKind k):kind(k) { ++liveHandles; }
    virtual ~LinuxHandle() { --liveHandles; }
};
struct EventHandle : LinuxHandle {
    EventHandle():LinuxHandle(HANDLE_EVENT) {}
    pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
    bool signaled=false, manual=false;
};
namespace th08 { namespace psp {
template<class... T> void BootLog(const char *, T...) {}
void PlatformArmExitWatchdog() {}
bool FlushBootLog() { return true; }
} }
namespace utils { void GuiDebugPrint(const char *) {} }
std::map<DWORD, std::vector<int>> g_threadMessages;
pthread_mutex_t g_messageMutex=PTHREAD_MUTEX_INITIALIZER;
// Intentionally reuse IDs in these sequential tests, like PSP's pthread pool.
DWORD CurrentThreadIdImpl() { return 7; }
DWORD timeGetTime() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void Sleep(DWORD ms) { usleep(ms*1000); }
int test_create(pthread_t *t, const pthread_attr_t *a, void *(*f)(void *), void *p) {
    return failCreate ? EAGAIN : pthread_create(t,a,f,p);
}
int test_join(pthread_t t, void **p) {
    if(failJoin) { failJoin=false; return EINVAL; }
    ++joins; return pthread_join(t,p);
}
#define pthread_create test_create
#define pthread_join test_join
struct Supervisor {
    HANDLE runningSubthreadHandle=nullptr;
    DWORD runningSubthreadID=0;
    std::atomic<int> subthreadActive{0}, subthreadCloseRequestActive{0};
    struct { bool receivedCloseMsg=false; } flags;
    ZunResult ThreadStart(LPTHREAD_START_ROUTINE, void *);
    void ThreadClose();
};
'''
        main = r'''
std::atomic<bool> releaseWorker{false};
DWORD fast_worker(void *p) { static_cast<Supervisor *>(p)->subthreadActive=FALSE; return 0; }
DWORD cancelled_worker(void *p) {
    auto *s=static_cast<Supervisor *>(p);
    while(!s->subthreadCloseRequestActive) sched_yield();
    s->subthreadActive=FALSE; return 0;
}
DWORD bgm_worker(void *) { while(!releaseWorker) sched_yield(); return 0; }
int main(int argc, char **) {
    if(argc>1) {
        releaseWorker=true;
        HANDLE h=CreateThread(nullptr,0,bgm_worker,nullptr,0,nullptr);
        assert(WaitForSingleObject(h,INFINITE)==WAIT_OBJECT_0);
        failJoin=true;
        std::set_terminate([] { std::_Exit(86); });
        CloseHandle(h);
        return 42; // PSP must not let an owner discard a failed join.
    }
    Supervisor s;
    for(int i=0;i<1000;++i) {
        assert(s.ThreadStart(fast_worker,&s)==ZUN_SUCCESS);
        assert(WaitForSingleObject(s.runningSubthreadHandle,INFINITE)==WAIT_OBJECT_0);
        assert(s.subthreadActive==FALSE);
        g_threadMessages[s.runningSubthreadID].push_back(18); // stale WM_QUIT
        poll_setup(&s);
        assert(!s.runningSubthreadHandle && liveHandles==0 && g_threadMessages.empty());
    }
    assert(joins==1000);
    assert(s.ThreadStart(cancelled_worker,&s)==ZUN_SUCCESS);
    s.ThreadClose();
    assert(!s.runningSubthreadHandle && !s.subthreadCloseRequestActive && liveHandles==0);
    HANDLE h=CreateThread(nullptr,0,bgm_worker,nullptr,0,nullptr);
    assert(WaitForSingleObject(h,0)==WAIT_TIMEOUT);
    releaseWorker=true;
    assert(WaitForSingleObject(h,INFINITE)==WAIT_OBJECT_0);
    assert(WaitForSingleObject(h,0)==WAIT_OBJECT_0 && liveHandles==1);
    assert(CloseHandle(h) && liveHandles==0 && joins==1002);
    failCreate=true;
    assert(s.ThreadStart(fast_worker,&s)==ZUN_ERROR);
    assert(!s.runningSubthreadHandle && !s.subthreadActive && s.flags.receivedCloseMsg);
    assert(liveHandles==0);
}
'''
        source = prelude + handle + definitions + "\nvoid poll_setup(Supervisor *s) {\n" + poll + "}\n" + main
        with tempfile.TemporaryDirectory(prefix="th08-thread-owner-") as tmp:
            cpp = Path(tmp) / "test.cpp"
            binary = Path(tmp) / "test"
            cpp.write_text(source)
            subprocess.run(["g++", "-std=c++17", "-O2", "-pthread", str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)
            failed_join = subprocess.run([str(binary), "join-failure"], timeout=10)
            self.assertEqual(failed_join.returncode, 86)


if __name__ == "__main__":
    unittest.main()
