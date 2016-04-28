KERN_MODS=
APPS=

ifeq ($(WHITELIST),y)
ifeq ($(ATMEL_ISI),y)
KERN_MODS += atmel-isi2
endif

else
KERN_MODS = memtest atmel-isi2
endif

all: $(KERN_MODS)

install_headers:
	echo installed

.FORCE:
ifeq ($(KERNELRELEASE),)
include $(PWD)/PolysatKern.mk
endif
DRIVER = $(KERN_MODS:%.c=%.ko)

ifneq ($(KERNELRELEASE),)
	obj-m := ${DRIVER:%=%.o}
else
	PWD := $(shell pwd)

modules: $(KERN_MODS)

modules_install: $(KERN_MODS)
ifeq ($(strip $(KERNELDIR)),)
	$(error "KERNELDIR is undefined!")
else
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install
endif

%.ko: %.c
$(KERN_MODS):
ifeq ($(strip $(KERNELDIR)),)
	$(error "KERNELDIR is undefined!")
else
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules 
endif

clean:
	rm -rf *~ *.ko *.o *.mod.c modules.order Module.symvers .tmp_versions $(APPS)

endif

.PHONY: modules modules_install
