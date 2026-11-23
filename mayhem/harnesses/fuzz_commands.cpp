// herbstclient command interpreter: each input line is one herbstclient call (arguments separated
// by TAB or NUL), dispatched through upstream's command table exactly as an IPC request is
// (XMainLoop::callCommand -> Commands::call). Reaches the meta-commands (chain, and, or,
// substitute, sprintf, foreach, compare, attr), completion (`complete`), tags, monitors, frames
// (split/load/dump), settings, rules, keybinds and the attribute/object tree.
#include "hlwm_fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    auto cmds = hlwm_fuzz::splitCommands(data, size);
    hlwm_fuzz::Session session;
    for (const auto& c : cmds) {
        session.call(c);
    }
    return 0;
}
