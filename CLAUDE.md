# east-c

C port of the East language runtime. Two packages:
- `packages/east-c/` — Core runtime (types, values, IR, compiler, builtins, serialization)
- `packages/east-c-std/` — Standard platform functions (console, fs, path, crypto, time, random, fetch, test)

## Build & Test

```bash
make build    # Build both packages
make test     # Run unit tests (ctest)
make clean    # Remove build directory
```

Or manually:
```bash
mkdir build && cd build && cmake .. && make -j$(nproc)
ctest --output-on-failure
```

## Compliance Tests

IR JSON test files are exported from the TypeScript `east` project and live in `/tmp/east-test-ir/`.

```bash
# Generate IR files (from ../east):
cd ../east && npm run test:export

# Run all compliance tests:
./scripts/run_compliance.sh                    # Uses /tmp/east-test-ir by default
./scripts/run_compliance.sh /path/to/ir/dir    # Or specify a custom directory

# Run a single compliance test:
./build/packages/east-c/test_compliance /tmp/east-test-ir/Array.json
```

Current status: **918 passed, 0 failed, 0 crashed** (out of 49 IR files)

## Architecture

- C11 standard, CMake build system
- Reference counting for memory management (EastValue, EastType)
- Tree-walking interpreter (not code generation)
- int64_t for integers (no bigint)
- Async preserved in IR but executed synchronously

## Reference Implementations

- TypeScript: `../east` (core), `../east-node/packages/east-node-std` (platform)
- Python: `../east-py` (core + platform in one package)

## Key Files

- `packages/east-c/include/east/` — All public headers
- `packages/east-c/src/` — Core implementation
- `packages/east-c/src/builtins/` — Builtin operations (integer, float, string, array, etc.)
- `packages/east-c/src/serialization/` — JSON, Beast2, CSV, East text format
- `packages/east-c/src/type_of_type.c` — IR JSON decoder (EastValue IR tree -> IRNode)
- `packages/east-c/tests/` — Unit tests and compliance test runner
- `packages/east-c-std/` — Platform functions (console, fs, path, crypto, time, random, fetch, test)
- `scripts/run_compliance.sh` — Runs all compliance tests in parallel
