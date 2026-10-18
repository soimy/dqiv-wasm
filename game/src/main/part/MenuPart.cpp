#pragma ipa file
#include "main/part/MenuPart.hpp"
#ifdef __EMSCRIPTEN__
#include <iostream>
#endif
#include "main/dss/RenderObject.hpp"
#include "main/dss/UnkDisplay.hpp"
#include "main/global/Global.hpp"
#include "main/dss/DssUtils.hpp"
#include "main/dss/Render.hpp"
#include "main/dss/UnkLanguage.hpp"
#include "main/dss/UnkSprite2D.hpp"
#include "main/dss/UnkVramTransfer.hpp"
#include "main/menu/MenuManager.hpp"
#include "main/sound/SoundManager.hpp"
#include "nitro/gx.h"
#include "nitro/gx.hpp"

static int s_frame;
MenuPart g_MenuPart;
static int s_isEnd;
static Render s_render;
static UnkMenuSprite s_logo[2];

ARM void MenuPart::initialize()
{
#ifdef __EMSCRIPTEN__
    // Measured, because the game stops inside this function on the browser and I would
    // rather not guess which line: the boot-logo stage is the first thing the game's own
    // scene code does, and it needs 240 frames before it starts the title. So the stages
    // are named, and the asset loads are the ones I would expect to matter.
    auto stage=[&](const char* what){std::cout<<"MenuPart initialize: "<<what<<"\n";};
    stage("start");
#endif
    func_020648cc();
    func_020648b8();
    GX_ResetBankForTex();
    GX_ResetBankForSubObj();
    GX_ResetBankForSubBg();
    GX_SetBankForTex(GX_VRAM_AB);
    data_0211e450.unkfunc_020861c4(0x40000, 0x4000);
#ifdef __EMSCRIPTEN__
    stage("display plugin");
#endif
    unkfunc_02080e90(&dss::g_DISPLAYPLUGIN_DOUBLE3D);
#ifdef __EMSCRIPTEN__
    stage("sound stop");
#endif
    SoundManager::stop(0);
#ifdef __EMSCRIPTEN__
    stage("logo 0");
#endif
    if (unkfunc_0208a104() == 2) {
        s_logo[0].unkfunc_02057d60("data/2d/sqex_logo_na.tex", 0);
    } else {
        s_logo[0].unkfunc_02057d60("data/2d/sqex_logo_eu.tex", 0);
    }
#ifdef __EMSCRIPTEN__
    stage("logo 0 built");
#endif
    s_logo[0].unkfunc_02057ee8();
    s_logo[0].unkfunc_02057f18(1);
    s_logo[0].unkfunc_02057f00(0x1f);
#ifdef __EMSCRIPTEN__
    stage("logo 1");
#endif
    if (unkfunc_0208a104() == 2) {
        s_logo[1].unkfunc_02057d60("data/2d/arte_logo_na.tex", 0);
    } else {
        s_logo[1].unkfunc_02057d60("data/2d/arte_logo_eu.tex", 0);
    }
#ifdef __EMSCRIPTEN__
    stage("logo 1 built");
#endif
    s_logo[1].unkfunc_02057edc();
    s_logo[1].unkfunc_02057f18(1);
    s_logo[1].unkfunc_02057f00(0x1f);
    s_isEnd = 0;
    s_frame = 0;
#ifdef __EMSCRIPTEN__
    stage("fade in black");
#endif
    g_Global.fadeInBlack(60);
#ifdef __EMSCRIPTEN__
    stage("done");
#endif
}

ARM void MenuPart::terminate()
{
    s_logo[0].unkfunc_02057e34();
    s_logo[1].unkfunc_02057e34();
    data_0211e450.unkfunc_02086278();
}

ARM void MenuPart::onExecutePart()
{
    s_frame++;
    if (s_isEnd == 0 && s_frame > 240) {
        g_Global.startTitle();
        s_isEnd = 1;
    }
}

ARM void MenuPart::onDrawPart()
{
    unkfunc_02084964();
    s_logo[0].unkfunc_02057ec0();
    s_logo[1].unkfunc_02057ec0();
}

ARM void MenuPart::onWindowPart()
{
}

ARM void MenuPart::onDebugPart()
{
}
