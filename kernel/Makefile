COMPILER = clang++
LINKER = ld.lld
CFLAGS = -O2 -Wall -g --target=x86_64-elf -ffreestanding -mno-red-zone -fno-exceptions -fno-rtti -std=c++17 -c
LFLAGS = --entry $(ENTRYPOINT) -z norelro --image-base 0x100000 --static
SRCROOT = ./src/
OBJROOT = ./obj/
BUILDDIR = ./build/
INCDIR = ./src/
SOURCES = $(wildcard $(SRCROOT)*.cpp)
OBJECTS = $(addprefix $(OBJROOT), $(notdir $(SOURCES:.cpp=.o)))
ENTRYPOINT = main
TARGET = kernel.bin

$(TARGET): $(OBJECTS)
	$(LINKER) $(LFLAGS) -o $(BUILDDIR)$@ $^

$(OBJROOT)%.o: $(SRCROOT)%.cpp
	$(COMPILER) $(CFLAGS) -I$(INCDIR) -o $@ -c $<

.PHONY: clean load run

run:
	$(MAKE) -C ../

load:
	sudo losetup /dev/loop0 ../hdd.img
	sudo mkdir -p ../mnt
	sudo mount /dev/loop0 ../mnt
	sudo mkdir -p ../mnt/os
	sudo cp ./build/kernel.bin ../mnt/os/kernel.bin
	sudo umount ../mnt
	sudo losetup -d /dev/loop0

clean:
	rm $(OBJROOT)*
	rm $(BUILDDIR)*