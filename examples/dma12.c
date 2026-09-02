#include <eclipse_io.h>
#include <stdio.h>
#include "fps.h"


//#define load_addr    0000200
#define results_addr 0000000


const unsigned int  data[]  = { 
  0100120, 0000000,
  0100040, 0000000,
  0100050, 0100000,
  0100020, 0100000,
  0100050, 0100000,
  0100020, 0100000,
  0100050, 0100000,
  0100020, 0100000,
};



#define FPU_MD_DMA_LEN (sizeof(data) / sizeof(data[0]))
const unsigned int return_data [FPU_MD_DMA_LEN] ={};

#define SETTLE_ITERS 2048
static void settle_delay(void) {
    volatile unsigned int i;
    for (i = 0; i < SETTLE_ITERS; i++) {
    }
}

int main(void) {
    
    IO_PULSE_CLEAR(077); /* NIOC 077 -- INTDS, disable interrupts */

    IO_PULSE_PULSE(FPU_DEV); /* NIOP 054 -- reset the FPU */
    IO_PULSE_START(FPU_DEV); /* NIOS 054 -- set the FPU to busy */
    IO_PULSE_START(FPU_AP1); /* NIOS 055 -- set the FPU_DMA to busy */

    host_dma_out((unsigned int)data,FPU_MD_DMA_LEN,00);
    host_dma_in((unsigned int)return_data,FPU_MD_DMA_LEN,00);
    
    for (int i = 0; i < FPU_MD_DMA_LEN; i++) {
        printf("%o, ", return_data[i]);
    }
    printf("\n");

 for(int i=0;i<FPU_MD_DMA_LEN;i++)
    {
    fpu_out(cmd_wtsr, i+results_addr);
    fpu_out(cmd_wtfn, fn_load_ma);
    printf("md[%o]: ",i+results_addr);
    fpu_out(cmd_wtfn, fn_examine_regmd_o1);
    settle_delay();
    printf("exp: %o ",fpu_in(cmd_rdlt));
    fpu_out(cmd_wtfn, fn_examine_regmd_o2);
    settle_delay();
    printf("mh: %o ",fpu_in(cmd_rdlt));
    fpu_out(cmd_wtfn, fn_examine_regmd_o3);
    settle_delay();
    printf("ml: %o\n",fpu_in(cmd_rdlt));
    }


}

