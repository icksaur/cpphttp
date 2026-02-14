# Simple Makefile wrapper for CMake
.PHONY: all build test clean release help

all: build

build:
	@mkdir -p build
	@cd build && cmake -DCMAKE_BUILD_TYPE=Debug .. && cmake --build .
	@echo "✓ Build complete: build/http_example, build/http_tests"

test: build
	@./build/http_tests

release:
	@mkdir -p build
	@cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && cmake --build .
	@echo "✓ Release build complete"

clean:
	@rm -rf build
	@echo "✓ Cleaned build directory"

help:
	@echo "Available targets:"
	@echo "  all (default) - Build debug version"
	@echo "  build         - Build debug version"
	@echo "  test          - Build and run tests"
	@echo "  release       - Build optimized version"
	@echo "  clean         - Remove build directory"
	@echo "  help          - Show this help"
