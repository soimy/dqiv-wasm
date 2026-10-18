#include "main/menu/UnkMenuSystem.hpp"
#ifdef __EMSCRIPTEN__
#include <iostream>
#endif
#include "main/menu/UnkMenuIconData.hpp"
#include "main/menu/MenuManager.hpp"

ARM void unkfunc_02052424()
{
    unkfunc_02051740();
}

ARM void unkfunc_02052430()
{
    unkfunc_0205060c();
    MenuManager::setup();
}

ARM void unkfunc_02052440()
{
    unkfunc_0204e6e4();
    unkfunc_02050340();
}

ARM void unkfunc_02052450()
{
}

ARM void unkfunc_02052454()
{
    unkfunc_0204e878();
    unkfunc_02050368();
}

ARM void unkfunc_02052464()
{
    // Measured, because this is where the browser stops now: the stage before this one
    // in GlobalGamePart::vf0c() prints and the one after it does not. The print is on
    // the FIRST line, before any call, so "did we get here at all" has an answer that
    // does not depend on which of the three calls blocks.
    std::cout<<"  unkfunc_02052464: ENTERED\n";
    MenuManager::execute();
    std::cout<<"  unkfunc_02052464: menu manager returned\n";
    unkfunc_0204e97c();
    std::cout<<"  unkfunc_02052464: unk_0204e97c returned\n";
    unkfunc_0205036c();
    std::cout<<"  unkfunc_02052464: unk_0205036c returned\n";
}

ARM void unkfunc_02052478()
{
    unkfunc_0204ea80();
    unkfunc_0205037c();
}
