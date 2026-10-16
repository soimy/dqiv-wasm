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
extern "C" {
void OS_LockMutex(OSMutex*) {}
void OS_UnlockMutex(OSMutex*) {}
void OS_PauseThread(OSThreadQueue*) {}
void OS_UnpauseThread(OSThreadQueue*) {}
}

namespace pc {
struct ThreadRuntime {
    std::function<void()> idle;
};
NitroThreads::NitroThreads():impl_(std::make_unique<ThreadRuntime>()) {}
NitroThreads::~NitroThreads() = default;
void NitroThreads::checkErrors() {}
void NitroThreads::setIdleHandler(std::function<void()> handler) { impl_->idle=std::move(handler); }
void NitroThreads::runIdle() { if(impl_->idle) impl_->idle(); }

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
// overlay objects land in; there is no such mechanism here, so the registry
// starts empty and the game boots with no overlays. M3 is where this gets real.
std::span<const pc::OverlayImage> gameOverlayImages() { return {}; }

extern "C" void func_02079e24(unsigned char* destination) {
    if(!pc::currentIdentity||!destination) throw std::runtime_error("Device identity requested without its native profile");
    std::memcpy(destination,pc::currentIdentity->address().data(),6);
}
