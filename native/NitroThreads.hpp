#pragma once
#include <functional>
#include <memory>
namespace pc {
struct ThreadRuntime;
class NitroThreads {
public:
    NitroThreads();
    ~NitroThreads();
    NitroThreads(const NitroThreads&)=delete;
    NitroThreads& operator=(const NitroThreads&)=delete;
    void checkErrors();
    void setIdleHandler(std::function<void()> handler);
#ifdef __EMSCRIPTEN__
    // A single-threaded host has no thread to run the idle handler on, so the
    // startup code calls this once per frame instead. That is the shape the wasm
    // frame loop has to take, and it is why this milestone is a redesign and not
    // just a re-link.
    void runIdle();
#endif
private:
    std::unique_ptr<ThreadRuntime> impl_;
};
unsigned suspendNativeInterrupts();
void resumeNativeInterrupts(unsigned depth);
#ifdef __EMSCRIPTEN__
// Single-threaded host: the threads the SDK creates are registered, not spawned,
// and this runs the ones marked runnable. The frame loop calls it once per frame,
// which is what makes the sound system's streaming thread work here.
void runPortableThreads();
#endif
}
