# Wrapper Makefile for CMake convenience

BUILD_DIR ?= build

.PHONY: all build image run run-debug clean

all: build

build: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR)

image: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR) --target image

run: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR) --target run

run-debug: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR) --target run-debug

clean:
	rm -rf $(BUILD_DIR)

$(BUILD_DIR)/CMakeCache.txt:
	cmake -S . -B $(BUILD_DIR)
