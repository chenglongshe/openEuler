# SPDX-License-Identifier: GPL-2.0
# Some of the tools (perf) use same make variables
# as in kernel build.
export srctree=
export objtree=

include scripts/Makefile.include

help:
	@echo 'Possible targets:'
	@echo ''
	@echo '  bpf                    - misc BPF tools'
	@echo ''
	@echo 'You can do:'
	@echo ' $$ make -C tools/ bpf_install'
	@echo ''
	@echo '  from the kernel command line to build and install bpf tools'
	@echo ''
	@echo '  $$ make tools/all'
	@echo ''
	@echo '  builds all tools.'
	@echo ''
	@echo '  $$ make tools/install'
	@echo ''
	@echo '  installs all tools.'
	@echo ''
	@echo 'Cleaning targets:'
	@echo ''
	@echo '  bpf_clean              - clean bpf build directory'
	@echo '  clean                  - clean bpf and related library build directories'

bpf: FORCE
	$(call descend,$@)

bpf/%: FORCE
	$(call descend,$@)

all: bpf

bpf_install:
	$(call descend,$(@:_install=),install)

install: bpf_install

bpf_clean:
	$(call descend,$(@:_clean=),clean)

libbpf_clean:
	$(call descend,lib/bpf,clean)

libsubcmd_clean:
	$(call descend,lib/subcmd,clean)

build_clean:
	$(call descend,build,clean)

clean: bpf_clean build_clean libbpf_clean libsubcmd_clean

.PHONY: FORCE
