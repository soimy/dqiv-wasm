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
#include "NitroVideo.hpp"
#include "Display.hpp"
#include <cstdio>
#include <exception>

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

    // --- compose sources ---------------------------------------------------
    // Stage B1 of docs/porting/07-GPU-COMPOSITION.md: the display engine's inputs
    // go up as textures, and the composition that will read them trusts one
    // layout - 1024 texels to a row, four rows per 4 KB block of VRAM, one 16-bit
    // entry per texel of palette RAM and OAM. A known pattern goes in through the
    // write path the game itself uses, and the texel the shader reads has to be
    // the byte that was written. A layout error here would surface much later as
    // a scrambled screen, inside a stage whose own correctness is being judged,
    // which is the worst place to find it.
    try {
        auto& video=activeVideo();
        // VRAM bank A's control byte (not DISPCNT): mode 1 maps it for the LCDC
        // at 0x06000000, which is what a write needs to go through; the game
        // sets the same thing up itself a moment later.
        auto* control=static_cast<std::uint8_t*>(video.reg(0x04000240,1));
        const std::uint8_t controlBefore=*control;
        *control=0x81u;
        const std::array<std::uint8_t,4> head{0x11,0x22,0x33,0x44};
        video.write(0x06000000,head);
        // Across the end of the first block: offset 0xffc is x 1020..1023 of row
        // 3, then x 0..3 of row 4 - two blocks, so two uploads. This is the one
        // place the row/block arithmetic can be wrong without any other probe
        // noticing.
        const std::array<std::uint8_t,8> edge{0xa0,0xa1,0xa2,0xa3,0xb0,0xb1,0xb2,0xb3};
        video.write(0x06000ffc,edge);
        *control=controlBefore;
        const std::array<std::uint8_t,8> entries{0x21,0x43,0x65,0x87,0xa9,0xcb,0xed,0x0f};
        video.write(0x05000000,entries);
        video.write(0x07000000,entries);
        window.gpu().uploadDisplaySources(video);

        auto compile=[&](unsigned type,const char* source) {
            const unsigned shader=gl::CreateShader(type);
            gl::ShaderSource(shader,1,&source,nullptr);gl::CompileShader(shader);
            int ok=0;gl::GetShaderiv(shader,GL_COMPILE_STATUS,&ok);
            if(!ok) {
                char log[512]={0};gl::GetShaderInfoLog(shader,511,nullptr,log);
                std::printf("selfcheck compose sources: shader %u did not compile: %s\n",type,log);
            }
            return shader;
        };
        const char* vertexSource="#version 300 es\n"
            "void main(){vec2 p=vec2((gl_VertexID==1||gl_VertexID==3)?1.0:-1.0,(gl_VertexID<2)?-1.0:1.0);"
            "gl_Position=vec4(p,0.0,1.0);}\n";
        const char* fragmentSource="#version 300 es\n"
            "precision highp float;precision highp usampler2D;\n"
            "uniform usampler2D uVram;uniform usampler2D uPalette;uniform usampler2D uOam;\n"
            "uniform int uWhich;uniform int uIndex;uniform float uExpect;out vec4 oColor;\n"
            "void main(){uint got=uWhich==0?texelFetch(uVram,ivec2(uIndex&1023,uIndex>>10),0).r:"
            "uWhich==1?texelFetch(uPalette,ivec2(uIndex,0),0).r:texelFetch(uOam,ivec2(uIndex,0),0).r;\n"
            "oColor=got==uint(uExpect+0.5)?vec4(0.0,1.0,0.0,1.0):vec4(1.0,0.0,0.0,1.0);}\n";
        const unsigned program=gl::CreateProgram();
        const unsigned vertex=compile(GL_VERTEX_SHADER,vertexSource),fragment=compile(GL_FRAGMENT_SHADER,fragmentSource);
        gl::AttachShader(program,vertex);gl::AttachShader(program,fragment);gl::LinkProgram(program);
        {
            int ok=0;gl::GetProgramiv(program,GL_LINK_STATUS,&ok);
            if(!ok) {
                char log[512]={0};gl::GetProgramInfoLog(program,511,nullptr,log);
                std::printf("selfcheck compose sources: program did not link: %s\n",log);
            }
        }
        unsigned framebuffer=0,color=0;
        gl::GenFramebuffers(1,&framebuffer);gl::BindFramebuffer(GL_FRAMEBUFFER,framebuffer);
        gl::GenTextures(1,&color);gl::BindTexture(GL_TEXTURE_2D,color);
        gl::TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        gl::TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        gl::TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        gl::FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,color,0);
        gl::Viewport(0,0,1,1);
        gl::UseProgram(program);
        // No attributes at all: the vertex shader builds its corners from
        // gl_VertexID, so an empty array object is enough.
        unsigned array=0;gl::GenVertexArrays(1,&array);gl::BindVertexArray(array);
        gl::ActiveTexture(GL_TEXTURE0);gl::BindTexture(GL_TEXTURE_2D,window.gpu().vramTexture());
        gl::ActiveTexture(GL_TEXTURE1);gl::BindTexture(GL_TEXTURE_2D,window.gpu().paletteTexture());
        gl::ActiveTexture(GL_TEXTURE2);gl::BindTexture(GL_TEXTURE_2D,window.gpu().oamTexture());
        gl::Uniform1i(gl::GetUniformLocation(program,"uVram"),0);
        gl::Uniform1i(gl::GetUniformLocation(program,"uPalette"),1);
        gl::Uniform1i(gl::GetUniformLocation(program,"uOam"),2);
        struct Probe {int which,index;unsigned expect;};
        // VRAM block 0's head and its edges, then palette and OAM entries: a
        // wrong layout shows up as the wrong byte at (1023,3)/(0,4).
        // Index is x + y*1024, so the block edge is 4095 (x 1023, row 3) and
        // 4096.. (row 4, x 0..): the arithmetic the shader and the uploader
        // have to agree on.
        const Probe probes[]={{0,0,0x11},{0,1,0x22},{0,4095,0xa3},{0,4096,0xb0},{0,4099,0xb3},
            {1,0,0x4321},{1,3,0x0fed},{2,1,0x8765}};
        bool sources=true;
        for(const auto& probe:probes) {
            gl::Uniform1i(gl::GetUniformLocation(program,"uWhich"),probe.which);
            gl::Uniform1i(gl::GetUniformLocation(program,"uIndex"),probe.index);
            gl::Uniform1f(gl::GetUniformLocation(program,"uExpect"),float(probe.expect));
            gl::DrawArrays(GL_TRIANGLE_STRIP,0,4);
            std::uint8_t pixel[4]={0,0,0,0};
            gl::ReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
            if(pixel[1]<200) {
                sources=false;
                std::printf("selfcheck compose sources: source %d index %d, want %04x, texel is not that\n",
                    probe.which,probe.index,probe.expect);
            }
        }
        gl::BindFramebuffer(GL_FRAMEBUFFER,0);
        gl::ActiveTexture(GL_TEXTURE0);
        gl::DeleteFramebuffers(1,&framebuffer);gl::DeleteTextures(1,&color);gl::DeleteProgram(program);
        gl::DeleteVertexArrays(1,&array);
        std::printf("selfcheck compose sources: %s\n",sources?"ok":"FAILED");
    } catch(const std::exception& error) {
        // A check that takes the game down at boot is worse than no check.
        std::printf("selfcheck compose sources: threw %s\n",error.what());
    }

    // --- gpu compose -------------------------------------------------------
    // Stage B2: one crafted 2D scene through Display (the reference on the CPU)
    // and through the GPU composition. A text background with a scrolled map,
    // sixteen palette banks, alpha blending onto the backdrop and a master
    // brightness exercises the addressing, the priority order, the six-bit
    // blend arithmetic and the brightness at once - and any of them wrong shows
    // as a colour that differs by one.
    try {
        auto& video=activeVideo();
        auto* bank=static_cast<std::uint8_t*>(video.reg(0x04000240,1));
        const std::uint8_t bankBefore=*bank;*bank=0x81;   // bank A at 0x06000000
        auto* control=static_cast<std::uint32_t*>(video.reg(0x04000000,4));
        auto* bg0=static_cast<std::uint16_t*>(video.reg(0x04000008,2));
        auto* hofs=static_cast<std::uint16_t*>(video.reg(0x04000010,2));
        auto* vofs=static_cast<std::uint16_t*>(video.reg(0x04000012,2));
        auto* bldcnt=static_cast<std::uint16_t*>(video.reg(0x04000050,2));
        auto* bldalpha=static_cast<std::uint16_t*>(video.reg(0x04000052,2));
        auto* master=static_cast<std::uint16_t*>(video.reg(0x0400006c,2));
        const unsigned controlBefore=*control,bg0Before=*bg0,hofsBefore=*hofs,vofsBefore=*vofs,
            bldcntBefore=*bldcnt,bldalphaBefore=*bldalpha,masterBefore=*master;
        std::vector<std::uint8_t> vram(0x8000,0);
        // Four 4bpp tiles at char base 0; index 0 occurs, so transparency is real.
        for(unsigned t=0;t<4;++t)for(unsigned r=0;r<8;++r)for(unsigned x=0;x<8;x+=2) {
            const unsigned lo=(t*5+r*3+x)%16,hi=(t*3+r*7+x+1)%16;
            vram[t*32+r*4+x/2]=std::uint8_t(lo|(hi<<4));
        }
        // Screen map at screen base 8 (0x4000): tiles and palette banks change
        // every two columns, so a wrong map stride is visible.
        for(unsigned my=0;my<32;++my)for(unsigned mx=0;mx<32;++mx) {
            const unsigned entry=((mx/2+my)%4)|((((mx+my/2)%16))<<12);
            vram[0x4000+(my*32+mx)*2]=std::uint8_t(entry);
            vram[0x4000+(my*32+mx)*2+1]=std::uint8_t(entry>>8);
        }
        video.write(0x06000000,vram);
        // Engine A's background palettes start at 0x05000000 and entry 0 is the
        // backdrop as well; entries 1.. are the sixteen-colour banks.
        std::vector<std::uint8_t> bgPalette(0x200,0);
        {
            const unsigned red=0x001f;
            bgPalette[0]=std::uint8_t(red);bgPalette[1]=std::uint8_t(red>>8);
        }
        for(unsigned i=1;i<256;++i) {
            const unsigned c=((i*7)&31)|(((i*3)&31)<<5)|(((i*11)&31)<<10);
            bgPalette[i*2]=std::uint8_t(c);bgPalette[i*2+1]=std::uint8_t(c>>8);
        }
        video.write(0x05000000,bgPalette);
        *control=0x00010100u;   // display mode 1, mode 0 (text), BG0 on
        *bg0=0x0800;            // priority 0, char base 0, 4bpp, screen base 8
        *hofs=3;*vofs=5;
        *bldcnt=0x2041;         // BG0 first target, alpha, backdrop second
        *bldalpha=0x0808;
        *master=0x4004;         // brightness up by 4/16
        auto run=[&](const char* label,unsigned blend) {
            *bldcnt=static_cast<std::uint16_t>(blend);*master=static_cast<std::uint16_t>(blend?0x4004:0);
            window.gpu().uploadDisplaySources(video);
            Display reference;
            reference.frame(video,{},true);
            std::string why;
            const bool composed=window.gpu().composeScreen(video,false,why);
            if(!composed) {std::printf("  gpu compose: %s not composed: %s\n",label,why.c_str());return 0u;}
            const auto gpu=window.gpu().readScreen(false);
            const auto cpu=reference.pixels();
            const std::size_t at=reference.mainOnTop()?0:std::size_t(256)*192;
            unsigned different=0;
            for(std::size_t i=0;i<std::size_t(256)*192;++i) {
                if(gpu[i]!=cpu[at+i]) {
                    if(different<3)std::printf("  gpu compose: %s (%zu,%zu) gpu %08x cpu %08x\n",label,i%256,i/256,gpu[i],cpu[at+i]);
                    ++different;
                }
            }
            return different;
        };
        const unsigned blended=run("blend",0x2041),plain=run("plain",0);
        const bool same=blended==0&&plain==0;
        std::printf("selfcheck gpu compose: %s (blend %u, plain %u of %u differ)\n",
            same?"ok":"FAILED",blended,plain,256u*192u);
        *control=controlBefore;*bg0=bg0Before;*hofs=hofsBefore;*vofs=vofsBefore;
        *bldcnt=bldcntBefore;*bldalpha=bldalphaBefore;*master=masterBefore;*bank=bankBefore;
    } catch(const std::exception& error) {
        std::printf("selfcheck gpu compose: threw %s\n",error.what());
    }
}
}
