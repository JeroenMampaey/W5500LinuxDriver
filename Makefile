.PHONY: install uninstall

obj-m += w5500_driver.o
w5500_driver-objs := w5500_main.o w5500_netdev.o w5500_controller.o

all: module overlay

module:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

overlay: w5500_overlay.dts
	dtc -@ -I dts -O dtb -o w5500_overlay.dtbo w5500_overlay.dts

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm -f w5500_overlay.dtbo

install: module overlay
	sudo insmod w5500_driver.ko
	sudo dtoverlay w5500_overlay.dtbo

uninstall:
	sudo dtoverlay -R w5500_overlay
	sudo rmmod w5500_driver