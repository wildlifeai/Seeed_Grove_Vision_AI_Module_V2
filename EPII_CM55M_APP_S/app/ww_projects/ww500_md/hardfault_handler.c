/*
 Copyright (c) 2020 Arm Limited (or its affiliates). All rights reserved.
 Use, modification and redistribution of this file is subject to your possession of a
 valid End User License Agreement for the Arm Product of which these examples are part of
 and your compliance with all applicable terms and conditions of such licence agreement.
 */

#include <stdio.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "WE2_device.h"
#include "xprintf.h"

#if 0
/* HardFault handler implementation that prints a message
   then exits the program early.
 */
void HardFault_Handler(void)
{
#if 0
    xprintf("HardFault occurred!\n");
#endif
    for(;;) {}
}
#endif
void HardFault_Handler(void) {
	/* Handling SAU related secure faults */
	xprintf("\r\nEntering HardFault interrupt!\r\n");
	if (SAU->SFSR != 0) {
		if (SAU->SFSR & SAU_SFSR_INVEP_Msk) {
			/* Invalid Secure state entry point */
			xprintf(
					"SAU->SFSR:INVEP fault: Invalid entry point to secure world.\r\n");
		} else if (SAU->SFSR & SAU_SFSR_AUVIOL_Msk) {
			/* AUVIOL: SAU violation  */
			xprintf(
					"SAU->SFSR:AUVIOL fault: SAU violation. Access to secure memory from normal world.\r\n");
		} else if (SAU->SFSR & SAU_SFSR_INVTRAN_Msk) {
			/* INVTRAN: Invalid transition from secure to normal world  */
			xprintf(
					"SAU->SFSR:INVTRAN fault: Invalid transition from secure to normal world.\r\n");
		} else {
			xprintf("Another SAU error.\r\n");
		}
		if (SAU->SFSR & SAU_SFSR_SFARVALID_Msk) {
			/* SFARVALID: SFAR contain valid address that caused secure violation */
			xprintf("Address that caused SAU violation is 0x%X.\r\n", (int) SAU->SFAR);
		}
	}

	/* Handling secure bus related faults */
	if (SCB->CFSR != 0) {
		if (SCB->CFSR & SCB_CFSR_IBUSERR_Msk) {
			/* IBUSERR: Instruction bus error on an instruction prefetch */
			xprintf(
					"SCB->BFSR:IBUSERR fault: Instruction bus error on an instruction prefetch.\r\n");
		} else if (SCB->CFSR & SCB_CFSR_PRECISERR_Msk) {
			/* PRECISERR: Instruction bus error on an instruction prefetch */
			xprintf("SCB->BFSR:PRECISERR fault: Precise data access error.\r\n");
		} else {
			xprintf("Security Another secure bus error 1.\r\n");
		}
		if (SCB->CFSR & SCB_CFSR_BFARVALID_Msk) {
			/* BFARVALID: BFAR contain valid address that caused secure violation */
			xprintf("Address that caused secure bus violation is 0x%X.\r\n",
					(int) SCB->BFAR);
		}
	}

	/* Handling non-secure bus related faults */
	if (SCB_NS->CFSR != 0) {
		if (SCB_NS->CFSR & SCB_CFSR_IBUSERR_Msk) {
			/* IBUSERR: Instruction bus error on an instruction prefetch */
			xprintf(
					"SCB_NS->BFSR:IBUSERR fault: Instruction bus error on an instruction prefetch.\r\n");
		} else if (SCB_NS->CFSR & SCB_CFSR_PRECISERR_Msk) {
			/* PRECISERR: Data bus error on an data read/write */
			xprintf(
					"SCB_NS->BFSR:PRECISERR fault: Precise data access error.\r\n");
		} else {
			xprintf("Security Another secure bus error 2.\r\n");
		}
		if (SCB_NS->CFSR & SCB_CFSR_BFARVALID_Msk) {
			/* BFARVALID: BFAR contain valid address that caused secure violation */
			xprintf("Address that caused secure bus violation is 0x%X.\r\n",
					(int) SCB_NS->BFAR);
		}
	}

#if 0
    /* Perform system RESET */
    SCB->AIRCR =
        (SCB->AIRCR & ~SCB_AIRCR_VECTKEY_Msk) | (0x05FAUL << SCB_AIRCR_VECTKEY_Pos) | SCB_AIRCR_SYSRESETREQ_Msk;
#else
	xprintf("SCB->CFSR:0x%08x\n", (int) SCB->CFSR);
	xprintf("SCB->BFAR:0x%08x\n", (int) SCB->BFAR);
	xprintf("SCB->HFSR:0x%08x\n", (int) SCB->HFSR);
	for (;;) {
	}
#endif
}

void NMI_Handler(void) {
	xprintf("\r\nEntering NMI_Handler interrupt!\r\n");
	for (;;) {
	}
}

void MemManage_Handler(void) {
	xprintf("\r\nEntering MemManage_Handler interrupt!\r\n");
	for (;;) {
	}
}
void BusFault_Handler(void) {
	xprintf("\r\nEntering BusFault_Handler interrupt!\r\n");
	xprintf("SCB->CFSR:0x%08x\n", (int) SCB->CFSR);
	xprintf("SCB->BFAR:0x%08x\n", (int) SCB->BFAR);
	xprintf("SCB->HFSR:0x%08x\n", (int) SCB->HFSR);
	for (;;) {
	}
}
void SecureFault_Handler(void) {
	xprintf("\r\nEntering SecureFault_Handler interrupt!\r\n");
	for (;;) {
	}
}

#if 0
void UsageFault_Handler(void) {
	xprintf("\r\nEntering UsageFault_Handler interrupt!\r\n");
	for (;;) {
	}
}
#else
// CGP - at the suggestion of ChatGPT after receiving this error
// just after this line:
// FatFS Task received event 'Save State' (0x0904). Rx data = 0x00000000
// https://chatgpt.com/share/6a8ce031-293c-83ec-a335-41a849572b5e

void UsageFault_Handler_C(uint32_t *stacked_regs)
{
    uint32_t cfsr = SCB->CFSR;
    uint32_t hfsr = SCB->HFSR;
    uint32_t shcsr = SCB->SHCSR;

    xprintf("\r\n*** USAGE FAULT ***\r\n");
    xprintf("CFSR  = 0x%08lX\r\n", cfsr);
    xprintf("HFSR  = 0x%08lX\r\n", hfsr);
    xprintf("SHCSR = 0x%08lX\r\n", shcsr);
    xprintf("UFSR  = 0x%04lX\r\n", (cfsr >> 16) & 0xFFFFUL);

    if (cfsr & SCB_CFSR_UNDEFINSTR_Msk)
        xprintf("  UNDEFINSTR: Undefined instruction\r\n");

    if (cfsr & SCB_CFSR_INVSTATE_Msk)
        xprintf("  INVSTATE: Invalid processor state\r\n");

    if (cfsr & SCB_CFSR_INVPC_Msk)
        xprintf("  INVPC: Invalid PC or EXC_RETURN\r\n");

    if (cfsr & SCB_CFSR_NOCP_Msk)
        xprintf("  NOCP: No coprocessor\r\n");

    if (cfsr & SCB_CFSR_UNALIGNED_Msk)
        xprintf("  UNALIGNED: Unaligned memory access\r\n");

    if (cfsr & SCB_CFSR_DIVBYZERO_Msk)
        xprintf("  DIVBYZERO: Divide by zero\r\n");

    xprintf("\r\nStacked registers:\r\n");
    xprintf("R0   = 0x%08lX\r\n", stacked_regs[0]);
    xprintf("R1   = 0x%08lX\r\n", stacked_regs[1]);
    xprintf("R2   = 0x%08lX\r\n", stacked_regs[2]);
    xprintf("R3   = 0x%08lX\r\n", stacked_regs[3]);
    xprintf("R12  = 0x%08lX\r\n", stacked_regs[4]);
    xprintf("LR   = 0x%08lX\r\n", stacked_regs[5]);
    xprintf("PC   = 0x%08lX\r\n", stacked_regs[6]);
    xprintf("xPSR = 0x%08lX\r\n", stacked_regs[7]);

    xprintf("\r\nCPU halted.\r\n");

    for (;;)
    {
    }
}


__attribute__((naked))
void UsageFault_Handler(void)
{
    __asm volatile (
        "TST   lr, #4       \n"
        "ITE   EQ           \n"
        "MRSEQ r0, MSP      \n"
        "MRSNE r0, PSP      \n"
        "B     UsageFault_Handler_C \n"
    );
}

#endif
