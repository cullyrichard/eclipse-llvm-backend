#ifndef _FPS_H
#define _FPS_H


#define FPU_DEV 054
#define FPU_AP1 055

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

    #define adp_out(cmd, value)                                                  \
    asm volatile(                                                            \
        "DOA %0,055\n\t"                                                     \
        "DOB %1,054\n\t"                                                     \
        :: "r"((unsigned int)(cmd)), "r"((unsigned int)(value)))

#define adp_in(cmd)                                                          \
    __extension__({                                                          \
        unsigned int _fpu_r;                                                 \
        asm volatile(                                                        \
            "DOA %1,055\n\t"                                                 \
            "DIB %0,054\n\t"                                                 \
            : "=r"(_fpu_r) : "r"((unsigned int)(cmd)));                      \
        _fpu_r;                                                              \
    })

#define fpu_clr(void)                                                        \
    asm volatile(                                                            \
        "DOA %0,055\n\t"                                                     \
        :: "r"((unsigned int)(0020200)))

#define fpu_sta(void)                                                        \
    __extension__({                                                          \
        unsigned int _fpu_r;                                                 \
        asm volatile(                                                        \
            "DIC %0,054\n\t"                                                 \
            : "=r"(_fpu_r));                                                 \
        _fpu_r;                                                              \
    })

#define fpu_reg(void)                                                        \
    __extension__({                                                          \
        unsigned int _fpu_r;                                                 \
        asm volatile(                                                        \
            "DIA %0,054\n\t"                                                 \
            : "=r"(_fpu_r));                                                 \
        _fpu_r;                                                              \
    })


typedef struct{
    int exp;
    int mh;
    int ml;
} fps_word_struct;

typedef union{
    unsigned int status;
    struct{
    unsigned ready:1;
    unsigned mask:1;
    unsigned suspend_err:1;
    unsigned command_err:1;
    unsigned rdack:1;
    unsigned diag_func:7;
    unsigned suspend_dma:1;
    unsigned host_dma:1;
    unsigned io_bus:1;
    unsigned dma_active:1;
    };
} fps_stat;

typedef union{
    unsigned int reg;
    struct{
    unsigned REGSELUSED:1;
    unsigned :1;
    unsigned REGSEL:5;
    unsigned :1;
    unsigned RDEC:5;
    unsigned :2;
    unsigned RW:1;
    };
} fps_reg;

void fps_run(unsigned int load_addr);
float scale_pow2(float x, int n);
float calculate_value(int exp, int mh, int ml);
fps_word_struct convert_dma_value(unsigned int hi, unsigned int low);
void load_psm(unsigned int pgm_addr, unsigned int fpu_pgm_len, const unsigned int fpu_pgm[]);
void load_md(unsigned int md_addr,unsigned int md_len,const unsigned int md_arr[]); 
fps_word_struct read_md(unsigned int addr);
void fps_prt_stat(unsigned int stat);
void host_dma_out(unsigned int data,unsigned int data_length, unsigned int results_addr,int fmt_flag);
void host_dma_in(unsigned int data, unsigned int data_length, unsigned int results_addr,int fmt_flag); 
void fps_prt_reg(unsigned int reg);


#define cmd_wtsr 0021031  /* write switch register */
#define cmd_wtfn 0022031  /* write function register (act on switch reg) */
#define cmd_rdfn 0022030  /* read function/status register */
#define cmd_rdlt 0023030  /* read light/data register */

#define fn_load_ma   0001002
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

#define fn_examine_regmd    0002015 
#define fn_examine_regmd_o1 0002035
#define fn_examine_regmd_o2 0002055
#define fn_examine_regmd_o3 0002075
#define fn_examine_regpsa   0002000
#define fn_examine_regtma   0002003

#define cmd_wr  0000001
#define cmd_rsu 0100000
#define cmd_pio_apdma 0000030
#define cmd_pio_hmal  0001030
#define cmd_pio_hmah  0002030
#define cmd_pio_wc    0005030
#define cmd_pio_ctl   0024030
#define cmd_adp_hmal  0020010
#define cmd_adp_wc2   0020020
#define cmd_adp_wc5   0020050
#define cmd_adp_ctl   0020060

#define ctl_intrq_ap 0040000
#define ctl_iapwc    0020000
#define ctl_ihalt    0010000
#define ctl_ihwc     0004000
#define ctl_ihenb    0002000
#define ctl_cc       0000200
#define ctl_apdma    0000100
#define ctl_wrthost  0000040
#define ctl_decapma  0000020
#define ctl_fmt_0    0000000
#define ctl_fmt_1    0000002
#define ctl_fmt_2    0000004
#define ctl_fmt_3    0000006
#define ctl_hdma     0000001


#endif
