.PHONY: build rebuild test clean install install-cli services-up services-down compliance compliance-std compliance-all leak-check leak-check-std leak-check-all

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
	@./scripts/run_compliance.sh

compliance-std: build
	@./scripts/run_compliance.sh /tmp/east-node-std build/packages/east-c-std/test_std_compliance

compliance-all: compliance compliance-std

# Memory leak checks
leak-check:
	@./scripts/run_leak_check.sh

leak-check-std:
	@./scripts/run_leak_check.sh /tmp/east-node-std packages/east-c-std/test_std_compliance

leak-check-all: leak-check leak-check-std

# Docker services (httpbin for fetch tests)
services-up:
	docker compose up -d --wait

services-down:
	docker compose down -v
