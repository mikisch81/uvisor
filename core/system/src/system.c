/*
 * Copyright (c) 2013-2016, ARM Limited, All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <uvisor.h>

/* All non-hookable system IRQs are by default weakly linked to the system
 * default handler. */
void UVISOR_ALIAS(isr_default_sys_handler) NonMaskableInt_IRQn_Handler(void);
void UVISOR_ALIAS(isr_default_sys_handler) HardFault_IRQn_Handler(void);
void UVISOR_ALIAS(isr_default_sys_handler) MemoryManagement_IRQn_Handler(void);
void UVISOR_ALIAS(isr_default_sys_handler) BusFault_IRQn_Handler(void);
void UVISOR_ALIAS(isr_default_sys_handler) UsageFault_IRQn_Handler(void);
void UVISOR_ALIAS(isr_default_sys_handler) SecureFault_IRQn_Handler(void);
void UVISOR_ALIAS(isr_default_sys_handler) SVCall_IRQn_Handler(void);
void UVISOR_ALIAS(isr_default_sys_handler) DebugMonitor_IRQn_Handler(void);

/* Privileged PendSV and SysTick hooks assume that they are called directly by
 * hardware. So, these handlers need their stack frame and registers to look
 * exactly like they were called directly by hardware. This means LR needs to
 * be EXC_RETURN, not some value placed there by the C compiler when it does a
 * branch with link. */
void UVISOR_NAKED PendSV_IRQn_Handler(void)
{
    asm volatile(
        "ldr  r0, %[priv_pendsv]\n"  /* Load the hook from the hook table. */
        "bx   r0\n"                  /* Branch to the hook (without link). */
        :: [priv_pendsv] "m" (g_priv_sys_hooks.priv_pendsv)
    );
}

void UVISOR_NAKED SysTick_IRQn_Handler(void)
{
/* On v8-M, we don't want to call the priv_systick hook, but our own scheduler
 * instead. */
#if defined (__ARM_FEATURE_CMSE) && (__ARM_FEATURE_CMSE == 3U)
    /* When switching from NS to S via secure exception or IRQ, the NS
     * registers are not stacked. Secure side has to read all this state so
     * that it can restore it when resuming that box. We push all this state
     * onto the stack and read it from C as a struct. When restoring state, we
     * write the register values to the stack from C and then pop the
     * registers. */
    asm volatile(
        "tst lr, #0x40\n"     /* Is source frame stacked on the secure side? */
        "it eq\n"
        "subeq sp, #0x20\n"   /* No, allocate a secure stack frame. */
        "push {r4-r11, lr}\n" /* Save registers not in exception frame. */
        "mov r0, sp\n"
        "bl scheduler_tick\n"
        "pop {r4-r11, lr}\n"  /* Restore registers not in exception frame. */
        "tst lr, #0x40\n"     /* Is dest frame stacked on the secure side? */
        "it eq\n"
        "addeq sp, #0x20\n"   /* No, deallocate the secure stack frame. */
        "bx lr\n"
    );
#else
    asm volatile(
        "ldr  r0, %[priv_systick]\n" /* Load the hook from the hook table. */
        "bx   r0\n"                  /* Branch to the hook (without link). */
        :: [priv_systick] "m" (g_priv_sys_hooks.priv_systick)
    );
#endif
}

/* Default vector table (placed in Flash) */
__attribute__((section(".isr"))) const TIsrVector g_isr_vector[ISR_VECTORS] =
{
    /* Initial stack pointer */
    (TIsrVector) &__uvisor_stack_top__,

    /* System IRQs */
    (TIsrVector) &main_entry,              /* -15 */
    NonMaskableInt_IRQn_Handler,           /* -14 */
    HardFault_IRQn_Handler,                /* -13 */
    MemoryManagement_IRQn_Handler,         /* -12 */
    BusFault_IRQn_Handler,                 /* -11 */
    UsageFault_IRQn_Handler,               /* -10 */
#if defined(ARCH_CORE_ARMv8M)
    SecureFault_IRQn_Handler,              /* - 9 */
#else /* defined(ARCH_CORE_ARMv8M) */
    isr_default_sys_handler,               /* - 9 */
#endif /* defined(ARCH_CORE_ARMv8M) */
    isr_default_sys_handler,               /* - 8 */
    isr_default_sys_handler,               /* - 7 */
    isr_default_sys_handler,               /* - 6 */
    SVCall_IRQn_Handler,                   /* - 5 */
    DebugMonitor_IRQn_Handler,             /* - 4 */
    isr_default_sys_handler,               /* - 3 */
    PendSV_IRQn_Handler,                   /* - 2 */
    SysTick_IRQn_Handler,                  /* - 1 */

    /* NVIC IRQs */
    /* Note: This is a GCC extension. */
    [NVIC_OFFSET ... (ISR_VECTORS - 1)] = isr_default_handler
};

/* Default privileged system hooks (placed in SRAM) */
UvisorPrivSystemHooks g_priv_sys_hooks = {
    .priv_svc_0 = __svc_not_implemented,
    .priv_pendsv = isr_default_sys_handler,
    .priv_systick = isr_default_sys_handler,
};

void UVISOR_NAKED UVISOR_NORETURN isr_default_sys_handler(void)
{
    /* Handle system IRQ with an unprivileged handler. */
    /* Note: The original lr and the MSP are passed to the actual handler */
    asm volatile(
        "mov r0, lr\n"
        "mrs r1, MSP\n"
        "bl vmpu_sys_mux_handler\n"
        "bx r0\n"
    );
}

void UVISOR_NAKED UVISOR_NORETURN isr_default_handler(void)
{
    /* Handle NVIC IRQ with an unprivileged handler.
     * Serving an IRQn in unprivileged mode is achieved by mean of two SVCalls:
     * The first one de-previliges execution, the second one re-privileges it. */
    /* Note: NONBASETHRDENA (in SCB) must be set to 1 for this to work. */
    asm volatile(
        "svc  %[unvic_in]\n"
        "svc  %[unvic_out]\n"
        "bx   lr\n"
        ::[unvic_in]  "i" ((UVISOR_SVC_ID_UNVIC_IN) & 0xFF),
          [unvic_out] "i" ((UVISOR_SVC_ID_UNVIC_OUT) & 0xFF)
    );
}
