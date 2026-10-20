#include "Profile.hpp"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace pc {
namespace {
struct Total {double microseconds=0;unsigned count=0;};
std::map<std::string,Total>& totals() {static std::map<std::string,Total> value;return value;}
unsigned frames=0;
}
bool profiling() {
#ifdef __EMSCRIPTEN__
    // There is no environment to read in the browser, so on wasm the switch has to be in code.
    return true;
#else
    static const bool enabled=std::getenv("DQIV_PROFILE")!=nullptr;return enabled;
#endif
}
void profileAdd(const char* stage,double microseconds,unsigned count) {
    auto& total=totals()[stage];total.microseconds+=microseconds;total.count+=count;
}
void profileFrame(unsigned every) {
    // DQIV_PROFILE=n prints every n frames instead.
    static const unsigned chosen=[] {const char* v=std::getenv("DQIV_PROFILE");return v?unsigned(std::atoi(v)):0u;}();
    if(chosen>0)every=chosen;
    if(!profiling()||++frames<every)return;
    std::printf("Profile per frame:");
    for(const auto& [stage,total]:totals())std::printf(" %s %.0fus/%u",stage.c_str(),total.microseconds/frames,total.count/frames);
    std::printf("\n");std::fflush(stdout);
#ifdef __EMSCRIPTEN__
    // The same stages on the page, so the numbers can be read without the console:
    // the browser has no environment to switch the profiler on in, and asking a
    // player to open DevTools mid-game is how this data stops arriving.
    {
        const char* want[]={"frame","render-callback","capture+display","display-compose",
            "display-read","render3D","r3d-prepare","texture-prepare","present","readNative-sync"};
        char line[512];int used=0;
        for(const char* stage:want) {
            const auto found=totals().find(stage);
            const unsigned value=found==totals().end()?0u:unsigned(found->second.microseconds/frames+0.5);
            used+=std::snprintf(line+used,sizeof(line)-std::size_t(used),"%s%s %u",used?" ":"",stage,value);
        }
        std::string script="window.__profile='";script+=line;script+="'";
        emscripten_run_script(script.c_str());
    }
#endif
    totals().clear();frames=0;
}
}
