COMPILER = clang++
LINKER = ld.lld
CFLAGS = -O2 -Wall -g --target=x86_64-unknown-none-elf -g -ffreestanding -mno-red-zone -fno-exceptions -fno-rtti -std=c++20 -I $(INCLUDEDIR) -c
LFLAGS = --entry $(ENTRYPOINT) -z norelro --image-base 0x100000 -g --static
INCLUDEDIR = ./dependencies/ozlib/include/
LIBDIR = ./dependencies/ozlib/lib/
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
	mkdir -p src
	mkdir -p obj
	mkdir -p build