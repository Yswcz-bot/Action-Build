# SPDX-License-Identifier: GPL-2.0
# Build against OnePlus GKI kernel tree with:
#   make O=<kernel>/out ARCH=arm64 LLVM=1 LLVM_IAS=1 M=$(CURDIR) modules
obj-m += splash_probe.o

ccflags-y += -Wall -Wno-unused-variable -Wno-unused-function

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
