.PHONY: all setup build clean check run status test install

all: build

setup:
	meson setup build

build:
	@if [ ! -d "build" ]; then meson setup build; fi
	ninja -C build

test: build
	meson test -C build --print-errorlogs

install: build
	ninja -C build install

clean:
	rm -rf build

check: test

status: build
	./build/zm status

run: build
	./build/zm $(ARGS)

