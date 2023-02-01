COMPILER = clang++
LINKER = ld.lld
CFLAGS = -v -O2 -Wall -g --target=x86_64-unknown-none-elf -ffreestanding -mno-red-zone -fno-exceptions -fno-rtti -std=c++20 -stdlib=libc++ -c
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
	$(COMPILER) $(CFLAGS) -o $@ -c $<

.PHONY: clean load run setup

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

setup:
	mkdir src
	mkdir obj
	mkdir build