#ifndef _FPS_H
#define _FPS_H

/* Driver primitives for the Data General FPS100 floating-point
 * coprocessor, device code 054. Extracted from test_fps_add.c so any
 * FPS100 program can reuse the protocol without duplicating it.
 *
 * Device 054 uses a two-register command protocol distinct from the
 * standard Busy/Done handshake eclipse_io.h's outa/ina etc. implement:
 * every "write" is a bare (no S/C pulse, no wait loop) DOA of a command
 * word followed by a bare DOB of a data word -- confirmed by the
 * original hand-assembled test_fps_add.asm (known to work on real
 * hardware) never polling SKPDN/SKPBN around these, unlike TTI/TTO
 * elsewhere in this project.
 *
 * fpu_out/fpu_in are macros, not functions, specifically so every call
 * expands inline at its call site instead of going through JSR. The
 * original .asm writes value/cmd with LDA/LDA immediately followed by
 * DOA/DOB -- a tight, fixed-length instruction block with essentially
 * no gap between successive device operations. A real *function* call
 * on this backend carries a full prologue, epilogue, and JSR/return
 * around that same DOA/DOB pair, which is a lot more elapsed real time
 * between operations than the original ever had. This device's actual
 * timing tolerances aren't documented anywhere accessible (see
 * DEBUGGING_NOTES.md), so this isn't a confirmed-necessary fix -- it's
 * the one structural difference between a from-functions conversion and
 * a hand-assembled version *known* to work on real hardware that
 * eclipseemu can't help verify either way, since it doesn't simulate
 * device 054 at all. Inlining gets the instruction cadence back much
 * closer to the original's.
 */

#define FPU_DEV 054

#define fpu_out(cmd, value)                                                  \
    asm volatile(                                                            \
        "DOA %0,054\n\t"                                                     \
        "DOB %1,054\n\t"                                                     \
        :: "r"((unsigned int)(cmd)), "r"((unsigned int)(value)))

#define fpu_in(cmd)                                                          \
    __extension__({                                                          \
        unsigned int _fpu_r;                                                 \
        asm volatile(                                                        \
            "DOA %1,054\n\t"                                                 \
            "DIB %0,054\n\t"                                                 \
            : "=r"(_fpu_r) : "r"((unsigned int)(cmd)));                      \
        _fpu_r;                                                              \
    })

    typedef struct{ 
    int exp; 
    int mh; 
    int ml;
} fps_word_struct; 


static float scale_pow2(float x, int n);
float calculate_value(int exp, int mh, int ml);
int load_psm(unsigned int pgm_addr,int fpu_pgm_len, const unsigned int fpu_pgm[]);
int load_md(unsigned int md_addr, int fpu_md_len, const unsigned int fpu_md[]);
fps_word_struct read_md(int addr);
/* hma is the *low* 16 bits of the host memory address only -- the high
 * half is always sent as a hardcoded 0 in fps_dma_host_out's own body,
 * never derived from hma, because host addresses here never exceed 16
 * bits (for now). unsigned long* would be a 32-bit pointer, wrong for
 * what this function actually dereferences (a single *hma read) and
 * inconsistent with the actual definition in fps.c -- confirmed via a
 * real "conflicting types" compile error before this was fixed. */
int fps_dma_host_out(const unsigned int * hma,unsigned int apdma,unsigned int wc);
/* -- FPU commands (write to the switch/function register) -- */
#define cmd_wtsr 0021031  /* write switch register */
#define cmd_wtfn 0022031  /* write function register (act on switch reg) */
#define cmd_rdfn 0022030  /* read function/status register */
#define cmd_rdlt 0023030  /* read light/data register */

/* -- FPU function codes -- */
#define fn_load_tma  0001003
#define fn_load_ps_0 0001010
#define fn_load_ps_1 0001030
#define fn_load_ps_2 0001050
#define fn_load_ps_3 0001370
#define fn_load_md_1  0001035
#define fn_load_md_2  0001055
#define fn_load_md_3  0001175



#define fn_start     0040000
#define fn_stop      0100000
/* = FN_DEP | FN_REG_MA (0001000 | 02 = 0001002) -- kept as a plain,
 * backward-compatible name: several programs (test_fps_add.c, this
 * project's original hand-converted driver) reference fn_load_ma
 * directly rather than the newer FN_ / FN_REG_ style constants below,
 * and dropping it silently broke them ("use of undeclared identifier
 * 'fn_load_ma'") when this header was rewritten. */
#define fn_load_ma          (FN_DEP | FN_REG_MA)
#define fn_examine_regmd    0002015  /* declared, unused -- matches the original */
#define fn_examine_regmd_o1 0002035
#define fn_examine_regmd_o2 0002055
#define fn_examine_regmd_o3 0002075
#define fn_examine_regpsa   0002000
#define fn_examine_regtma   0002003


	#define FN_STOP      0100000
	#define FN_START     0040000
	#define FN_CONT      0020000
	#define FN_STEP      0010000
	#define FN_RESET     0004000
	#define FN_EXAM      0002000
	#define FN_DEP       0001000
	#define FN_BREAK     0000400
	#define FN_INC_TMA   0000300
	#define FN_INC_DPA   0000200
	#define FN_INC_MA    0000100
	#define FN_WORD3     0000060
	#define FN_WORD2     0000040
	#define FN_WORD1     0000020

	#define FN_REG_PSA        00
	#define FN_REG_SPD        01
	#define FN_REG_MA         02
	#define FN_REG_TMA        03
	#define FN_REG_DPA        04
	#define FN_REG_SPFN       05
	#define FN_REG_APSTATUS   06
	#define FN_REG_DA         07

	#define FN_MEM_SP     005
	#define FN_MEM_PS     010
	#define FN_MEM_INBS   011
	#define FN_MEM_DPX    013
	#define FN_MEM_DPY    014
	#define FN_MEM_MD     015
	#define FN_MEM_TM     017

	#define CMD_REG_SR   0021000
	#define CMD_REG_FN   0022000
	#define CMD_REG_LT   0023000
	#define CMD_PIO      0000030
	#define CMD_WR       0000001
	#define CMD_RSU      0100000

	#define CMD_DMA_HMAH    0002030
	#define CMD_DMA_HMAL    0001010
	#define CMD_DMA_WC      0005050
	#define CMD_DMA_APDMA   0000030
	#define CMD_DMA_CTL     0024060
	
	#define CTL_INTRQ_AP   0040000
	#define CTL_IAPWC      0020000
	#define CTL_IHALT      0010000
	#define CTL_IHWC       0004000
	#define CTL_IHENB      0002000
	#define CTL_CC         0000200
	#define CTL_APDMA      0000100
	#define CTL_WRTHOST    0000040
	#define CTL_DECAPMA    0000020
	#define CTL_FMT_1      0000002
	#define CTL_FMT_2      0000004
	#define CTL_FMT_3      0000006
	#define CTL_HDMA       0000001




#define cmd_wr  0000001
#define cmd_rsu 0100000

#define cmd_dma_hmah  0002030
#define cmd_dma_hmal  0001010
#define cmd_dma_wc    0005050
#define cmd_dma_apdma 0000030
#define cmd_dma_ctl   0024060

#define ctl_intrq_ap 0040000
#define ctl_iapwc    0020000
#define ctl_ihalt    0010000
#define ctl_ihwc     0004000
#define ctl_ihenb    0002000
#define ctl_cc       0000200
#define ctl_apdma    0000100
#define ctl_wrthost  0000040
#define ctl_decapma  0000020
#define ctl_fmt_1    0000002
#define ctl_fmt_2    0000004
#define ctl_fmt_3    0000006
#define ctl_hdma     0000001

#define cmd_wthmah cmd_dma_hmah | cmd_wr
#define cmd_wthmal cmd_dma_hmal | cmd_wr
#define cmd_wtwc cmd_dma_wc | cmd_wr
#define cmd_wtapdma cmd_dma_apdma | cmd_wr
#define cmd_wtctl cmd_dma_ctl | cmd_wr
#define cmd_wtctl_rl cmd_dma_ctl | cmd_wr | cmd_rsu
#endif





