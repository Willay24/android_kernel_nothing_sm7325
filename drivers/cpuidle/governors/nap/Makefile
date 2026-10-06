# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the NAP cpuidle governor
#
 
obj-$(CONFIG_CPU_IDLE_GOV_NAP) += cpuidle_gov_nap.o
 
cpuidle_gov_nap-y := nap.o nap_fpu.o nap_nn_neon.o
 
# Kernel builds with -mgeneral-regs-only on ARM64, which stops the compiler
# from using the FP/SIMD registers.  FPU/NEON-using files need it removed.
#
# CRITICAL: nap.o is intentionally compiled with NORMAL kernel flags
# (-mgeneral-regs-only).  All floating-point code lives in nap_fpu.o and
# nap_nn_neon.o.  This ensures the compiler cannot emit FP instructions
# in governor callbacks (nap_select, nap_reflect, etc.), which would
# silently corrupt userspace FPSIMD register state.
#
# Do NOT add CFLAGS_REMOVE/CFLAGS for nap.o — it must stay FPU-free.
FPU_KILL_FLAGS := -mgeneral-regs-only
 
CFLAGS_REMOVE_nap_fpu.o        += $(FPU_KILL_FLAGS)
CFLAGS_REMOVE_nap_nn_neon.o    += $(FPU_KILL_FLAGS)