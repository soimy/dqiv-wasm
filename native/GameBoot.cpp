// Dragon Quest IV for Windows: the original game, run natively with the PC
// window, renderer and sound.
#include "Assets.hpp"
#include "AssetPack.hpp"
#include "GameServices.hpp"
#include "NitroFilesystem.hpp"
#include "NitroBackup.hpp"
#include "NitroGeometry.hpp"
#include "GxTextures.hpp"
#include "Display.hpp"
#include "GameWindow.hpp"
#include "FramePresenter.hpp"
#include "DualScreen.hpp"
#include "Profile.hpp"
#include "NitroVideo.hpp"
#include "NitroThreads.hpp"
#include "NitroSound.hpp"
#include "MobileSound.hpp"
#include "NitroPower.hpp"
#include "NitroOverlays.hpp"
#include "NitroIdentity.hpp"
#include "NitroWireless.hpp"
#include "Input.hpp"
#include "Settings.hpp"
#ifdef _MSC_VER
#include <share.h>
#endif
// Laptops with two graphics chips run the game on the discrete one.
#ifndef __EMSCRIPTEN__
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement=1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance=1;
}
#else
// On the web the browser picks the adapter; there is no equivalent switch to
// make here, so the two symbols simply do not exist on wasm. This is also where
// the main loop comes from, since main() cannot block here.
#include <emscripten/emscripten.h>
// Defined in wasm/src/gpu-selfcheck.cpp, which is not in cmake/Sources.cmake, so
// the MSVC build neither sees the call nor the definition.
namespace pc {void gpuSelfCheck(GameWindow&,GxTextures&);}
#endif
extern "C" {
#include <nitro/os/thread.h>
}
#include "main/global/UnkGameApplication.hpp"
#include "main/global/GlobalDQ4.hpp"
#include <array>
#include <cstdio>
#include <exception>
#include <iostream>
#ifndef __EMSCRIPTEN__
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

static_assert(sizeof(void*) == 4, "The original game resources contain 32-bit pointers");
static_assert(sizeof(DataObject) == 0x10);
static_assert(sizeof(UnkGameApplication) == 0xb4);
std::span<const pc::OverlayImage> gameOverlayImages();
#ifdef __EMSCRIPTEN__
// Constructed first, in wasm/src/early-contexts.cpp, which is listed first so
// clang's static-init order puts them before the game's own constructors. clang
// ignores #pragma init_seg, so on wasm those constructors run at startup and the
// contexts cannot be late the way they are on MSVC. See the note in that file.
namespace pc {
extern const std::filesystem::path earlySavePath;
extern const std::filesystem::path earlyIdentityPath;
extern NitroIdentity earlyIdentity;
extern NitroVideo earlyVideo;
extern NitroPower earlyPower;
extern NitroOverlays earlyOverlays;
}   // namespace pc
#endif

namespace {
// The game has no console: its output goes to a journal beside the save.
std::string journalPath() {return (pc::saveDirectory()/"journal.txt").string();}
// The first start of this version brings the save and the settings of an
// earlier one (kept in %LOCALAPPDATA%) into the saves folder.
void adoptPreviousSave() {
    const auto previous=pc::previousSaveDirectory();
    if(previous.empty()||std::filesystem::exists(pc::defaultSavePath()))return;
    for(const char* name:{"dqiv_eur.sav","dqiv_eur.sav.identity","settings.ini"}) {
        std::error_code ignored;
        if(std::filesystem::exists(previous/name,ignored))
            std::filesystem::copy_file(previous/name,pc::saveDirectory()/name,std::filesystem::copy_options::skip_existing,ignored);
    }
}
void showError(const std::string& text) {
    const auto message="Dragon Quest IV a rencontre une erreur et doit se fermer.\n\n"+text+"\n\nJournal : "+journalPath();
#ifdef __EMSCRIPTEN__
    // No dialog box here. The message goes to the journal, which is what this
    // sentence is pointing at anyway.
    std::cerr << message << '\n' << std::flush;
#else
    MessageBoxA(nullptr,message.c_str(),"Dragon Quest IV",MB_OK|MB_ICONERROR);
#endif
}
// The game's resources: packed inside the executable or, in a build tree, the
// assets folder (the game's data) and the addons folder (the PC version's
// additions) beside it or beside one of its parent folders.
pc::Assets openResources() {
    const auto executable=pc::executablePath();
    if(pc::hasAssetPack(executable))return pc::Assets(executable);
    pc::Assets assets;
    for(const char* name:{"assets","addons"})
        for(auto directory=executable.parent_path();!directory.empty();) {
            if(std::filesystem::is_directory(directory/name)) {assets.add(directory/name);break;}
            const auto parent=directory.parent_path();if(parent==directory)break;directory=parent;
        }
#ifdef __EMSCRIPTEN__
    // The browser build runs on data the player supplied: the pack their machine built
    // (the route this repo already has - build.ps1, dqiv_pack), or their own ROM. The
    // page writes whichever it gets to /pack, once, before main(). When it has not,
    // the pack is allowed to be empty so the run gets through setup() and the
    // self-check and stops at the first thing that genuinely needs an asset. On
    // Windows this stays a hard error, and it should - a Windows build without its
    // data is a broken build.
    //
    // Read in place, not extracted, measured both ways: extracting blocks the page's
    // own main thread on a 128 MB ROM. What makes reading in place affordable is that
    // Assets keeps the source in memory, so a file inside it is a slice - MEMFS copies
    // the whole file on every open, and doing that per file is what blocked.
    if(std::filesystem::is_regular_file("/pack")) {
        pc::Assets supplied("/pack");
        std::cout<<"Using what the page put at /pack: "<<supplied.count()<<" files\n";
        return supplied;
    }
    if(!assets.contains("data/SOUND/sound_data.sdat"))std::cout<<"No asset pack: running with an empty one\n";
#else
    if(!assets.contains("data/SOUND/sound_data.sdat"))throw std::runtime_error("Game data not found: build the game with build.ps1");
#endif
    return assets;
}
// Everything the frame needs, in one place. On Windows this lived on main's
// stack; on wasm main() returns as soon as the loop is registered, so the run has
// to outlive it and the loop becomes a callback. Keeping one definition here is
// what stops that from becoming two copies of this file.
//
// Declaration order is build order, and destruction runs in reverse, so clearWait
// sits last - exactly where it used to sit - and still destructs first.
struct GameRun {
    pc::Assets assets{openResources()};
    pc::GameWindow window{false,false};
    pc::GameServices services{assets};
    pc::NitroFilesystem filesystem{assets};
#ifdef __EMSCRIPTEN__
    // Bound, not owned. These three are constructed early, see above.
    const std::filesystem::path& savePath{pc::earlySavePath};
    const std::filesystem::path& identityPath{pc::earlyIdentityPath};
    pc::NitroIdentity& identity{pc::earlyIdentity};
#else
    const std::filesystem::path savePath{pc::defaultSavePath()};
    std::filesystem::path identityPath{savePath.string()+".identity"};
    pc::NitroIdentity identity{identityPath};
#endif
    pc::SaveStore save{savePath};
    pc::NitroBackup backup{save};
#ifdef __EMSCRIPTEN__
    pc::NitroVideo& video{pc::earlyVideo};
    pc::NitroPower& power{pc::earlyPower};
#else
    pc::NitroVideo video;
    pc::NitroPower power;
#endif
    pc::GxGeometry geometry;
    pc::NitroGeometry geometryContext{geometry};
    pc::GxTextures textures;
    pc::Display display;
    pc::NitroSound sound;
    pc::MobileSound mobileSound{assets,48000};
    pc::NitroThreads threads;
#ifdef __EMSCRIPTEN__
    pc::NitroOverlays& overlays{pc::earlyOverlays};
#else
    pc::NitroOverlays overlays{gameOverlayImages()};
#endif
    pc::FramePresenter presenter{window,display,textures};
    OSThreadQueue frameQueue{};
    std::array<std::int16_t,1600> samples{};
    unsigned renderedSwap=0;
    bool fastFrame=false;
    unsigned boostFrames=0;
    bool running=true;
    int scene=-1;
    unsigned frame=0;
    struct ClearWait {~ClearWait(){pc::setWaitCallback({});pc::setFrameCallback({});}} clearWait;

    void setup() {
        // Both DS screens become one picture, presented by the GPU.
        pc::dualScreen().unified=true;
        pc::dualScreen().canvasWidth=384*16/9.f;
        window.applySettings();
#ifdef __EMSCRIPTEN__
        // The first thing drawn on a real GPU, put through the real shaders with
        // answers we already know. See wasm/src/gpu-selfcheck.cpp.
        gpuSelfCheck(window,textures);
#endif
        if(!overlays.load(38)) throw std::runtime_error("Cannot construct main game module");
        pc::setFrameCallback([this] {
            pc::ProfileScope frameTiming("render-callback");
            if(geometry.swaps()!=renderedSwap) {
                renderedSwap=geometry.swaps();
                auto state=geometry.raster;
                state.fogColor=*static_cast<unsigned*>(video.reg(0x04000358,4));
                pc::GxRenderOptions options;
                options.displayControl=*static_cast<unsigned short*>(video.reg(0x04000060,2));
                options.wBuffer=(geometry.swapParameters()&2)!=0;options.manualSort=(geometry.swapParameters()&1)!=0;
                presenter.frame(video,geometry,state,options,false);
            }
            if(!fastFrame)presenter.present();
        });
        // Faster battles (settings): only one game frame in 2, 3 or 4 is
        // shown, paced and heard; the music keeps its tempo.
        threads.setIdleHandler([this] {
            running=window.poll();
            const bool shown=!fastFrame;
            const int boost=data_0210bb94.unkfunc_0205810c()==13?pc::gameSettings().battleBoost+1:1;
            fastFrame=boost>1&&++boostFrames%unsigned(boost)!=0;
            if(shown) {
                pc::ProfileScope timing("sound");
                sound.render(samples);mobileSound.mix(samples);
            }
            power.advanceFrame();pc::advanceWireless();
            if(shown) {window.audio(samples);window.pace();}
            OS_UnpauseThread(&frameQueue);
        });
        pc::setWaitCallback([this] {OS_PauseThread(&frameQueue);threads.checkErrors();});
    }
    void step() {
        // The journal keeps the scene changes (battle, town, field...).
        const auto currentScene=int(data_0210bb94.unkfunc_0205810c());
        if(scene!=currentScene) {scene=currentScene;std::cout<<"Scene "<<scene<<", frame "<<frame<<'\n';}
        threads.checkErrors();
        {
            pc::ProfileScope stepTiming("frame");
            if(!data_0210bb94.nativeStep()) {running=false;return;}
        }
        pc::profileFrame();
        geometry.clearTriangles();
        ++frame;
#ifdef __EMSCRIPTEN__
        // The journal only prints on a scene change, so "the journal stopped growing"
        // does not mean the game stopped - and a WebGL canvas gives the same image
        // back while it is showing the same scene. So the frame counter is the thing
        // that answers whether the loop is running, and this is the cheapest place to
        // read it from the browser. Printing the first few frames as well, because a
        // counter that only speaks every 60 frames cannot tell "stopped at 1" from
        // "never started".
        if(frame<3||frame%60==0)std::cout<<"frame "<<frame<<"\n";
#endif
    }
};
GameRun* gameRun=nullptr;   // the wasm loop callback needs a handle to it
}

int main() {
    {
        std::filesystem::create_directories(pc::saveDirectory());
        adoptPreviousSave();
        // The previous session's journal (its error, after a crash) stays readable.
        std::error_code ignored;
        std::filesystem::copy_file(journalPath(),pc::saveDirectory()/"journal-precedent.txt",
            std::filesystem::copy_options::overwrite_existing,ignored);
        // Shared opening: a second running copy of the game must not lose its
        // output (an exclusive journal would leave it without a stdout).
#ifdef __EMSCRIPTEN__
        if(FILE* file=std::fopen(journalPath().c_str(),"w"))std::fclose(file);
#else
        if(FILE* file=_fsopen(journalPath().c_str(),"w",_SH_DENYWR))std::fclose(file);
#endif
#ifdef __EMSCRIPTEN__
        constexpr const char* nullDevice="/dev/null";
#else
        constexpr const char* nullDevice="NUL";
#endif
        if(!std::freopen(journalPath().c_str(),"a",stdout))std::freopen(nullDevice,"w",stdout);
        if(!std::freopen(journalPath().c_str(),"a",stderr))std::freopen(nullDevice,"w",stderr);
    }
    try {
        std::cout.setf(std::ios::unitbuf);
        pc::loadSettings();
        static GameRun run;   // static so it outlives main() on wasm, see below
        run.setup();
        std::cout << "Original application initialization\n";
        data_020c4fe4.unkfunc_02057f8c(0);
        data_0210bb94.nativeStart(data_020c4fe4.vf10());
#ifdef __EMSCRIPTEN__
        // main() cannot block here. It returns as soon as the loop is registered,
        // which is why the run above is static and why the loop is a callback.
        // 0 means use the browser's own frame timing.
        gameRun=&run;
        emscripten_set_main_loop([] { gameRun->step(); },0,1);
        return 0;
#else
        while(run.running)run.step();
        run.threads.checkErrors();
        data_0210bb94.nativeStop();
        pc::setButtons(0);
        return 0;
#endif
    } catch (const std::exception& error) {
        std::cerr << "Dragon Quest IV stopped: " << error.what() << '\n';
        std::cerr.flush();showError(error.what());
        return 1;
    }
}
