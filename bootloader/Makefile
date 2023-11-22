BOOTX64.EFI: boot.c
	x86_64-w64-mingw32-gcc -Wall -Wextra -e uefi_main -nostdlib -fno-builtin -Wl,--subsystem,10 -o BOOTX64.EFI boot.c

.PHONY: load

load:
	sudo losetup /dev/loop0 ../hdd.img
	sudo mkdir -p ../mnt
	sudo mount /dev/loop0 ../mnt
	sudo mkdir -p ../mnt/EFI/BOOT
	sudo cp ./BOOTX64.EFI ../mnt/EFI/BOOT/BOOTX64.EFI
	sudo umount ../mnt
	sudo losetup -d /dev/loop0
