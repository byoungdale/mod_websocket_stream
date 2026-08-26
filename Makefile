.PHONY: configure build check clean

configure:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug

build: configure
	cmake --build build

check: build
	cmake -DMODULE_FILE="$(CURDIR)/build/mod_websocket_stream.so" \
		-P cmake/verify-module.cmake
	ctest --test-dir build --output-on-failure

clean:
	cmake -E rm -rf build

-include scripts/dev.mk
