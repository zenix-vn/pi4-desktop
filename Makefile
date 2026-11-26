# Build tren chinh Raspberry Pi 4 (can linux-headers cua kernel dang chay).
obj-m += panel-jd9366.o jd9366-touch.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all: modules jd9366-pi4.dtbo

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

jd9366-pi4.dtbo: jd9366-pi4-overlay.dts
	dtc -@ -I dts -O dtb -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f jd9366-pi4.dtbo

.PHONY: all modules clean
