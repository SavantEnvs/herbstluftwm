// Shared in-process setup for the herbstluftwm fuzz harnesses.
//
// herbstluftwm has no library target: the window manager is one executable whose setup lives in
// src/main.cpp. This header compiles that file into the harness (its main() renamed) so the
// harness uses upstream's own command table (commands(root)) and replicates main()'s setup
// against a private Xvfb server started once per process.
//
// State model (docs/rules/common.md, "each input's result depends only on that input"):
//   kept across inputs (read-only for the WM): the Xvfb server, the X connection, the Ewmh and
//     IpcServer objects (the WM selection and the hidden WM window).
//   rebuilt per input: the whole object tree (Root: tags, monitors, frames, settings, theme,
//     rules, keybinds, mousebinds, clients, attributes) and the command table. After each input
//     the Root is shut down and destroyed exactly as main() does on `quit`, and pending X events
//     are discarded.
//   allow-list: commands that start processes or change the process environment (spawn, wmexec,
//     reload, setenv, export, unsetenv) are removed from the command table, so neither a literal
//     nor a synthesised (chain/substitute/sprintf/foreach) call can reach them.
#pragma once

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#define main hlwm_upstream_main
#include "main.cpp"
#undef main

// Commands::initialize() only installs a table once; the table (defined in command.cpp) is
// cleared at the end of every session so the next session's Root gets its own table.
namespace Commands {
extern std::shared_ptr<const CommandTable> command_table;
}

// CommandTable has no mutator. Reach its private map through the explicit-instantiation access
// rule ([temp.explicit]) to erase the blocked commands; upstream code is not changed.
namespace hlwm_fuzz {
using CommandMap = std::unordered_map<std::string, CommandBinding>;
CommandMap CommandTable::* commandTableMap();
template <CommandMap CommandTable::* M>
struct CommandTableMapAccess {
    friend CommandMap CommandTable::* commandTableMap() { return M; }
};
template struct CommandTableMapAccess<&CommandTable::map>;
}  // namespace hlwm_fuzz

extern char** environ;

namespace hlwm_fuzz {

static XConnection* g_X = nullptr;
static Ewmh* g_ewmh = nullptr;
static IpcServer* g_ipc = nullptr;
static Globals g_globals;

static pid_t s_xvfbPid = -1;

static void stopXvfb() {
    if (s_xvfbPid > 0) {
        kill(s_xvfbPid, SIGKILL);
        waitpid(s_xvfbPid, nullptr, 0);
        s_xvfbPid = -1;
    }
}

// Start a private Xvfb; -displayfd lets the server pick a free display number and report it,
// -terminate stops it once this process (its only client) disconnects.
static bool startXvfb() {
    int fds[2];
    if (pipe(fds) != 0) {
        return false;
    }
    std::string fdArg = std::to_string(fds[1]);
    const char* xvfb = getenv("HLWM_FUZZ_XVFB");
    if (!xvfb) {
        xvfb = "/usr/bin/Xvfb";
    }
    std::vector<std::string> args = {
        xvfb, "-displayfd", fdArg, "-screen", "0", "800x600x24",
        "-nolisten", "tcp", "-terminate", "-noreset",
    };
    std::vector<char*> argv;
    for (auto& a : args) {
        argv.push_back(&a[0]);
    }
    argv.push_back(nullptr);
    // fork+exec rather than posix_spawn so the child can ask for SIGKILL when this process
    // dies: -terminate alone does not reliably stop Xvfb, and a server outliving the harness
    // keeps any tracer that follows children (Mayhem's coverage collection, strace -f) waiting
    // until the per-input timeout, so no execution ever completes.
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent) {
            _exit(127);
        }
        close(fds[0]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, 1);
            dup2(devnull, 2);
        }
        execve(xvfb, argv.data(), environ);
        _exit(127);
    }
    close(fds[1]);
    s_xvfbPid = pid;
    atexit(stopXvfb);
    char buf[32] = {};
    size_t got = 0;
    while (got < sizeof(buf) - 1) {
        ssize_t n = read(fds[0], buf + got, sizeof(buf) - 1 - got);
        if (n <= 0) {
            break;
        }
        got += static_cast<size_t>(n);
        if (memchr(buf, '\n', got)) {
            break;
        }
    }
    close(fds[0]);
    if (got == 0) {
        return false;
    }
    std::string display = ":" + std::string(buf, strcspn(buf, "\n"));
    setenv("DISPLAY", display.c_str(), 1);
    return true;
}

static void initOnce() {
    if (g_X) {
        return;
    }
    if (!getenv("HLWM_FUZZ_USE_DISPLAY") && !startXvfb()) {
        fprintf(stderr, "hlwm_fuzz: cannot start Xvfb\n");
        abort();
    }
    setlocale(LC_CTYPE, "");
    XConnection::setExitOnError(false);
    g_X = XConnection::connect();
    g_display = g_X->display();
    if (!g_display) {
        fprintf(stderr, "hlwm_fuzz: cannot open display\n");
        abort();
    }
    g_ewmh = new Ewmh(*g_X);
    if (!g_ewmh->acquireScreenSelection(false) || g_X->otherWmListensRoot()) {
        fprintf(stderr, "hlwm_fuzz: another window manager is running\n");
        abort();
    }
    g_X->tryInitTransparency();
    sigaction_signal(SIGCHLD, [](int) {});
    g_root = g_X->root();
    XSelectInput(g_X->display(), g_X->root(),
                 SubstructureRedirectMask|SubstructureNotifyMask|ButtonPressMask
                 |EnterWindowMask|LeaveWindowMask|StructureNotifyMask);
    g_ewmh->installWmWindow();
    g_ipc = new IpcServer(*g_X);
    FontData::s_xconnection = g_X;
    g_globals.importTagsFromEwmh = false;
    g_globals.globalAutostartPath = "/nonexistent";
    g_globals.autostartPath = "/nonexistent";
}

// One window-manager session: built in the constructor (as main() does before its loop) and torn
// down in the destructor (as main() does after `quit`).
class Session {
public:
    Session() {
        initOnce();
        root_ = make_shared<Root>(g_globals, *g_X, *g_ewmh, *g_ipc);
        Root::setRoot(root_);
        unique_ptr<CommandTable> table = commands(root_);
        for (const char* blocked : {"spawn", "wmexec", "reload", "setenv", "export", "unsetenv"}) {
            ((*table).*commandTableMap()).erase(blocked);
        }
        Commands::initialize(std::move(table));
        loop_ = new XMainLoop(*g_X, root_.get());
        g_main_loop = loop_;
        root_->monitors()->ensure_monitors_are_available();
        tag_force_update_flags();
        all_monitors_apply_layout();
        g_ewmh->updateAll();
    }
    ~Session() {
        Commands::command_table.reset();
        root_->shutdown();
        root_.reset();
        Root::setRoot(root_);
        g_main_loop = nullptr;
        delete loop_;
        g_exec_before_quit = false;
        g_exec_args.clear();
        XSync(g_X->display(), True);  // discard events this session caused
    }
    // Run one command the way XMainLoop::callCommand does for a herbstclient request.
    int call(const std::vector<std::string>& argv, std::string* out = nullptr) {
        std::ostringstream output;
        std::ostringstream error;
        std::string commandName = argv.empty() ? "" : argv[0];
        auto input = argv.empty()
            ? Input("", argv)
            : Input(argv[0], std::vector<std::string>(argv.begin() + 1, argv.end()));
        OutputChannels channels(commandName, output, error);
        int exitCode = Commands::call(input, channels);
        if (out) {
            *out = output.str();
        }
        return exitCode;
    }
private:
    shared_ptr<Root> root_;
    XMainLoop* loop_ = nullptr;
};

// Split `data` into commands (one per line) and arguments (separated by TAB or NUL), the argv
// lists herbstclient would send.
static std::vector<std::vector<std::string>> splitCommands(const uint8_t* data, size_t size) {
    std::vector<std::vector<std::string>> cmds;
    std::vector<std::string> cur;
    std::string arg;
    bool any = false;
    for (size_t i = 0; i <= size; i++) {
        char c = (i < size) ? static_cast<char>(data[i]) : '\n';
        if (c == '\n') {
            if (any || !arg.empty()) {
                cur.push_back(arg);
            }
            if (!cur.empty()) {
                cmds.push_back(cur);
            }
            cur.clear();
            arg.clear();
            any = false;
        } else if (c == '\t' || c == '\0') {
            cur.push_back(arg);
            arg.clear();
            any = true;
        } else {
            arg += c;
            any = true;
        }
    }
    return cmds;
}

}  // namespace hlwm_fuzz
