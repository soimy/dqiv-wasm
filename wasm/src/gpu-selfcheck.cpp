// The first thing the ES 3.0 renderer does on a real GPU, and the two things
// that changed on the way there, checked against known answers.
//
//   byte order - WebGL2 has no BGRA format for texture uploads, so
//   native/PixelOrder.hpp swizzles the game's 0xAARRGGBB words into RGBA before
//   the upload. A known recipe word goes in and the same word has to come back.
//   That is uploadOrder -> the upload -> the ES compose shader -> ReadPixels ->
//   fromReadback: the whole path, end to end, not one link of it.
//
//   clip - GLSL ES 3.00 has no gl_ClipDistance, so the four DS clip values ride
//   down as a varying and the fragment shader discards. A triangle wholly inside
//   the screen draws, one wholly outside does not, and one straddling the edge
//   draws part of itself.
//
// What this can and cannot show. The four clip values for w = 1 are exactly the
// normalised device coordinates, so a triangle outside them would be clipped by
// the hardware anyway and the straddling one gives the same answer either way.
// The change is about determinism where a triangle straddles the edge - the
// hardware built-in's interpolation is explicitly implementation-dependent, a
// varying is perspective-correct - so this is a parity check against the desktop
// path, not a demonstration that the varying is what did it. It is honest as a
// check that the ES path produces the picture the desktop path produces.
//
// There is no browser automation in this project, so this is one line in the
// console for a person to read when they open the page. wasm only: it is not
// registered in cmake/Sources.cmake, so the MSVC build does not see it and its
// warning distribution stays exactly what it was.
#include "GameWindow.hpp"
#include "GxTextures.hpp"
#include "GxRasterizer.hpp"
#include <cstdio>

namespace pc {
namespace {
// A triangle in the clip space the shaders see. w = 1, so the four clip values
// are w+x, w-x, w+y and w-y: the DS screen's own edges.
std::array<GxVertex,3> triangle(float centre) {
    std::array<GxVertex,3> out;
    const float x[3]={centre-0.3f,centre+0.3f,centre},y[3]={-0.3f,-0.3f,0.3f};
    for(unsigned i=0;i<3;++i) {out[i].clip={x[i],y[i],0.f,1.f};out[i].color=0x7fff;}
    return out;
}
}

void gpuSelfCheck(GameWindow& window,GxTextures& textures) {
    // --- byte order -------------------------------------------------------
    // Recipe planes, 3 planes of 256 x 384 plus the inset's 192 rows, so 704
    // rows and 4 planes. Plane 0 holds the final colour when the kind (its
    // alpha) is 0, which is the one path through the compose shader that needs
    // nothing else: no 3D layer, no brightness, no high-resolution picture.
    std::vector<std::uint32_t> recipes(std::size_t(256)*704*4,0);
    constexpr std::uint32_t want[4]={0x11223344,0x55667788,0x99aabbcc,0xdeadbeef};
    for(unsigned i=0;i<4;++i)recipes[std::size_t(191)*256+128+i]=want[i];
    GpuLayout layout;
    layout.threeRects={{GpuLayout::Rect{0,0,0,0},GpuLayout::Rect{0,0,0,0}}};
    // A window exactly the size of the canvas: the scale is then 1, the whole
    // canvas is the viewport, and the fragment at the centre of pixel (128,192)
    // is the one that reads this texel. readPresented returns rows top-down, so
    // row 191 is the one the shader reached through p.y = 191.5.
    window.gpu().present(layout,recipes,256,384,false);
    int width=0,height=0;
    auto back=window.gpu().readPresented(width,height);
    bool order=false;
    if(width==256&&height==384&&back.size()==std::size_t(256)*384) {
        order=true;
        for(unsigned i=0;i<4;++i) {
            const auto got=back[std::size_t(191)*256+128+i];
            if(got!=want[i]) {order=false;std::printf("selfcheck byte order: in %08x, out %08x\n",want[i],got);}
        }
    } else std::printf("selfcheck byte order: readback came back %dx%d\n",width,height);
    std::printf("selfcheck byte order: %s\n",order?"ok":"FAILED");

    // --- clip ---------------------------------------------------------------
    // The clear colour is 0, so anything lit is a pixel the triangle drew.
    GxRasterState state;
    GxRenderOptions options;
    auto lit=[&](float centre) {
        std::array<GxTriangle,1> t;t[0].vertices=triangle(centre);
        std::array<GpuBatch,1> b;b[0].triangles=std::span<const GxTriangle>(t.data(),1);
        window.gpu().render3D(std::span<const GpuBatch>(b.data(),1),state,textures,options,256,192);
        std::vector<std::uint32_t> pixels;window.gpu().readNative(pixels,true);
        unsigned n=0;for(auto p:pixels)if(p!=0u)++n;
        return n;
    };
    const unsigned inside=lit(0.f),straddling=lit(1.f),outside=lit(1.5f);
    const bool clip=inside>straddling&&straddling>0&&outside==0;
    std::printf("selfcheck clip: inside %u, straddling %u, outside %u -> %s\n",
        inside,straddling,outside,clip?"ok":"FAILED");
}
}
