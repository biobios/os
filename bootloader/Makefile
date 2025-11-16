BOOTX64.EFI: boot.o
	lld-link /subsystem:efi_application /entry:uefi_main /out:BOOTX64.EFI boot.o

boot.o: boot.c
	clang -target x86_64-pc-win32-coff -Wall -Wextra -mno-red-zone -fno-stack-protector -fshort-wchar -c boot.c

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