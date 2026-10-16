// These two SDK entry points were listed as needing adaptation to the host C++
// ABI. They do not. The host mangling of unkfunc_02068ec8(int) and
// unkfunc_02068eec(UnkG2dSprite*) is byte-for-byte the name the SDK already
// uses - _Z16unkfunc_02068ec8i and _Z16unkfunc_02068eecP12UnkG2dSprite - so the
// wrappers named the very symbol they were adapting, and each one called itself.
//
// On MSVC that stayed invisible: the wrapper object sits in the executable, which
// is always part of the link, while the SDK object sits in a static library that
// nothing else needed, so the call resolved to the wrapper and the real
// implementation in g2d_softsprite.c was never reached. wasm-ld does pull that
// object, so the collision came out as a duplicate-symbol link error instead.
//
// The wrappers are gone and both backends now reach the real code. This file is
// the marker for what used to be here; it has no code in it.
struct UnkG2dSprite;
