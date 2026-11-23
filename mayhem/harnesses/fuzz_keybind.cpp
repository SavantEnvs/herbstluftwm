// Key and mouse binding combos: the first input line is a combo string (modifiers + keysym, or
// modifiers + button), the remaining lines are the bound command and its arguments. The combo is
// bound with `keybind` and `mousebind`, listed, and unbound again with `keyunbind`.
#include "hlwm_fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::vector<std::string> lines;
    std::string cur;
    for (size_t i = 0; i <= size; i++) {
        char c = (i < size) ? static_cast<char>(data[i]) : '\n';
        if (c == '\n' || c == '\0') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    std::vector<std::string> bound(lines.begin() + 1, lines.end());
    if (bound.empty()) {
        bound = {"close"};
    }
    hlwm_fuzz::Session session;
    std::vector<std::string> kb = {"keybind", lines[0]};
    kb.insert(kb.end(), bound.begin(), bound.end());
    session.call(kb);
    std::vector<std::string> mb = {"mousebind", lines[0]};
    mb.insert(mb.end(), bound.begin(), bound.end());
    session.call(mb);
    session.call({"list_keybinds"});
    session.call({"keyunbind", lines[0]});
    session.call({"mouseunbind"});
    session.call({"keyunbind", "--all"});
    return 0;
}
