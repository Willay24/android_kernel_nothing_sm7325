# SPDX-License-Identifier: GPL-2.0
GCOV_PROFILE		:= n
PGO_PROFILE		:= n
KCOV_INSTRUMENT		:= n
KASAN_SANITIZE		:= n
UBSAN_SANITIZE		:= n

obj-y	+= fs.o instrument.o

# The raw profile format is private to LLVM: llvm-profdata only accepts the
# exact version (and struct layout) its own LLVM release produces. Ask the
# compiler what it emits for a trivial function and refuse to build if it
# does not match what pgo.h implements, instead of silently producing a
# profile that llvm-profdata will reject later.
#
#   Override with: make PGO_SKIP_CHECK=1 ...
ifeq ($(CONFIG_PGO_CLANG),y)
ifeq ($(PGO_SKIP_CHECK),)

pgo-want-version := 10
pgo-want-layout  := { i64, i64, i64, i64, ptr, ptr, i32, [3 x i16], i32 }

pgo-probe := $(shell ir=$$(echo 'int pgo_probe(int x) { return x; }' | \
		$(CC) -x c - -O0 -fprofile-generate -S -emit-llvm -o - 2>/dev/null); \
	v=$$(echo "$$ir" | sed -n 's/^@__llvm_profile_raw_version = .*constant i64 \([0-9][0-9]*\).*/\1/p'); \
	l=$$(echo "$$ir" | grep -c '^@__profd_pgo_probe = .*{ i64, i64, i64, i64, ptr, ptr, i32, \[3 x i16\], i32 }'); \
	echo "$$(( $${v:-0} & 4294967295 )) $$l")

pgo-have-version := $(word 1,$(pgo-probe))
pgo-layout-ok    := $(word 2,$(pgo-probe))

ifeq ($(pgo-have-version),0)
$(error PGO: could not query the raw profile version from '$(CC)'. \
	Is it a recent clang? Use PGO_SKIP_CHECK=1 to bypass this check)
endif
ifneq ($(pgo-have-version),$(pgo-want-version))
$(error PGO: this compiler emits raw profile version $(pgo-have-version), \
	kernel/pgo implements version $(pgo-want-version). kernel/pgo/pgo.h and \
	fs.c must be updated from the compiler's InstrProfData.inc. \
	Use PGO_SKIP_CHECK=1 to bypass this check)
endif
ifneq ($(pgo-layout-ok),1)
$(error PGO: raw profile version matches but the per-function data struct \
	differs from $(pgo-want-layout). Update struct llvm_prf_data in \
	kernel/pgo/pgo.h. Use PGO_SKIP_CHECK=1 to bypass this check)
endif

endif
endif
