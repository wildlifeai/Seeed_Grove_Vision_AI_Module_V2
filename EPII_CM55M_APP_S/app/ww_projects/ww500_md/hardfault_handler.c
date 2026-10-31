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
#include "hx_drv_watchdog.h"

#include "FreeRTOS.h"
#include "task.h"

/*
 * What the fault handlers do (13 Oct 2026):
 *
 * Every handler here used to print a message and then loop for ever, which left the AI processor dead until
 * RESET or a power cycle. Now each one starts the watchdog, prints a report, and resets the processor.
 * The reset is followed by a cold boot.
 *
 * The reset is a system reset request (SCB->AIRCR SYSRESETREQ), made straight after the report.
 * This does work from a fault handler: seen on the bench on 13 Oct 2026, 0.5s after the report.
 * (A comment in image_task.c says NVIC_SystemReset() does not work. That was tried from a task.)
 *
 * The watchdog is the backup, in case printing the report faults again or the reset request fails.
 * A system reset request does not stop the watchdog, so watchdog_init() stops it at the start of the next boot.
 *
 * A failed configASSERT() is handled in the same way - see vAssertCalled().
 *
 * The 'hardfault' console command forces a hard fault to test this, and 'assert' a failed assert.
 * See _Documentation/development reports/2026-10-12_cameraErrors
 */

// The watchdog is started before the report is printed, in case printing the report faults again.
// This is long enough for the report (about 20ms at 921600 baud).
// The hardware resets at the second timeout, so the backup reset comes at twice this time.
#define FAULT_RESET_DELAY_MS	1000

/**
 * Starts the watchdog so that the processor is reset FAULT_RESET_DELAY_MS from now
 */
static void faultResetStart(void) {
	WATCHDOG_CFG_T wdg_cfg;

	wdg_cfg.period = FAULT_RESET_DELAY_MS;
	wdg_cfg.ctrl = WATCHDOG_CTRL_CPU;
	wdg_cfg.state = WATCHDOG_STATE_DC;
	wdg_cfg.type = WATCHDOG_RESET;
	hx_drv_watchdog_start(WATCHDOG_ID_0, &wdg_cfg, NULL);
}

/**
 * Prints the fault status registers
 */
static void faultPrintStatus(void) {
	uint32_t cfsr = SCB->CFSR;

	xprintf("SCB->CFSR:0x%08x\n", (int) cfsr);
	xprintf("SCB->HFSR:0x%08x\n", (int) SCB->HFSR);
	// These two hold an address only when their valid bit is set
	xprintf("SCB->BFAR:0x%08x%s\n", (int) SCB->BFAR, (cfsr & SCB_CFSR_BFARVALID_Msk) ? "" : " (not valid)");
	xprintf("SCB->MMFAR:0x%08x%s\n", (int) SCB->MMFAR, (cfsr & SCB_CFSR_MMARVALID_Msk) ? "" : " (not valid)");

	// The usage fault bits. A usage fault that can't be taken (for example inside an interrupt handler)
	// arrives as a hard fault with HFSR bit 30 (FORCED) set.
	if (cfsr & SCB_CFSR_UNDEFINSTR_Msk) {
		xprintf("  UNDEFINSTR: Undefined instruction\n");
	}
	if (cfsr & SCB_CFSR_INVSTATE_Msk) {
		xprintf("  INVSTATE: Invalid processor state (often a jump to a bad address)\n");
	}
	if (cfsr & SCB_CFSR_INVPC_Msk) {
		xprintf("  INVPC: Invalid PC or EXC_RETURN\n");
	}
	if (cfsr & SCB_CFSR_NOCP_Msk) {
		xprintf("  NOCP: No coprocessor\n");
	}
	if (cfsr & SCB_CFSR_UNALIGNED_Msk) {
		xprintf("  UNALIGNED: Unaligned memory access\n");
	}
	if (cfsr & SCB_CFSR_DIVBYZERO_Msk) {
		xprintf("  DIVBYZERO: Divide by zero\n");
	}
}

/**
 * Prints the registers that the processor pushed onto the stack when the fault happened
 *
 * PC is the address of the instruction that faulted (or the one after it). LR is where that code was called from.
 */
static void faultPrintStack(uint32_t *stacked_regs) {
	xprintf("Stacked registers:\n");
	xprintf("R0   = 0x%08x\n", (int) stacked_regs[0]);
	xprintf("R1   = 0x%08x\n", (int) stacked_regs[1]);
	xprintf("R2   = 0x%08x\n", (int) stacked_regs[2]);
	xprintf("R3   = 0x%08x\n", (int) stacked_regs[3]);
	xprintf("R12  = 0x%08x\n", (int) stacked_regs[4]);
	xprintf("LR   = 0x%08x\n", (int) stacked_regs[5]);
	xprintf("PC   = 0x%08x\n", (int) stacked_regs[6]);
	xprintf("xPSR = 0x%08x\n", (int) stacked_regs[7]);
}

/**
 * Resets the processor. Does not return.
 */
static void faultHalt(void) {
	xprintf(">>> Reset after fault\n\n");

	// Give the last characters time to leave the UART (about 10ms: 20,000,000 of these took 0.5s on the bench)
	for (volatile uint32_t i = 0; i < 400000; i++) {
	}

	// System reset request
	SCB->AIRCR =
			(SCB->AIRCR & ~SCB_AIRCR_VECTKEY_Msk) | (0x05FAUL << SCB_AIRCR_VECTKEY_Pos) | SCB_AIRCR_SYSRESETREQ_Msk;

	// If we are still here the watchdog started by faultResetStart() resets us, 2 x FAULT_RESET_DELAY_MS after it
	// was started
	for (;;) {
	}
}

#ifdef WW_ASSERT_RESETS
/**
 * Called when a configASSERT() fails - see FreeRTOSConfig.h
 *
 * This used to disable interrupts and loop for ever. Now it says which assert failed, then resets
 * the processor in the same way as a fault. A cold boot follows.
 *
 * If a debugger is attached it stops at a breakpoint here instead, so the failure can be examined.
 *
 * The 'assert' console command forces one to test this.
 *
 * @param file the source file of the assert (name only, no path)
 * @param line its line number
 */
void vAssertCalled(const char * file, int line) {
	taskDISABLE_INTERRUPTS();

	if (DCB->DHCSR & DCB_DHCSR_C_DEBUGEN_Msk) {
		// A debugger is attached. Stop here: 'file' and 'line' say which assert failed.
		__asm volatile ("bkpt #0 \n");
	}

	faultResetStart();

	// There is no current task before the scheduler starts
	xprintf("\r\nASSERT failed: %s:%d in task '%s'\r\n", file, line,
			(xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) ? "(none)" : pcTaskGetName(NULL));

	faultHalt();
}
#endif // WW_ASSERT_RESETS

/**
 * Common code for the faults that have no decoding of their own
 *
 * @param name the name of the fault
 * @param stacked_regs the stack at the time of the fault
 */
static void faultCommon(const char * name, uint32_t *stacked_regs) {
	faultResetStart();
	xprintf("\r\nEntering %s interrupt!\r\n", name);
	faultPrintStatus();
	faultPrintStack(stacked_regs);
	faultHalt();
}

// Defines a fault handler that finds the stack in use when the fault happened (MSP or PSP)
// and passes it to a C function
#define FAULT_HANDLER(handler, c_function) \
	__attribute__((naked)) void handler(void) { \
		__asm volatile ( \
				"TST   lr, #4       \n" \
				"ITE   EQ           \n" \
				"MRSEQ r0, MSP      \n" \
				"MRSNE r0, PSP      \n" \
				"B     " #c_function " \n" \
		); \
	}

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
__attribute__((used)) void HardFault_Handler_C(uint32_t *stacked_regs) {
	faultResetStart();

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
	// Only the bus fault bits of CFSR. (This used to test the whole register, so a usage fault
	// was reported as "Another secure bus error".)
	if ((SCB->CFSR & SCB_CFSR_BUSFAULTSR_Msk) != 0) {
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
	if ((SCB_NS->CFSR & SCB_CFSR_BUSFAULTSR_Msk) != 0) {
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

	faultPrintStatus();
	faultPrintStack(stacked_regs);
	faultHalt();
}

FAULT_HANDLER(HardFault_Handler, HardFault_Handler_C)

void NMI_Handler(void) {
	faultResetStart();
	xprintf("\r\nEntering NMI_Handler interrupt!\r\n");
	faultHalt();
}

__attribute__((used)) void MemManage_Handler_C(uint32_t *stacked_regs) {
	faultCommon("MemManage_Handler", stacked_regs);
}
FAULT_HANDLER(MemManage_Handler, MemManage_Handler_C)

__attribute__((used)) void BusFault_Handler_C(uint32_t *stacked_regs) {
	faultCommon("BusFault_Handler", stacked_regs);
}
FAULT_HANDLER(BusFault_Handler, BusFault_Handler_C)

__attribute__((used)) void SecureFault_Handler_C(uint32_t *stacked_regs) {
	faultCommon("SecureFault_Handler", stacked_regs);
}
FAULT_HANDLER(SecureFault_Handler, SecureFault_Handler_C)

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

__attribute__((used)) void UsageFault_Handler_C(uint32_t *stacked_regs)
{
    faultResetStart();

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

    xprintf("\r\n");
    faultHalt();
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
