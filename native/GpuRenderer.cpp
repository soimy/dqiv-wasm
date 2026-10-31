#include "GpuRenderer.hpp"
#include "Display.hpp"
#include "DualScreen.hpp"
#include "HdPictures.hpp"
#include "GlLoader.hpp"
#include "GxTextures.hpp"
#include "Profile.hpp"
#include "PixelOrder.hpp"
#include "Diagnostics.hpp"
#include <algorithm>
#include "Parallel.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>

namespace pc {
namespace {
// Profiling only (DQIV_PROFILE=gpu): waits for the GPU, charging its time to `stage`.
void gpuCheckpoint(const char* stage) {
    static const bool enabled=[]{const char* v=std::getenv("DQIV_PROFILE");return v&&std::string(v)=="gpu";}();
    if(enabled) {ProfileScope timing(stage);gl::Finish();}
}
// Where a GL call goes wrong on WebGL2, reported at the call. Draining the error
// queue at the end of a cycle - which is what the self-check does - tells you that
// something failed but not which call raised it, and on WebGL2 that difference is
// the whole answer. Costs nothing on the desktop path.
#ifdef __EMSCRIPTEN__
void gpuTraceAt(const char* where) {
    if(!gpuTrace)return;
    if(unsigned e=gl::GetError())std::printf("  trace %s: gl error 0x%x\n",where,e);
}
#define DQIV_TRACE(where) gpuTraceAt(where)
#else
#define DQIV_TRACE(where)
#endif
}
#ifdef __EMSCRIPTEN__
bool gpuTrace=false;   // defined here, declared in the header: the self-check turns it on
#endif
namespace {
// The only divergence between the two backends. Everything these shaders use —
// layout(location=), flat uvec4, textureLod, textureSize, texelFetch,
// isampler2D, gl_FragDepth, dynamic indexing of uniform arrays — is in GLSL ES
// 3.00. Precision qualifiers are required in ES fragment shaders and legal to
// state explicitly on desktop, so they ride along with the version line.
// Integer samplers have no default precision in ES at all, so this states one -
// composeFragment is the only shader that uses an isampler2D, and glslang
// stopped it until the line was there.
// gl_ClipDistance is the single exception, handled below.
#ifdef __EMSCRIPTEN__
#define DQIV_GLSL_HEADER "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp isampler2D;\n"
#else
#define DQIV_GLSL_HEADER "#version 330 core\n"
#endif
// The upload format this backend accepts. The channel shifts and the swizzle
// live in PixelOrder.hpp, so the layout probe can assert the round trip.
#ifdef __EMSCRIPTEN__
constexpr GLuint kUploadFormat=GL_RGBA;   // WebGL2 has no BGRA format
#else
constexpr GLuint kUploadFormat=GL_BGRA;   // desktop GL accepts 0xAARRGGBB directly
#endif
const char* sceneVertex=DQIV_GLSL_HEADER R"(
layout(location=0) in vec4 aPosition;
layout(location=1) in vec4 aColor;
layout(location=2) in vec4 aTexture;
layout(location=3) in uvec4 aParams;
layout(location=4) in vec4 aClip;
layout(location=5) in vec4 aRect;
out vec3 vColor;
out vec2 vUv;
flat out vec2 vSize;
flat out uvec4 vParams;
flat out vec4 vRect;
out float vW;
out vec4 vClip;
void main() {
    gl_Position=aPosition;vColor=aColor.rgb;vUv=aTexture.xy;vSize=aTexture.zw;vParams=aParams;vW=aPosition.w;vRect=aRect;
    // The DS screen's own edges, for overlays placed inside a larger target.
    // GLSL ES 3.00 has no gl_ClipDistance, so the values ride down as a varying
    // and the fragment shader discards. This is also what makes the behaviour
    // deterministic: the hardware built-in's interpolation is explicitly
    // implementation-dependent, a varying is perspective-correct.
    vClip=aClip;
})";
// DS 3D pixel pipeline: vertex colour, texture blend modes, toon/highlight,
// alpha test. Colour channels are in the hardware's 0..63 range until output.
const char* sceneFragment=DQIV_GLSL_HEADER R"(
in vec3 vColor;
in vec2 vUv;
flat in vec2 vSize;
flat in uvec4 vParams;
flat in vec4 vRect;
in float vW;
in vec4 vClip;
uniform sampler2D uTexture;
uniform int uControl;
uniform float uAlphaReference;
uniform vec3 uToon[32];
uniform int uWBuffer;
uniform int uPass;
uniform bool uClipEnabled;
layout(location=0) out vec4 oColor;
layout(location=1) out vec4 oAttributes;
void main() {
    if(uClipEnabled&&min(min(vClip.x,vClip.y),min(vClip.z,vClip.w))<0.0)discard;
    uint attributes=vParams.x,flags=vParams.z;
    uint mode=(attributes>>4)&3u;
    float alpha=float((attributes>>16)&31u);
    vec3 color=clamp(vColor*63.0,0.0,63.0),highlight=vec3(0.0);
    if(mode==2u) {
        vec3 shade=uToon[min(31,int(color.r)>>1)];
        if((uControl&2)!=0) {color=vec3(color.r);highlight=shade;} else color=shade;
    }
    if((flags&1u)!=0u) {
        vec2 uv=vUv;
        if((flags&4u)!=0u) {
            // A part cut from a sheet (monster sprites): filtering stays inside
            // its own texels, half an upscaled texel from its edges, or the
            // neighbouring picture's transparent border opens a seam.
            vec2 inset=0.5*vSize/vec2(textureSize(uTexture,0));
            uv=clamp(uv,min(vRect.xy+inset,vRect.zw),max(vRect.zw-inset,vRect.xy));
        }
        // Sheet parts read the full-size texels: smaller mipmap levels mix in
        // the neighbouring pictures of the sheet.
        vec4 texel=(flags&4u)!=0u?textureLod(uTexture,uv/vSize,0.0):texture(uTexture,uv/vSize);
        if((flags&4u)!=0u) {
            // A part's outline follows its own texels exactly: filtering would
            // blend in the transparent texels beside a seam between two parts
            // and open a line there (enlarged monsters). Colours stay filtered.
            ivec2 size=textureSize(uTexture,0);
            vec4 sharp=texelFetch(uTexture,clamp(ivec2(floor(uv/vSize*vec2(size))),ivec2(0),size-1),0);
            vec3 colour=texel.a>0.01?texel.rgb/texel.a:(sharp.a>0.0?sharp.rgb/sharp.a:vec3(0.0));
            texel=vec4(colour*sharp.a,sharp.a);
        }
        float textureAlpha=texel.a*31.0;
        vec3 textureColor=texel.a>0.0?texel.rgb/texel.a*63.0:vec3(0.0);
        if(mode==1u) color=mix(color,textureColor,clamp(textureAlpha/31.0,0.0,1.0));
        else {color=((textureColor+1.0)*(color+1.0)-1.0)/64.0;alpha=((textureAlpha+1.0)*(alpha+1.0)-1.0)/32.0;}
    }
    color=min(color+highlight,vec3(63.0));
    if((flags&2u)!=0u) {if(alpha<15.5) discard;alpha=31.0;}
    if(alpha<0.5) discard;
    if((uControl&4)!=0 && alpha<=uAlphaReference) discard;
    float a=clamp(alpha/31.0,0.0,1.0);
    oColor=vec4(color/63.0*a,a);
    bool fog=((attributes>>15)&1u)!=0u;
    if(uPass==0) oAttributes=vec4(fog?1.0:0.0,float((attributes>>24)&63u)/63.0,0.0,1.0);
    else oAttributes=vec4(0.0,0.0,0.0,fog?0.0:1.0);
    // Hardware Z is (z+1)*0x7fffff: the far plane stays below the 0xffffff clear depth.
    float depth=uWBuffer!=0?clamp(vW*4096.0/16777215.0,0.0,1.0):gl_FragCoord.z*(16777214.0/16777215.0);
    if(((attributes>>14)&1u)!=0u) depth-=512.0/16777215.0;
    gl_FragDepth=depth;
})";
const char* fullscreenVertex=DQIV_GLSL_HEADER R"(
out vec2 vPosition;
void main() {
    vec2 corner=vec2(float(gl_VertexID&1),float((gl_VertexID>>1)&1));
    vPosition=corner;gl_Position=vec4(corner*2.0-1.0,0.0,1.0);
})";
// Two fog settings (lower and upper part of the target) blended over a band,
// for the unified two-screen picture; both are equal otherwise.
const char* fogFragment=DQIV_GLSL_HEADER R"(
uniform sampler2D uColor;
uniform sampler2D uDepth;
uniform sampler2D uAttributes;
uniform int uEnabled[2];
uniform int uAlphaOnly[2];
uniform vec4 uFogColor[2];
uniform float uFogTable[64];
uniform float uFogOffset[2];
uniform float uFogStep[2];
uniform float uSplit;
uniform float uBand;
out vec4 oColor;
vec4 fog(int i,vec4 color,float depth) {
    if(uEnabled[i]==0) return color;
    float position=uFogStep[i]>0.0?(depth-uFogOffset[i])/uFogStep[i]-1.0:(depth>uFogOffset[i]?31.0:0.0);
    position=clamp(position,0.0,31.0);
    int low=int(position),high=min(31,low+1);
    float density=mix(uFogTable[i*32+low],uFogTable[i*32+high],position-float(low))/128.0;
    vec4 straight=color.a>0.0?vec4(color.rgb/color.a,color.a):vec4(0.0);
    if(uAlphaOnly[i]!=0) straight.a=mix(straight.a,uFogColor[i].a,density);
    else straight=mix(straight,uFogColor[i],density);
    return vec4(straight.rgb*straight.a,straight.a);
}
void main() {
    ivec2 p=ivec2(gl_FragCoord.xy);
    vec4 color=texelFetch(uColor,p,0);
    if(texelFetch(uAttributes,p,0).r>0.5) {
        float depth=texelFetch(uDepth,p,0).r*16777215.0/512.0;
        float upper=uBand>0.0?smoothstep(uSplit-uBand,uSplit+uBand,gl_FragCoord.y):step(uSplit,gl_FragCoord.y);
        color=mix(fog(0,color,depth),fog(1,color,depth),upper);
    }
    oColor=color;
})";
// Interface pictures (premultiplied) placed over the presented frame.
const char* overlayVertex=DQIV_GLSL_HEADER R"(
uniform vec4 uRect;
out vec2 vPosition;
void main() {
    vec2 corner=vec2(float(gl_VertexID&1),float((gl_VertexID>>1)&1));
    vPosition=vec2(corner.x,1.0-corner.y);
    gl_Position=vec4(mix(uRect.xy,uRect.zw,corner),0.0,1.0);
})";
const char* overlayFragment=DQIV_GLSL_HEADER R"(
in vec2 vPosition;
uniform sampler2D uPicture;
out vec4 oColor;
void main() {oColor=texture(uPicture,vPosition);})";
const char* reduceFragment=DQIV_GLSL_HEADER R"(
uniform sampler2D uSource;
uniform vec2 uOrigin;
uniform vec2 uScale;
out vec4 oColor;
void main() {
    vec2 origin=uOrigin+floor(gl_FragCoord.xy)*uScale;
    vec4 sum=vec4(0.0);
    for(int y=0;y<4;++y) for(int x=0;x<4;++x) sum+=texture(uSource,(origin+(vec2(x,y)+0.5)*uScale/4.0)/vec2(textureSize(uSource,0)));
    oColor=sum/16.0;
})";
// The capture unit's output for one screen, stage B4: the picture quantised the
// way Display::capture stores it in VRAM - five bits a channel, plus the alpha bit
// that says the pixel was opaque - and laid out as the two bytes NitroVideo::capture
// writes. Reading it back is one transfer instead of a per-pixel loop in the frame.
const char* captureFragment=DQIV_GLSL_HEADER R"(
precision highp float;
in vec2 vPosition;
out vec4 oColor;
uniform sampler2D uPicture;   // the screen's reduced 3D picture, premultiplied, rows bottom-up
void main() {
    // Both pictures are bottom-up, so the screen row needs no flip here.
    ivec2 p=ivec2(int(vPosition.x*256.0),int(vPosition.y*192.0));
    vec4 t=texelFetch(uPicture,p,0);
    uint r=uint(t.r*255.0+0.5),g=uint(t.g*255.0+0.5),b=uint(t.b*255.0+0.5),a=uint(t.a*255.0+0.5);
    // Back to straight colour exactly as GpuRenderer::unpremultiplyNative does.
    uint k=a==0u?0u:(255u*65536u+a/2u)/a;
    r=min(255u,(r*k+32768u)>>16);g=min(255u,(g*k+32768u)>>16);b=min(255u,(b*k+32768u)>>16);
    // Display::color15, then its alpha bit.
    uint c=(r<<16)|(g<<8)|b;
    uint v=((c>>19)&31u)|((c>>6)&0x3e0u)|((c<<7)&0x7c00u);
    if(a!=0u)v|=0x8000u;
    oColor=vec4(float(v&255u)/255.0,float(v>>8)/255.0,0.0,1.0);
})";
// Final picture: per-pixel DS composition recipes (see Display) applied to
// the high-resolution 3D layer, then master brightness.
const char* composeFragment=DQIV_GLSL_HEADER R"(
in vec2 vPosition;
uniform sampler2D uRecipe0;
uniform sampler2D uRecipe1;
uniform sampler2D uRecipe2;
uniform sampler2D uRecipe3;
uniform sampler2D uHdAtlas;
uniform sampler2D uThree0;
uniform sampler2D uThree1;
uniform isampler2D uSideColumns;
uniform vec2 uCanvas;
uniform vec4 uThreeRect0;
uniform vec4 uThreeRect1;
uniform int uSides;
uniform int uBackdrop3D;
uniform vec4 uInset;
uniform int uPlaced;
uniform float uTopScale;
uniform int uBand;
uniform float uInsetAlpha;
uniform vec4 uScreen0;
uniform vec4 uScreen1;
uniform int uThreeScreenRow;
uniform float uScale;
// The two screens composed on the GPU, when the canvas has nothing but them to
// draw (docs/porting/07, stage B3): the top one and the bottom one, in the place
// the recipes would have had them. They carry the 2D layers, the 3D layer and the
// blends already, so the recipe path below is not used at all.
uniform sampler2D uScreenA;
uniform sampler2D uScreenB;
uniform int uComposed;
out vec4 oColor;
vec4 three;
vec3 brightness(vec3 color,float code) {
    int value=int(code*255.0+0.5),mode=value>>5;
    float amount=float(value&31)/16.0;
    if(mode==1) return color+(1.0-color)*amount;
    if(mode==2) return color-color*amount;
    return color;
}
// One DS pixel's recipe applied to this fragment's 3D sample.
int rowFirst=0,rowLast=383;
bool insetCell=false;
vec3 composedPixel(ivec2 cell) {
    int row=cell.y;
    int y=row<192?row:row-192;
    vec2 uv=vec2((float(clamp(cell.x,0,255))+0.5)/256.0,1.0-(float(y)+0.5)/192.0);
    vec3 colour;
    if(row<192)colour=texture(uScreenA,uv).rgb;
    else colour=texture(uScreenB,uv).rgb;
    return colour;
}
vec3 pixel(ivec2 cell) {
    if(uComposed!=0)return composedPixel(cell);
    bool side=!insetCell&&(cell.x<0||cell.x>255);
    if(side&&uSides==0) return vec3(0.0);
    ivec2 r=ivec2(clamp(cell.x,0,255),clamp(cell.y,rowFirst,rowLast));
    vec4 r0=texelFetch(uRecipe0,r,0);
    int kind=int(r0.a*255.0+0.5);
    vec4 r1,r2;
    if(side&&kind!=1) {
        // Beside the screens: the 3D scene with the effects of the nearest
        // column of the row where it shows directly (2D windows may cover the
        // edge), found once per row and side (GpuRenderer::present).
        int column=texelFetch(uSideColumns,ivec2(cell.x<0?0:1,r.y),0).r;
        if(column>=0) {r.x=column;r0=texelFetch(uRecipe0,r,0);r1=texelFetch(uRecipe1,r,0);r2=texelFetch(uRecipe2,r,0);}
        else {r1=vec4(0.0);r2=vec4(0.0,0.0,0.0,texelFetch(uRecipe2,r,0).a);}
        kind=1;
    } else {r1=texelFetch(uRecipe1,r,0);r2=texelFetch(uRecipe2,r,0);}
    // A screen's bare backdrop lets the 3D scene through (wide battles); that
    // 3D keeps the fades of the screen it comes from.
    if(uBackdrop3D!=0&&kind==0&&(int(r1.a*255.0+0.5)&8)!=0) {
        ivec2 source=ivec2(r.x,uThreeScreenRow);
        kind=1;
        r1=vec4(0.0,0.0,0.0,float(int(texelFetch(uRecipe1,source,0).a*255.0+0.5)&2)/255.0);
        r2=texelFetch(uRecipe2,source,0);r2.rg=vec2(0.0);
    }
    vec3 color=r0.rgb;
    if(kind!=0) {
        int flags=int(r1.a*255.0+0.5);
        vec3 below=r1.rgb;
        vec3 straight=three.a>0.0?three.rgb/three.a:below;
        vec3 over=three.rgb+below*(1.0-three.a);
        float eva=r2.r*255.0/128.0,evb=r2.g*255.0/128.0;
        if(kind==1) {
            if(three.a<0.999) color=(flags&1)!=0?over:straight;
            else color=straight;
            if(three.a>=0.999 && (flags&2)!=0) color=brightness(color,r2.b);
        } else color=min(vec3(1.0),r0.rgb*eva+over*evb);
    }
    return brightness(color,r2.a);
}
// A high-resolution picture over the DS pixel at `cell` (recipe plane 3),
// sampled where this fragment falls in that pixel.
vec3 hdOver(vec3 color,vec2 cell) {
    if(uComposed!=0)return color;   // the composed screens already carry what HD would replace
    vec2 at=cell+0.5;
    ivec2 c=ivec2(floor(at));
    if(!insetCell&&(c.x<0||c.x>255)) return color;
    c=ivec2(clamp(c.x,0,255),clamp(c.y,rowFirst,rowLast));
    uvec4 v=uvec4(texelFetch(uRecipe3,c,0)*255.0+0.5);
    uint tag=(v.a<<24)|(v.r<<16)|(v.g<<8)|v.b;
    if(tag==0u) return color;
    uint picture=(tag>>24)-1u;
    vec2 size=vec2(float(((tag>>18)&63u)+1u),float(((tag>>12)&63u)+1u));
    vec2 uv=clamp((vec2(float((tag>>6)&63u),float(tag&63u))+fract(at))/size,vec2(0.5/128.0),vec2(1.0-0.5/128.0));
    vec2 origin=vec2(float(picture%16u),float(picture/16u))*128.0;
    vec4 hd=texture(uHdAtlas,(origin+uv*128.0)/2048.0);
    if(hd.a<=0.0) return color;
    return mix(color,brightness(hd.rgb/hd.a,texelFetch(uRecipe2,c,0).a),hd.a);
}
// Sharp bilinear sampling of the DS pixel grid around `cell`.
vec3 sharp(vec2 cell,float scale) {
    vec2 base=floor(cell),weight=clamp((cell-base-0.5)*scale+0.5,0.0,1.0);
    ivec2 b=ivec2(base);
    // Most fragments lie inside one DS pixel: neighbours only at its borders.
    vec3 top=pixel(b);
    if(weight.x>0.0) top=mix(top,pixel(b+ivec2(1,0)),weight.x);
    if(weight.y>0.0) {
        vec3 bottom=pixel(b+ivec2(0,1));
        if(weight.x>0.0) bottom=mix(bottom,pixel(b+ivec2(1,1)),weight.x);
        top=mix(top,bottom,weight.y);
    }
    return top;
}
void main() {
    vec2 p=vec2(vPosition.x,1.0-vPosition.y)*uCanvas;
    three=vec4(0.0);
    vec2 t=(p-uThreeRect0.xy)/max(uThreeRect0.zw,vec2(1e-6));
    vec2 u=(p-uThreeRect1.xy)/max(uThreeRect1.zw,vec2(1e-6));
    if(uThreeRect0.z>0.0&&all(greaterThanEqual(t,vec2(0.0)))&&all(lessThan(t,vec2(1.0)))) three=texture(uThree0,vec2(t.x,1.0-t.y));
    else if(uThreeRect1.z>0.0&&all(greaterThanEqual(u,vec2(0.0)))&&all(lessThan(u,vec2(1.0)))) three=texture(uThree1,vec2(u.x,1.0-u.y));
    // Sharp bilinear: DS pixels stay square and crisp at any scale, with
    // one presented pixel of blending at their borders.
    vec2 cell=vec2(p.x-(uCanvas.x-256.0)*0.5,p.y)-0.5;
    float scale=uScale;
    bool topCell=false;
    if(uTopScale<1.0&&p.y<192.0) {
        // The smaller top screen at the canvas top; elsewhere the 3D scene.
        vec2 local=(p-vec2((uCanvas.x-256.0*uTopScale)*0.5,0.0))/uTopScale;
        if(all(greaterThanEqual(local,vec2(0.0)))&&all(lessThan(local,vec2(256.0,192.0)))) {cell=local-0.5;scale=uScale*uTopScale;topCell=true;}
        else cell=vec2(-2.0,clamp(local.y,0.0,191.0));
    }
    if(uBand!=0&&!topCell) {
        // The bottom screen's band: its plates above it (recipe rows 384..;
        // only below a smaller top screen) and beside it (rows 448.., left
        // then right), where they are opaque.
        vec2 b=vec2(p.x-(uCanvas.x-256.0)*0.5,p.y-128.0),at=vec2(-1.0);
        int first=0;
        if(b.y>=0.0&&b.y<64.0&&b.x>=0.0&&b.x<256.0) {if(uTopScale<1.0) {at=vec2(b.x,384.0+b.y);first=384;}}
        else if(b.y>=0.0&&b.y<256.0&&b.x>=-128.0&&b.x<0.0) {at=vec2(b.x+128.0,448.0+b.y);first=448;}
        else if(b.y>=0.0&&b.y<256.0&&b.x>=256.0&&b.x<384.0) {at=vec2(b.x-128.0,448.0+b.y);first=448;}
        if(first!=0&&(int(texelFetch(uRecipe1,ivec2(at),0).a*255.0+0.5)&8)==0) {
            cell=at-0.5;rowFirst=first;rowLast=first==384?447:703;
        }
    }
    if(uPlaced!=0) {
        // Each screen in its own rectangle; nothing elsewhere.
        vec2 a=(p-uScreen0.xy)/max(uScreen0.zw,vec2(1e-6)),b=(p-uScreen1.xy)/max(uScreen1.zw,vec2(1e-6));
        int s=-1;vec4 rect;vec2 at;
        if(uScreen0.z>0.0&&all(greaterThanEqual(a,vec2(0.0)))&&all(lessThan(a,vec2(1.0)))) {s=0;rect=uScreen0;at=a;}
        else if(uScreen1.z>0.0&&all(greaterThanEqual(b,vec2(0.0)))&&all(lessThan(b,vec2(1.0)))) {s=1;rect=uScreen1;at=b;}
        if(s<0) {oColor=vec4(0.0,0.0,0.0,1.0);return;}
        cell=at*vec2(256.0,192.0)+vec2(0.0,192.0*float(s))-0.5;scale=uScale*rect.z/256.0;
        rowFirst=192*s;rowLast=rowFirst+191;insetCell=true;
    }
    vec3 color=hdOver(sharp(cell,scale),cell);
    vec2 i=(p-uInset.xy)/max(uInset.zw,vec2(1e-6));
    if(uInset.z>0.0&&uInsetAlpha>0.0&&all(greaterThanEqual(i,vec2(0.0)))&&all(lessThan(i,vec2(1.0)))) {
        // The inset: the top screen at a reduced size, over its own 3D,
        // faded in once its picture is ready.
        three=texture(uThree1,vec2(i.x,1.0-i.y));
        rowFirst=384;rowLast=575;insetCell=true;
        vec2 insetPosition=i*vec2(256.0,192.0)+vec2(0.0,384.0)-0.5;
        color=mix(color,hdOver(sharp(insetPosition,uScale*uInset.z/256.0),insetPosition),uInsetAlpha);
    }
    oColor=vec4(color,1.0);
})";

// The DS display engine's 2D composition, per pixel, on the GPU: stage B2 of
// docs/porting/07-GPU-COMPOSITION.md. It reads the same sources the CPU path
// reads - VRAM through the LCDC mapping, palette RAM - and reproduces its
// arithmetic exactly (six-bit channels, integer blends), because anything that
// rounds differently would show up as a colour that is off by one.
const char* compose2dFragment=DQIV_GLSL_HEADER R"(
precision highp usampler2D;
in vec2 vPosition;
out vec4 oColor;
uniform usampler2D uVram;     // physical VRAM, 1024 texels to a row
uniform usampler2D uPalette;  // palette RAM, one 16-bit entry per texel
uniform usampler2D uOam;      // object attribute memory, one 16-bit entry per texel
uniform highp sampler2D uScreen3d;   // this screen's reduced 3D picture, when the caller has one
uniform float uThreeOn;             // BG0 is showing that picture (DISPCNT says BG0 draws 3D)
uniform int uBackdrop3D;            // a bare backdrop shows that picture too (wide battles)
uniform int uGraphics;              // the layers alone: what the canvas shows before the capture mux
uniform vec4 uBgCnt[4];       // BGxCNT
uniform vec4 uBgOff[4];       // BGxHOFS, BGxVOFS
uniform vec4 uWinBase;        // background region: physical offset of each 128 KB slot, -1 unmapped
uniform vec4 uWinSize;        // its length
uniform vec4 uObjWinBase;     // sprite region: the same, 128 KB slots
uniform vec4 uObjWinSize;     // its length
uniform vec4 uScreen;         // backdrop colour (15-bit), DISPCNT low 24 bits, BLDCNT, BLDALPHA
uniform vec4 uEffects;        // BLDY, MASTER_BRIGHT, background palette base, unused
uniform vec4 uMode;           // display mode, bank mask, DISPCNT bits 24-31, engine (0 main, 1 sub)
uniform vec4 uBgParam[2];     // BG2/BG3 PA,PB,PC,PD
uniform vec4 uBgRef[2];       // BG2/BG3 reference point: x low 16 bits, x high, y low, y high
uniform vec4 uWin[2];         // WININ, WINOUT, WH0, WV0; then WH1, WV1
// The full DISPCNT: the low 24 bits come from uScreen, the top byte from uMode
// (a float cannot hold all 32 bits of the register exactly).
uint displayControl() {return uint(int(uScreen.y))|(uint(int(uMode.z))<<24);}
// Display's range(): a window register holds two byte endpoints; a backwards
// pair means the range wraps round the end of the screen.
bool inWindow(uint value,uint endpoints) {
    uint start=endpoints>>8u,end=endpoints&255u;
    return start<=end?(value>=start&&value<end):(value>=start||value<end);
}
// Display::engine's mask: the layers a pixel shows (BG0-3 in bits 0-3, objects
// in bit 4) and whether the colour effect applies (bit 5). Outside every window
// it is WINOUT; the object window takes WINOUT's high byte, window 1 takes
// WININ's high byte, and window 0 - checked last - takes WININ's low byte.
uint windowMask(ivec2 p,bool objectWindow) {
    uint control=displayControl();
    if((control&0xe000u)==0u)return 63u;
    uint winin=uint(int(uWin[0].x)),winout=uint(int(uWin[0].y));
    uint mask=winout&63u;
    if((control&0x8000u)!=0u&&objectWindow)mask=(winout>>8u)&63u;
    if((control&0x4000u)!=0u&&inWindow(uint(p.x),uint(int(uWin[1].x)))&&inWindow(uint(p.y),uint(int(uWin[1].y))))
        mask=(winin>>8u)&63u;
    if((control&0x2000u)!=0u&&inWindow(uint(p.x),uint(int(uWin[0].z)))&&inWindow(uint(p.y),uint(int(uWin[0].w))))
        mask=winin&63u;
    return mask;
}
// One 128 KB slot of a region: the physical base the engine sees there (-1
// unmapped) and its length. Display::engine reads each engine's backgrounds
// and sprites as one flat region, so the compositor does the same.
uint windowByte(vec4 bases,vec4 sizes,int offset) {
    int slot=offset>>17,within=offset&0x1ffff;
    int base=int(bases[slot]);
    if(base<0||within>=int(sizes[slot]))return 0u;
    int physical=base+within;
    return texelFetch(uVram,ivec2(physical&1023,physical>>10),0).r;
}
)" R"(
// The two regions an engine reads, each masked to its own length the way
// Display::engine masks: 512 KB of backgrounds and 256 KB of sprites on engine
// A, 128 KB of each on engine B.
uint vram8(int offset) {return windowByte(uWinBase,uWinSize,offset&(int(uMode.w)!=0?0x1ffff:0x7ffff));}
uint vram16(int offset) {return vram8(offset)|(vram8(offset+1)<<8);}
uint vramObj8(int offset) {return windowByte(uObjWinBase,uObjWinSize,offset&(int(uMode.w)!=0?0x1ffff:0x3ffff));}
uint paletteEntry(int index) {return texelFetch(uPalette,ivec2(index,0),0).r;}
// Physical VRAM, for the display's own bitmap modes (no window mapping).
uint vramPhysical(int offset) {return texelFetch(uVram,ivec2(offset&1023,offset>>10),0).r;}
// The readback path takes the 3D target's premultiplied colour back to straight
// ARGB (GpuRenderer::unpremultiplyNative), reciprocal table and all, and this is
// the same arithmetic so that the two pictures are identical pixel for pixel.
uint straight(vec4 c) {
    uint r=uint(c.r*255.0+0.5),g=uint(c.g*255.0+0.5),b=uint(c.b*255.0+0.5),a=uint(c.a*255.0+0.5);
    if(a==255u)return 0xff000000u|(r<<16)|(g<<8)|b;
    if(a==0u)return 0u;
    uint k=(255u*65536u+a/2u)/a;
    r=min(255u,(r*k+32768u)>>16);g=min(255u,(g*k+32768u)>>16);b=min(255u,(b*k+32768u)>>16);
    return (a<<24)|(r<<16)|(g<<8)|b;
}
// Display::color15 followed by Display::rgb: five bits of each channel, expanded
// again. The capture keeps only those five, so a picture that skipped this would
// differ by the low bits of every channel.
uint quantise(uint c) {
    uint r=((c>>16)&255u)>>3,g=((c>>8)&255u)>>3,b=(c&255u)>>3;
    r=(r<<3)|(r>>2);g=(g<<3)|(g>>2);b=(b<<3)|(b>>2);
    return 0xff000000u|(r<<16)|(g<<8)|b;
}
// BGR555 to ARGB, expanded the way Display::rgb does: (v<<3)|(v>>2).
uint rgb15(uint c) {
    uint r=c&31u,g=(c>>5)&31u,b=(c>>10)&31u;
    r=(r<<3)|(r>>2);g=(g<<3)|(g>>2);b=(b<<3)|(b>>2);
    return 0xff000000u|(r<<16)|(g<<8)|b;
}
// Display::blend, exactly: six-bit channels, clamped at 63, shifted by `shift`,
// then expanded back to eight bits.
uint blend(uint a,uint b,int eva,int evb,int shift) {
    uint result=0u;
    for(int s=0;s<3;++s) {
        int sh=s*8;
        int av=int((a>>uint(sh))&255u)>>2;
        int bv=int((b>>uint(sh))&255u)>>2;
        int v=min(63,(av*eva+bv*evb)>>shift);
        result|=uint((v<<2)|(v>>4))<<uint(sh);
    }
    return 0xff000000u|result;
}
uint brightness(uint c,int mode,int amount) {
    if(mode!=1&&mode!=2)return c;
    return mode==1?blend(c,0xffffffffu,16-amount,amount,4):blend(c,0xff000000u,16-amount,amount,4);
}
// The kind of background Display::background lays down for a layer: 0=text,
// 1=8-bit affine tilemap, 2=extended affine tilemap, 3=large bitmap.
int bgType(int bg,int mode) {
    if(bg<2)return 0;
    if(mode==6)return 3;
    if(bg==3)return mode>=3?2:(mode!=0?1:0);
    return mode==5?2:((mode==2||mode==4)?1:0);
}
// A background's pixel, or 0 where it is transparent. Text and affine tile
// maps: the bitmap affine kinds (Display's type 3, and type 2 with BGxCNT bit
// 7) are still refused by composeSupported.
uint bgPixel(int bg,int x,int y) {
    uint control=displayControl();
    int mode=int(control&7u);
    int cnt=int(uBgCnt[bg].x);
    int charBase=((cnt>>2)&15)*0x4000;
    int screenBase=((cnt>>8)&31)*0x800;
    // Only the main engine has the extra 64 KB bases above its 128 KB window.
    if(int(uMode.w)==0) {
        charBase+=int((control>>24)&7u)*0x10000;
        screenBase+=int((control>>27)&7u)*0x10000;
    }
    int size=(cnt>>14)&3;
    int type=bgType(bg,mode);
    if(type!=0) {
        // The affine matrix projects each screen pixel into the map. Tiles are
        // always 256 colours here; the map holds one byte per tile (type 1) or
        // a 16-bit entry with flip bits (type 2). BGxCNT bit 13 is the overflow
        // wrap bit for these.
        int dim=128<<size;
        int pa=int(uBgParam[bg-2].x),pb=int(uBgParam[bg-2].y);
        int pc=int(uBgParam[bg-2].z),pd=int(uBgParam[bg-2].w);
        int refx=int(uBgRef[bg-2].x)+(int(uBgRef[bg-2].y)<<16);
        int refy=int(uBgRef[bg-2].z)+(int(uBgRef[bg-2].w)<<16);
        int sx=(refx+x*pa+y*pb)>>8,sy=(refy+x*pc+y*pd)>>8;
        if((cnt&0x2000)==0&&(sx<0||sy<0||sx>=dim||sy>=dim))return 0u;
        sx&=dim-1;sy&=dim-1;
        int tx=sx&7,ty=sy&7;
        int tile;
        if(type==1) tile=int(vram8(screenBase+(sy>>3)*(dim>>3)+(sx>>3)));
        else {
            uint entry=vram16(screenBase+((sy>>3)*(dim>>3)+(sx>>3))*2);
            tile=int(entry&1023u);
            if((entry&0x400u)!=0u)tx=7-tx;
            if((entry&0x800u)!=0u)ty=7-ty;
        }
        uint index=vram8(charBase+tile*64+ty*8+tx);
        if(index==0u)return 0u;
        return rgb15(paletteEntry(int(uEffects.z)+int(index)));
    }
    int w=(size&1)!=0?512:256,h=(size&2)!=0?512:256;
    int hofs=int(uBgOff[bg].x),vofs=int(uBgOff[bg].y);
    int sx=(x+hofs)&(w-1),sy=(y+vofs)&(h-1);
    uint entry=vram16(screenBase+(((sy>>3)*(w>>3))+(sx>>3))*2);
    int tile=int(entry&1023u),bank=int((entry>>12)&15u);
    int px=sx&7,py=sy&7;
    uint index;
    bool eight=((cnt>>7)&1)!=0;
    if(eight) index=vram8(charBase+tile*64+py*8+px);
    else {
        uint packed=vram8(charBase+tile*32+py*4+(px>>1));
        index=((px&1)!=0)?(packed>>4):(packed&15u);
    }
    if(index==0u)return 0u;
    int paletteBase=int(uEffects.z);
    return rgb15(paletteEntry(paletteBase+(eight?int(index):bank*16+int(index))));
}
// Display::objects: the sprite layer's pixel, or 0 where no visible object
// covers it. Objects are taken in OAM order and the lowest priority number wins,
// a tie going to the lower id, so the scan keeps a hit only when it beats the
// one before it. Affine (rotation/scaling), bitmap and mosaicked objects are
// still refused by composeSupported. An OBJ-mode 2 sprite draws nothing itself:
// its opaque pixels are the object window, reported through `windowHit`.
uint objPixel(int x,int y,out int priority,out int semi,out bool windowHit) {
    // Widths and heights per shape (three of them) and size (four), in a row,
    // as Display::objects has them.
    const int widths[12]=int[12](8,16,32,64,16,32,32,64,8,8,16,32);
    const int heights[12]=int[12](8,16,32,64,8,8,16,32,16,32,32,64);
    uint control=displayControl();
    int obase=int(uMode.w)!=0?512:0;    // engine B's OAM follows engine A's
    bool mapped=(control&16u)!=0u;      // DISPCNT bit 4: one-dimensional character map
    int charOffset=mapped?int((control>>20)&3u):0;
    uint found=0u;priority=5;semi=0;windowHit=false;
    for(int id=0;id<128;++id) {
        int at=obase+id*4;
        int a=int(texelFetch(uOam,ivec2(at,0),0).r);
        int b=int(texelFetch(uOam,ivec2(at+1,0),0).r);
        int shape=a>>14,size=b>>14;
        if(shape==3)continue;                       // not a sprite Display draws
        if((a&0x100)!=0)continue;                   // affine: refused
        if((a&0x200)!=0)continue;                   // double size without affine
        int mode=(a>>10)&3;
        if(mode==3)continue;                        // bitmap: refused
        int w=widths[shape*4+size],h=heights[shape*4+size];
        // Objects wrap round the screen: their place is a byte, and the width
        // and height are counted modulo the screen from there.
        int dx=(x-(b&511))&511,dy=(y-(a&255))&255;
        if(dx>=w||dy>=h)continue;
        int u=dx,v=dy;
        if((b&0x1000)!=0)u=w-1-u;
        if((b&0x2000)!=0)v=h-1-v;
        bool eight=(a&0x2000)!=0;
        int bytes=eight?64:32;
        // The third attribute is only needed once the pixel is covered, which
        // most of the 128 entries are not.
        int c=int(texelFetch(uOam,ivec2(at+2,0),0).r);
        // The tile number is a byte offset into character memory in the 2D
        // mapping and a tile index in the 1D one, which also steps by the base.
        int start=(c&(eight?1022:1023))*(mapped?32<<charOffset:32);
        int tileOffset=mapped?(v>>3)*((w>>3)*bytes)+(u>>3)*bytes:(v>>3)*1024+(u>>3)*bytes;
        int pixel=(v&7)*8+(u&7);
        uint index=vramObj8(start+tileOffset+(eight?pixel:pixel>>1));
        if(!eight)index=(index>>uint((pixel&1)*4))&15u;
        if(index==0u)continue;
        // Display marks the object window before any priority decision: it is
        // the sprite's opaque pixels, wherever it sits in OAM order.
        if(mode==2) {windowHit=true;continue;}
        int p=(c>>10)&3;
        if(found!=0u&&p>=priority)continue;
        // Extended per-object palettes are refused, so the colours come from
        // palette RAM: entry 0x100 for engine A, 0x300 for engine B.
        found=rgb15(paletteEntry((int(uMode.w)!=0?0x300:0x100)+int(index)+(!eight?((c>>12)&15)*16:0)));
        priority=p;semi=mode==1?1:0;
    }
    return found;
}
void main() {
    ivec2 p=ivec2(int(vPosition.x*256.0),191-int(vPosition.y*192.0));
    uint control=displayControl();
    int bldcnt=int(uScreen.z),bldalpha=int(uScreen.w);
    uint backdrop=rgb15(uint(int(uScreen.x)));
    uint colour[5];int key[5];
    for(int i=0;i<5;++i){colour[i]=0u;key[i]=99;}
    // BG0 showing the 3D screen: its colour, BG0's priority, and the five bits of
    // alpha its top byte carries. Display::engine lays it down the same way - the
    // picture, scrolled horizontally, with nothing where the scroll wraps past the
    // right edge - and it is not a tilemap, so bgPixel is not asked.
    int threeAlpha=-1;
    if(int(uThreeOn)!=0) {
        int sx=(p.x+int(uBgOff[0].x))&511;
        if(sx<256) {
            uint t=straight(texelFetch(uScreen3d,ivec2(sx,191-p.y),0));
            if((t>>24)!=0u) {
                colour[0]=t|0xff000000u;key[0]=(int(uBgCnt[0].x)&3)*5+1;
                threeAlpha=int((t>>24)&255u)>>3;
            }
        }
    }
    for(int bg=0;bg<4;++bg) {
        if((control&(0x100u<<uint(bg)))==0u)continue;
        if(bg==0&&int(uMode.w)==0&&(control&8u)!=0u)continue;   // drawn above
        uint c=bgPixel(bg,p.x,p.y);
        if(c==0u)continue;
        colour[bg]=c;key[bg]=(int(uBgCnt[bg].x)&3)*5+bg+1;
    }
    // Objects are the fifth layer and, at equal priority, come before BG0..3.
    int objPriority=5,objSemi=0;bool objWindowHit=false;
    if((control&0x1000u)!=0u) {
        uint o=objPixel(p.x,p.y,objPriority,objSemi,objWindowHit);
        if(o!=0u) {colour[4]=o;key[4]=objPriority*5;}
    }
    // Display's windows: this pixel's mask says which of the five layers it
    // shows (bits 0-4) and whether the colour effect applies at all (bit 5).
    uint mask=windowMask(p,objWindowHit);
    // The two highest-priority layers decide the colour, as Display::compose.
    int first=5,second=5,k1=99,k2=99;
    for(int l=0;l<5;++l) {
        if((mask&(1u<<uint(l)))==0u||colour[l]==0u)continue;
        if(key[l]<k1){second=first;k2=k1;first=l;k1=key[l];}
        else if(key[l]<k2){second=l;k2=key[l];}
    }
    uint top=first==5?backdrop:(colour[first]|0xff000000u);
    uint below=second==5?backdrop:(colour[second]|0xff000000u);
    int eva=bldalpha&31,evb=(bldalpha>>8)&31,effect=(bldcnt>>6)&3;
    bool target2=(second==5)?((bldcnt&0x2000)!=0):((bldcnt&(0x100<<second))!=0);
    bool target1=(first==5)?((bldcnt&0x20)!=0):((bldcnt&(1<<first))!=0);
    uint c=top;
    // Display::compose's chain. The 3D layer, when it is the top one, blends by
    // its own five bits of alpha and is not brightened afterwards; a
    // semi-transparent object blends against what is behind it next; then the
    // effect modes. The mask's bit 5 is the window's "effect" switch: outside
    // it - or in a window that leaves it out - the layers show flat.
    int sceneAlpha=first==0?threeAlpha:-1;
    if((mask&32u)!=0u) {
        if(target2&&sceneAlpha>=0&&sceneAlpha<31)c=blend(top,below,sceneAlpha+1,31-sceneAlpha,5);
        else if(target2&&first==4&&objSemi!=0)c=blend(top,below,eva,evb,4);
        else if(target1&&effect==1&&target2&&sceneAlpha<0)c=blend(top,below,eva,evb,4);
        else if(target1&&effect>=2)c=brightness(top,effect-1,int(uEffects.x));
    }
    // Display mode 2: the screen is one VRAM bank shown as a 256x192 bitmap,
    // and none of the layers above are drawn at all. A frame the canvas presents
    // before the display mode picks its source keeps the layers instead - that is
    // Display's graphics recipes, copied where engine() does graphics_ = output.
    if(int(uMode.x)==2&&uGraphics==0) {
        if(int(uEffects.w)!=0) {
            // The bank holds nothing but this picture - the capture wrote it, whole
            // screen, at the start of the bank - so it is sampled where it comes
            // from instead of where it was copied to (stage B2d).
            c=quantise(straight(texture(uScreen3d,vPosition)));
        } else {
            int bank=int((control>>18)&3u);
            if(((int(uMode.y)>>bank)&1)==0)c=0xff000000u;
            else {
                int at=(bank<<17)+(p.y*256+p.x)*2;
                c=rgb15(vramPhysical(at)|(vramPhysical(at+1)<<8));
            }
        }
    } else if(uGraphics==0&&uBackdrop3D!=0&&first==5) {
        // Nothing 2D covers this pixel: in a battle the 3D camera spans the whole
        // canvas and the screen's bare backdrop is the hole it shows through. On
        // the canvas that is Display's recipe flag 8, drawn with the wide scene;
        // here it is the screen's own slice of the picture, so a soft pixel in a
        // wide window - the price of baking it in (docs/porting/07, stage B3).
        // A fully transparent picture pixel keeps the backdrop, as the canvas'
        // "below" would.
        uint t=straight(texelFetch(uScreen3d,ivec2(p.x,191-p.y),0));
        if(t!=0u) {
            c=t|0xff000000u;
            // The picture takes the brightness of the screen's 3D layer, the way
            // Display brightens a first-target 3D pixel (BG0 draws 3D here).
            if(int(uThreeOn)!=0&&(bldcnt&1)!=0&&effect>=2)c=brightness(c,effect-1,int(uEffects.x));
        }
    }
    int master=int(uEffects.y);
    c=brightness(c,(master>>14)&3,min(16,master&31));
    // c is ARGB: R in bits 16-23, B in 0-7. The target is RGBA.
    oColor=vec4(float((c>>16)&255u)/255.0,float((c>>8)&255u)/255.0,float(c&255u)/255.0,1.0);
})";

unsigned compile(unsigned type,const char* source) {
    const auto shader=gl::CreateShader(type);
    gl::ShaderSource(shader,1,&source,nullptr);gl::CompileShader(shader);
    int ok=0;gl::GetShaderiv(shader,GL_COMPILE_STATUS,&ok);
    if(!ok) {
        char log[2048]{};gl::GetShaderInfoLog(shader,sizeof log,nullptr,log);gl::DeleteShader(shader);
        throw std::runtime_error(std::string("Shader compilation failed: ")+log);
    }
    return shader;
}
unsigned link(const char* vertex,const char* fragment) {
    const auto program=gl::CreateProgram(),v=compile(GL_VERTEX_SHADER,vertex),f=compile(GL_FRAGMENT_SHADER,fragment);
    gl::AttachShader(program,v);gl::AttachShader(program,f);gl::LinkProgram(program);
    gl::DeleteShader(v);gl::DeleteShader(f);
    int ok=0;gl::GetProgramiv(program,GL_LINK_STATUS,&ok);
    if(!ok) {
        char log[2048]{};gl::GetProgramInfoLog(program,sizeof log,nullptr,log);gl::DeleteProgram(program);
        throw std::runtime_error(std::string("Shader link failed: ")+log);
    }
    return program;
}
int uniform(unsigned program,const char* name) {return gl::GetUniformLocation(program,name);}
float six(unsigned five) {return float(five?five*2+1:0);}
std::uint64_t hashPixels(const std::vector<std::uint32_t>& pixels) {
    std::uint64_t hash=1469598103934665603ull;
    for(auto p:pixels) {hash^=p;hash*=1099511628211ull;}
    return hash;
}
// Scale2x (EPX): doubles a pixel-art image, continuing its edges instead of blurring them.
std::vector<std::uint32_t> scale2x(const std::vector<std::uint32_t>& source,int width,int height) {
    std::vector<std::uint32_t> result(source.size()*4);
    auto at=[&](int x,int y){return source[std::size_t(std::clamp(y,0,height-1))*width+std::clamp(x,0,width-1)];};
    for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
        const auto p=at(x,y),a=at(x,y-1),b=at(x+1,y),c=at(x-1,y),d=at(x,y+1);
        auto* out=result.data()+std::size_t(y*2)*width*2+x*2;
        out[0]=c==a&&c!=d&&a!=b?a:p;
        out[1]=a==b&&a!=c&&b!=d?b:p;
        out[width*2]=d==c&&d!=b&&c!=a?c:p;
        out[width*2+1]=b==d&&b!=a&&d!=c?d:p;
    }
    return result;
}
// A texture's pixels as uploaded: upscaled (Scale2x passes up to `scale`)
// and premultiplied.
std::vector<std::uint32_t> texturePixels(const Texture& texture,int scale,int& width,int& height) {
    auto pixels=texture.pixels;width=texture.width;height=texture.height;
    for(int factor=1;factor<scale&&width*height<=256*256;factor*=2) {
        pixels=scale2x(pixels,width,height);width*=2;height*=2;
    }
    for(auto& p:pixels) {
        const auto a=p>>24;
        auto channel=[&](unsigned shift){return (((p>>shift)&255)*a+127)/255<<shift;};
        p=(a<<24)|channel(16)|channel(8)|channel(0);
    }
    return pixels;
}
struct GpuVertex {float position[4],color[4],texture[4];std::uint32_t params[4];float clip[4],rect[4];};
struct Prepared {
    // The three vertices live in a separate array, indexed by `first`: a town frame
    // is ~3,500 of these, and carrying 288 bytes of vertex data in each one meant a
    // megabyte copied through the vector (and again on the way out) every frame.
    std::uint32_t first=0;
    unsigned texture=0,wrap=0,attributes=0;
    float minY=0,maxY=0;
    bool translucent=false;
    // Orthographic pixel art (2D maps, sprites) samples its upscaled texels
    // directly: filtering would blend neighbouring tiles of a texture atlas.
    bool crisp=false;
};
std::array<float,4> apply(const GpuMatrix& m,const std::array<float,4>& v) {
    std::array<float,4> r{};
    for(unsigned row=0;row<4;++row)r[row]=m[row*4]*v[0]+m[row*4+1]*v[1]+m[row*4+2]*v[2]+m[row*4+3]*v[3];
    return r;
}
}
GpuMatrix gpuIdentity() {return {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};}
GpuMatrix gpuMultiply(const GpuMatrix& a,const GpuMatrix& b) {
    GpuMatrix r{};
    for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col)
        for(unsigned k=0;k<4;++k)r[row*4+col]+=a[row*4+k]*b[k*4+col];
    return r;
}
GpuMatrix gpuPlacement(float left,float top,float right,float bottom) {
    const float sx=(right-left)/2,sy=(top-bottom)/2,cx=(right+left)/2,cy=(top+bottom)/2;
    return {sx,0,0,cx,0,sy,0,cy,0,0,1,0,0,0,0,1};
}
GpuRenderer::GpuRenderer() {
    program_=link(sceneVertex,sceneFragment);
    fogProgram_=link(fullscreenVertex,fogFragment);
    composeProgram_=link(fullscreenVertex,composeFragment);
    compose2dProgram_=link(fullscreenVertex,compose2dFragment);
    reduceProgram_=link(fullscreenVertex,reduceFragment);
    captureProgram_=link(fullscreenVertex,captureFragment);
    overlayProgram_=link(overlayVertex,overlayFragment);
    gl::GenVertexArrays(1,&vertexArray_);gl::GenVertexArrays(1,&emptyArray_);gl::GenBuffers(1,&vertexBuffer_);
    gl::BindVertexArray(vertexArray_);gl::BindBuffer(GL_ARRAY_BUFFER,vertexBuffer_);
    for(unsigned i=0;i<3;++i) {
        gl::EnableVertexAttribArray(i);
        gl::VertexAttribPointer(i,4,GL_FLOAT,GL_FALSE,sizeof(GpuVertex),reinterpret_cast<void*>(std::uintptr_t(i*16)));
    }
    gl::EnableVertexAttribArray(3);
    gl::VertexAttribIPointer(3,4,GL_UNSIGNED_INT,sizeof(GpuVertex),reinterpret_cast<void*>(std::uintptr_t(48)));
    gl::EnableVertexAttribArray(4);
    gl::VertexAttribPointer(4,4,GL_FLOAT,GL_FALSE,sizeof(GpuVertex),reinterpret_cast<void*>(std::uintptr_t(64)));
    gl::EnableVertexAttribArray(5);
    gl::VertexAttribPointer(5,4,GL_FLOAT,GL_FALSE,sizeof(GpuVertex),reinterpret_cast<void*>(std::uintptr_t(80)));
    gl::BindVertexArray(0);
    gl::GenTextures(4,recipeTextures_);
    for(auto texture:recipeTextures_) {
        gl::BindTexture(GL_TEXTURE_2D,texture);
        gl::TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,256,GpuLayout::recipeRows,0,kUploadFormat,GL_UNSIGNED_BYTE,nullptr);
        gl::TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);gl::TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    }
    gl::GenQueries(4,frameQueries_);
    gl::GenTextures(1,&hdAtlas_);gl::BindTexture(GL_TEXTURE_2D,hdAtlas_);
    gl::TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,hdCellSize*hdAtlasCells,hdCellSize*hdAtlasCells,0,kUploadFormat,GL_UNSIGNED_BYTE,nullptr);
    gl::TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);gl::TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    gl::GenTextures(1,&sideColumns_);gl::BindTexture(GL_TEXTURE_2D,sideColumns_);
    gl::TexImage2D(GL_TEXTURE_2D,0,GL_R32I,2,384,0,GL_RED_INTEGER,GL_INT,nullptr);
    gl::TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);gl::TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
}
GpuRenderer::~GpuRenderer() {
    for(auto& [key,texture]:textures_)gl::DeleteTextures(1,&texture.name);
    release(scene_[0]);release(scene_[1]);release(fogged_[0]);release(fogged_[1]);release(native_[0]);release(native_[1]);
    release(output_);release(world_);
    if(readBuffers_[0])gl::DeleteBuffers(2,readBuffers_);
    if(queryActive_)gl::EndQuery(GL_TIME_ELAPSED);
    gl::DeleteQueries(4,frameQueries_);
    gl::DeleteTextures(4,recipeTextures_);gl::DeleteTextures(1,&sideColumns_);gl::DeleteTextures(1,&hdAtlas_);
    gl::DeleteBuffers(1,&vertexBuffer_);gl::DeleteVertexArrays(1,&vertexArray_);gl::DeleteVertexArrays(1,&emptyArray_);
    for(auto program:{program_,fogProgram_,composeProgram_,reduceProgram_,captureProgram_,overlayProgram_})gl::DeleteProgram(program);
    for(auto& [id,picture]:overlays_)gl::DeleteTextures(1,&picture.name);
}
void GpuRenderer::drawOverlay(unsigned id,std::span<const std::uint32_t> pixels,int width,int height,bool changed,
    int x,int y,int windowWidth,int windowHeight) {
    auto& picture=overlays_[id];
    if(!picture.name) {
        gl::GenTextures(1,&picture.name);gl::BindTexture(GL_TEXTURE_2D,picture.name);
        gl::TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);gl::TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        gl::TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);gl::TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        changed=true;
    }
    gl::ActiveTexture(GL_TEXTURE0);gl::BindTexture(GL_TEXTURE_2D,picture.name);
    if(changed||picture.width!=width||picture.height!=height) {
        gl::PixelStorei(GL_UNPACK_ALIGNMENT,4);
        std::vector<std::uint32_t> scratch;
        gl::TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,kUploadFormat,GL_UNSIGNED_BYTE,uploadOrder(pixels,scratch));
        picture.width=width;picture.height=height;
    }
    gl::BindFramebuffer(GL_FRAMEBUFFER,0);gl::Viewport(0,0,windowWidth,windowHeight);
    gl::UseProgram(overlayProgram_);gl::BindVertexArray(emptyArray_);
    gl::Uniform1i(uniform(overlayProgram_,"uPicture"),0);
    const float x0=2.f*x/windowWidth-1,x1=2.f*(x+width)/windowWidth-1,y0=1-2.f*(y+height)/windowHeight,y1=1-2.f*y/windowHeight;
    gl::Uniform4f(uniform(overlayProgram_,"uRect"),x0,y0,x1,y1);
    gl::Enable(GL_BLEND);gl::BlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
    gl::DrawArrays(GL_TRIANGLE_STRIP,0,4);
    gl::Disable(GL_BLEND);
}
int GpuRenderer::supersample() const {return std::clamp(settings.autoQuality?std::min(settings.supersample,autoSupersample_):settings.supersample,1,4);}
void GpuRenderer::beginFrameTiming() {
#ifndef __EMSCRIPTEN__
    if(queryActive_||queriesPending_==4)return;
    gl::BeginQuery(GL_TIME_ELAPSED,frameQueries_[queryHead_]);queryActive_=true;
#endif
}
void GpuRenderer::endFrameTiming() {
#ifndef __EMSCRIPTEN__
    // GL_TIME_ELAPSED and glGetQueryObjectui64v are not in GLES 3.0, so WebGL2
    // has no GPU-time query: nothing gets timed, gpuSamples_ never reaches the
    // threshold below, and automatic supersampling stays at the user's setting
    // until EXT_disjoint_timer_query_webgl2 is wired up.
    if(queryActive_) {gl::EndQuery(GL_TIME_ELAPSED);queryActive_=false;queryHead_=(queryHead_+1)%4;++queriesPending_;}
    // Results arrive a few frames later; collect those that are ready.
    while(queriesPending_) {
        const auto query=frameQueries_[(queryHead_+4-queriesPending_)%4];
        unsigned available=0;gl::GetQueryObjectuiv(query,GL_QUERY_RESULT_AVAILABLE,&available);
        if(!available)break;
        GLuint64 time=0;gl::GetQueryObjectui64v(query,GL_QUERY_RESULT,&time);
        --queriesPending_;gpuTime_+=double(time);++gpuSamples_;
        if(profiling())profileAdd("gpu-frame",double(time)/1000);
    }
#endif
    if(gpuSamples_<30)return;
    // Over 11 ms of GPU work per frame leaves too little margin for 60 frames
    // per second: lower the supersampling. Raise it again after a long quiet spell.
    const double milliseconds=gpuTime_/gpuSamples_/1e6;
    const int current=supersample();
    gpuTime_=0;gpuSamples_=0;
    if(!settings.autoQuality)return;
    // Lower after two busy half-seconds; raise when the next factor (cost
    // growing with its square) would still leave a wide margin.
    const double next=milliseconds*double((current+1)*(current+1))/double(current*current);
    // Thresholds for 60 frames per second (12 and 7 ms), scaled to the frame rate.
    const double budget=settings.frameMilliseconds/(1000.0/60);
    if(milliseconds>12*budget&&current>1) {
        quietWindows_=0;
        if(++busyWindows_>=2) {
            autoSupersample_=current-1;busyWindows_=0;
            std::printf("GPU %.1f ms par image: supersampling x%d\n",milliseconds,autoSupersample_);
        }
    } else if(next<7*budget&&current<settings.supersample) {
        busyWindows_=0;
        if(++quietWindows_>=4) {
            autoSupersample_=current+1;quietWindows_=0;
            std::printf("GPU %.1f ms par image: supersampling x%d\n",milliseconds,autoSupersample_);
        }
    } else quietWindows_=busyWindows_=0;
}
void GpuRenderer::keepWorld() {
    const auto& source=fogApplied_[0]?fogged_[0]:scene_[0];
    if(!source.framebuffer)return;
    resize(world_,source.width,source.height,false,false);
    gl::BindFramebuffer(GL_READ_FRAMEBUFFER,source.framebuffer);gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER,world_.framebuffer);
    gl::BlitFramebuffer(0,0,source.width,source.height,0,0,source.width,source.height,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    gl::BindFramebuffer(GL_FRAMEBUFFER,0);
}
unsigned GpuRenderer::threeColor(unsigned slot) const {return fogApplied_[slot]?fogged_[slot].color:scene_[slot].color;}
void GpuRenderer::release(Target& target) {
    if(target.framebuffer)gl::DeleteFramebuffers(1,&target.framebuffer);
    for(auto* texture:{&target.color,&target.attributes,&target.depth})if(*texture)gl::DeleteTextures(1,texture);
    target=Target{};
}
void GpuRenderer::resize(Target& target,int width,int height,bool attributes,bool depth) {
    if(target.framebuffer&&target.width==width&&target.height==height)return;
    release(target);
    target.width=width;target.height=height;
    auto texture=[&](unsigned& name,int internal,unsigned format,unsigned type,bool linear) {
        gl::GenTextures(1,&name);gl::BindTexture(GL_TEXTURE_2D,name);
        gl::TexImage2D(GL_TEXTURE_2D,0,internal,width,height,0,format,type,nullptr);
        gl::TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,linear?GL_LINEAR:GL_NEAREST);
        gl::TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,linear?GL_LINEAR:GL_NEAREST);
        gl::TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);gl::TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    };
    gl::GenFramebuffers(1,&target.framebuffer);gl::BindFramebuffer(GL_FRAMEBUFFER,target.framebuffer);
    texture(target.color,GL_RGBA8,GL_RGBA,GL_UNSIGNED_BYTE,true);
    gl::FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,target.color,0);
    if(attributes) {
        texture(target.attributes,GL_RGBA8,GL_RGBA,GL_UNSIGNED_BYTE,false);
        gl::FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT1,GL_TEXTURE_2D,target.attributes,0);
    }
    if(depth) {
        texture(target.depth,GL_DEPTH24_STENCIL8,GL_DEPTH_STENCIL,GL_UNSIGNED_INT_24_8,false);
        gl::FramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,target.depth,0);
    }
    const unsigned buffers[2]={GL_COLOR_ATTACHMENT0,GL_COLOR_ATTACHMENT1};
    gl::DrawBuffers(attributes?2:1,buffers);
    if(gl::CheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)throw std::runtime_error("Incomplete OpenGL framebuffer");
}
unsigned GpuRenderer::upload(const Texture& texture,std::uint64_t key) {
    auto& entry=textures_[key];entry.lastUse=renders_;
    // Decoded pixels only change with a new decode (a new version).
    if(entry.name&&entry.generation==texture.version)return entry.name;
    entry.generation=texture.version;
    ProfileScope timing("texture-check");
    const auto hash=hashPixels(texture.pixels);
    if(entry.name&&entry.hash==hash&&entry.width==texture.width&&entry.height==texture.height)return entry.name;
    int width=0,height=0;
    const auto premultiplied=texturePixels(texture,settings.textureScale,width,height);
    finishUpload(entry,premultiplied,width,height,hash,texture,true);
    return entry.name;
}
void GpuRenderer::finishUpload(GpuTexture& entry,const std::vector<std::uint32_t>& premultiplied,int width,int height,
    std::uint64_t hash,const Texture& texture,bool mipmaps) {
    ProfileScope uploadTiming("texture-upload");
    if(!entry.name)gl::GenTextures(1,&entry.name);
    gl::BindTexture(GL_TEXTURE_2D,entry.name);
    gl::PixelStorei(GL_UNPACK_ALIGNMENT,4);
    std::vector<std::uint32_t> scratch;
    gl::TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,kUploadFormat,GL_UNSIGNED_BYTE,uploadOrder(premultiplied,scratch));
    // Mipmaps and anisotropic filtering for surfaces seen at grazing angles;
    // a provisional texture (upscaled in a later frame) has its level 0 only.
    gl::TexParameteri(GL_TEXTURE_2D,0x813d,mipmaps?1000:0); // GL_TEXTURE_MAX_LEVEL
    if(mipmaps)gl::GenerateMipmap(GL_TEXTURE_2D);
    if(!anisotropyChecked_) {
        gl::TexParameteri(GL_TEXTURE_2D,0x84fe,8); // GL_TEXTURE_MAX_ANISOTROPY, where supported
        anisotropy_=gl::GetError()==GL_NO_ERROR;anisotropyChecked_=true;
        while(gl::GetError()!=GL_NO_ERROR) {}
    } else if(anisotropy_)gl::TexParameteri(GL_TEXTURE_2D,0x84fe,8);
    entry.hash=hash;entry.width=texture.width;entry.height=texture.height;entry.generation=texture.version;
}
void GpuRenderer::dropTextures() {
    for(auto& [key,entry]:textures_)if(entry.name) {samplingState_.erase(entry.name);gl::DeleteTextures(1,&entry.name);}
    textures_.clear();upgrades_.clear();
}
void GpuRenderer::prepareTextures(std::span<const GpuBatch> batches,GxTextures& textures,const GxRenderOptions& options) {
    if(uploadedScale_!=settings.textureScale) {dropTextures();uploadedScale_=settings.textureScale;}
    // The textures this frame shows for the first time (a new map brings
    // dozens): their upscaling runs in parallel before the uploads.
    if(!(options.displayControl&1))return;
    struct Pending {std::uint64_t key;GpuTexture* entry;const Texture* texture;std::uint64_t hash;std::vector<std::uint32_t> pixels;int width=0,height=0;};
    std::vector<Pending> pending;
    std::uint64_t lastKey=~0ull;
    for(const auto& batch:batches)for(const auto& triangle:batch.triangles) {
        if(!(triangle.perspective?batch.perspective:batch.orthographic))continue;
        if(batch.layer>=0&&triangle.layer!=batch.layer)continue;
        const auto parameters=triangle.material.texture,format=(parameters>>26)&7;
        if(!format||(format==7&&batch.worldScreen>=0))continue;
        const std::uint64_t key=(std::uint64_t(triangle.material.palette)<<32)|(parameters&0x3ff0ffffu);
        if(key==lastKey)continue;
        lastKey=key;
        auto& entry=textures_[key];
        const Texture* texture=nullptr;
        try {texture=&textures.texture(parameters,triangle.material.palette);} catch(const std::exception&) {continue;}
        if(entry.name&&entry.generation==texture->version)continue;
        if(std::any_of(pending.begin(),pending.end(),[&](const Pending& p){return p.entry==&entry;}))continue;
        const auto hash=hashPixels(texture->pixels);
        if(entry.name&&entry.hash==hash&&entry.width==texture->width&&entry.height==texture->height) {
            entry.generation=texture->version;
            continue;
        }
        pending.push_back({key,&entry,texture,hash});
    }
    // A new map shows dozens of new textures at once: a few are uploaded
    // upscaled, the others as they are, upscaled over the next frames.
    constexpr std::size_t fullPerFrame=2;
    ProfileScope timing("texture-prepare");
    DQIV_PARALLEL_FOR_EACH(pending.begin(),pending.end(),[&](Pending& p) {
        const bool full=std::size_t(&p-pending.data())<fullPerFrame;
        p.pixels=texturePixels(*p.texture,full?settings.textureScale:1,p.width,p.height);
    });
    for(auto& p:pending) {
        const bool provisional=std::size_t(&p-pending.data())>=fullPerFrame&&settings.textureScale>1;
        finishUpload(*p.entry,p.pixels,p.width,p.height,p.hash,*p.texture,!provisional);
        if(provisional)
            upgrades_.push_back({p.key,*p.texture,p.hash});
    }
}
void GpuRenderer::upgradeTextures() {
    // Textures first uploaded as they are: a few more upscaled each frame.
    constexpr std::size_t perFrame=4;
    if(upgrades_.empty())return;
    ProfileScope timing("texture-upgrade");
    const std::size_t count=std::min(perFrame,upgrades_.size());
    std::vector<std::vector<std::uint32_t>> pixels(count);
    std::vector<std::array<int,2>> sizes(count);
    std::vector<std::size_t> indices(count);
    for(std::size_t i=0;i<count;++i)indices[i]=i;
    DQIV_PARALLEL_FOR_EACH(indices.begin(),indices.end(),[&](std::size_t i) {
        pixels[i]=texturePixels(upgrades_[i].texture,settings.textureScale,sizes[i][0],sizes[i][1]);
    });
    for(std::size_t i=0;i<count;++i) {
        auto& upgrade=upgrades_[i];
        // Unless the texture was replaced or released meanwhile.
        const auto found=textures_.find(upgrade.key);
        if(found!=textures_.end()&&found->second.hash==upgrade.hash&&found->second.generation==upgrade.texture.version)
            finishUpload(found->second,pixels[i],sizes[i][0],sizes[i][1],upgrade.hash,upgrade.texture,true);
    }
    upgrades_.erase(upgrades_.begin(),upgrades_.begin()+std::ptrdiff_t(count));
}
void GpuRenderer::render3D(std::span<const GpuBatch> batches,const GxRasterState& state,GxTextures& textures,
    const GxRenderOptions& options,int width,int height,const GpuFogSplit& fog,unsigned slot) {
    // Supersampling stays within a 4096-pixel target (4K windows render at 1x).
    beginFrameTiming();
    int ss=supersample();
    while(ss>1&&std::max(width,height)*ss>4096)--ss;
    auto& fogged=fogged_[slot&1];lastSlot_=slot&1;++renders_;
    auto& scene=scene_[slot&1];
    resize(scene,width*ss,height*ss,true,true);resize(fogged,width*ss,height*ss,false,false);
    DQIV_TRACE("after resize");
    if(!betweenFrame) {
        prepareTextures(batches,textures,options);
        upgradeTextures();
    }
    DQIV_TRACE("after prepareTextures");
    std::vector<Prepared> polygons;
    std::vector<GpuVertex> staged;   // three per polygon, in submission order
    std::size_t triangleCount=0;
    for(const auto& batch:batches)triangleCount+=batch.triangles.size();
    {   // Reserve once: a town frame is ~3,500 triangles, and growing the vector from
        // zero reallocated and copied it a dozen times every frame.
        polygons.reserve(triangleCount);
        staged.reserve(triangleCount*3);
    }
    std::optional<ProfileScope> stage(std::in_place,"r3d-prepare");
    std::uint32_t cachedViewport=0;const GpuMatrix* cachedTarget=nullptr;GpuMatrix cachedMatrix{};
    std::uint64_t cachedTexture=~0ull;unsigned cachedName=0;float cachedSize[2]={1,1};
    for(const auto& batch:batches)for(const auto& triangle:batch.triangles) {
        if(!(triangle.perspective?batch.perspective:batch.orthographic))continue;
        if(batch.layer>=0&&triangle.layer!=batch.layer)continue;
        const auto& placement=batch.placement;
        const auto attributes=triangle.material.polygon,alpha=(attributes>>16)&31;
        if(!(attributes&0xc0)||!alpha)continue;
        bool outsideFar=false;
        for(const auto& v:triangle.vertices)outsideFar|=v.clip[2]>v.clip[3];
        if(outsideFar&&!(attributes&(1<<12)))continue;
        const auto viewport=triangle.viewport;
        const float vx=float(viewport&255),vy=float((viewport>>8)&255);
        const float vw=float(((viewport>>16)&255)+1)-vx,vh=float((viewport>>24)+1)-vy;
        if(vw<=0||vh<=0)continue;
        const GpuMatrix viewportMatrix{vw/256,0,0,vx/128+vw/256-1, 0,vh/192,0,vy/96+vh/192-1, 0,0,1,0, 0,0,0,1};
        const GpuMatrix* target=&placement.world;
        if(!triangle.perspective) {
            float left=1e9f,right=-1e9f;
            for(const auto& v:triangle.vertices) {
                const auto p=apply(viewportMatrix,v.clip);
                if(p[3]!=0) {left=std::min(left,p[0]/p[3]);right=std::max(right,p[0]/p[3]);}
            }
            // Only plain full-width quads (fades, flashes) widen; textured ones are pictures.
            bool plain=!((triangle.material.texture>>26)&7)||!(options.displayControl&1);
            if(!plain) {
                // A texture stretched from a texel or two is a flat colour too.
                float u0=1e9f,u1=-1e9f,v0=1e9f,v1=-1e9f;
                for(const auto& v:triangle.vertices) {u0=std::min(u0,v.u);u1=std::max(u1,v.u);v0=std::min(v0,v.v);v1=std::max(v1,v.v);}
                plain=u1-u0<=2&&v1-v0<=2;
            }
            target=plain&&left<=-0.999f&&right>=0.999f?&placement.screenWide:&placement.screen;
        }
        // Consecutive triangles mostly share their viewport and placement.
        if(viewport!=cachedViewport||target!=cachedTarget) {
            cachedMatrix=gpuMultiply(*target,viewportMatrix);cachedViewport=viewport;cachedTarget=target;
        }
        const auto& matrix=cachedMatrix;
        Prepared polygon;polygon.attributes=attributes;
        polygon.first=static_cast<std::uint32_t>(staged.size());
        staged.resize(polygon.first+3);   // rolled back below if the triangle is culled
        const auto parameters=triangle.material.texture,format=(parameters>>26)&7,mode=(attributes>>4)&3;
        const bool textured=format&&(options.displayControl&1);
        float size[2]={1,1};
        // A screen effect's captured picture: the kept world picture instead,
        // its rows from the top (render targets store rows bottom-up).
        const bool fromWorld=textured&&batch.worldScreen>=0&&world_.color&&format==7&&
            ((parameters>>20)&7)==5&&((parameters>>23)&7)==5;
        float worldRow=0;
        if(fromWorld) {
            polygon.texture=world_.color;size[0]=256;size[1]=-384;worldRow=192.f*float(batch.worldScreen)-384;
            polygon.wrap=0;polygon.crisp=true;
        } else if(textured) {
            const std::uint64_t key=(std::uint64_t(triangle.material.palette)<<32)|(parameters&0x3ff0ffffu);
            if(key!=cachedTexture) {
                const auto& decoded=textures.texture(parameters,triangle.material.palette);
                cachedName=upload(decoded,key);cachedSize[0]=float(decoded.width);cachedSize[1]=float(decoded.height);cachedTexture=key;
            }
            polygon.texture=cachedName;size[0]=cachedSize[0];size[1]=cachedSize[1];
            polygon.wrap=(parameters>>16)&15;
            polygon.crisp=!triangle.perspective;
        }
        polygon.translucent=(alpha<31)||(textured&&!(mode&1)&&(format==1||format==6));
        bool visible=true,allFront=true;
        float screen[3][2]{};
        polygon.minY=1e9f;polygon.maxY=-1e9f;
        // The texels the triangle covers: a texture that doesn't repeat is
        // sampled only inside them (parts of a sprite sheet).
        float rect[4]={1e9f,1e9f,-1e9f,-1e9f};
        for(const auto& v:triangle.vertices) {
            rect[0]=std::min(rect[0],v.u);rect[1]=std::min(rect[1],v.v+worldRow);
            rect[2]=std::max(rect[2],v.u);rect[3]=std::max(rect[3],v.v+worldRow);
        }
        const bool keepInside=textured&&!fromWorld&&!(((parameters>>16)&3));
        // Pictures cut from a sheet (a monster's parts, characters): drawn at
        // a higher resolution, parts meeting on a DS pixel edge can leave a
        // hairline between them. Opaque ones grow by half a DS pixel all
        // around, their texture carried on (the margin repeats their edge
        // texels), so neighbouring parts overlap instead.
        std::array<GxVertex,3> grown=triangle.vertices;
        // Battle sprites land on the DS pixel grid, as on the console.
        std::array<GxVertex,3> snapped=triangle.vertices;
        if(keepInside&&triangle.perspective&&wideBattleFrame())for(auto& v:snapped)if(v.clip[3]>0) {
            float x=v.clip[0]/v.clip[3],y=v.clip[1]/v.clip[3];
            snapBattlePoint(x,y);
            v.clip[0]=x*v.clip[3];v.clip[1]=y*v.clip[3];
        }
        grown=snapped;
        if(keepInside&&!polygon.translucent) {
            const auto& t=snapped;
            bool front=true;
            for(unsigned i=0;i<3&&front;++i)front=t[i].clip[3]>0;
            if(front) {
                // DS screen pixels (texture coordinates taken as affine over half a pixel);
                // the wide battle scene spans the whole canvas, as wide as it is.
                const bool wideScene=triangle.perspective&&wideBattleFrame();
                const float unitX=wideScene?dualScreen().canvasWidth*0.5f:128.f,unitY=wideScene?192.f:96.f;
                float x[3],y[3];
                for(unsigned i=0;i<3;++i) {x[i]=t[i].clip[0]/t[i].clip[3]*unitX;y[i]=t[i].clip[1]/t[i].clip[3]*unitY;}
                const float ex1=x[1]-x[0],ey1=y[1]-y[0],ex2=x[2]-x[0],ey2=y[2]-y[0];
                const float det=ex1*ey2-ex2*ey1;
                if(std::abs(det)>0.5f) {
                    // Texture coordinates follow the screen (affine on a card). One
                    // reciprocal, not four divisions: this runs per opaque sprite.
                    const float invDet=1.f/det;
                    const float du1=t[1].u-t[0].u,dv1=t[1].v-t[0].v,du2=t[2].u-t[0].u,dv2=t[2].v-t[0].v;
                    const float dudx=(du1*ey2-du2*ey1)*invDet,dudy=(du2*ex1-du1*ex2)*invDet;
                    const float dvdx=(dv1*ey2-dv2*ey1)*invDet,dvdy=(dv2*ex1-dv1*ex2)*invDet;
                    const float side=det>0?1.f:-1.f;
                    // Half a DS pixel, more for an enlarged sprite (battle zoom):
                    // the overlap covers a good half of one of its texels (more
                    // would cover the neighbouring parts' outlines).
                    const float texelsPerPixel=std::sqrt(std::abs(dudx*dvdy-dudy*dvdx));
                    const float grow=std::clamp(0.6f/std::max(texelsPerPixel,1e-3f),0.5f,3.f);
                    // A card (every corner on its picture's rectangle) grows as
                    // a whole, in its texture: the two triangles of a part keep
                    // a straight common outline. Grown corner by corner, the
                    // acute corner at its diagonal pulled that outline inwards
                    // and left a hairline between two parts (a bullfinch's legs).
                    float u0=t[0].u,u1=t[0].u,v0=t[0].v,v1=t[0].v;
                    for(unsigned i=1;i<3;++i) {u0=std::min(u0,t[i].u);u1=std::max(u1,t[i].u);v0=std::min(v0,t[i].v);v1=std::max(v1,t[i].v);}
                    constexpr float corner=0.05f;
                    bool card=u1-u0>corner&&v1-v0>corner;
                    for(unsigned i=0;i<3&&card;++i)
                        card=(std::abs(t[i].u-u0)<corner||std::abs(t[i].u-u1)<corner)&&(std::abs(t[i].v-v0)<corner||std::abs(t[i].v-v1)<corner);
                    const float jacobian=dudx*dvdy-dudy*dvdx;
                    if(card&&std::abs(jacobian)>1e-6f) {
                        const float texels=std::max(0.6f,0.5f*texelsPerPixel);
                        for(unsigned i=0;i<3;++i) {
                            const float du=std::abs(t[i].u-u0)<corner?-texels:texels,dv=std::abs(t[i].v-v0)<corner?-texels:texels;
                            const float ox=(dvdy*du-dudy*dv)/jacobian,oy=(dudx*dv-dvdx*du)/jacobian;
                            grown[i].clip[0]+=ox/unitX*t[i].clip[3];grown[i].clip[1]+=oy/unitY*t[i].clip[3];
                            grown[i].u+=du;grown[i].v+=dv;
                        }
                    } else for(unsigned i=0;i<3;++i) {
                        // Each corner moves out along both its edges' normals.
                        const unsigned a=(i+2)%3,b=(i+1)%3;
                        auto normal=[&](unsigned from,unsigned to,float& nx,float& ny) {
                            const float dx=x[to]-x[from],dy=y[to]-y[from],length=std::sqrt(dx*dx+dy*dy);
                            nx=side*dy/length;ny=-side*dx/length;
                        };
                        float n1x,n1y,n2x,n2y;normal(a,i,n1x,n1y);normal(i,b,n2x,n2y);
                        const float dot=std::max(n1x*n2x+n1y*n2y,-0.5f);
                        const float ox=grow*(n1x+n2x)/(1+dot),oy=grow*(n1y+n2y)/(1+dot);
                        grown[i].clip[0]+=ox/unitX*t[i].clip[3];grown[i].clip[1]+=oy/unitY*t[i].clip[3];
                        grown[i].u+=dudx*ox+dudy*oy;grown[i].v+=dvdx*ox+dvdy*oy;
                    }
                }
            }
        }
        for(unsigned i=0;i<3;++i) {
            const auto& v=grown[i];
            const auto p=apply(matrix,v.clip);
            auto& out=staged[polygon.first+i];
            std::copy(p.begin(),p.end(),out.position);
            for(unsigned c=0;c<3;++c)out.color[c]=six((v.color>>(5*c))&31)/63.f;
            out.color[3]=1;
            out.texture[0]=v.u;out.texture[1]=v.v+worldRow;out.texture[2]=size[0];out.texture[3]=size[1];
            out.params[0]=attributes;out.params[1]=parameters;
            // Overlays placed on one screen of a larger target stop at that
            // screen's edges, as the hardware clips them; in wide battles the
            // nearly invisible ones (alpha 1: the light around a target, which
            // only keeps the dimming overlay of its polygon ID away) follow the
            // monsters beside that screen.
            if(target==&placement.screen&&!(wideBattleFrame()&&alpha<=1)) {
                const float w=v.clip[3];
                out.clip[0]=w+v.clip[0];out.clip[1]=w-v.clip[0];out.clip[2]=w+v.clip[1];out.clip[3]=w-v.clip[1];
            } else out.clip[0]=out.clip[1]=out.clip[2]=out.clip[3]=1;
            out.params[2]=(textured?1u:0u)|(polygon.translucent?0u:2u)|(keepInside?4u:0u);out.params[3]=0;
            std::copy(rect,rect+4,out.rect);
            if(p[3]<=0)allFront=false;
            else {screen[i][0]=p[0]/p[3];screen[i][1]=-p[1]/p[3];}
            const float y=p[3]>0?-p[1]/p[3]:0;
            polygon.minY=std::min(polygon.minY,y);polygon.maxY=std::max(polygon.maxY,y);
        }
        if(allFront) {
            // Orientation in a y-down screen, as the hardware tests it.
            const float area=(screen[1][0]-screen[0][0])*(screen[2][1]-screen[0][1])-(screen[1][1]-screen[0][1])*(screen[2][0]-screen[0][0]);
            if(area==0)visible=false;
            else if(area<0?!(attributes&0x80):!(attributes&0x40))visible=false;
        }
        if(visible) {staged.resize(polygon.first+3);polygons.push_back(polygon);}
        else staged.resize(polygon.first);
    }
    stage.emplace("r3d-sort");
    // Opaque polygons first in submission order, then translucent ones by
    // their screen extent (unless the game sorts them itself).
    std::vector<std::uint32_t> order;order.reserve(polygons.size());
    for(std::uint32_t i=0;i<polygons.size();++i)if(!polygons[i].translucent)order.push_back(i);
    const auto opaque=order.size();
    for(std::uint32_t i=0;i<polygons.size();++i)if(polygons[i].translucent)order.push_back(i);
    if(!options.manualSort)
        std::stable_sort(order.begin()+std::ptrdiff_t(opaque),order.end(),[&](std::uint32_t i,std::uint32_t j) {
            const auto& a=polygons[i];const auto& b=polygons[j];
            return a.maxY!=b.maxY?a.maxY<b.maxY:a.minY<b.minY;
        });
    std::vector<GpuVertex> vertices;vertices.resize(order.size()*3);
    for(std::size_t k=0;k<order.size();++k) {
        const auto* from=staged.data()+polygons[order[k]].first;
        std::copy_n(from,3,vertices.data()+k*3);
    }

    stage.emplace("r3d-draw");
    gl::BindFramebuffer(GL_FRAMEBUFFER,scene.framebuffer);
    gl::Viewport(0,0,scene.width,scene.height);
    gl::Disable(GL_SCISSOR_TEST);gl::Disable(GL_CULL_FACE);
    gl::ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);gl::DepthMask(GL_TRUE);gl::StencilMask(0xff);
    const unsigned clear=state.clearColor,clearAlpha=(clear>>16)&31;
    const float ca=clearAlpha/31.f;
    const float clearColor[4]={six(clear&31)/63.f*ca,six((clear>>5)&31)/63.f*ca,six((clear>>10)&31)/63.f*ca,ca};
    const float clearAttributes[4]={(clear>>15)&1?1.f:0.f,float((clear>>24)&63)/63.f,0,1};
    gl::ClearBufferfv(GL_COLOR,0,clearColor);gl::ClearBufferfv(GL_COLOR,1,clearAttributes);
    gl::ClearBufferfi(GL_DEPTH_STENCIL,0,float((unsigned(state.clearDepth)<<9)|511)/16777215.f,0);
    DQIV_TRACE("after clears");
    if(!vertices.empty()) {
        gl::UseProgram(program_);
        gl::Uniform1i(uniform(program_,"uTexture"),0);
        gl::Uniform1i(uniform(program_,"uControl"),int(options.displayControl));
        gl::Uniform1f(uniform(program_,"uAlphaReference"),float(options.alphaTest&31));
        gl::Uniform1i(uniform(program_,"uWBuffer"),options.wBuffer?1:0);
        float toon[96];
        for(unsigned i=0;i<32;++i)for(unsigned c=0;c<3;++c)toon[i*3+c]=six((state.toonTable[i]>>(5*c))&31);
        gl::Uniform3fv(uniform(program_,"uToon"),32,toon);
        const int passLocation=uniform(program_,"uPass");
        gl::BindVertexArray(vertexArray_);gl::BindBuffer(GL_ARRAY_BUFFER,vertexBuffer_);
        gl::BufferData(GL_ARRAY_BUFFER,vertices.size()*sizeof(GpuVertex),vertices.data(),GL_STREAM_DRAW);
        DQIV_TRACE("after BufferData");
        gl::Enable(GL_DEPTH_TEST);gl::Enable(GL_STENCIL_TEST);gl::ActiveTexture(GL_TEXTURE0);
        const int clipLocation=uniform(program_,"uClipEnabled");gl::Uniform1i(clipLocation,1);
        const bool blending=(options.displayControl&8)!=0;
        const int filter=settings.smoothTextures?GL_LINEAR:GL_NEAREST;
        std::size_t start=0;
        while(start<order.size()) {
            const auto& first=polygons[order[start]];
            const auto attributes=first.attributes,mode=(attributes>>4)&3,id=(attributes>>24)&63;
            const unsigned kind=mode==3?(id?2:1):0;
            auto same=[&](const Prepared& p) {
                const auto a=p.attributes,m=(a>>4)&3,i=(a>>24)&63;
                return p.texture==first.texture&&p.wrap==first.wrap&&p.crisp==first.crisp&&p.translucent==first.translucent&&
                    (m==3?(i?2u:1u):0u)==kind&&((a^attributes)&((1u<<14)|(1u<<11)))==0&&(!first.translucent&&kind==0||i==id);
            };
            std::size_t end=start+1;while(end<order.size()&&same(polygons[order[end]]))++end;
            // Bound every time, not only when there is a texture. An untextured
            // batch used to leave whatever the last cycle put on unit 0 - and the
            // last cycle's readNative leaves fogged_'s texture there, which is
            // attached to a framebuffer, and that is what made the second draw fail.
            gl::BindTexture(GL_TEXTURE_2D,first.texture);
            if(first.texture) {
                const bool smooth=settings.smoothTextures&&!first.crisp;
                // Sampling parameters are only sent when they change for this texture.
                const unsigned sampling=first.wrap|(smooth?16u:0u)|(filter==GL_LINEAR?32u:0u);
                auto& known=samplingState_[first.texture];
                if(known!=sampling+1) {
                    known=sampling+1;
                    auto wrap=[&](bool repeat,bool flip){return repeat?(flip?GL_MIRRORED_REPEAT:GL_REPEAT):GL_CLAMP_TO_EDGE;};
                    gl::TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,wrap(first.wrap&1,first.wrap&4));
                    gl::TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,wrap(first.wrap&2,first.wrap&8));
                    gl::TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,smooth?GL_LINEAR_MIPMAP_LINEAR:GL_NEAREST);
                    gl::TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,smooth?filter:GL_NEAREST);
                }
            }
            gl::DepthFunc(GL_LESS);
            gl::ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
            if(kind==1) {
                // Shadow volume mask: flag pixels where the depth test fails.
                gl::Uniform1i(passLocation,1);gl::Disable(GL_BLEND);gl::ColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);
                gl::DepthMask(GL_FALSE);gl::StencilFunc(GL_ALWAYS,0x80,0xff);gl::StencilMask(0x80);
                gl::StencilOp(GL_KEEP,GL_REPLACE,GL_KEEP);
            } else if(kind==2) {
                gl::Uniform1i(passLocation,1);
                if(first.translucent&&blending) {gl::Enable(GL_BLEND);gl::BlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);} else gl::Disable(GL_BLEND);
                gl::DepthMask((attributes&(1<<11))?GL_TRUE:GL_FALSE);
                gl::StencilFunc(GL_EQUAL,0x80,0x80);gl::StencilMask(0x80);gl::StencilOp(GL_KEEP,GL_KEEP,GL_ZERO);
            } else if(first.translucent) {
                gl::Uniform1i(passLocation,1);
                if(blending) {gl::Enable(GL_BLEND);gl::BlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);} else gl::Disable(GL_BLEND);
                gl::DepthMask((attributes&(1<<11))?GL_TRUE:GL_FALSE);
                gl::StencilFunc(GL_NOTEQUAL,int(0x40|id),0x7f);gl::StencilMask(0x7f);gl::StencilOp(GL_KEEP,GL_KEEP,GL_REPLACE);
            } else {
                gl::Uniform1i(passLocation,0);gl::Disable(GL_BLEND);gl::DepthMask(GL_TRUE);
                gl::StencilFunc(GL_ALWAYS,0,0xff);gl::StencilMask(0x7f);gl::StencilOp(GL_KEEP,GL_KEEP,GL_REPLACE);
            }
            gl::DrawArrays(GL_TRIANGLES,int(start*3),int((end-start)*3));
            DQIV_TRACE("after DrawArrays");
            start=end;
        }
        gl::Disable(GL_BLEND);gl::Disable(GL_DEPTH_TEST);gl::Disable(GL_STENCIL_TEST);
        gl::Uniform1i(clipLocation,0);
        gl::DepthMask(GL_TRUE);gl::StencilMask(0xff);gl::ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    }
    gpuCheckpoint("gpu-scene");
    for(auto it=textures_.begin();it!=textures_.end();) {
        // Textures unused for a few seconds are released.
        if(renders_-it->second.lastUse>240) {
            samplingState_.erase(it->second.name);gl::DeleteTextures(1,&it->second.name);it=textures_.erase(it);
        } else ++it;
    }
    // Fog uses the stored depth and the per-pixel fog flag. Without fog the
    // scene itself is the result.
    const GxRasterState* fogs[2]={&state,fog.upper?fog.upper:&state};
    fogApplied_[slot&1]=fogs[0]->fogEnabled||fogs[1]->fogEnabled;
    if(!fogApplied_[slot&1])return;
    gl::BindFramebuffer(GL_FRAMEBUFFER,fogged.framebuffer);gl::Viewport(0,0,fogged.width,fogged.height);
    gl::UseProgram(fogProgram_);gl::BindVertexArray(emptyArray_);
    gl::ActiveTexture(GL_TEXTURE0);gl::BindTexture(GL_TEXTURE_2D,scene.color);
    gl::ActiveTexture(GL_TEXTURE1);gl::BindTexture(GL_TEXTURE_2D,scene.depth);
    gl::ActiveTexture(GL_TEXTURE2);gl::BindTexture(GL_TEXTURE_2D,scene.attributes);
    gl::Uniform1i(uniform(fogProgram_,"uColor"),0);gl::Uniform1i(uniform(fogProgram_,"uDepth"),1);gl::Uniform1i(uniform(fogProgram_,"uAttributes"),2);
    int enabled[2],alphaOnly[2];float colors[8],table[64],offsets[2],steps[2];
    for(unsigned i=0;i<2;++i) {
        const auto& f=*fogs[i];const auto c=f.fogColor;
        enabled[i]=f.fogEnabled?1:0;alphaOnly[i]=f.fogAlphaOnly?1:0;
        colors[i*4]=six(c&31)/63.f;colors[i*4+1]=six((c>>5)&31)/63.f;colors[i*4+2]=six((c>>10)&31)/63.f;colors[i*4+3]=((c>>16)&31)/31.f;
        for(unsigned n=0;n<32;++n)table[i*32+n]=f.fogTable[n]==127?128.f:float(f.fogTable[n]&127);
        offsets[i]=float(f.fogOffset);steps[i]=float(0x400u>>std::min(15u,f.fogSlope));
    }
    gl::Uniform1iv(uniform(fogProgram_,"uEnabled"),2,enabled);gl::Uniform1iv(uniform(fogProgram_,"uAlphaOnly"),2,alphaOnly);
    gl::Uniform4fv(uniform(fogProgram_,"uFogColor"),2,colors);gl::Uniform1fv(uniform(fogProgram_,"uFogTable"),64,table);
    gl::Uniform1fv(uniform(fogProgram_,"uFogOffset"),2,offsets);gl::Uniform1fv(uniform(fogProgram_,"uFogStep"),2,steps);
    gl::Uniform1f(uniform(fogProgram_,"uSplit"),fog.split*float(fogged.height));
    gl::Uniform1f(uniform(fogProgram_,"uBand"),fog.band*float(fogged.height));
    gl::DrawArrays(GL_TRIANGLE_STRIP,0,4);
    gl::ActiveTexture(GL_TEXTURE0);
    gpuCheckpoint("gpu-fog");
}
namespace {
// Un-premultiply through a reciprocal table. The division form did three integer
// divisions per pixel - 147,456 a frame, and integer division is one of the slower
// wasm instructions. Rounding is the same to within one count.
void unpremultiplyNative(const std::vector<std::uint32_t>& raw,std::vector<std::uint32_t>& pixels) {
    static const struct Reciprocal {
        unsigned value[256];
        Reciprocal() {value[0]=0;for(unsigned a=1;a<256;++a)value[a]=(255u*65536u+a/2)/a;}
    } reciprocal;
    pixels.resize(256*192);
    for(unsigned y=0;y<192;++y)for(unsigned x=0;x<256;++x) {
        const auto p=raw[(191-y)*256+x],a=p>>24;
        const unsigned r=reciprocal.value[a];
        auto channel=[&](unsigned shift){return std::min(255u,((((p>>shift)&255)*r)+32768u)>>16);};
        pixels[y*256+x]=(a<<24)|(channel(kShiftR)<<16)|(channel(kShiftG)<<8)|channel(kShiftB);
    }
}
}
void GpuRenderer::queueNative(const std::array<float,4>& source) {
    // Two targets: the readback of the previous frame is what the CPU holds, and
    // that is also the picture a display-mode-2 capture shows. The one just drawn
    // must not overwrite it before the compositor has had this frame.
    nativeSlot_^=1;
    auto& native=native_[nativeSlot_];
    resize(native,256,192,false,false);
    gl::BindFramebuffer(GL_FRAMEBUFFER,native.framebuffer);gl::Viewport(0,0,256,192);
    DQIV_TRACE("readNative bind");
    gl::UseProgram(reduceProgram_);gl::BindVertexArray(emptyArray_);
    const auto& fogged=fogged_[lastSlot_];
    gl::ActiveTexture(GL_TEXTURE0);gl::BindTexture(GL_TEXTURE_2D,threeColor(lastSlot_));
    gl::Uniform1i(uniform(reduceProgram_,"uSource"),0);
    // Source rectangle in target fractions, y from the top (GL rows start at the bottom).
    gl::Uniform2f(uniform(reduceProgram_,"uOrigin"),source[0]*fogged.width,(1-source[1]-source[3])*fogged.height);
    gl::Uniform2f(uniform(reduceProgram_,"uScale"),source[2]*fogged.width/256.f,source[3]*fogged.height/192.f);
    gl::DrawArrays(GL_TRIANGLE_STRIP,0,4);
    DQIV_TRACE("readNative draw");
    nativeDrawn_=true;   // the next frame takes it, before queuing its own scene
    ++nativeFrames_;
}
void GpuRenderer::capturePicture(unsigned picture) {
    if(!picture)return;
    ProfileScope timing("capture-gpu");
    resize(capture_,256,192,false,false);
    gl::BindFramebuffer(GL_FRAMEBUFFER,capture_.framebuffer);gl::Viewport(0,0,256,192);
    gl::Disable(GL_BLEND);gl::Disable(GL_DEPTH_TEST);gl::Disable(GL_SCISSOR_TEST);
    gl::ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    gl::UseProgram(captureProgram_);gl::BindVertexArray(emptyArray_);
    gl::ActiveTexture(GL_TEXTURE0);gl::BindTexture(GL_TEXTURE_2D,picture);
    gl::Uniform1i(uniform(captureProgram_,"uPicture"),0);
    gl::DrawArrays(GL_TRIANGLE_STRIP,0,4);
    gl::ActiveTexture(GL_TEXTURE0);gl::BindTexture(GL_TEXTURE_2D,0);
    gl::BindFramebuffer(GL_FRAMEBUFFER,0);
}
std::vector<std::uint16_t> GpuRenderer::readCapture() {
    std::vector<std::uint16_t> words(std::size_t(256)*192);
    if(!capture_.framebuffer)return words;
    std::vector<std::uint8_t> raw(std::size_t(256)*192*4);
    gl::BindFramebuffer(GL_FRAMEBUFFER,capture_.framebuffer);
    gl::PixelStorei(GL_PACK_ALIGNMENT,4);
    gl::ReadPixels(0,0,256,192,GL_RGBA,GL_UNSIGNED_BYTE,raw.data());
    gl::BindFramebuffer(GL_FRAMEBUFFER,0);
    // R and G carry the word's two bytes; rows come back bottom-up.
    for(int y=0;y<192;++y)for(int x=0;x<256;++x) {
        const auto* p=raw.data()+(std::size_t(191-y)*256+x)*4;
        words[std::size_t(y)*256+x]=static_cast<std::uint16_t>(p[0]|(p[1]<<8));
    }
    return words;
}
void GpuRenderer::copyScreenSlot(int from,int to) {
    if(from<0||from>3||to<0||to>3||from==to||!screens_[from].framebuffer)return;
    resize(screens_[to],256,192,false,false);
    gl::BindFramebuffer(GL_READ_FRAMEBUFFER,screens_[from].framebuffer);
    gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER,screens_[to].framebuffer);
    gl::BlitFramebuffer(0,0,256,192,0,0,256,192,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    gl::BindFramebuffer(GL_FRAMEBUFFER,0);
}
void GpuRenderer::readCaptureInto(std::uint16_t* out,std::size_t count) {
    const auto words=readCapture();
    std::copy_n(words.begin(),std::min(count,words.size()),out);
}
void GpuRenderer::takeNative(std::vector<std::uint32_t>& pixels) {
    if(diag::skipReadback)return;
    ProfileScope timing("readNative");
    static thread_local std::vector<std::uint32_t> raw(256*192);
    if(!nativeDrawn_) {pixels.assign(256*192,0);return;}
    auto& native=native_[nativeSlot_];   // the one the previous frame reduced into
    resize(native,256,192,false,false);
    gl::BindFramebuffer(GL_FRAMEBUFFER,native.framebuffer);
    gl::PixelStorei(GL_PACK_ALIGNMENT,4);
    gl::ReadPixels(0,0,256,192,kUploadFormat,GL_UNSIGNED_BYTE,raw.data());
    DQIV_TRACE("readNative readpixels");
    nativeDrawn_=false;
    unpremultiplyNative(raw,pixels);
}
void GpuRenderer::readNative(std::vector<std::uint32_t>& pixels,bool immediate,const std::array<float,4>& source,unsigned key) {
    if(diag::skipReadback)return;
#ifdef __EMSCRIPTEN__
    // WebGL2 has no mapped-buffer read here (glGetBufferSubData is not in Emscripten's
    // GL library and MAP_READ is refused), so the desktop pipelining cannot be used as
    // written. The display path takes the picture one frame late instead - at the top
    // of the next frame, before that frame's scene is queued (takeNative) - because
    // reading it after the scene made glReadPixels wait for the scene. Measured with
    // the readback switched off: the same town ran 36 -> 47 fps.
    if(!immediate) {queueNative(source);return;}
#endif
    ProfileScope timing(immediate?"readNative-sync":"readNative");
    queueNative(source);
    static thread_local std::vector<std::uint32_t> raw(256*192);
    gl::PixelStorei(GL_PACK_ALIGNMENT,4);
    key&=1;
    if(immediate||!readPending_[key]) gl::ReadPixels(0,0,256,192,kUploadFormat,GL_UNSIGNED_BYTE,raw.data());
    DQIV_TRACE("readNative readpixels");
    if(immediate) readPending_[key]=false;
    else {
        if(!readBuffers_[0]) {
            gl::GenBuffers(2,readBuffers_);
            for(auto buffer:readBuffers_) {
                gl::BindBuffer(GL_PIXEL_PACK_BUFFER,buffer);
                gl::BufferData(GL_PIXEL_PACK_BUFFER,raw.size()*4,nullptr,GL_STREAM_READ);
            }
        }
        // Each key (screen) collects what its previous call queued, then queues this frame.
        gl::BindBuffer(GL_PIXEL_PACK_BUFFER,readBuffers_[key]);
        if(readPending_[key]) {
#ifndef __EMSCRIPTEN__
            if(const auto* mapped=static_cast<const std::uint32_t*>(gl::MapBufferRange(GL_PIXEL_PACK_BUFFER,0,raw.size()*4,GL_MAP_READ_BIT))) {
                std::copy_n(mapped,raw.size(),raw.data());gl::UnmapBuffer(GL_PIXEL_PACK_BUFFER);
            }
#endif
        }
        gl::ReadPixels(0,0,256,192,kUploadFormat,GL_UNSIGNED_BYTE,nullptr);
        gl::BindBuffer(GL_PIXEL_PACK_BUFFER,0);
        readPending_[key]=true;
    }
    unpremultiplyNative(raw,pixels);
}
void GpuRenderer::uploadDisplaySources(NitroVideo& video) {
    // 1024 texels to a row, so 4 KB of VRAM is exactly four rows and a written
    // block is a contiguous range of rows: one transfer per run of blocks.
    constexpr int vramWidth=1024,vramHeight=int(0xa4000)/vramWidth,rowsPerBlock=int(NitroVideo::vramBlockSize)/vramWidth;
    const std::span<const std::uint8_t> vram=video.physicalVram();
    const std::span<const std::uint32_t> stamps=video.vramStamps();
    if(vramWidth*vramHeight!=int(vram.size()))throw std::runtime_error("Display sources expect 656 KB of VRAM");
    if(!sourceVram_) {
        auto create=[&](unsigned& texture,int width,int height,unsigned internal,unsigned format,unsigned type) {
            gl::GenTextures(1,&texture);
            gl::BindTexture(GL_TEXTURE_2D,texture);
            gl::TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
            gl::TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
            gl::TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
            gl::TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
            gl::TexImage2D(GL_TEXTURE_2D,0,GLint(internal),width,height,0,format,type,nullptr);
        };
        create(sourceVram_,vramWidth,vramHeight,GL_R8UI,GL_RED_INTEGER,GL_UNSIGNED_BYTE);
        create(sourcePalette_,1024,1,GL_R16UI,GL_RED_INTEGER,GL_UNSIGNED_SHORT);
        create(sourceOam_,1024,1,GL_R16UI,GL_RED_INTEGER,GL_UNSIGNED_SHORT);
        // A texture allocated without data holds anything, and an unwritten block
        // would never be sent because its stamp already matches: the first call
        // sends everything.
        gl::PixelStorei(GL_UNPACK_ALIGNMENT,1);
        gl::BindTexture(GL_TEXTURE_2D,sourceVram_);
        gl::TexSubImage2D(GL_TEXTURE_2D,0,0,0,vramWidth,vramHeight,GL_RED_INTEGER,GL_UNSIGNED_BYTE,vram.data());
        gl::BindTexture(GL_TEXTURE_2D,sourcePalette_);
        gl::TexSubImage2D(GL_TEXTURE_2D,0,0,0,1024,1,GL_RED_INTEGER,GL_UNSIGNED_SHORT,video.palette().data());
        gl::BindTexture(GL_TEXTURE_2D,sourceOam_);
        gl::TexSubImage2D(GL_TEXTURE_2D,0,0,0,1024,1,GL_RED_INTEGER,GL_UNSIGNED_SHORT,video.oam().data());
        std::copy(stamps.begin(),stamps.end(),vramSeen_.begin());
        paletteSeen_=video.paletteStamp();oamSeen_=video.oamStamp();
        gl::BindTexture(GL_TEXTURE_2D,0);
        return;
    }
    gl::PixelStorei(GL_UNPACK_ALIGNMENT,1);
    for(std::size_t block=0;block<stamps.size();) {
        if(stamps[block]==vramSeen_[block]) {++block;continue;}
        std::size_t end=block;
        while(end+1<stamps.size()&&stamps[end+1]!=vramSeen_[end+1])++end;
        gl::BindTexture(GL_TEXTURE_2D,sourceVram_);
        gl::TexSubImage2D(GL_TEXTURE_2D,0,0,int(block)*rowsPerBlock,vramWidth,int(end-block+1)*rowsPerBlock,
            GL_RED_INTEGER,GL_UNSIGNED_BYTE,vram.data()+block*NitroVideo::vramBlockSize);
        for(auto b=block;b<=end;++b)vramSeen_[b]=stamps[b];
        block=end+1;
    }
    if(video.paletteStamp()!=paletteSeen_) {
        gl::BindTexture(GL_TEXTURE_2D,sourcePalette_);
        gl::TexSubImage2D(GL_TEXTURE_2D,0,0,0,1024,1,GL_RED_INTEGER,GL_UNSIGNED_SHORT,video.palette().data());
        paletteSeen_=video.paletteStamp();
    }
    if(video.oamStamp()!=oamSeen_) {
        gl::BindTexture(GL_TEXTURE_2D,sourceOam_);
        gl::TexSubImage2D(GL_TEXTURE_2D,0,0,0,1024,1,GL_RED_INTEGER,GL_UNSIGNED_SHORT,video.oam().data());
        oamSeen_=video.oamStamp();
    }
    gl::BindTexture(GL_TEXTURE_2D,0);
}
bool GpuRenderer::composeSupported(NitroVideo& video,bool sub,const Display& display,bool scene3d,std::string& why,bool backdrop3D) const {
    const unsigned base=0x04000000+(sub?0x1000:0);
    const auto reg16=[&](unsigned address) {return unsigned(*static_cast<const std::uint16_t*>(video.reg(address,2)));};
    const unsigned control=*static_cast<const std::uint32_t*>(video.reg(base,4));
    why.clear();
    // What this stage handles. Everything else stays with Display, which is the
    // reference the GPU path is checked against; a scene that falls back keeps
    // working exactly as before.
    // Two of Display's states are not in the registers at all - the battle band
    // (sprites outside the bottom screen) and the menu's world background - so
    // they are asked directly.
    if(display.bandOn()) {why="battle band";return false;}
    if(display.worldBackground()) {why="world background";return false;}
    // HD pictures replace tiles on the canvas, not in VRAM, so a screen composed
    // from VRAM would show the tiles underneath instead.
    if(display.showsHdPictures()) {why="hd pictures";return false;}
    // A battle's bare backdrop shows the wide 3D scene on the canvas. It can only
    // be baked into a screen that has the picture, and the sub screen's slice of
    // the scene is not the picture this compositor holds.
    if(backdrop3D&&!scene3d) {why="backdrop 3D";return false;}
    if(control&128) {why="forced blank";return false;}
    // Display::engine masks the sub engine's display mode with 1, not 3.
    const unsigned displayMode=(control>>16)&(sub?1:3);
    if(displayMode!=1&&displayMode!=2) {why="display mode "+std::to_string(displayMode);return false;}
    // Display mode 2 shows a VRAM bank outright: no layers, so nothing else to check.
    if(displayMode==2)return true;
    const unsigned mode=control&7;
    if(mode>5) {why="background mode "+std::to_string(mode);return false;}
    if(control&(1u<<30)) {why="extended palettes";return false;}
    // BG0 drawing the 3D screen needs the picture itself; without it Display
    // draws nothing at all there, so there is nothing to compose either.
    if(!sub&&(control&0x108)==0x108&&!scene3d) {why="BG0 draws 3D";return false;}
    for(unsigned bg=0;bg<4;++bg)if(control&(0x100<<bg)) {
        const unsigned cnt=reg16(base+8+bg*2);
        // Affine backgrounds are tile maps here: the 8-bit map of modes 1/2/4
        // and the 16-bit one of modes 3/4/5. The two bitmap kinds - the large
        // bitmap of mode 6 and BGxCNT bit 7 on an extended affine layer - are
        // not (and DQIV's own frames do not set them).
        const unsigned type=bg==3?(mode>=3?2:(mode?1:0)):bg==2?(mode==5?2:(mode==2||mode==4?1:0)):0;
        if(type==2&&(cnt&0x80)) {why="affine bitmap background";return false;}
        if(cnt&0x40) {why="mosaic";return false;}
    }
    // Objects are drawn as text objects only at this stage: affine
    // (rotation/scaling) ones need the OAM parameter block, bitmap ones the
    // object character mapping, and mosaicked ones the mosaic register. A scene
    // with any of those stays with Display.
    if(control&0x1000) {
        const auto oam=video.oam();
        const unsigned obase=sub?0x400:0;
        const auto half=[&](unsigned offset) {
            return unsigned(oam[offset])|(unsigned(oam[offset+1])<<8);
        };
        for(unsigned id=0;id<128;++id) {
            const unsigned attributes=half(obase+id*8);
            const bool affine=(attributes&0x100)!=0;
            if((!affine&&(attributes&0x200))||(attributes>>14)==3)continue;
            if(affine) {why="affine sprites";return false;}
            const unsigned objectMode=(attributes>>10)&3;
            if(objectMode==2)continue;   // OBJ window: the sprite only shapes the mask
            if(objectMode==3) {why="bitmap sprites";return false;}
            if(attributes&0x1000) {why="sprite mosaic";return false;}
        }
    }
    return true;
}
bool GpuRenderer::composeScreen(NitroVideo& video,bool sub,const Display& display,std::string& why,unsigned scene3d,bool capture,bool backdrop3D,bool graphics,int target) {
    if(!composeSupported(video,sub,display,scene3d!=0,why,backdrop3D))return false;
    const unsigned base=0x04000000+(sub?0x1000:0);
    const auto reg16=[&](unsigned address) {return unsigned(*static_cast<const std::uint16_t*>(video.reg(address,2)));};
    const unsigned control=*static_cast<const std::uint32_t*>(video.reg(base,4));
    if(!sourceVram_)uploadDisplaySources(video);
    std::array<std::uint32_t,4> windowSizes{};
    std::array<std::uint32_t,4> objectSizes{};
    const auto windowBases=video.vramWindowBases(sub,false,windowSizes);
    const auto objectBases=video.vramWindowBases(sub,true,objectSizes);
    float bgCnt[16]={0},bgOff[16]={0},bgParam[8]={0},bgRef[8]={0};
    for(unsigned bg=0;bg<4;++bg) {
        bgCnt[bg*4]=float(reg16(base+8+bg*2));
        bgOff[bg*4]=float(reg16(base+0x10+bg*4)&511);
        bgOff[bg*4+1]=float(reg16(base+0x12+bg*4)&511);
    }
    // BG2/BG3 affine matrices and reference points. The reference is exact
    // only as two floats (it is 28-bit), so it goes up as low half + sign-
    // extended high half and is reassembled in the shader.
    for(unsigned layer=2;layer<4;++layer) {
        const unsigned affine=base+0x20+(layer==3?0x10:0);
        const int slot=int(layer-2);
        const auto s16=[&](unsigned address) {return int(static_cast<std::int16_t>(reg16(address)));};
        const auto s28=[&](unsigned address) {
            return int(*static_cast<const std::uint32_t*>(video.reg(address,4))<<4)>>4;
        };
        bgParam[slot*4]=float(s16(affine));bgParam[slot*4+1]=float(s16(affine+2));
        bgParam[slot*4+2]=float(s16(affine+4));bgParam[slot*4+3]=float(s16(affine+6));
        const int refx=s28(affine+8),refy=s28(affine+12);
        bgRef[slot*4]=float(refx&0xffff);bgRef[slot*4+1]=float(refx>>16);
        bgRef[slot*4+2]=float(refy&0xffff);bgRef[slot*4+3]=float(refy>>16);
    }
    const float windowBase[4]={float(windowBases[0]),float(windowBases[1]),float(windowBases[2]),float(windowBases[3])};
    const float windowLength[4]={float(windowSizes[0]),float(windowSizes[1]),float(windowSizes[2]),float(windowSizes[3])};
    const float objectBase[4]={float(objectBases[0]),float(objectBases[1]),float(objectBases[2]),float(objectBases[3])};
    const float objectLength[4]={float(objectSizes[0]),float(objectSizes[1]),float(objectSizes[2]),float(objectSizes[3])};
    const auto palette=video.palette();
    const auto paletteWord=[&](std::size_t offset) {
        return unsigned(palette[offset])|(unsigned(palette[offset+1])<<8);
    };
    // The backdrop is palette entry 0 of the engine's own palette block.
    const float screen[4]={float(paletteWord(sub?0x400:0)),float(control&0xffffffu),float(reg16(base+0x50)),float(reg16(base+0x52))};
    // Background palettes: engine A's start at 0x05000000 (entry 0), engine B's
    // at 0x05000400 (entry 0x200). Entry 0 doubles as the backdrop of each.
    // The last component says the screen is one picture the compositor has as a
    // texture: display mode 2 then samples that instead of the VRAM bank.
    const bool captureShown=capture&&scene3d&&((control>>16)&(sub?1:3))==2;
    const float effects[4]={float(std::min(16u,reg16(base+0x54)&31)),float(reg16(base+0x6c)),float(sub?0x200:0),
        captureShown?1.f:0.f};
    int bankMask=0;
    for(unsigned bank=0;bank<4;++bank)if(video.lcdcBank(bank))bankMask|=1<<bank;
    const float displayMode[4]={float((control>>16)&(sub?1:3)),float(bankMask),float(control>>24),float(sub?1:0)};
    // The four targets: 0 and 1 are the engines' own pictures, which is what a
    // verification reads back; 2 and 3 are the DS top and bottom screens as the
    // canvas shows them (the engine that drives a screen can change).
    Target& sink=screens_[target<0?(sub?1:0):target];
    resize(sink,256,192,false,false);
    gl::BindFramebuffer(GL_FRAMEBUFFER,sink.framebuffer);
    gl::Viewport(0,0,256,192);
    gl::Disable(GL_BLEND);gl::Disable(GL_DEPTH_TEST);gl::Disable(GL_SCISSOR_TEST);
    gl::ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    gl::UseProgram(compose2dProgram_);gl::BindVertexArray(emptyArray_);
    gl::ActiveTexture(GL_TEXTURE0);gl::BindTexture(GL_TEXTURE_2D,sourceVram_);
    gl::ActiveTexture(GL_TEXTURE1);gl::BindTexture(GL_TEXTURE_2D,sourcePalette_);
    gl::ActiveTexture(GL_TEXTURE2);gl::BindTexture(GL_TEXTURE_2D,sourceOam_);
    const bool threeShown=captureShown||(!sub&&(control&0x108)==0x108)||(backdrop3D&&scene3d!=0);
    gl::ActiveTexture(GL_TEXTURE3);gl::BindTexture(GL_TEXTURE_2D,threeShown?scene3d:0);
    gl::Uniform1i(uniform(compose2dProgram_,"uVram"),0);
    gl::Uniform1i(uniform(compose2dProgram_,"uPalette"),1);
    gl::Uniform1i(uniform(compose2dProgram_,"uOam"),2);
    gl::Uniform1i(uniform(compose2dProgram_,"uScreen3d"),3);
    gl::Uniform1f(uniform(compose2dProgram_,"uThreeOn"),!sub&&(control&0x108)==0x108&&scene3d?1.f:0.f);
    gl::Uniform1i(uniform(compose2dProgram_,"uBackdrop3D"),backdrop3D&&scene3d?1:0);
    gl::Uniform1i(uniform(compose2dProgram_,"uGraphics"),graphics?1:0);
    gl::Uniform4fv(uniform(compose2dProgram_,"uBgCnt"),4,bgCnt);
    gl::Uniform4fv(uniform(compose2dProgram_,"uBgOff"),4,bgOff);
    gl::Uniform4fv(uniform(compose2dProgram_,"uWinBase"),1,windowBase);
    gl::Uniform4fv(uniform(compose2dProgram_,"uWinSize"),1,windowLength);
    gl::Uniform4fv(uniform(compose2dProgram_,"uObjWinBase"),1,objectBase);
    gl::Uniform4fv(uniform(compose2dProgram_,"uObjWinSize"),1,objectLength);
    gl::Uniform4fv(uniform(compose2dProgram_,"uScreen"),1,screen);
    gl::Uniform4fv(uniform(compose2dProgram_,"uEffects"),1,effects);
    gl::Uniform4fv(uniform(compose2dProgram_,"uMode"),1,displayMode);
    gl::Uniform4fv(uniform(compose2dProgram_,"uBgParam"),2,bgParam);
    gl::Uniform4fv(uniform(compose2dProgram_,"uBgRef"),2,bgRef);
    const float windowRegs[8]={float(reg16(base+0x48)),float(reg16(base+0x4a)),float(reg16(base+0x40)),
        float(reg16(base+0x44)),float(reg16(base+0x42)),float(reg16(base+0x46)),0.f,0.f};
    gl::Uniform4fv(uniform(compose2dProgram_,"uWin"),2,windowRegs);
    gl::DrawArrays(GL_TRIANGLE_STRIP,0,4);
    gl::ActiveTexture(GL_TEXTURE0);
    gl::BindFramebuffer(GL_FRAMEBUFFER,0);
    return true;
}
std::vector<std::uint32_t> GpuRenderer::readScreen(bool sub) {
    const auto& target=screens_[sub?1:0];
    std::vector<std::uint32_t> raw(std::size_t(256)*192),pixels(raw.size());
    if(!target.framebuffer)return pixels;
    gl::BindFramebuffer(GL_FRAMEBUFFER,target.framebuffer);gl::PixelStorei(GL_PACK_ALIGNMENT,4);
    gl::ReadPixels(0,0,256,192,GL_RGBA,GL_UNSIGNED_BYTE,raw.data());
    gl::BindFramebuffer(GL_FRAMEBUFFER,0);
    for(int row=0;row<192;++row)std::copy_n(raw.data()+std::size_t(191-row)*256,256,pixels.data()+std::size_t(row)*256);
    for(auto& p:pixels) {
        const unsigned a=p>>24,r=p&255,g=(p>>8)&255,b=(p>>16)&255;
        p=(a<<24)|(r<<16)|(g<<8)|b;
    }
    return pixels;
}
std::vector<std::uint32_t> GpuRenderer::readScreenSlot(int slot) {
    if(slot<0||slot>6)return std::vector<std::uint32_t>(std::size_t(256)*192);
    const auto& target=screens_[slot];
    std::vector<std::uint32_t> raw(std::size_t(256)*192),pixels(raw.size());
    if(!target.framebuffer)return pixels;
    gl::BindFramebuffer(GL_FRAMEBUFFER,target.framebuffer);gl::PixelStorei(GL_PACK_ALIGNMENT,4);
    gl::ReadPixels(0,0,256,192,GL_RGBA,GL_UNSIGNED_BYTE,raw.data());
    gl::BindFramebuffer(GL_FRAMEBUFFER,0);
    for(int row=0;row<192;++row)std::copy_n(raw.data()+std::size_t(191-row)*256,256,pixels.data()+std::size_t(row)*256);
    for(auto& p:pixels) {const unsigned a=p>>24,r=p&255,g=(p>>8)&255,b=(p>>16)&255;p=(a<<24)|(r<<16)|(g<<8)|b;}
    return pixels;
}
void GpuRenderer::present(const GpuLayout& layout,std::span<const std::uint32_t> recipes,int windowWidth,int windowHeight,bool stretch) {
    constexpr std::size_t plane=std::size_t(256)*GpuLayout::recipeRows;
    if(recipes.size()!=plane*4)throw std::runtime_error("Composition recipes have an unexpected size");
    // Stage B3: both screens were composed on the GPU and the canvas has nothing
    // but them to draw, so the canvas samples those two pictures and the recipes -
    // four planes of 256 x 704, 2.9 MB a frame - are not uploaded at all.
    const bool composed=layout.composed[0]>=0&&layout.composed[0]<7&&
        layout.composed[1]>=0&&layout.composed[1]<7;
    if(!composed&&!diag::skipUpload)
    for(unsigned plane=0;plane<4;++plane) {
        gl::BindTexture(GL_TEXTURE_2D,recipeTextures_[plane]);
        gl::PixelStorei(GL_UNPACK_ALIGNMENT,4);
        std::vector<std::uint32_t> scratch;
        gl::TexSubImage2D(GL_TEXTURE_2D,0,0,0,256,GpuLayout::recipeRows,kUploadFormat,GL_UNSIGNED_BYTE,
            uploadOrder(std::span<const std::uint32_t>(recipes.data()+plane*std::size_t(256)*GpuLayout::recipeRows,std::size_t(256)*GpuLayout::recipeRows),scratch));
    }
    std::optional<ProfileScope> stage(std::in_place,"present-upload");
    for(auto& upload:takeHdUploads()) {
        // Scale2x twice, then premultiplied for filtering.
        auto pixels=scale2x(scale2x(upload.pixels,hdSourceSize,hdSourceSize),hdSourceSize*2,hdSourceSize*2);
        for(auto& p:pixels) {
            const unsigned a=p>>24;
            auto channel=[&](unsigned shift){return (((p>>shift)&255)*a+127)/255;};
            p=(a<<24)|(channel(16)<<16)|(channel(8)<<8)|channel(0);
        }
        gl::BindTexture(GL_TEXTURE_2D,hdAtlas_);gl::PixelStorei(GL_UNPACK_ALIGNMENT,4);
        std::vector<std::uint32_t> scratch;
        gl::TexSubImage2D(GL_TEXTURE_2D,0,(upload.cell%hdAtlasCells)*hdCellSize,(upload.cell/hdAtlasCells)*hdCellSize,
            hdCellSize,hdCellSize,kUploadFormat,GL_UNSIGNED_BYTE,uploadOrder(pixels,scratch));
    }
    if(layout.sides) {
        // Per row, the column whose 3D effects continue beside the screens: the
        // nearest one showing the 3D layer directly (recipe kind 1), searched
        // from that side for 33 columns, then from the other edge for 32.
        std::array<std::int32_t,2*384> columns;
        auto direct=[&](unsigned y,int x){return (recipes[y*256+x]>>24)==1;};
        for(unsigned y=0;y<384;++y)for(unsigned side=0;side<2;++side) {
            int found=-1;
            for(int i=0;i<65&&found<0;++i) {
                const int along=i<33?i:255-(i-33),x=side?255-along:along;
                if(direct(y,x))found=x;
            }
            columns[y*2+side]=found;
        }
        gl::BindTexture(GL_TEXTURE_2D,sideColumns_);
        gl::TexSubImage2D(GL_TEXTURE_2D,0,0,0,2,384,GL_RED_INTEGER,GL_INT,columns.data());
    }
    stage.emplace("present-draw");
    beginFrameTiming();
    // A new window size starts again from the highest quality.
    if(output_.width!=windowWidth||output_.height!=windowHeight) {autoSupersample_=2;gpuTime_=0;gpuSamples_=0;}
    // Composed off screen, so a hidden window can still be read back.
    resize(output_,windowWidth,windowHeight,false,false);
    gl::BindFramebuffer(GL_FRAMEBUFFER,output_.framebuffer);
    gl::Viewport(0,0,windowWidth,windowHeight);
    gl::ClearColor(0,0,0,1);gl::Clear(GL_COLOR_BUFFER_BIT);
    int x=0,y=0,w=windowWidth,h=windowHeight;
    if(!stretch) {
        const float scale=std::min(windowWidth/layout.canvasWidth,windowHeight/layout.canvasHeight);
        w=int(std::lround(layout.canvasWidth*scale));h=int(std::lround(layout.canvasHeight*scale));
        x=(windowWidth-w)/2;y=(windowHeight-h)/2;
    }
    gl::Viewport(x,y,w,h);
    gl::UseProgram(composeProgram_);gl::BindVertexArray(emptyArray_);
    for(unsigned plane=0;plane<3;++plane) {
        gl::ActiveTexture(GL_TEXTURE0+plane);gl::BindTexture(GL_TEXTURE_2D,recipeTextures_[plane]);
        gl::Uniform1i(uniform(composeProgram_,("uRecipe"+std::to_string(plane)).c_str()),int(plane));
    }
    for(unsigned slot=0;slot<2;++slot) {
        const auto& rect=layout.threeRects[slot];
        const bool used=fogged_[slot].framebuffer&&rect[2]>0&&rect[3]>0;
        gl::ActiveTexture(GL_TEXTURE3+slot);gl::BindTexture(GL_TEXTURE_2D,fogged_[slot].framebuffer?threeColor(slot):0);
        const auto suffix=std::to_string(slot);
        gl::Uniform1i(uniform(composeProgram_,("uThree"+suffix).c_str()),int(3+slot));
        gl::Uniform4f(uniform(composeProgram_,("uThreeRect"+suffix).c_str()),rect[0],rect[1],used?rect[2]:0.f,used?rect[3]:0.f);
    }
    gl::ActiveTexture(GL_TEXTURE5);gl::BindTexture(GL_TEXTURE_2D,sideColumns_);
    gl::Uniform1i(uniform(composeProgram_,"uSideColumns"),5);
    gl::ActiveTexture(GL_TEXTURE6);gl::BindTexture(GL_TEXTURE_2D,recipeTextures_[3]);
    gl::Uniform1i(uniform(composeProgram_,"uRecipe3"),6);
    gl::ActiveTexture(GL_TEXTURE7);gl::BindTexture(GL_TEXTURE_2D,hdAtlas_);
    gl::Uniform1i(uniform(composeProgram_,"uHdAtlas"),7);
    gl::ActiveTexture(GL_TEXTURE8);gl::BindTexture(GL_TEXTURE_2D,composed?screens_[layout.composed[0]].color:0);
    gl::Uniform1i(uniform(composeProgram_,"uScreenA"),8);
    gl::ActiveTexture(GL_TEXTURE9);gl::BindTexture(GL_TEXTURE_2D,composed?screens_[layout.composed[1]].color:0);
    gl::Uniform1i(uniform(composeProgram_,"uScreenB"),9);
    gl::Uniform1i(uniform(composeProgram_,"uComposed"),composed?1:0);
    gl::Uniform2f(uniform(composeProgram_,"uCanvas"),layout.canvasWidth,layout.canvasHeight);
    gl::Uniform1i(uniform(composeProgram_,"uSides"),layout.sides&&fogged_[0].framebuffer?1:0);
    gl::Uniform1i(uniform(composeProgram_,"uPlaced"),layout.placed?1:0);
    gl::Uniform1f(uniform(composeProgram_,"uTopScale"),layout.topScale);
    gl::Uniform1i(uniform(composeProgram_,"uBand"),layout.band?1:0);
    gl::Uniform1f(uniform(composeProgram_,"uInsetAlpha"),layout.insetAlpha);
    gl::Uniform4f(uniform(composeProgram_,"uScreen0"),layout.screens[0][0],layout.screens[0][1],layout.screens[0][2],layout.screens[0][3]);
    gl::Uniform4f(uniform(composeProgram_,"uScreen1"),layout.screens[1][0],layout.screens[1][1],layout.screens[1][2],layout.screens[1][3]);
    gl::Uniform4f(uniform(composeProgram_,"uInset"),layout.inset[0],layout.inset[1],fogged_[1].framebuffer?layout.inset[2]:0.f,layout.inset[3]);
    gl::Uniform1i(uniform(composeProgram_,"uBackdrop3D"),layout.backdrop3D&&fogged_[0].framebuffer?1:0);
    gl::Uniform1i(uniform(composeProgram_,"uThreeScreenRow"),layout.threeScreenBottom?288:96);
    gl::Uniform1f(uniform(composeProgram_,"uScale"),float(w)/layout.canvasWidth);
    gpuCheckpoint("gpu-upload");
    gl::DrawArrays(GL_TRIANGLE_STRIP,0,4);
    gl::ActiveTexture(GL_TEXTURE0);
    gpuCheckpoint("gpu-compose");
#ifdef __EMSCRIPTEN__
    // Measured: the compose fills the offscreen surface with the picture (every pixel,
    // changing frame to frame) but the blit to the default framebuffer puts nothing on the
    // canvas. So on wasm the picture is drawn to the canvas with the overlay program - the
    // path that already works for the settings UI - instead of blitted.
    gl::BindFramebuffer(GL_FRAMEBUFFER,0);gl::Viewport(0,0,windowWidth,windowHeight);
    gl::UseProgram(overlayProgram_);gl::BindVertexArray(emptyArray_);
    gl::ActiveTexture(GL_TEXTURE0);gl::BindTexture(GL_TEXTURE_2D,output_.color);
    gl::Uniform1i(uniform(overlayProgram_,"uPicture"),0);
    // The overlay program's rect is (left, top, right, bottom) in NDC, so a full-screen
    // blit is (-1, 1, 1, -1); the other order flips the picture upside down.
    gl::Uniform4f(uniform(overlayProgram_,"uRect"),-1.f,1.f,1.f,-1.f);
    gl::DrawArrays(GL_TRIANGLE_STRIP,0,4);
#else
    gl::BindFramebuffer(GL_READ_FRAMEBUFFER,output_.framebuffer);gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER,0);
    gl::BlitFramebuffer(0,0,windowWidth,windowHeight,0,0,windowWidth,windowHeight,GL_COLOR_BUFFER_BIT,GL_NEAREST);
#endif
    gpuCheckpoint("gpu-blit");
#ifdef __EMSCRIPTEN__
    // Measured from inside the game's own GL context, right after the blit, because a
    // readPixels from the debugger can land on a different buffer. This says whether the
    // canvas has the composed picture or the compose itself is drawing black.
    static unsigned canvasProbe=0;
    if(++canvasProbe%60==0) {
        std::vector<std::uint32_t> px(std::size_t(windowWidth)*windowHeight);
        gl::BindFramebuffer(GL_FRAMEBUFFER,0);gl::PixelStorei(GL_PACK_ALIGNMENT,4);
        gl::ReadPixels(0,0,windowWidth,windowHeight,GL_RGBA,GL_UNSIGNED_BYTE,px.data());
        std::uint64_t n=0,s=0;for(auto p:px)if(p){++n;s+=p;}
        const auto at=[&](int x,int y){return px[std::size_t(y)*windowWidth+x];};
        std::printf("  canvas probe %u: %dx%d nonblack %llu sum %llu top=%08x mid=%08x bot=%08x\n",canvasProbe,windowWidth,windowHeight,(unsigned long long)n,(unsigned long long)s,at(windowWidth/2,windowHeight/8),at(windowWidth/2,windowHeight/2),at(windowWidth/2,windowHeight*7/8));
        fflush(stdout);   // the journal is a freopen'd file; C stdio buffers it whole
    }
#endif
    endFrameTiming();
    gl::BindFramebuffer(GL_FRAMEBUFFER,0);
}
std::vector<std::uint32_t> GpuRenderer::readPresented(int& width,int& height) {
    width=output_.width;height=output_.height;
    std::vector<std::uint32_t> raw(std::size_t(width)*height),pixels(raw.size());
    gl::BindFramebuffer(GL_FRAMEBUFFER,output_.framebuffer);gl::PixelStorei(GL_PACK_ALIGNMENT,4);
    gl::ReadPixels(0,0,width,height,kUploadFormat,GL_UNSIGNED_BYTE,raw.data());
    gl::BindFramebuffer(GL_FRAMEBUFFER,0);
    for(int row=0;row<height;++row)std::copy_n(raw.data()+std::size_t(height-1-row)*width,width,pixels.data()+std::size_t(row)*width);
#ifdef __EMSCRIPTEN__
    // The readback comes back in RGBA byte order; the caller wants the game's
    // ARGB convention (SDL_PIXELFORMAT_ARGB8888).
    for(auto& p:pixels) {const unsigned a=p>>24,r=p&255,g=(p>>8)&255,b=(p>>16)&255;p=(a<<24)|(r<<16)|(g<<8)|b;}
#endif
    return pixels;
}
}
