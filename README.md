# East-C

C runtime for the [East](https://github.com/elaraai/east) programming language.

East-C executes compiled East IR programs natively, providing a high-performance alternative to the TypeScript and Python runtimes. It implements the full East type system, tree-walking interpreter, builtin operations, and serialization formats in portable C11.

## Features

- **C11 standard** — portable across Linux, macOS, and other POSIX systems
- **Reference counting with cycle collector** — CPython-style trial-deletion GC for deterministic memory management
- **Tree-walking interpreter** — faithfully executes East IR without code generation
- **Full serialization** — Beast2, JSON, CSV, and East text formats
- **Standard platform** — console, filesystem, path, crypto, time, random, fetch, and test functions
- **CLI** — run East IR programs from the command line with `east-c run`

## Build & Test

Requires CMake 3.16+ and a C11 compiler.

```bash
make build    # Build all packages
make test     # Run unit tests (ctest)
make clean    # Remove build directory
```

Or manually:

```bash
mkdir build && cd build && cmake .. && make -j$(nproc)
ctest --output-on-failure
```

## Compliance Tests

IR JSON test files are exported from the TypeScript [east](https://github.com/elaraai/east) project.

```bash
# Generate IR files (from ../east):
cd ../east && EXPORT_TEST_IR=/tmp/east-test-ir npm run test:export

# Run all compliance tests:
./scripts/run_compliance.sh

# Run a single compliance test:
./build/packages/east-c/test_compliance /tmp/east-test-ir/Array.json

# Run memory leak checks (ASAN):
./scripts/run_leak_check.sh
```

## CLI Usage

```bash
# Run an East IR program:
east-c run program.json -p std -o output.beast2

# Show version info:
east-c version
```

Install to `~/.local/bin`:

```bash
make install-cli
```

## Project Structure

| Package | Description |
|---------|-------------|
| `packages/east-c` | Core runtime — types, values, IR, compiler, builtins, serialization |
| `packages/east-c-std` | Standard platform functions — console, fs, path, crypto, time, random, fetch, test |
| `packages/east-c-cli` | Command-line interface for running East IR programs |

## License

This project is licensed under the [Business Source License 1.1](LICENSE.md).

For commercial licensing, contact support@elara.ai.

## Contributing

We welcome contributions! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for details.

**Note**: Contributors must sign our [CLA](CLA.md) before we can accept pull requests. This allows us to offer commercial licenses while keeping the project open source.

### Ecosystem

- **[East Node](https://github.com/elaraai/east-node)**: Node.js platform functions for I/O, databases, and system operations. Connect East programs to filesystems, SQL/NoSQL databases, cloud storage, and network services.
  - [@elaraai/east-node-std](https://www.npmjs.com/package/@elaraai/east-node-std): Filesystem, console, HTTP fetch, crypto, random distributions, timestamps
  - [@elaraai/east-node-io](https://www.npmjs.com/package/@elaraai/east-node-io): SQLite, PostgreSQL, MySQL, MongoDB, S3, FTP, SFTP
  - [@elaraai/east-node-cli](https://www.npmjs.com/package/@elaraai/east-node-cli): CLI for running East IR programs in Node.js

- **[East Python](https://github.com/elaraai/east-py)**: Python runtime and platform functions for data science and machine learning. Execute East programs with access to optimization solvers, gradient boosting, neural networks, and model explainability.
  - [@elaraai/east-py-datascience](https://www.npmjs.com/package/@elaraai/east-py-datascience): TypeScript types for optimization, gradient boosting, neural networks, explainability

- **[East UI](https://github.com/elaraai/east-ui)**: East types and expressions for building dashboards and interactive layouts. Define UIs as data structures that render consistently across React, web, and other environments.
  - [@elaraai/east-ui](https://www.npmjs.com/package/@elaraai/east-ui): 50+ typed UI components for layouts, forms, charts, tables, dialogs
  - [@elaraai/east-ui-components](https://www.npmjs.com/package/@elaraai/east-ui-components): React renderer with Chakra UI styling

- **[e3 - East Execution Engine](https://github.com/elaraai/e3)**: Durable execution engine for running East pipelines at scale. Features Git-like content-addressable storage, automatic memoization, task queuing, and real-time monitoring.
  - [@elaraai/e3](https://www.npmjs.com/package/@elaraai/e3): SDK for authoring e3 packages with typed tasks and pipelines
  - [@elaraai/e3-core](https://www.npmjs.com/package/@elaraai/e3-core): Git-like object store, task queue, result caching
  - [@elaraai/e3-types](https://www.npmjs.com/package/@elaraai/e3-types): Shared type definitions for e3 packages
  - [@elaraai/e3-cli](https://www.npmjs.com/package/@elaraai/e3-cli): `e3 init`, `e3 run`, `e3 logs` commands for managing and monitoring tasks
  - [@elaraai/e3-api-client](https://www.npmjs.com/package/@elaraai/e3-api-client): HTTP client for remote e3 servers
  - [@elaraai/e3-api-server](https://www.npmjs.com/package/@elaraai/e3-api-server): REST API server for e3 repositories

## Links

- **Website**: [https://elaraai.com/](https://elaraai.com/)
- **Repository**: [https://github.com/elaraai/east](https://github.com/elaraai/east)
- **Issues**: [https://github.com/elaraai/east/issues](https://github.com/elaraai/east/issues)
- **Email**: support@elara.ai

## About Elara

East is developed by [Elara AI Pty Ltd](https://elaraai.com/), an AI-powered platform that creates economic digital twins of businesses that optimize performance. Elara combines business objectives, decisions and data to help organizations make data-driven decisions across operations, purchasing, sales and customer engagement, and project and investment planning. East powers the computational layer of Elara solutions, enabling the expression of complex business logic and data in a simple, type-safe and portable language.

