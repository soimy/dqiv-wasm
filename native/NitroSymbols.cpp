// These two SDK entry points used the ARM compiler's literal C++ symbol names.
// Adapt their real implementations to the host C++ ABI.
struct UnkG2dSprite;
extern "C" void _Z16unkfunc_02068ec8i(unsigned int);
extern "C" void _Z16unkfunc_02068eecP12UnkG2dSprite(UnkG2dSprite*);
#ifdef _MSC_VER
// The adapter is needed here and only here. MSVC mangles these differently
// (?unkfunc_02068ec8@@YAXH@Z), so the game's call needs this bridge to reach the
// ARM spelling the SDK actually defines. On an Itanium-ABI host the C++ name
// already mangles to that same spelling, so keeping the adapter there would
// collide with the SDK definition it forwards to - wasm-ld says so, loudly.
void unkfunc_02068ec8(int attributes) { _Z16unkfunc_02068ec8i(static_cast<unsigned int>(attributes)); }
void unkfunc_02068eec(UnkG2dSprite* sprite) { _Z16unkfunc_02068eecP12UnkG2dSprite(sprite); }
#endif
