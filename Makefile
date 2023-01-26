.PHONY: run

run:
	qemu-system-x86_64 -s -S -m 4G -device qemu-xhci,id=xhci -bios /usr/share/ovmf/OVMF.fd -hda ./hdd.img &
