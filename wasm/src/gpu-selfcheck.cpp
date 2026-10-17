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
#include "GlLoader.hpp"
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
    gpuTrace=true;   // report each GL error at the call that left it behind
    // --- byte order -------------------------------------------------------
    // Recipe planes, 3 planes of 256 x 384 plus the inset's 192 rows, so 704
    // rows and 4 planes. Plane 0 holds the final colour when the kind (its
    // alpha) is 0, which is the one path through the compose shader that needs
    // nothing else: no 3D layer, no brightness, no high-resolution picture.
    //
    // The kind has to be 0, so the top byte of each test word is fixed and only
    // the lower three are free. The first run in a real browser used words with
    // arbitrary top bytes and all four came back black - a non-zero kind routes
    // to the blend path, and the blend returns black when the other planes are
    // empty. That is exactly what this check is for, so it has to be exact.
    //
    // What the round trip should give. Measured in a real browser, and it came
    // back as the identity - the lower three bytes unchanged, the alpha forced to
    // 0xff by the shader - which is what it should be: uploadOrder is identity on
    // wasm, kUploadFormat is GL_RGBA there, and readPresented's RGBA-to-ARGB
    // conversion is identity too. So this is not a check that bytes are swapped;
    // it is a check that the three of them agree, which is the real question, and
    // four distinct patterns through upload, texture, shader and readback is the
    // way to ask it.
    std::vector<std::uint32_t> recipes(std::size_t(256)*704*4,0);
    constexpr std::uint32_t want[4]={0x00112233,0x00445566,0x00778899,0x00aabbcc};
    constexpr std::uint32_t expect[4]={0xff112233,0xff445566,0xff778899,0xffaabbcc};
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
            if(got!=expect[i]) {order=false;std::printf("selfcheck byte order: in %08x, want %08x, got %08x\n",want[i],expect[i],got);}
        }
    } else std::printf("selfcheck byte order: readback came back %dx%d\n",width,height);
    std::printf("selfcheck byte order: %s\n",order?"ok":"FAILED");

    // --- clip ---------------------------------------------------------------
    // The clear colour is 0, so anything lit is a pixel the triangle drew.
    GxRasterState state;
    GxRenderOptions options;
    // The answer needs a probe inside the loop, not more reading. Report the GL
    // error after each cycle: if the draw is failing, something will be left set.
    // Report every error the cycle left behind, in order, not just the last one:
    // 0x502 is GL_INVALID_FRAMEBUFFER_OPERATION, and which call raises it is the
    // difference between "the draw fails" and "the readback fails".
    auto err=[&]() {
        unsigned last=0,e;
        while((e=gl::GetError())) {std::printf("  gl error 0x%x\n",e);last=e;}
        return last;
    };
    auto lit=[&](float centre) {
        std::array<GxTriangle,1> t;t[0].vertices=triangle(centre);
        std::array<GpuBatch,1> b;b[0].triangles=std::span<const GxTriangle>(t.data(),1);
        window.gpu().render3D(std::span<const GpuBatch>(b.data(),1),state,textures,options,256,192);
        std::vector<std::uint32_t> pixels;window.gpu().readNative(pixels,true);
        unsigned n=0;for(auto p:pixels)if(p!=0u)++n;
        std::printf("selfcheck lit: centre %+.1f -> %u lit, gl error 0x%x\n",centre,n,err());
        return n;
    };
    const unsigned inside=lit(0.f),straddling=lit(1.f),outside=lit(1.5f);
    // Two more points, because the first run in a real browser put the straddling
    // case at 0 and that is not what the geometry says. If the count falls off
    // smoothly from 0 to 1.5 the clip emulation is fine and my expectation was
    // the thing that was wrong; if it goes to zero the moment any vertex leaves
    // the screen, the triangle is being culled whole.
    const unsigned midA=lit(0.5f),midB=lit(0.8f);
    // And the same triangle again. Everything about centre 0 is inside the clip
    // region, so if this second one comes back 0 the problem is not the clip at
    // all - it is that only the first render3D call in the sequence draws.
    const unsigned again=lit(0.f);
    // Decisive version: ask for the straddling triangle first and the inside one
    // second. If the counts swap, only the first call in the sequence draws and
    // the clip comparison cannot work as written.
    const unsigned firstStraddling=lit(1.f),secondInside=lit(0.f);
    std::printf("selfcheck clip gradient: 0->%u, 0.5->%u, 0.8->%u, 1.0->%u, 1.5->%u, again-0->%u\n",
        inside,midA,midB,straddling,outside,again);
    std::printf("selfcheck clip swapped: straddling-first %u, inside-second %u\n",
        firstStraddling,secondInside);
    // Which of the two is broken: render3D, or the readback after it? Two
    // triangles in ONE call, in bands that do not overlap, so the count should be
    // double if drawing works and single if only the first thing in the process
    // ever lands.
    auto litTwo=[&]() {
        std::array<GxTriangle,2> t;t[0].vertices=triangle(0.f);t[1].vertices=triangle(-0.6f);
        std::array<GpuBatch,1> b;b[0].triangles=std::span<const GxTriangle>(t.data(),2);
        window.gpu().render3D(std::span<const GpuBatch>(b.data(),1),state,textures,options,256,192);
        std::vector<std::uint32_t> pixels;window.gpu().readNative(pixels,true);
        unsigned n=0;for(auto p:pixels)if(p!=0u)++n;
        std::printf("selfcheck lit: two triangles -> %u lit, gl error 0x%x\n",n,err());
        return n;
    };
    const unsigned two=litTwo();
    std::printf("selfcheck clip two-in-one-call: %u (expect double of the single count)\n",two);
    // Decisive: which is the rule - "the first render3D call in the process
    // draws" - or something about the geometry? Do the two-triangle call FIRST,
    // then the single one. If the counts swap, the rule is confirmed.
    const unsigned twoFirst=litTwo();
    const unsigned singleSecond=lit(0.f);
    std::printf("selfcheck clip order swapped: two-in-one-call-first %u, single-second %u\n",
        twoFirst,singleSecond);
    // Which half raises it: read twice with nothing drawn in between. If the
    // second read is clean, the readback path is fine and the draw is what fails.
    {
        std::vector<std::uint32_t> a,b;
        window.gpu().readNative(a,true);
        std::printf("  read 1: %u lit, gl error 0x%x\n",[&]{unsigned n=0;for(auto p:a)if(p!=0u)++n;return n;}(),err());
        window.gpu().readNative(b,true);
        std::printf("  read 2: %u lit, gl error 0x%x\n",[&]{unsigned n=0;for(auto p:b)if(p!=0u)++n;return n;}(),err());
    }
    const bool clip=inside>straddling&&straddling>0&&outside==0;
    std::printf("selfcheck clip: inside %u, straddling %u, outside %u -> %s\n",
        inside,straddling,outside,clip?"ok":"FAILED");
}
}
