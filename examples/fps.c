#include "fps.h"
#include <eclipse_io.h>
#include <stdio.h>

void fps_run(unsigned int load_addr){
    unsigned int status;
     fpu_out(cmd_wtsr, load_addr);
    fpu_out(cmd_wtfn, fn_start);

    do {
        status = fpu_in(cmd_rdfn);
    } while ((status & fn_stop) == 0);

}

float scale_pow2(float x, int n) {
    union {
        float f;
        unsigned long u;
    } v;
    v.f = x;
    unsigned long bits = v.u;
    int exp_field = (int)((bits >> 23) & 0xFFUL);
    if (exp_field == 0) {
        return x;
    }
    int new_exp = exp_field + n;
    if (new_exp <= 0) {
        return (bits & 0x80000000UL) ? -0.0f : 0.0f;
    }

    if (new_exp >= 255) {
        new_exp = 254;
    }
    bits = (bits & 0x807FFFFFUL) | ((unsigned long)new_exp << 23);
    v.u = bits;

    return v.f;
}


fps_word_struct convert_dma_value(unsigned int hi, unsigned int low){
    fps_word_struct split_value;
    split_value.exp=hi>>6;
    split_value.mh=((hi&0o77)<<6)|(low>>10);
    split_value.ml=((low&0o1777)<<6);
return split_value;

}

float calculate_value(int exp, int mh, int ml) {
    exp = exp - 512;
    long mant_int = (((long)mh & 0xFFF) << 16) | ((long)ml & 0xFFFF);
    if (mant_int & (1L << 27)) {
        mant_int = mant_int - (1L << 28);
    }
    float mant = scale_pow2((float)mant_int, -27);
    float val = scale_pow2(mant, exp);
    return val;
}

void load_psm(unsigned int pgm_addr, unsigned int fpu_pgm_len,  const unsigned int fpu_pgm[]){
    fpu_out(cmd_wtsr, pgm_addr);
    fpu_out(cmd_wtfn, fn_load_tma);
    static const unsigned int fn_load_ps[4] = {fn_load_ps_0, fn_load_ps_1,
                                                fn_load_ps_2, fn_load_ps_3};
    /* `<`, not `<=`: fpu_pgm_len is the halfword count (e.g. 340 for an
     * 85-PS-location program), always an exact multiple of 4, so `i <=
     * fpu_pgm_len` runs one extra outer iteration and reads
     * fpu_pgm[fpu_pgm_len .. fpu_pgm_len+3] -- 4 words past the end of
     * the caller's array, and sends a spurious 5th round of load-PS
     * commands with garbage data to the FPS100 board. Triggers on every
     * call whose length is a multiple of 4, including MANDEL240's 340. */
    for (unsigned int i = 0; i < fpu_pgm_len; i += 4) {
        for (unsigned int k = 0; k < 4; k++) {
            fpu_out(cmd_wtsr, fpu_pgm[i + k]);
            fpu_out(cmd_wtfn, fn_load_ps[k]);
        }
    }

}
void load_md(unsigned int md_addr,unsigned int md_len, const unsigned int md_arr[]){
    fpu_out(cmd_wtsr, md_addr);
    fpu_out(cmd_wtfn, fn_load_ma);
    static const unsigned int fn_load_md[3] = { fn_load_md_1,
                                                fn_load_md_2, fn_load_md_3};
    for (unsigned int i = 0; i < md_len; i += 3) {
        for (unsigned int k = 0; k < 3; k++) {
            fpu_out(cmd_wtsr, md_arr[i + k]);
            fpu_out(cmd_wtfn, fn_load_md[k]);
        }
    }

}   
fps_word_struct read_md(unsigned int results_addr) {

    fps_word_struct read_internal_struct;

    fpu_out(cmd_wtsr, results_addr);
    fpu_out(cmd_wtfn, fn_load_ma);

    fpu_out(cmd_wtfn, fn_examine_regmd_o1);
    //settle_delay();
    read_internal_struct.exp = fpu_in(cmd_rdlt);

    fpu_out(cmd_wtfn, fn_examine_regmd_o2);
    //settle_delay();
    read_internal_struct.mh = fpu_in(cmd_rdlt);

    fpu_out(cmd_wtfn, fn_examine_regmd_o3);
    //settle_delay();
    read_internal_struct.ml = fpu_in(cmd_rdlt);

    return read_internal_struct;

}


void host_dma_out(unsigned int data,unsigned int data_length,unsigned int results_addr, int fmt_flag)
{
    fpu_clr();

    adp_out(cmd_adp_hmal|cmd_wr,data);
    adp_out(cmd_adp_wc2|cmd_wr,data_length);
    adp_out(cmd_adp_ctl|cmd_wr,ctl_apdma | fmt_flag | ctl_cc | ctl_hdma);

    fpu_clr();

    fpu_out(cmd_pio_hmah|cmd_wr,00);
    fpu_out(cmd_pio_hmal|cmd_wr,data);
    fpu_out(cmd_pio_wc|cmd_wr,data_length);
    fpu_out(cmd_pio_apdma|cmd_wr,results_addr);
    fpu_out(cmd_pio_ctl|cmd_wr,ctl_apdma |  fmt_flag | ctl_cc);

    fpu_clr();

    adp_out(cmd_adp_wc5|cmd_wr|cmd_rsu,data_length);
    while(fpu_sta()&01);

}

void host_dma_in(unsigned int data, unsigned int data_length,unsigned int results_addr,int fmt_flag)

{
    IO_PULSE_PULSE(FPU_DEV); /* NIOP 054 -- reset the FPU */

    fpu_clr();

    adp_out(cmd_adp_hmal|cmd_wr,data);
    adp_out(cmd_adp_wc2|cmd_wr,data_length);
    adp_out( fmt_flag |cmd_adp_ctl|cmd_wr,ctl_wrthost|ctl_apdma | ctl_cc | ctl_hdma);
    fpu_clr();

    fpu_out(cmd_pio_hmah|cmd_wr,00);
    fpu_out(cmd_pio_hmal|cmd_wr,data);
    fpu_out(cmd_pio_wc|cmd_wr,data_length);
    fpu_out(cmd_pio_apdma|cmd_wr,results_addr);
    fpu_out(cmd_pio_ctl|cmd_wr,ctl_wrthost | fmt_flag  | ctl_apdma | ctl_cc);
    fpu_clr();

    adp_out(cmd_adp_wc5|cmd_wr|cmd_rsu,data_length);
    while(fpu_sta()&01);

}


void fps_prt_stat(unsigned int stat){
    printf("stat %o : ",stat);
    fps_stat st;
    st.status=stat;
    printf("rdy: %d ",st.ready);
    printf("msk: %d ",st.mask);
    printf("susErr: %d ",st.suspend_err);
    printf("cmdErr: %d ",st.command_err);
    printf("rdack: %d ",st.rdack);
    printf("diag: %o ",st.diag_func);
    printf("susDMA: %d ",st.suspend_dma);
    printf("hstDMA: %d ",st.host_dma);
    printf("io: %d ",st.io_bus);
    printf("DMA: %d\n",st.dma_active);
}

void fps_prt_reg(unsigned int reg){
    printf("stat %o : ",reg);
    fps_reg rg;
    rg.reg=reg;
    printf("REGSELUSED: %o ",rg.REGSELUSED);
    printf("REGSEL: %o ",rg.REGSEL);
    printf("RDEC: %o ",rg.RDEC);
    printf("RW: %d\n",rg.RW);
}