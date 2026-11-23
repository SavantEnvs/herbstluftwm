// Window rules: the input (split on newline, TAB or NUL) is the argument list of one `rule` call —
// conditions (class=, instance=, title~ regexes, pid, fixedsize, ...) and consequences (tag=,
// floating_geometry=, monitor=, keys_inactive=, ...). The stored rule is then printed with
// `list_rules` and all rules are removed with `unrule --all`.
#include "hlwm_fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::vector<std::string> args = {"rule"};
    std::string arg;
    for (size_t i = 0; i <= size; i++) {
        char c = (i < size) ? static_cast<char>(data[i]) : '\n';
        if (c == '\n' || c == '\t' || c == '\0') {
            args.push_back(arg);
            arg.clear();
        } else {
            arg += c;
        }
    }
    hlwm_fuzz::Session session;
    session.call({"add", "foo"});
    session.call(args);
    session.call({"list_rules"});
    session.call({"unrule", "--all"});
    session.call({"list_rules"});
    return 0;
}
