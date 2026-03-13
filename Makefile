.PHONY: build rebuild test clean install install-cli services-up services-down compliance compliance-std compliance-wasm compliance-all leak-check leak-check-std leak-check-all setup-wasm wasm wasm-clean

build:
	@mkdir -p build && cd build && cmake .. && cmake --build . -j$$(nproc)

rebuild: clean build

test: build
	@cd build && ctest --output-on-failure

clean:
	@rm -rf build

install: build
	@cd build && cmake --install .

install-cli: build
	install -d $(HOME)/.local/bin
	install build/packages/east-c-cli/east-c $(HOME)/.local/bin/east-c
	@echo "Installed east-c to $(HOME)/.local/bin/east-c"

# Compliance tests
compliance: build
	@./packages/east-c/scripts/run_compliance.sh

compliance-std: build
	@./packages/east-c-std/scripts/test_compliance.sh

compliance-wasm:
	@./packages/east-c-wasm/scripts/run_compliance.sh

compliance-all: compliance compliance-std compliance-wasm

# Memory leak checks
leak-check:
	@./packages/east-c/scripts/run_leak_check.sh

leak-check-std:
	@./packages/east-c/scripts/run_leak_check.sh /tmp/east-node-std packages/east-c-std/test_std_compliance

leak-check-all: leak-check leak-check-std

# Docker services (httpbin for fetch tests)
services-up:
	docker compose up -d --wait

services-down:
	docker compose down -v

# ---- WebAssembly (east-c-wasm) ----

EMSDK_DIR := tools/emsdk
EMSDK_VERSION := latest

# One-time setup: clone and install Emscripten SDK
setup-wasm:
	@if [ ! -d "$(EMSDK_DIR)" ]; then \
		echo "Cloning emsdk..."; \
		mkdir -p tools; \
		git clone https://github.com/emscripten-core/emsdk.git $(EMSDK_DIR); \
	fi
	@echo "Installing Emscripten $(EMSDK_VERSION)..."
	@cd $(EMSDK_DIR) && ./emsdk install $(EMSDK_VERSION)
	@cd $(EMSDK_DIR) && ./emsdk activate $(EMSDK_VERSION)
	@echo ""
	@echo "Emscripten installed. Run 'make wasm' to build."

# Build WASM output
EMCMAKE := $(EMSDK_DIR)/upstream/emscripten/emcmake
EMMAKE := $(EMSDK_DIR)/upstream/emscripten/emmake

wasm:
	@if [ ! -f "$(EMCMAKE)" ]; then \
		echo "Error: emsdk not found. Run 'make setup-wasm' first."; \
		exit 1; \
	fi
	@export EMSDK=$(CURDIR)/$(EMSDK_DIR) && \
		export EM_CONFIG=$(CURDIR)/$(EMSDK_DIR)/.emscripten && \
		export PATH=$(CURDIR)/$(EMSDK_DIR)/upstream/emscripten:$(CURDIR)/$(EMSDK_DIR)/upstream/bin:$(CURDIR)/$(EMSDK_DIR)/node/22.16.0_64bit/bin:$$PATH && \
		mkdir -p build-wasm && \
		cd build-wasm && \
		$(CURDIR)/$(EMCMAKE) cmake .. -DCMAKE_BUILD_TYPE=Release && \
		cmake --build . -j$$(nproc)
	@mkdir -p packages/east-c-wasm/dist/wasm
	@cp build-wasm/packages/east-c-wasm/east-c.js packages/east-c-wasm/dist/wasm/
	@cp build-wasm/packages/east-c-wasm/east-c.wasm packages/east-c-wasm/dist/wasm/
	@echo '{ "type": "module" }' > packages/east-c-wasm/dist/wasm/package.json
	@echo ""
	@wasm_size=$$(wc -c < packages/east-c-wasm/dist/wasm/east-c.wasm); \
		echo "Built east-c.wasm ($$(( wasm_size / 1024 )) KB)"

wasm-clean:
	@rm -rf build-wasm
	@rm -rf packages/east-c-wasm/dist/wasm
