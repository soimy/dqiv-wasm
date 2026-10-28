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
    // as a colour that differs by one. The affine runs add the matrix, the
    // 8-bit and 16-bit maps, the flip bits, the overflow wrap bit and - for
    // engine B - the other VRAM window and palette block.
    try {
        auto& video=activeVideo();
        auto* bankA=static_cast<std::uint8_t*>(video.reg(0x04000240,1));
        auto* bankC=static_cast<std::uint8_t*>(video.reg(0x04000242,1));
        const std::uint8_t bankABefore=*bankA,bankCBefore=*bankC;
        *bankA=0x81;   // bank A at 0x06000000
        auto reg16=[&](unsigned address) {return static_cast<std::uint16_t*>(video.reg(address,2));};
        auto reg32=[&](unsigned address) {return static_cast<std::uint32_t*>(video.reg(address,4));};
        auto* control=reg32(0x04000000);
        auto* bg0=reg16(0x04000008);
        auto* bg3=reg16(0x0400000e);
        auto* hofs=reg16(0x04000010);
        auto* vofs=reg16(0x04000012);
        auto* bldcnt=reg16(0x04000050);
        auto* bldalpha=reg16(0x04000052);
        auto* master=reg16(0x0400006c);
        auto* subControl=reg32(0x04001000);
        auto* subBg3=reg16(0x0400100e);
        const unsigned controlBefore=*control,bg0Before=*bg0,bg3Before=*bg3,hofsBefore=*hofs,vofsBefore=*vofs,
            bldcntBefore=*bldcnt,bldalphaBefore=*bldalpha,masterBefore=*master,
            subControlBefore=*subControl,subBg3Before=*subBg3;
        std::array<std::uint16_t,12> affineBefore{},subAffineBefore{};
        for(unsigned i=0;i<12;++i) {
            affineBefore[i]=*reg16(0x04000030+i*2);
            subAffineBefore[i]=*reg16(0x04001030+i*2);
        }
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
        auto run=[&](const char* label,unsigned blend,bool subEngine=false,unsigned brightness=0x4004) {
            *bldcnt=std::uint16_t(blend);*master=std::uint16_t(blend?brightness:0);
            window.gpu().uploadDisplaySources(video);
            Display reference;
            reference.frame(video,{},true);
            std::string why;
            const bool composed=window.gpu().composeScreen(video,subEngine,reference,why);
            // A scene this stage refuses is a failed run, not a quiet pass: every
            // case here is one it says it handles.
            if(!composed) {
                std::printf("  gpu compose: %s not composed: %s\n",label,why.c_str());
                return 256u*192u;
            }
            const auto gpu=window.gpu().readScreen(subEngine);
            const auto cpu=reference.pixels();
            const std::size_t at=(((subEngine?1:0)==0)==reference.mainOnTop())?0:std::size_t(256)*192;
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
        // --- affine backgrounds ------------------------------------------------
        // Tiles are 256 colours; the map is one byte per tile in modes 1/2/4
        // (8-bit) or a 16-bit entry with flip bits in modes 3/4/5. Engine B
        // reads its own window (0x06200000) and palette block (0x05000400).
        auto tiles=[](unsigned count) {
            std::vector<std::uint8_t> chars(std::size_t(count)*64);
            for(unsigned t=0;t<count;++t)for(unsigned p=0;p<64;++p)
                chars[std::size_t(t)*64+p]=std::uint8_t((t+p)%9==0?0:((p*29+t*37+1)&255));
            return chars;
        };
        std::vector<std::uint8_t> map16(32*32*2);
        for(unsigned my=0;my<32;++my)for(unsigned mx=0;mx<32;++mx) {
            unsigned entry=(mx*5+my*3)%16;
            if(((mx+2*my)&7)==0)entry|=0x400;    // horizontal flip
            if(((3*mx+my)&7)==0)entry|=0x800;    // vertical flip
            map16[(my*32+mx)*2]=std::uint8_t(entry);
            map16[(my*32+mx)*2+1]=std::uint8_t(entry>>8);
        }
        std::vector<std::uint8_t> map8(256);
        for(unsigned i=0;i<256;++i)map8[i]=std::uint8_t((i*5+(i/16)*3)%16);
        auto setAffine=[&](unsigned at,int pa,int pb,int pc,int pd,int refx,int refy) {
            *reg16(at)=std::uint16_t(pa);*reg16(at+2)=std::uint16_t(pb);
            *reg16(at+4)=std::uint16_t(pc);*reg16(at+6)=std::uint16_t(pd);
            *reg32(at+8)=std::uint32_t(refx);*reg32(at+12)=std::uint32_t(refy);
        };
        const auto chars=tiles(16);
        // Char base 1 and map base 1 are DISPCNT bits 24-26 and 27-29, so this
        // also checks that the 32-bit register reaches the shader unharmed.
        video.write(0x06010000,chars);
        video.write(0x06018000,map16);
        *control=0x09010803u;   // display mode 1, BG mode 3, BG3 on, bases above
        *bg3=0x5002;            // priority 2, extended map at 16, 256x256, no wrap
        setAffine(0x04000030,0x00c0,0x0040,-0x0040,0x00c0,0x4000,0x2000);
        const unsigned affine16=run("affine16",0);
        // Mode 1 makes BG3 an 8-bit-map affine layer: here it wraps a doubled
        // 128x128 map, so negative source coordinates are masked rather than
        // dropped.
        video.write(0x06018000,map8);
        *control=0x09010801u;
        *bg3=0x3001;            // priority 1, map at 16, wrap, 128x128
        setAffine(0x04000030,0x0200,0,0,0x0200,0x0100,0x0100);
        const unsigned affine8=run("affine8",0);
        // Engine B: bank C at 0x06200000, its own palette block, its own matrix.
        *bankC=0x84;
        video.write(0x06200000,chars);
        video.write(0x06208000,map16);
        {
            std::vector<std::uint8_t> palette(0x200,0);
            const unsigned blue=0x7c00;palette[0]=std::uint8_t(blue);palette[1]=std::uint8_t(blue>>8);
            for(unsigned i=1;i<256;++i) {
                const unsigned c=((i*11)&31)|(((i*5)&31)<<5)|(((i*13)&31)<<10);
                palette[i*2]=std::uint8_t(c);palette[i*2+1]=std::uint8_t(c>>8);
            }
            video.write(0x05000400,palette);
        }
        *subControl=0x00010803u;  // display mode 1, BG mode 3, BG3 on
        *subBg3=0x7000;           // priority 0, map at 16, wrap, 256x256
        setAffine(0x04001030,0x00a0,-0x0050,0x0050,0x00a0,0x3000,0x2800);
        const unsigned affineSub=run("affine-sub",0,true);
        // --- sprites ----------------------------------------------------------
        // Objects are the fifth layer and come before a background at the same
        // priority. This scene puts a solid 4bpp object over BG1, a
        // semi-transparent one at BG1's own priority (so the blend has to reach
        // the layer below), one that wraps round the corner of the screen, one
        // flipped both ways, one 8bpp 32x32 one, a later object at the first
        // one's priority (the lower id has to win), and one with the double-size
        // bit but no affine flag, which Display::objects skips whole.
        std::vector<std::uint8_t> oamBefore(0x800,0);
        video.read(0x07000000,oamBefore);
        auto* bg1=reg16(0x0400000a);
        const unsigned bg1Before=*bg1;
        auto* bankB=static_cast<std::uint8_t*>(video.reg(0x04000241,1));
        auto* bankD=static_cast<std::uint8_t*>(video.reg(0x04000243,1));
        const std::uint8_t bankBBefore=*bankB,bankDBefore=*bankD;
        *bankB=0x82;   // bank B at 0x06400000: the object region of engine A
        // Sprite characters: a pattern in which zeros do occur, so transparency
        // is real, and in which both 4bpp nibbles and 8bpp bytes are varied.
        std::vector<std::uint8_t> chars2(0x4000,0);
        for(std::size_t i=0;i<chars2.size();++i)chars2[i]=std::uint8_t((i*37+11)&255);
        video.write(0x06400000,chars2);
        // Engine A's object palette is entries 0x100..0x1ff, the sixteen banks
        // of a 4bpp object and every colour of an 8bpp one.
        std::vector<std::uint8_t> objPalette(0x200,0);
        for(unsigned i=0;i<256;++i) {
            const unsigned c=(i*7+1)&31|((i*5+3)&31)<<5|((i*3+2)&31)<<10;
            objPalette[i*2]=std::uint8_t(c);objPalette[i*2+1]=std::uint8_t(c>>8);
        }
        video.write(0x05000200,objPalette);
        // An object's three attributes: place and mode, character and flips,
        // palette bank and priority.
        auto sprite=[](std::vector<std::uint8_t>& oam,unsigned id,unsigned y,int x,unsigned shape,
            unsigned size,bool eight,unsigned mode,unsigned tile,unsigned bank,unsigned priority,
            bool hflip=false,bool vflip=false) {
            const unsigned attributes[3]={shape<<14|(y&255)|(mode<<10)|(eight?0x2000u:0u),
                size<<14|(hflip?0x1000u:0u)|(vflip?0x2000u:0u)|(unsigned(x)&511),
                (bank<<12)|(priority<<10)|tile};
            for(unsigned i=0;i<3;++i) {
                oam[id*8+i*2]=std::uint8_t(attributes[i]&255);
                oam[id*8+i*2+1]=std::uint8_t((attributes[i]>>8)&255);
            }
        };
        std::vector<std::uint8_t> oam(0x800,0);
        sprite(oam,0,40,32,0,1,false,0,4,3,0);
        sprite(oam,1,40,64,0,1,false,1,6,5,1);                    // semi-transparent
        sprite(oam,2,188,250,0,0,false,0,20,2,1);                 // wraps at the corner
        sprite(oam,3,100,100,0,1,false,0,22,4,2,true,true);       // flipped both ways
        sprite(oam,4,60,150,0,2,true,0,8,0,3);                    // 32x32, 256 colours
        sprite(oam,5,0,0,0,3,false,0,0,0,0);                      // double size, no affine
        oam[5*8+1]=0x02;                                          // ...and bit 9: skipped
        sprite(oam,6,44,70,0,0,false,0,24,6,1);                   // loses to id 1
        video.write(0x07000000,oam);
        *control=0x00111200u;   // display mode 1, BG mode 0, BG1 and objects on
        *bg1=0x0801;            // priority 1, char base 0, 4bpp, screen base 8
        *bldalpha=0x0808;
        // BG1 and the objects are both first and second targets, so the
        // semi-transparent object blends and the others are left alone.
        const unsigned objDiff=run("obj",0x1252,false,0);
        const auto objectOn=window.gpu().readScreen(false);
        // A scene whose objects did not draw at all would compare equal to any
        // composition, so the same scene is composed again with the object layer
        // off: the pixels that change are the ones the sprites cover.
        *control=0x00110200u;   // the same, objects off
        const unsigned objOffDiff=run("obj-off",0x1252,false,0);
        const auto objectOff=window.gpu().readScreen(false);
        *control=0x00111200u;
        unsigned objectPixels=0;
        for(std::size_t i=0;i<objectOn.size();++i)objectPixels+=objectOn[i]!=objectOff[i];
        // The one-dimensional character map puts a sprite's tiles in a row and
        // lifts them to the object character base of DISPCNT bits 20-21.
        std::vector<std::uint8_t> chars1d(0x2000,0);
        for(std::size_t i=0;i<chars1d.size();++i)chars1d[i]=std::uint8_t((i*29+7)&255);
        video.write(0x06410000,chars1d);
        std::vector<std::uint8_t> oam1d(0x800,0);
        sprite(oam1d,0,40,40,0,1,false,0,2,4,0);
        sprite(oam1d,1,40,80,0,1,true,0,16,0,1);
        video.write(0x07000000,oam1d);
        *control=0x00111210u;   // the same, with the 1D character map above
        const unsigned objOneD=run("obj1d",0x1252,false,0);
        const auto oneDOn=window.gpu().readScreen(false);
        *control=0x00110210u;   // the same, objects off
        const unsigned objOneDOffDiff=run("obj1d-off",0x1252,false,0);
        const auto oneDOff=window.gpu().readScreen(false);
        *control=0x00111210u;
        unsigned oneDPixels=0;
        for(std::size_t i=0;i<oneDOn.size();++i)oneDPixels+=oneDOn[i]!=oneDOff[i];
        // Engine B reads its objects from 0x06600000, its OAM from 0x07000400
        // and its object palette from entries 0x300..0x3ff: all three differ
        // from engine A's and each is a constant in the shader.
        *bankD=0x84;
        video.write(0x06600000,chars2);
        for(unsigned i=0;i<256;++i) {
            const unsigned c=(i*11+5)&31|((i*13+7)&31)<<5|((i*17+9)&31)<<10;
            objPalette[i*2]=std::uint8_t(c);objPalette[i*2+1]=std::uint8_t(c>>8);
        }
        video.write(0x05000600,objPalette);
        std::vector<std::uint8_t> subOam(0x400,0);   // engine B's half of OAM
        sprite(subOam,0,80,100,0,1,false,0,4,3,0);
        sprite(subOam,1,86,120,0,0,true,1,6,2,1);
        video.write(0x07000400,subOam);
        *subControl=0x00011000u;   // display mode 1, objects on, no backgrounds
        const unsigned objSub=run("obj-sub",0,true,0);
        const auto subOn=window.gpu().readScreen(true);
        *subControl=0x00010000u;   // the same, objects off
        const unsigned objSubOff=run("obj-sub-off",0,true,0);
        const auto subOff=window.gpu().readScreen(true);
        *subControl=0x00011000u;
        unsigned subPixels=0;
        for(std::size_t i=0;i<subOn.size();++i)subPixels+=subOn[i]!=subOff[i];
        // Display mode 2: the bank itself is the picture, as a 16-bit bitmap.
        // It has to be written while bank A is CPU-visible at 0x06000000, then
        // handed to the LCDC (0x80) so that the display sees it. Pixel 0 is red:
        // with every pixel black, a composition that shows nothing at all would
        // compare equal to a broken one.
        {
            std::vector<std::uint8_t> bitmap(std::size_t(256)*192*2);
            for(unsigned i=0;i<256u*192u;++i) {
                const unsigned colour=i==0?0x001fu:(((i*13)&31)|(((i*5)&31)<<5)|(((i*7)&31)<<10));
                bitmap[i*2]=std::uint8_t(colour);bitmap[i*2+1]=std::uint8_t(colour>>8);
            }
            video.write(0x06000000,bitmap);
        }
        *control=0x00020000u;   // display mode 2, bank A
        *bankA=0x80;            // bank A mapped to the LCDC, so it is displayed
        const unsigned bitmapDiff=run("mode2",0);
        const bool bitmapShown=window.gpu().readScreen(false).front()==0xffff0000u;
        const unsigned bitmapRun=bitmapShown?bitmapDiff:256u*192u;
        // A thousand pixels is well under what the seven objects of the first
        // scene cover and well over what a composition that dropped them shows.
        const bool objectsShown=objectPixels>1000&&oneDPixels>400&&subPixels>300;
        const bool same=blended==0&&plain==0&&bitmapRun==0&&affine16==0&&affine8==0&&affineSub==0
            &&objDiff==0&&objOffDiff==0&&objOneD==0&&objOneDOffDiff==0
            &&objSub==0&&objSubOff==0&&objectsShown;
        std::printf("selfcheck gpu compose: %s (blend %u, plain %u, mode2 %u, affine16 %u, affine8 %u, affine-sub %u, obj %u/%u, obj1d %u/%u, obj-sub %u/%u of %u differ)\n",
            same?"ok":"FAILED",blended,plain,bitmapRun,affine16,affine8,affineSub,
            objDiff,objectPixels,objOneD,oneDPixels,objSub,subPixels,256u*192u);
        *control=controlBefore;*bg0=bg0Before;*bg3=bg3Before;*bg1=bg1Before;*hofs=hofsBefore;*vofs=vofsBefore;
        *bldcnt=bldcntBefore;*bldalpha=bldalphaBefore;*master=masterBefore;
        *subControl=subControlBefore;*subBg3=subBg3Before;
        for(unsigned i=0;i<12;++i) {
            *reg16(0x04000030+i*2)=affineBefore[i];
            *reg16(0x04001030+i*2)=subAffineBefore[i];
        }
        video.write(0x07000000,oamBefore);
        *bankA=bankABefore;*bankC=bankCBefore;*bankB=bankBBefore;*bankD=bankDBefore;
    } catch(const std::exception& error) {
        std::printf("selfcheck gpu compose: threw %s\n",error.what());
    }
}
}
