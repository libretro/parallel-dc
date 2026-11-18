#include <switch.h>

typedef struct {
    u64 regs[29];
    u64 pc;
} mcontext_t;

typedef struct ucontext_t {
    mcontext_t        uc_mcontext;
} ucontext_t;

/* newlib's siginfo_t has no si_addr: this one, in its place */
#include <signal.h>
typedef struct {
    void* si_addr;
} nx_siginfo_t;
#define siginfo_t nx_siginfo_t