
all: kernel_mod mkfs.dfs 

mkfs.dfs: mkfs.dfs.c dfs.h
	gcc -Wall -o $@ $<

#view.dfs: view.dfs.c dfs.h
#	gcc -Wall -o $@ $<

ifneq ($(KERNELRELEASE),)
# kbuild part of makefile, for backwards compatibility
include Kbuild

else
# normal makefile
KDIR ?= /usr/src/linux-headers-`uname -r`

kernel_mod:
	$(MAKE) -C $(KDIR) M=$$PWD

endif
