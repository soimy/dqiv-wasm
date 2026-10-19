// Portable stand-ins for the three platform files that are Windows-only upstream:
//   native/SaveStore.cpp      (CreateFileW / MoveFileExW / FlushFileBuffers)
//   native/NitroThreads.cpp   (Win32 threads; also defines the OS_* mutex API)
//   native/NitroIdentity.cpp  (adapter MAC address + machine identity)
//
// These are deliberately naive: they exist so that a wasm32 build links and runs,
// not to be the final implementation. The whole gap is ~55 lines, which is the
// honest size of the non-portable surface — see docs/porting/02-FINDINGS.md.
#include "SaveStore.hpp"
#include "NitroThreads.hpp"
#include "NitroIdentity.hpp"
#include "NitroOverlays.hpp"
#include "NitroVideo.hpp"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>   // emscripten_sleep, which is what makes a parked thread yield
#include <iostream>
#include <string>
#include "Profile.hpp"    // the frame-stage timings, which are on by default here
#endif
extern "C" {
#include <nitro/os/thread.h>
#include <nitro/os/mutex.h>
#include "snd/snd_internal.h"   // UnkList, for the stream thread's work predicate (path 2)
}
#include <algorithm>
#include <csetjmp>   // the jump pair that lets a portable thread park and give the turn back
#include <cstring>
#include <functional>
#include <fstream>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <system_error>

namespace pc {
std::filesystem::path saveDirectory() { return std::filesystem::path("/saves"); }
std::filesystem::path defaultSavePath() { return saveDirectory() / "dqiv_eur.sav"; }
std::filesystem::path previousSaveDirectory() { return {}; }

SaveStore::SaveStore(std::filesystem::path path) : path_(std::move(path)) {
    std::error_code ignored;
    std::filesystem::create_directories(path_.parent_path(), ignored);
    bytes_.fill(0xff);
    if (std::filesystem::exists(path_)) {
        std::ifstream in(path_, std::ios::binary);
        if (in) in.read(reinterpret_cast<char*>(bytes_.data()), static_cast<std::streamsize>(bytes_.size()));
    }
}
SaveStore::~SaveStore() = default;
void SaveStore::read(std::uint32_t offset, std::span<std::uint8_t> destination) const {
    if (offset > size || destination.size() > size - offset) throw std::out_of_range("Backup range exceeds 64 KiB");
    std::copy_n(bytes_.begin() + offset, destination.size(), destination.begin());
}
void SaveStore::write(std::uint32_t offset, std::span<const std::uint8_t> source) {
    if (offset > size || source.size() > size - offset) throw std::out_of_range("Backup range exceeds 64 KiB");
    std::copy(source.begin(), source.end(), bytes_.begin() + offset);
    // TODO(port): publish atomically, as the Windows implementation does.
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes_.data()), static_cast<std::streamsize>(bytes_.size()));
}
}

// Single-threaded host: the original mutex/thread-queue entry points become no-ops.
namespace pc {
namespace {
std::function<void()> idleHandler;   // what NitroThreads::setIdleHandler stores
bool boundaryArmed=true;   // a boundary is due: set by the frame callback, cleared when it runs
// The coroutine state for the SDK's threads. It lives here because the park point is in
// OS_PauseThread below, which is where a thread hands the turn back.
struct PortableThread { OSThread* sdk; void (*function)(void*); void* argument; bool runnable; bool started; jmp_buf resume; };
PortableThread portableThreads[16];
unsigned portableCount=0;
unsigned pauseQueueCalls=0;    // how often the game parks on a queue
unsigned unpauseCalls=0;       // and how often anything unpauses one
jmp_buf schedulerReturn;
bool insideThread=false;
PortableThread* currentThread=nullptr;
PortableThread* find(OSThread* sdk);   // defined below with the rest of the registry; OS_UnpauseThread needs it early
}
void wasmBoundaryArm() { boundaryArmed=true; }   // called by GameBoot's frame callback
}
extern "C" {
void OS_LockMutex(OSMutex*) {}
void OS_UnlockMutex(OSMutex*) {}
void OS_PauseThread(OSThreadQueue* queue) {
    // Path 2, decided after two measurements: emscripten_sleep(0) does NOT unwind
    // (the journal stops after one frame), and emscripten_set_main_loop fires once
    // because main() returns. So the scheduler cannot park a thread mid-call; instead
    // runPortableThreads() skips a thread when it has no work, and the thread gets its
    // turn exactly when work exists - which is the game's own "park to request arrival"
    // semantics. A real queue pointer means "a thread is waiting": nothing to do here,
    // readiness is detected by the scheduler. A null queue is the frame boundary itself,
    // which is what pcWasmVCount() asks for.
    if (queue) {
        pc::pauseQueueCalls++;
        // A portable thread parks here. Its body never returns, so this is where the turn
        // goes back to the scheduler; the matching longjmp is in resumeThread(). A thread
        // that is resumed comes back through this same point and continues its body.
        if (pc::insideThread && pc::currentThread) {
            // The SDK links the thread onto this queue before it suspends, and an unpause
            // looks threads up by that link. So the link has to exist before the turn goes
            // back, otherwise the game queues work, unpauses, and gets nothing.
            auto* t = pc::currentThread;
            queue->head = t->sdk; queue->tail = t->sdk;
            t->sdk->queue = queue; t->sdk->list.next = nullptr; t->sdk->list.prev = nullptr;
            if (setjmp(t->resume) == 0) longjmp(pc::schedulerReturn, 1);
            // Resumed by the scheduler: the body continues from here.
        }
        return;
    }
    // This is the frame boundary on a single-threaded host. The game's own code
    // calls it when it wants to wait for the next frame, so what the Windows build
    // does on an idle thread happens here instead, and the threads the SDK creates
    // get their turn at the same point. Nothing else needs to drive the loop.
    //
    // It runs once per frame, not once per call, and that is a measured decision:
    // GX_GetVCount() on wasm now reaches the boundary through this function, and
    // GlobalGamePart::vf10() reads VCount five times, so without the guard the
    // boundary would run five times a frame.
    //
    // The guard is armed by the frame callback, so exactly one boundary runs per game
    // frame - the first read after the last swap - and every other read is a plain
    // read that returns without running the idle handler. Measured before this: the
    // boundary ran 28 times a frame, so pace() waited 28 times and the game got about
    // 3 frames a second instead of the 60 the browser gives it.
    if (!pc::boundaryArmed) return;
    pc::boundaryArmed = false;
    if(pc::idleHandler) { pc::idleHandler(); }
    pc::runPortableThreads();
}
int pcWasmVCount(void) {
    // What GX_GetVCount() asks the emulated hardware for on a wasm host. Reaching the
    // boundary is the whole job: on the real DS the game's `while (GX_GetVCount() >
    // 180)` means "wait for the scanline counter to pass 180 next frame", and here the
    // boundary IS that next frame, so the read resolves at the same moment the real
    // one would. The value is 0 so the guard fails, which is what the hardware gives
    // at the start of a frame.
    OS_PauseThread(0);
    return 0;
}
void OS_UnpauseThread(OSThreadQueue* queue) {
    // It was an empty stub, so a game that queued work and unpaused the thread waiting on
    // it got nothing: the thread stayed runnable=0 forever and the loop blocked right
    // after it answered the clock. Wake the head of the queue, which is what the SDK does.
    pc::unpauseCalls++;
    auto* t = queue ? queue->head : nullptr;
    if(!t) return;
    queue->head = nullptr; queue->tail = nullptr;
    t->queue = nullptr;
    if(auto* thread=pc::find(t)) thread->runnable=true;
}
}

namespace pc {
struct ThreadRuntime {};   // nothing to hold; the handler lives in idleHandler above
NitroThreads::NitroThreads():impl_(std::make_unique<ThreadRuntime>()) {}
NitroThreads::~NitroThreads() = default;
void NitroThreads::checkErrors() {}
void NitroThreads::setIdleHandler(std::function<void()> handler) { idleHandler=std::move(handler); }
void NitroThreads::runIdle() { if(idleHandler) idleHandler(); }

namespace {
NitroIdentity* currentIdentity=nullptr;   // single-threaded host, so no thread_local
}
NitroIdentity::NitroIdentity(const std::filesystem::path& input) {
    if(currentIdentity) throw std::runtime_error("A native identity context is already active");
    std::filesystem::create_directories(input.parent_path());
    std::ifstream file(input,std::ios::binary);
    if(file&&file.read(reinterpret_cast<char*>(address_.data()),static_cast<std::streamsize>(address_.size()))
        &&(address_[0]&3)==2) {
        // Already there, and still valid.
    } else {
        std::random_device random;
        for(auto& byte:address_) byte=static_cast<std::uint8_t>(random());
        address_[0]=(address_[0]&0xfc)|2;   // locally administered, unicast
        std::ofstream out(input,std::ios::binary|std::ios::trunc);
        out.write(reinterpret_cast<const char*>(address_.data()),static_cast<std::streamsize>(address_.size()));
        // TODO(port): publish atomically, as the Windows implementation does.
    }
    currentIdentity=this;
}
NitroIdentity::~NitroIdentity() {currentIdentity=nullptr;}
}

// The overlay registry. On MSVC this function is generated from the section the
// overlay objects land in (#pragma data_seg/bss_seg plus __declspec(allocate),
// via cmake/NativeOverlays.cmake). clang ignores all three, so there is no such
// mechanism here.
//
// M3a: the registry has to contain overlay 38, because that is what main() loads
// first, and GameBoot treats a missing one as fatal. The ranges are empty, so
// snapshot() copies zero bytes and restore() copies nothing back - the module is
// registered, but its writable image is not real yet. That is M3b, and the only
// lever clang gives us for it is a two-pass link: read the addresses out of the
// first link's map file, generate the table with them, link again.
std::span<const pc::OverlayImage> gameOverlayImages() {
    static const pc::OverlayImage images[] = {
        {38, {nullptr, nullptr}, {nullptr, nullptr}, nullptr, nullptr},
    };
    return images;
}

extern "C" void func_02079e24(unsigned char* destination) {
    if(!pc::currentIdentity||!destination) throw std::runtime_error("Device identity requested without its native profile");
    std::memcpy(destination,pc::currentIdentity->address().data(),6);
}

// The thread API the sound system uses - snd_capture.c creates a thread and wakes
// it, snd_main.c initialises a mutex. A single-threaded host cannot spawn one, so
// the SDK's threads are registered here and run when marked runnable, which is
// what runPortableThreads() does, once per frame, from the frame loop. The stack
// and priority the caller passes are recorded and unused: there is one stack, the
// wasm one, and one thread of control.
extern "C" { extern char data_02113480[]; }   // the stream thread global, defined in snd_arcstrm.c
namespace pc {
namespace {
// The coroutine state lives at the top of the file, where OS_PauseThread can see it.
PortableThread* find(OSThread* sdk) {
    for(unsigned i=0;i<portableCount;++i) if(portableThreads[i].sdk==sdk) return &portableThreads[i];
    return nullptr;
}
// Path 2: the scheduler skips a thread that has no work, so it can return. The game's
// own semantics are "park until a request arrives", so a thread's turn is exactly when
// it has work. Threads with no predicate keep the old behaviour (run when marked
// runnable); the stream thread gets one below, keyed by the SDK pointer its caller
// passed to func_020787c4. Its argument is the UnkSndArcStrmThread* itself, whose
// requestList sits at 0x4E0, and UnkList.numObjects is at 0x08 - so numObjects>0 means
// a request is queued and the thread should get its turn.
bool streamHasWork(const PortableThread& t) {
    return reinterpret_cast<UnkList*>((char*)t.argument + 0x4E0)->numObjects > 0;
}
struct WorkPredicate { const void* sdk; bool (*hasWork)(const PortableThread&); };
WorkPredicate workPredicates[] = { {&data_02113480[0], streamHasWork} };
bool hasWork(const PortableThread& t) {
    for(const auto& p : workPredicates) if(p.sdk==t.sdk) return p.hasWork(t);
    return true;   // no predicate: run when marked runnable, as before
}
void resumeThread(PortableThread& t) {
    insideThread=true;currentThread=&t;
    if(setjmp(schedulerReturn)==0) {
        if(t.started) longjmp(t.resume,1);   // continue at its park point
        else { t.started=true; t.function(t.argument); }   // first run, from the top
    }
    insideThread=false;currentThread=nullptr;   // back here because the thread parked
}
}
void runPortableThreads() {
    // Measured, because the browser stops inside the frame boundary now: GX_GetVCount()
    // on wasm reaches this, and the print before it in GlobalGamePart::vf0c() is the
    // last one that speaks. So the boundary runs and does not come back. Naming which
    // part of it blocks.
    //
    // The per-frame prints are gone: at 60 frames a second they cost enough CPU that
    // the main thread never gives the debugger a gap, and the journal grew faster than
    // anything could read it.
    // Measured, because the game stops right after it enters Scene 3: naming which
    // threads exist and which have work is how we tell a parked thread from a dead loop.
    // The frame-stage timings. This is the frame boundary, so it is where the averages
    // get reported. Every 30 frames, not the default 120: the game freezes soon after it
    // enters a real scene, and at 120 the window rarely closes, so Scene 3 never reports.
    profileFrame(30);
    static unsigned probeCount=0;
    if(++probeCount%60==0) {
        std::cout<<"  scheduler probe: "<<portableCount<<" threads"
                 <<" parks="<<pauseQueueCalls<<" unpauses="<<unpauseCalls;
        for(unsigned i=0;i<portableCount;++i) {
            auto& t=portableThreads[i];
            std::cout<<" ["<<i<<"] runnable="<<(t.runnable?1:0)
                     <<" started="<<(t.started?1:0)<<" work="<<(hasWork(t)?1:0);
        }
        std::cout<<"\n";
    }
    for(unsigned i=0;i<portableCount;++i) {
        auto& thread=portableThreads[i];
        if(thread.runnable&&thread.function) {
            if(!hasWork(thread)) continue;
            thread.runnable=false;
            resumeThread(thread);
        }
    }
}
}
extern "C" {
void func_020787c4(OSThread* sdk,void (*function)(void*),void* argument,void* stack,unsigned size,unsigned priority) {
    if(pc::portableCount==16) throw std::runtime_error("Portable thread table full");
    pc::portableThreads[pc::portableCount++]=pc::PortableThread{sdk,function,argument,false};
}
void OS_WakeupThreadDirect(void* pointer) {
    if(auto* thread=pc::find(static_cast<OSThread*>(pointer))) thread->runnable=true;
}
void OS_InitMutex(OSMutex*) {}
}
