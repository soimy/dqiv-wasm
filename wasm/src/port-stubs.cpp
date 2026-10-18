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
#include <iostream>
#include <string>
#endif
extern "C" {
#include <nitro/os/thread.h>
#include <nitro/os/mutex.h>
}
#include <algorithm>
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
}
}
extern "C" {
void OS_LockMutex(OSMutex*) {}
void OS_UnlockMutex(OSMutex*) {}
void OS_PauseThread(OSThreadQueue*) {
    // This is the frame boundary on a single-threaded host. The game's own code
    // calls it when it wants to wait for the next frame, so what the Windows build
    // does on an idle thread happens here instead, and the threads the SDK creates
    // get their turn at the same point. Nothing else needs to drive the loop.
    //
    // It runs once per frame, not once per call, and that is a measured decision:
    // GX_GetVCount() on wasm now reaches the boundary through this function, and
    // GlobalGamePart::vf10() reads VCount five times, so without the guard the
    // boundary would run five times a frame.
    static unsigned frameId = 0;
    static unsigned lastRun = ~0u;
    if (lastRun == frameId) return;
    lastRun = frameId;
    ++frameId;
    if(pc::idleHandler) { std::cout<<"  boundary: idle handler\n"; pc::idleHandler(); std::cout<<"  boundary: idle handler returned\n"; }
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
void OS_UnpauseThread(OSThreadQueue*) {}
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
namespace pc {
namespace {
struct PortableThread { OSThread* sdk; void (*function)(void*); void* argument; bool runnable; };
PortableThread portableThreads[16];
unsigned portableCount=0;
PortableThread* find(OSThread* sdk) {
    for(unsigned i=0;i<portableCount;++i) if(portableThreads[i].sdk==sdk) return &portableThreads[i];
    return nullptr;
}
}
void runPortableThreads() {
    // Measured, because the browser stops inside the frame boundary now: GX_GetVCount()
    // on wasm reaches this, and the print before it in GlobalGamePart::vf0c() is the
    // last one that speaks. So the boundary runs and does not come back. Naming which
    // part of it blocks.
    std::cout<<"  runPortableThreads: "+std::to_string(portableCount)+" threads\n";
    for(unsigned i=0;i<portableCount;++i) {
        auto& thread=portableThreads[i];
        if(thread.runnable&&thread.function) {
            std::cout<<"    thread "+std::to_string(i)+" running\n";
            thread.runnable=false;thread.function(thread.argument);
            std::cout<<"    thread "+std::to_string(i)+" returned\n";
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
