#include "CrashLog.hpp"
#include <csignal>
#include <ctime>
#include <ucontext.h>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>

namespace miyoofin {

namespace {

int g_fd = -1;

void writeNumber(int fd, long value)
{
    char buf[24];
    int n = 0;
    bool negative = value < 0;
    unsigned long v =
        negative ? static_cast<unsigned long>(-value) : static_cast<unsigned long>(value);
    char rev[24];
    do {
        rev[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v);
    int o = 0;
    if (negative)
        buf[o++] = '-';
    while (n)
        buf[o++] = rev[--n];
    (void)!write(fd, buf, static_cast<size_t>(o));
}

void writeHex(int fd, unsigned long value)
{
    static const char digits[] = "0123456789abcdef";
    char buf[20];
    int n = 0;
    do {
        buf[n++] = digits[value & 15];
        value >>= 4;
    } while (value);
    (void)!write(fd, "0x", 2);
    while (n)
        (void)!write(fd, &buf[--n], 1);
}

void handler(int sig, siginfo_t* info, void* context)
{
    if (g_fd >= 0) {
#if defined(__arm__)
        // The faulting instruction and caller: the backtrace cannot unwind past the signal.
        const ucontext_t* uc = static_cast<const ucontext_t*>(context);
        (void)!write(g_fd, "pc=", 3);
        writeHex(g_fd, uc->uc_mcontext.arm_pc);
        (void)!write(g_fd, " lr=", 4);
        writeHex(g_fd, uc->uc_mcontext.arm_lr);
        (void)!write(g_fd, " addr=", 6);
        writeHex(g_fd, reinterpret_cast<unsigned long>(info->si_addr));
        (void)!write(g_fd, "\nstack:", 7);
        const unsigned long* sp = reinterpret_cast<const unsigned long*>(uc->uc_mcontext.arm_sp);
        for (int i = 0; i < 160; ++i) {
            (void)!write(g_fd, " ", 1);
            writeHex(g_fd, sp[i]);
        }
        (void)!write(g_fd, "\n", 1);
#else
        (void)context;
        (void)info;
#endif
        static const char head[] = "--- fatal signal ";
        (void)!write(g_fd, head, sizeof(head) - 1);
        writeNumber(g_fd, sig);
        (void)!write(g_fd, " at t=", 6);
        writeNumber(g_fd, static_cast<long>(time(nullptr)));
        (void)!write(g_fd, "\n", 1);
        void* frames[40];
        const int n = backtrace(frames, 40);
        backtrace_symbols_fd(frames, n, g_fd);
        (void)!write(g_fd, "\n", 1);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

} // namespace

void installCrashLog()
{
    g_fd = open("crash.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    void* warm[2];
    (void)backtrace(warm, 2); // loads libgcc now, not inside the handler
    struct sigaction action
    {};
    action.sa_sigaction = handler;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL})
        sigaction(sig, &action, nullptr);
}

} // namespace miyoofin
