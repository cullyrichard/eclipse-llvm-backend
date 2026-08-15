#include "fps.h"

static float scale_pow2(float x, int n) {
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

int load_psm(unsigned int pgm_addr,int fpu_pgm_len, const unsigned int fpu_pgm[]){
    fpu_out(cmd_wtsr, pgm_addr);
    fpu_out(cmd_wtfn, FN_DEP | FN_REG_TMA);
    static const unsigned int fn_load_ps[4] = {fn_load_ps_0, fn_load_ps_1,
                                                fn_load_ps_2, fn_load_ps_3};
    for (unsigned int i = 0; i < fpu_pgm_len; i += 4) {
        for (unsigned int k = 0; k < 4; k++) {
            fpu_out(cmd_wtsr, fpu_pgm[i + k]);
            fpu_out(cmd_wtfn, fn_load_ps[k]);
        }
    }
    return 0; 
}

int load_md(unsigned int md_addr, int fpu_md_len, const unsigned int fpu_md[]){
    static const unsigned int fn_load_md[3] = {fn_load_md_1, fn_load_md_2, fn_load_md_3}; 
	fpu_out(cmd_wtsr, md_addr); 
	fpu_out(cmd_wtfn, FN_DEP | FN_REG_MA); 
    for (unsigned int md_location = 0; md_location < fpu_md_len;md_location++){
        for(unsigned int md_word_segment_iterator = 0; md_word_segment_iterator < 3; md_word_segment_iterator++){
            fpu_out(cmd_wtsr, fpu_md[md_location+md_word_segment_iterator]); 
            fpu_out(cmd_wtfn, fn_load_md[md_word_segment_iterator]);
        }
    }
    return 0;
}

int fps_dma_host_out(const unsigned int * hma,unsigned int apdma,unsigned int wc)
{
    fpu_out(cmd_wthmah,0);
    fpu_out(cmd_wthmal,*hma);
    fpu_out(cmd_wtwc,wc);
    fpu_out(cmd_wtapdma,apdma);
    fpu_out(cmd_wtctl,ctl_apdma|ctl_cc);
    fpu_out(cmd_wtctl_rl,ctl_apdma|ctl_cc|ctl_hdma);
    return 0;
}

fps_word_struct read_md(int addr){ 
    fps_word_struct fps_out; 

    fpu_out(cmd_wtsr, addr); //set up the address to read from in MD_REG
    fpu_out(cmd_wtfn, FN_DEP | FN_REG_MA); //Send the Addr to the FPS to load from MA
    fpu_out(cmd_wtfn, fn_examine_regmd_o1); //provide the upper 16 bits of REGMD to the interface
    fps_out.exp = fpu_in(cmd_rdlt); // bits appear in the ligts reg
    fpu_out(cmd_wtfn, fn_examine_regmd_o2);
    fps_out.mh = fpu_in(cmd_rdlt);
    fpu_out(cmd_wtfn, fn_examine_regmd_o3);
    fps_out.ml = fpu_in(cmd_rdlt);

    return fps_out;
}


