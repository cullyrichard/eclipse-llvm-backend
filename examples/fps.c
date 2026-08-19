#include "fps.h"



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
    for (unsigned int i = 0; i <= fpu_pgm_len; i += 4) {
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
