.PHONY: all setup build clean check run status

all: build

setup:
	meson setup build

build:
	@if [ ! -d "build" ]; then meson setup build; fi
	ninja -C build

clean:
	rm -rf build

check:
	@echo "Checking syntax of all C++20 header interfaces..."
	@for f in $$(find src/ -name "*.hpp"); do \
		g++ -std=c++20 -Isrc -fsyntax-only "$$f" || exit 1; \
	done
	@echo "✔ All C++20 interfaces valid!"

status: build
	./build/zm status

run: build
	./build/zm $(ARGS)
