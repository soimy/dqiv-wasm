// Portable stand-ins for the three platform files that are Windows-only upstream:
//   native/SaveStore.cpp      (CreateFileW / MoveFileExW / FlushFileBuffers)
//   native/NitroThreads.cpp   (Win32 threads; also defines the OS_* mutex API)
//   native/NitroIdentity.cpp  (adapter MAC address + machine identity)
//
// These are deliberately naive: they exist so that a wasm32 build links and runs,
// not to be the final implementation. The whole gap is ~55 lines, which is the
// honest size of the non-portable surface — see docs/porting/02-FINDINGS.md.
#include "SaveStore.hpp"
extern "C" {
#include <nitro/os/thread.h>
#include <nitro/os/mutex.h>
}
#include <algorithm>
#include <fstream>
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
void func_02079e24(unsigned char* destination) { for (int i = 0; i < 6; ++i) destination[i] = 0; }
}
