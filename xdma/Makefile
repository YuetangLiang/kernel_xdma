SHELL = /bin/bash
ifneq ($(xvc_bar_num),)
	XVC_FLAGS += -D__XVC_BAR_NUM__=$(xvc_bar_num)
endif

ifneq ($(xvc_bar_offset),)
	XVC_FLAGS += -D__XVC_BAR_OFFSET__=$(xvc_bar_offset)
endif

$(warning XVC_FLAGS: $(XVC_FLAGS).)

topdir := $(shell cd $(src)/.. && pwd)

TARGET_MODULE:=xdma

EXTRA_CFLAGS := -I$(topdir)/include -I${src}/zynq $(XVC_FLAGS)
EXTRA_CFLAGS += -Wno-unused-function -Wno-unused-variable -Wno-unused-label
#-D__LIBXDMA_DEBUG__ -Wno-unused-function -Wno-unused-variable -Wno-unused-label
#EXTRA_CFLAGS += -DINTERNAL_TESTING

ifneq ($(KERNELRELEASE),)
	$(TARGET_MODULE)-objs := libxdma.o xdma_cdev.o cdev_ctrl.o cdev_events.o cdev_sgdma.o cdev_xvc.o cdev_bypass.o xdma_mod.o zynq_gps.o zynq_main.o
	obj-m := $(TARGET_MODULE).o
else
	BUILDSYSTEM_DIR:=/lib/modules/$(shell uname -r)/build
	PWD:=$(shell pwd)
all : 
	$(MAKE) -C $(BUILDSYSTEM_DIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(BUILDSYSTEM_DIR) M=$(PWD) clean
	-rm -f *.o.ur-safe TAGS

install: all
	$(MAKE) -C $(BUILDSYSTEM_DIR) M=$(PWD) modules_install
	depmod -a
#	cp ../../etc/udev/rules.d/* /etc/udev/rules.d/
	install -m 644 etc/udev/rules.d/60-xdma.rules        /etc/udev/rules.d/
	install -m 644 etc/udev/rules.d/xdma-udev-command.sh /etc/udev/rules.d/
	-rmmod -s xdma || true
	-modprobe xdma
# insmod xdma.ko poll_mode=1

uninstall:
	-rm -f /etc/udev/rules.d/60-xdma.rules
	-rm -f /etc/udev/rules.d/xdma-udev-command.sh
	-rmmod -s xdma || true
	-rm -f /lib/modules/`uname -r`/extra/xdma.ko
	depmod -a

endif
