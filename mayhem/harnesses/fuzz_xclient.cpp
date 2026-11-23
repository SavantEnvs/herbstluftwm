// X client windows: the input is a stream of operations a client application performs on its
// windows over a second X connection — create, map, unmap, destroy, move/resize, set ICCCM/EWMH
// properties (WM_NAME, WM_CLASS, WM_HINTS, WM_NORMAL_HINTS, WM_TRANSIENT_FOR, _NET_WM_* ...)
// to fuzz bytes, and send EWMH client messages to the root window — interleaved with a few
// fixed herbstluftwm commands (max layout, tabbed_max, floating, fullscreen, tag moves). After
// every operation the window manager handles the resulting X events with upstream's own event
// handlers (the body of XMainLoop::run, minus its blocking select() and the XFixes selection
// events, which only concern the WM selection), so manage/unmanage,
// ewmh.cpp, rules, decorations and tab bars are exercised.
#include "hlwm_fuzz_common.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>

namespace hlwm_fuzz {
using HandlerTable = XMainLoop::EventHandler[LASTEvent];
HandlerTable XMainLoop::* xmainloopHandlers();
template <HandlerTable XMainLoop::* H>
struct XMainLoopAccess {
    friend HandlerTable XMainLoop::* xmainloopHandlers() { return H; }
};
template struct XMainLoopAccess<&XMainLoop::handlerTable_>;
}  // namespace hlwm_fuzz

namespace {

Display* g_client = nullptr;

class Bytes {
public:
    Bytes(const uint8_t* d, size_t n) : d_(d), n_(n) {}
    bool empty() const { return i_ >= n_; }
    uint8_t u8() { return i_ < n_ ? d_[i_++] : 0; }
    uint16_t u16() { return static_cast<uint16_t>(u8() | (u8() << 8)); }
    uint32_t u32() { return static_cast<uint32_t>(u16() | (static_cast<uint32_t>(u16()) << 16)); }
    std::vector<uint8_t> take(size_t len) {
        len = std::min(len, n_ - std::min(i_, n_));
        std::vector<uint8_t> v(d_ + i_, d_ + i_ + len);
        i_ += len;
        return v;
    }
private:
    const uint8_t* d_;
    size_t n_;
    size_t i_ = 0;
};

const char* kPropAtoms[] = {
    "WM_NAME", "_NET_WM_NAME", "WM_CLASS", "WM_HINTS", "WM_NORMAL_HINTS", "WM_TRANSIENT_FOR",
    "_NET_WM_WINDOW_TYPE", "_NET_WM_STATE", "_NET_WM_PID", "_NET_WM_DESKTOP", "_NET_WM_STRUT",
    "_NET_WM_STRUT_PARTIAL", "WM_CLIENT_LEADER", "_NET_WM_ICON_NAME", "WM_WINDOW_ROLE",
    "_NET_WM_WINDOW_OPACITY", "_NET_WM_USER_TIME", "_NET_WM_ICON",
};
const char* kTypeAtoms[] = {
    "STRING", "UTF8_STRING", "CARDINAL", "ATOM", "WINDOW", "WM_HINTS", "WM_SIZE_HINTS",
    "_NET_WM_WINDOW_TYPE_DIALOG", "_NET_WM_WINDOW_TYPE_DOCK", "_NET_WM_STATE_FULLSCREEN",
    "_NET_WM_STATE_DEMANDS_ATTENTION", "_NET_WM_WINDOW_TYPE_UTILITY", "_NET_WM_WINDOW_TYPE_NORMAL",
};
const char* kMessageAtoms[] = {
    "_NET_ACTIVE_WINDOW", "_NET_WM_STATE", "_NET_CURRENT_DESKTOP", "_NET_WM_DESKTOP",
    "_NET_WM_MOVERESIZE", "_NET_RESTACK_WINDOW", "_NET_REQUEST_FRAME_EXTENTS", "WM_CHANGE_STATE",
    "_NET_MOVERESIZE_WINDOW", "_NET_NUMBER_OF_DESKTOPS", "_NET_SHOWING_DESKTOP",
};
const std::vector<std::vector<std::string>> kCommands = {
    {"set_layout", "max"}, {"set", "tabbed_max", "1"}, {"set", "tabbed_max", "0"},
    {"split", "explode"}, {"focus", "right"}, {"cycle", "+1"}, {"floating", "toggle"},
    {"fullscreen", "toggle"}, {"add", "t2"}, {"move", "t2"}, {"use", "t2"}, {"use_index", "0"},
    {"remove"}, {"set_attr", "clients.focus.minimized", "on"}, {"jumpto", "urgent"},
    {"rule", "floating=on"}, {"unrule", "--all"}, {"set", "focus_follows_mouse", "1"},
    {"shift", "left"}, {"set_attr", "tags.focus.floating", "toggle"}, {"apply_rules", "--all"},
    {"pseudotile", "toggle"}, {"set_layout", "grid"}, {"bring", "urgent"},
};

template <size_t N>
Atom pick(const char* const (&names)[N], uint8_t b) {
    return XInternAtom(g_client, names[b % N], False);
}

void pump(XMainLoop* loop) {
    XSync(g_client, False);
    Display* d = hlwm_fuzz::g_X->display();
    auto& handlers = loop->*hlwm_fuzz::xmainloopHandlers();
    XSync(d, False);
    XEvent event;
    while (XQLength(d)) {
        XNextEvent(d, &event);
        if (event.type < LASTEvent) {
            XMainLoop::EventHandler handler = handlers[event.type];
            if (handler != nullptr) {
                (loop->*handler)(&event);
            }
        }
        Root::get()->watchers->scanForChanges();
        XSync(d, False);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    hlwm_fuzz::Session session;
    if (!g_client) {
        g_client = XOpenDisplay(nullptr);
        if (!g_client) {
            abort();
        }
    }
    XMainLoop* loop = g_main_loop;
    Window rootWin = DefaultRootWindow(g_client);
    Atom wmDelete = XInternAtom(g_client, "WM_DELETE_WINDOW", False);
    std::vector<Window> wins;
    Bytes in(data, size);
    while (!in.empty()) {
        uint8_t op = in.u8() % 8;
        uint8_t sel = in.u8();
        Window w = wins.empty() ? None : wins[sel % wins.size()];
        switch (op) {
        case 0:
            if (wins.size() < 16) {
                int x = in.u8(), y = in.u8();
                unsigned wd = in.u8() + 1u, ht = in.u8() + 1u;
                Window nw = XCreateSimpleWindow(g_client, rootWin, x, y, wd, ht, 0, 0, 0);
                // close/_NET_CLOSE_WINDOW then send WM_DELETE_WINDOW instead of XKillClient,
                // which would kill this harness's client connection.
                XSetWMProtocols(g_client, nw, &wmDelete, 1);
                wins.push_back(nw);
            }
            break;
        case 1:
            if (w) XMapWindow(g_client, w);
            break;
        case 2:
            if (w) XUnmapWindow(g_client, w);
            break;
        case 3:
            if (w) {
                XDestroyWindow(g_client, w);
                wins.erase(wins.begin() + (sel % wins.size()));
            }
            break;
        case 4: {
            if (!w) break;
            Atom prop = pick(kPropAtoms, in.u8());
            Atom type = pick(kTypeAtoms, in.u8());
            uint8_t fmtSel = in.u8();
            int format = (fmtSel & 1) ? 32 : 8;
            size_t count = in.u8() % 24;
            auto bytes = in.take(format == 32 ? count * 4 : count);
            if (format == 32) {
                std::vector<long> longs(bytes.size() / 4);
                for (size_t k = 0; k < longs.size(); k++) {
                    uint32_t v;
                    memcpy(&v, &bytes[k * 4], 4);
                    longs[k] = static_cast<long>(v);
                }
                XChangeProperty(g_client, w, prop, type, 32, PropModeReplace,
                                reinterpret_cast<unsigned char*>(longs.data()),
                                static_cast<int>(longs.size()));
            } else {
                XChangeProperty(g_client, w, prop, type, 8, PropModeReplace,
                                bytes.data(), static_cast<int>(bytes.size()));
            }
            break;
        }
        case 5: {
            XEvent ev = {};
            ev.xclient.type = ClientMessage;
            ev.xclient.window = w ? w : rootWin;
            ev.xclient.message_type = pick(kMessageAtoms, in.u8());
            ev.xclient.format = 32;
            for (int k = 0; k < 5; k++) {
                ev.xclient.data.l[k] = static_cast<long>(in.u32() % 64);
            }
            if (in.u8() & 1) {
                ev.xclient.data.l[1] = static_cast<long>(pick(kTypeAtoms, in.u8()));
            }
            XSendEvent(g_client, rootWin, False,
                       SubstructureNotifyMask | SubstructureRedirectMask, &ev);
            break;
        }
        case 6:
            if (w) {
                XWindowChanges wc = {};
                wc.x = static_cast<int16_t>(in.u16());
                wc.y = static_cast<int16_t>(in.u16());
                wc.width = in.u8() + 1;
                wc.height = in.u8() + 1;
                wc.stack_mode = in.u8() % 5;
                XConfigureWindow(g_client, w,
                                 in.u8() & (CWX | CWY | CWWidth | CWHeight | CWStackMode), &wc);
            }
            break;
        case 7:
            session.call(kCommands[sel % kCommands.size()]);
            break;
        }
        pump(loop);
    }
    for (Window w : wins) {
        XDestroyWindow(g_client, w);
    }
    pump(loop);
    return 0;
}
