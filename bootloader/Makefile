BOOTX64.EFI: boot.c
	x86_64-w64-mingw32-gcc -Wall -Wextra -e uefi_main -nostdlib -fno-builtin -Wl,--subsystem,10 -o BOOTX64.EFI boot.c

.PHONY: load

load:
	using_loopdev=$$(losetup -f); \
	sudo losetup $$using_loopdev ../hdd.img; \
	sudo mkdir -p ../mnt; \
	sudo mount $$using_loopdev ../mnt; \
	sudo mkdir -p ../mnt/EFI/BOOT; \
	sudo cp ./BOOTX64.EFI ../mnt/EFI/BOOT/BOOTX64.EFI; \
	sudo umount ../mnt; \
	sudo losetup -d $$using_loopdev;