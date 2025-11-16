COMPILER = clang++
LINKER = ld.lld
CFLAGS = -O2 -Wall -Wextra -Wpadded -g --target=x86_64-unknown-none-elf -g -ffreestanding -mno-red-zone -fno-exceptions -fno-rtti -std=c++20 -I $(INCLUDEDIR) -c
LFLAGS = --entry $(ENTRYPOINT) -z norelro --image-base 0x100000 -g --static -L$(LIBDIR) -lcommonstd -lozstd
INCLUDEDIR = ./dependencies/ozlib/include/
LIBDIR = ./dependencies/ozlib/lib/
SRCROOT = ./src/
OBJROOT = ./obj/
BUILDDIR = ./build/
INCDIR = ./src/
SOURCES = $(wildcard $(SRCROOT)*.cpp)
OBJECTS = $(addprefix $(OBJROOT), $(notdir $(SOURCES:.cpp=.o)))
ENTRYPOINT = init
TARGET = kernel.bin

$(TARGET): $(OBJECTS)
	$(LINKER) $(LFLAGS) -o $(BUILDDIR)$@ $^

$(OBJROOT)%.o: $(SRCROOT)%.cpp
	$(COMPILER) $(CFLAGS) -o $@ -c $<

.PHONY: clean load run setup

run:
	$(MAKE) -C ../

load:
	using_loopdev=$$(losetup -f); \
	sudo losetup $$using_loopdev ../hdd.img; \
	sudo mkdir -p ../mnt; \
	sudo mount $$using_loopdev ../mnt; \
	sudo mkdir -p ../mnt/os; \
	sudo cp ./build/kernel.bin ../mnt/os/kernel.bin; \
	sudo umount ../mnt; \
	sudo losetup -d $$using_loopdev;

clean:
	rm $(OBJROOT)*
	rm $(BUILDDIR)*

setup:
	mkdir -p src
	mkdir -p obj
	mkdir -p build