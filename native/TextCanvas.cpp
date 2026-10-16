#include "TextCanvas.hpp"
#ifndef __EMSCRIPTEN__
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <algorithm>
#include <stdexcept>
#include <string>

namespace pc {
namespace {
#ifndef __EMSCRIPTEN__
std::wstring wide(std::string_view utf8) {
    if(utf8.empty())return {};
    const int count=MultiByteToWideChar(CP_UTF8,0,utf8.data(),int(utf8.size()),nullptr,0);
    std::wstring result(std::size_t(count),L'\0');
    MultiByteToWideChar(CP_UTF8,0,utf8.data(),int(utf8.size()),result.data(),count);
    return result;
}
#endif
std::uint32_t blend(std::uint32_t below,std::uint32_t argb,unsigned coverage) {
    // Straight colour over a premultiplied pixel, with coverage 0..255.
    const unsigned a=((argb>>24)*coverage+127)/255;
    std::uint32_t result=0;
    for(unsigned shift:{0u,8u,16u}) {
        const unsigned c=((argb>>shift)&255)*a/255,b=(below>>shift)&255;
        result|=std::min(255u,c+b*(255-a)/255)<<shift;
    }
    const unsigned alpha=std::min(255u,a+((below>>24)&255)*(255-a)/255);
    return result|(alpha<<24);
}
}
TextCanvas::TextCanvas(int width,int height):width_(std::max(1,width)),height_(std::max(1,height)),
    pixels_(std::size_t(width_)*height_) {
#ifndef __EMSCRIPTEN__
    // No GDI here: the mask stays null and text() has nothing to draw with.
    // clear() and fill() are pure CPU and work on both backends.
    BITMAPINFO info{};
    info.bmiHeader.biSize=sizeof(info.bmiHeader);info.bmiHeader.biWidth=width_;info.bmiHeader.biHeight=-height_;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    dc_=CreateCompatibleDC(nullptr);
    void* bits=nullptr;
    bitmap_=CreateDIBSection(static_cast<HDC>(dc_),&info,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!dc_||!bitmap_)throw std::runtime_error("Interface text surface unavailable");
    mask_=static_cast<std::uint32_t*>(bits);
    previous_=SelectObject(static_cast<HDC>(dc_),static_cast<HBITMAP>(bitmap_));
    SetBkMode(static_cast<HDC>(dc_),TRANSPARENT);SetTextColor(static_cast<HDC>(dc_),RGB(255,255,255));
#endif
}
TextCanvas::~TextCanvas() {
#ifndef __EMSCRIPTEN__
    SelectObject(static_cast<HDC>(dc_),static_cast<HGDIOBJ>(previous_));
    DeleteObject(static_cast<HBITMAP>(bitmap_));DeleteDC(static_cast<HDC>(dc_));
#endif
}
void TextCanvas::clear(std::uint32_t argb) {
    const unsigned a=argb>>24;
    std::uint32_t premultiplied=a<<24;
    for(unsigned shift:{0u,8u,16u})premultiplied|=(((argb>>shift)&255)*a/255)<<shift;
    std::fill(pixels_.begin(),pixels_.end(),premultiplied);
}
void TextCanvas::fill(int x,int y,int width,int height,std::uint32_t argb) {
    const int x0=std::max(0,x),y0=std::max(0,y),x1=std::min(width_,x+width),y1=std::min(height_,y+height);
    for(int row=y0;row<y1;++row)for(int column=x0;column<x1;++column) {
        auto& p=pixels_[std::size_t(row)*width_+column];p=blend(p,argb,255);
    }
}
void TextCanvas::text(int x,int y,std::string_view utf8,int size,std::uint32_t argb,bool bold,int align,int boxWidth) {
#ifdef __EMSCRIPTEN__
    // No GDI, so no system font renderer. The overlay keeps its rectangle and
    // loses its text; a portable rasterizer (stb_truetype plus a shipped font)
    // is the open decision this stands on.
    (void)x;(void)y;(void)utf8;(void)size;(void)argb;(void)bold;(void)align;(void)boxWidth;
    return;
#else
    const auto string=wide(utf8);
    if(string.empty())return;
    HDC dc=static_cast<HDC>(dc_);
    HFONT font=CreateFontW(-size,0,0,0,bold?FW_BOLD:FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    HGDIOBJ previous=SelectObject(dc,font);
    RECT rect{x,y,boxWidth>0?x+boxWidth:width_,std::min(height_,y+size*2)};
    // The text pass: white on black in the mask, then the colour where it covers.
    for(int row=std::max(0,int(rect.top));row<std::min(height_,int(rect.bottom));++row)
        std::fill_n(mask_+std::size_t(row)*width_+std::max(0,int(rect.left)),std::max(0,std::min(width_,int(rect.right))-std::max(0,int(rect.left))),0u);
    GdiFlush();
    const UINT format=DT_SINGLELINE|DT_NOPREFIX|DT_TOP|(align==1?DT_CENTER:align==2?DT_RIGHT:DT_LEFT);
    DrawTextW(dc,string.c_str(),int(string.size()),&rect,format);
    GdiFlush();
    SelectObject(dc,previous);DeleteObject(font);
    for(int row=std::max(0,int(rect.top));row<std::min(height_,int(rect.bottom));++row)
        for(int column=std::max(0,int(rect.left));column<std::min(width_,int(rect.right));++column) {
            const auto m=mask_[std::size_t(row)*width_+column];
            const unsigned coverage=std::max({m&255,(m>>8)&255,(m>>16)&255});
            if(coverage){auto& p=pixels_[std::size_t(row)*width_+column];p=blend(p,argb,coverage);}
        }
#endif
}
}
