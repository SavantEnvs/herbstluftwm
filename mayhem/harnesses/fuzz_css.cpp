// Theme CSS: the whole input is a style sheet, given to the theme twice through the public
// attribute interface: as `theme.style_override` (CssSource parsed from the string) and as a file
// named by `theme.name`. `debug_css` then matches it against a frame tree with tabbed clients.
#include "hlwm_fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string css(reinterpret_cast<const char*>(data), size);
    hlwm_fuzz::Session session;
    session.call({"set_attr", "theme.style_override", css});
    session.call({"debug_css", "--print-css", "--stylesheet=" + css});
    const char* tmpdir = getenv("TMPDIR");
    std::string path = std::string(tmpdir && *tmpdir ? tmpdir : "/tmp") + "/hlwm_fuzz_css_XXXXXX";
    int fd = mkstemp(&path[0]);
    if (fd >= 0) {
        size_t off = 0;
        while (off < size) {
            ssize_t n = write(fd, data + off, size - off);
            if (n <= 0) {
                break;
            }
            off += static_cast<size_t>(n);
        }
        close(fd);
        session.call({"set_attr", "theme.name", path});
        unlink(path.c_str());
    }
    return 0;
}
