.PHONY: run

run:
	qemu-system-x86_64 -s -S -m 4G -M q35 -device qemu-xhci,id=xhci -bios /usr/share/ovmf/OVMF.fd -hda ./hdd.img &

hdd.img:
	qemu-img create -f raw hdd.img 200M
	mkfs.fat -n 'OZ OS' -s 2 -f 2 -R 32 -F 32 hdd.img
