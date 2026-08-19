#include <eclipse_io.h>
#include <stdio.h>
#include "fps.h"


//#define load_addr    0000200
#define results_addr 0000100

#define FPU_AP1 055

const unsigned int  data[]  = { 
  0100120, 0000000,
  0100040, 0000000,
  0100050, 0100000,
  0100020, 0100000
};
#define FPU_MD_DMA_LEN (sizeof(data) / sizeof(data[0]))

#define cmd_wr  0000001
#define cmd_rsu 0100000
#define cmd_dma_hmah  0002030
#define cmd_dma_hmal  0001010
#define cmd_dma_wc    0005050
#define cmd_dma_apdma 0000030
#define cmd_dma_ctl   0024060

void test_hmah(void)
{
    printf("\n***Test HMAH PIO***\n");
    fpu_out(cmd_dma_hmah|cmd_wr,0052525);
    printf("hamh: %o\n",fpu_in(cmd_dma_hmah));
    fpu_out(cmd_dma_hmah|cmd_wr,0125252);
    printf("hamh: %o\n",fpu_in(cmd_dma_hmah));
    fpu_out(cmd_dma_hmah|cmd_wr,00);
    printf("hamh: %o\n",fpu_in(cmd_dma_hmah));
}


void test_hmal(void)
{
    printf("\n***Test HMAL***\n");
    fpu_out(cmd_dma_hmal|cmd_wr,0052525);
    printf("haml_pio: %o\n",fpu_in(0001030));
    printf("haml: %o\n",fpu_in(cmd_dma_hmal));
    fpu_out(cmd_dma_hmal|cmd_wr,0125252);
    printf("haml_pio: %o\n",fpu_in(0001030));
    printf("haml: %o\n",fpu_in(cmd_dma_hmal));
    fpu_out(cmd_dma_hmal|cmd_wr,0000000);
    printf("haml_pio: %o\n",fpu_in(0001030));
    printf("haml: %o\n",fpu_in(cmd_dma_hmal));
    printf("*Test HMAL PIO*\n");
    fpu_out(0001030|cmd_wr,0052525);
    printf("haml_pio: %o\n",fpu_in(0001030));
    fpu_out(0001030|cmd_wr,0125252);
    printf("haml_pio: %o\n",fpu_in(0001030));
    fpu_out(0001030|cmd_wr,0000000);
    printf("haml_pio: %o\n",fpu_in(0001030));
}

void test_wc(void)
{
    printf("\n***Test WC***\n");
    fpu_out(cmd_dma_wc|cmd_wr,0052525);
    printf("wc_pio: %o\n",fpu_in(0005030));
    printf("wc: %o\n",fpu_in(cmd_dma_wc));
    fpu_out(cmd_dma_wc|cmd_wr,0125252);
    printf("wc_pio: %o\n",fpu_in(0005030));
    printf("wc: %o\n",fpu_in(cmd_dma_wc));
    fpu_out(cmd_dma_wc|cmd_wr,0000000);
    printf("wc_pio: %o\n",fpu_in(0005030));
    printf("wc: %o\n",fpu_in(cmd_dma_wc));
    printf("*Test WC PIO*\n");
    fpu_out(0005030|cmd_wr,0052525);
    printf("wc_pio: %o\n",fpu_in(0005030));
    fpu_out(0005030|cmd_wr,0125252);
    printf("wc_pio: %o\n",fpu_in(0005030));
    fpu_out(0005030|cmd_wr,0000000);
    printf("wc_pio: %o\n",fpu_in(0005030));
}

void test_apdma(void)
{
    printf("\n*Test APDMA PIO*\n");
    fpu_out(cmd_dma_apdma|cmd_wr,0052525);
    printf("apdma_pio: %o\n",fpu_in(cmd_dma_apdma));
    fpu_out(cmd_dma_apdma|cmd_wr,0125252);
    printf("apdma_pio: %o\n",fpu_in(cmd_dma_apdma));
    fpu_out(cmd_dma_apdma|cmd_wr,0000000);
    printf("apdma_pio: %o\n",fpu_in(cmd_dma_apdma));
}

void test_ctl(void)
{
    printf("\n***Test CTL***\n");
    printf("*avoiding bit 15 as HDMA EN*\n");
    fpu_out(cmd_dma_ctl|cmd_wr,0052524);
    printf("wc_pio: %o\n",fpu_in(0024030));
    printf("wc: %o\n",fpu_in(cmd_dma_ctl));
    fpu_out(cmd_dma_ctl|cmd_wr,0125252);
    printf("wc_pio: %o\n",fpu_in(0024030));
    printf("wc: %o\n",fpu_in(cmd_dma_ctl));
    fpu_out(cmd_dma_ctl|cmd_wr,0000000);
    printf("wc_pio: %o\n",fpu_in(0024030));
    printf("wc: %o\n",fpu_in(cmd_dma_ctl));
    printf("*Test WC PIO*\n");
    fpu_out(0024030|cmd_wr,0052525);
    printf("wc_pio: %o\n",fpu_in(0024030));
    fpu_out(0024030|cmd_wr,0125252);
    printf("wc_pio: %o\n",fpu_in(0024030));
    fpu_out(0024030|cmd_wr,0000000);
    printf("wc_pio: %o\n",fpu_in(0024030));
}

int main(void) {
    
    IO_PULSE_CLEAR(077); /* NIOC 077 -- INTDS, disable interrupts */

    IO_PULSE_PULSE(FPU_DEV); /* NIOP 054 -- reset the FPU */
    IO_PULSE_START(FPU_DEV); /* NIOS 054 -- set the FPU to busy */
    IO_PULSE_START(FPU_AP1); /* NIOS 055 -- set the FPU_DMA to busy */

    test_hmah();
    test_hmal();
    test_wc();
    test_apdma();
    test_ctl();
    
    return 0;

}

