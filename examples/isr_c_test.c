// C-level port of interrupt_test12.s: TTO fires first (near-instant),
// RTC fires second (~200k steps later, default AC line frequency), both
// armed simultaneously. The handler is now real compiler-generated code
// (not hand-written assembly) reached via __attribute__((interrupt)) --
// this is the end-to-end proof that a C ISR, compiled through the normal
// pipeline (Clang attribute -> LLVM IR "interrupt" fn attr -> AC0/AC1
// callee-saved handling -> RETI's NIOS 077 + JMP @0 pair), behaves
// exactly like the hand-verified raw-assembly version.
volatile int first_dev = 0;
volatile int second_dev = 0;
volatile int call_count = 0;

void __attribute__((interrupt)) handler(void) {
    int dev;
    asm volatile("DIB %0,077" : "=r"(dev));
    asm volatile("NIOC 011");  // clear TTO's own flags (harmless if RTC fired)
    asm volatile("NIOS 014");  // (re)start RTC (harmless if TTO fired)
    if (call_count == 0) {
        first_dev = dev;
        call_count = 1;
    } else {
        second_dev = dev;
        asm volatile("HALT");
    }
}

void _start(void) {
    // `handler` is only reachable via the interrupt vector at memory
    // location 1 (added by hand, post-compile -- see the .s file), which
    // is invisible to LLVM's call graph. Without a genuine reference like
    // this, opt's globaldce (run by the real eclipse-cc pipeline, and
    // mirrored manually here) sees `handler` as unreferenced dead code
    // and drops it entirely.
    volatile void *keep_handler_alive = (void *)&handler;
    (void)keep_handler_alive;

    asm volatile("NIOS 077");             // INTEN
    asm volatile("NIOS 014");             // start RTC ticking
    asm volatile("DOAS %0,011" :: "r"(101)); // start TTO output ('e')
    for (;;) {}
}
