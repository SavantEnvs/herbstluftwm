// Frame-tree layout description: the whole input is the LAYOUT argument of `load` (the format
// `dump` prints), parsed by FrameParser and applied by FrameTree. The tree is then re-serialised
// with `dump` and walked by `layout`, and the dumped text is loaded back into a second tag.
#include "hlwm_fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string layout(reinterpret_cast<const char*>(data), size);
    hlwm_fuzz::Session session;
    session.call({"add", "second"});
    if (session.call({"load", layout}) != 0) {
        return 0;
    }
    std::string dumped;
    session.call({"dump"}, &dumped);
    session.call({"layout"});
    session.call({"load", "second", dumped});
    session.call({"cycle_layout"});
    session.call({"split", "explode"});
    session.call({"remove"});
    return 0;
}
