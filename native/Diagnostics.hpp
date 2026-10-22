#pragma once
// Runtime switches for "what does this stage cost on the machine that is actually
// dropping frames". The headless build and a real Metal browser disagree about the
// GPU, so this has to be toggled where the frames are slow; a keypress is cheaper
// than a rebuild and a reload, and pressing the same key again puts it back.
namespace pc::diag {
inline bool skipPresent = false;    // no recipe upload, no draw, no swap
inline bool skip3D = false;         // no 3D scene render at all
inline bool skipReadback = false;   // no glReadPixels of the 3D screen
inline bool skipUpload = false;     // keep the last recipe planes on the GPU
}
