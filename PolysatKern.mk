export ARCH?=arm
export CROSS_COMPILE?=$(ARM_TOOLCHAIN_PATH)/$(ARM_BIN_SUBPATH)
export KERNELDIR?=$(shell ls -d `echo $(ARM_TOOLCHAIN_PATH) | sed -e 's1/output/host/.*$$1/output/build1'`/linux-* | head -1)

ifeq ($(TARGET),arm)
	CC=$(ARM_TOOLCHAIN_PATH)/$(ARM_BIN_SUBPATH)cc
	STRIP=$(ARM_TOOLCHAIN_PATH)/$(ARM_BIN_SUBPATH)strip
	LIB_PATH=$(ARM_TOOLCHAIN_PATH)/lib
	INC_PATH=$(ARM_TOOLCHAIN_PATH)/include
	BIN_PATH=$(ARM_TOOLCHAIN_PATH)/bin
	SBIN_PATH=$(ARM_TOOLCHAIN_PATH)/sbin
	ETC_PATH=$(ARM_TOOLCHAIN_PATH)/etc
	LOCAL_BIN_PATH=$(ARM_TOOLCHAIN_PATH)/etc
else
	LIB_PATH=/usr/lib
	INC_PATH=/usr/include
	BIN_PATH=/usr/bin
	SBIN_PATH=/usr/sbin
	ETC_PATH=/etc
	LOCAL_BIN_PATH=/usr/local/bin
endif
