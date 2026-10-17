// The contexts the game's global constructors need, in a file that is listed
// first so clang's static-init order puts them first.
//
// Why this file exists: on MSVC these contexts live inside GameRun, and
// cmake/NativeOverlays.cmake gives every /main/ source a forced-include header
// with #pragma init_seg(".i38m", pcOverlayExit38). That pragma suppresses CRT
// startup construction, so the game's constructors - UnkDisplay's global object
// among them - run later, when the overlay registry loads overlay 38 inside
// main(), by which time GameRun has constructed the contexts.
//
// clang ignores #pragma init_seg, #pragma data_seg and #pragma bss_seg on wasm
// (measured: -Wunknown-pragmas), and it ignores __attribute__((init_priority))
// too (-Wignored-attributes). So on wasm the game's constructors run at startup
// and the contexts cannot be late - they have to be early instead. Link order is
// the only lever clang leaves us, and it does work: exe objects before library
// objects, and within a target in source order. Both measured, not assumed.
//
// This is the half of M3 that clang can do. The other half - real snapshot and
// restore of each overlay's writable image - needs the addresses, and the only
// way to get them is a two-pass link.
#include "NitroVideo.hpp"
#include "NitroPower.hpp"
#include "NitroOverlays.hpp"
#include "NitroIdentity.hpp"
#include "SaveStore.hpp"
#include <filesystem>

// Declared at global scope, exactly as GameBoot.cpp declares it - the definition
// in port-stubs.cpp is global too.
std::span<const pc::OverlayImage> gameOverlayImages();

namespace pc {

// Declaration order is construction order, so the paths come before the identity
// that is built from them. The extern is not decoration: a namespace-scope const
// has internal linkage in C++, so without it these two would be invisible to
// GameBoot.cpp and the link would fail.
extern const std::filesystem::path earlySavePath;
extern const std::filesystem::path earlyIdentityPath;
const std::filesystem::path earlySavePath{defaultSavePath()};
const std::filesystem::path earlyIdentityPath{earlySavePath.string() + ".identity"};
NitroIdentity earlyIdentity{earlyIdentityPath};
NitroVideo earlyVideo;
NitroPower earlyPower;
NitroOverlays earlyOverlays{gameOverlayImages()};

}   // namespace pc
