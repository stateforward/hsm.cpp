# hsm_bench

`hsm_bench` is a dedicated benchmarking tool for the C++ hierarchical state machine library **hsm**. It compares dispatch/transition throughput across multiple backends (hsm and several other HSM/FSM libraries plus vanilla baselines) over a small, fixed set of scenarios.

By default, `hsm_bench`:
- Runs **all compiled-in libraries** (backends) and **all scenarios**.
- Uses a fairly high number of iterations so results are statistically meaningful.
- Prints a human-readable summary to stdout.
- Writes structured CSV and JSON snapshots into a `.snapshots/` directory (relative to the working directory), one pair of files per run.

## Build

From the top-level `hsm` project directory, the bench target is enabled by default:

```sh
cmake -B cmake-build-release -S . -DBUILD_BENCH=ON
cmake --build cmake-build-release --target hsm_bench -j4
```

The resulting binary is located at:

- `cmake-build-*/bench/hsm_bench`

## Scenarios

The benchmark currently exercises these logical scenarios:

- `ping_pong` – Simple A<->B transitions without guards.
- `hierarchical` – Parent/child hierarchy with entry/exit (P/C1/C2).
- `deep` – Deeper nested hierarchy (L1/L2/L3a<->L3b).
- `guarded` – Guarded self-transition where only every other dispatch transitions.
- `traffic_light` – A more complex hierarchical traffic light controller.

Each backend implements semantically equivalent behavior for these scenarios so throughput numbers are comparable.

## Libraries (backends)

At build time, `hsm_bench` attempts to include support for the following libraries:

- `hsm` – The native hsm backend (always available).
- `hsmcpp` – hsmcpp hierarchical state machine library.
- `boost_msm` – Boost.MSM (if Boost headers are available).
- `qp` – QP/C++ framework.
- `sml` – Boost-ext.SML.
- `hfsm2` – HFSM2 library.
- `vanilla_switch` – Hand-written switch-based FSM baselines.
- `vanilla_fp` – Hand-written function-pointer-based FSM baselines.

Whether a given backend is actually compiled depends on its CMake options and whether its dependencies can be fetched or found. At **runtime**, when no `--libs` filter is specified, `hsm_bench` runs **all backends that were successfully compiled in**.

## Default behavior (no flags)

Running `hsm_bench` with no arguments:

```sh
./bench/hsm_bench
```

will:

- Run all scenarios: `ping_pong`, `hierarchical`, `deep`, `guarded`, `traffic_light`.
- Run all available backends for each scenario.
- Use the following defaults:
  - `warmup_iterations = 1000`
  - `iterations = 100000`
  - `baseline = hsm`
  - `output_prefix = .snapshots`
  - `format = all` (text + CSV + JSON)
- Print a configuration header and a scenario-by-scenario table to stdout.
- Write:
  - `.snapshots/hsm_bench_results_<TIMESTAMP>.csv`
  - `.snapshots/hsm_bench_results_<TIMESTAMP>.json`

## Command line options

`hsm_bench` accepts several flags to control what is benchmarked and how results are emitted.

### Iteration control

- `--warmup=N`
  - Number of warm-up iterations per scenario/backend before timing starts.
  - Default: `1000`.
- `--iterations=N`
  - Number of measured iterations per scenario/backend.
  - Default: `100000`.

### Scenario selection

- `--scenarios=id1,id2,...`
  - Restrict the benchmark to specific scenario ids (e.g. `ping_pong,deep`).
  - Valid ids: `ping_pong`, `hierarchical`, `deep`, `guarded`, `traffic_light`.
  - When provided, this takes precedence over `--filter`.
- `--filter=substr`
  - Legacy behavior: run only scenarios whose human-readable name contains `substr`.
  - Ignored if `--scenarios` is present.

If neither `--scenarios` nor `--filter` is specified, **all scenarios** are run.

### Library selection

- `--libs=list`
  - Comma-separated list of library names (e.g. `hsm,vanilla_switch,sml`).
  - Special token: `all` – explicitly selects all compiled-in backends.

If `--libs` is omitted, `hsm_bench` automatically runs **all available backends** compiled into the binary.

### Baseline configuration

- `--baseline=name`
  - Sets which library is used as the baseline for the `% vs base` column.
  - Default: `hsm`.

### Output format and snapshots

- `--format=text|csv|json|all`
  - Controls which outputs are produced.
  - `text` – only the human-readable table to stdout.
  - `csv` – only CSV snapshot.
  - `json` – only JSON snapshot.
  - `all` – text + CSV + JSON.
  - Default: `all`.

- `--output-prefix=PATH`
  - Directory (or path prefix) under which CSV/JSON snapshots are written.
  - Default: `.snapshots` – snapshots are written under a `.snapshots/` directory relative to the working directory.

Each run creates a new pair of snapshot files with a timestamp in the filename; existing snapshots are not overwritten.

## CMake custom target: `bench`

A convenience CMake custom target named `bench` is provided so you can build
and run `hsm_bench` via CMake:

```sh
# From the build directory
cmake --build . --target bench
```

This will:
- Build `hsm_bench` if it is out of date.
- Run `hsm_bench` with its built-in defaults (all scenarios, all libraries,
  `iterations = 100000`, snapshots under `.snapshots/`).

You can also configure arguments via the `HSM_BENCH_ARGS` cache variable,
which must be a CMake list (semicolon-separated) of command-line arguments.
For example:

```sh
# Configure the bench arguments (only needs to be done when changing args)
cmake .. -DHSM_BENCH_ARGS="--format=text;--scenarios=ping_pong;--libs=hsm,vanilla_switch"

# Build and run with the configured arguments
cmake --build . --target bench
```

If `HSM_BENCH_ARGS` is empty, `hsm_bench` runs with its default behavior.

## Bash helper script: `hsm`

At the repository root there is a small `hsm` script that acts as a
convenience CLI for building and running both benchmarks and tests. It
forwards all additional arguments directly to the underlying tools.

From the repo root:

```sh
# Build (if needed) and run hsm_bench with defaults
./hsm bench

# Build (if needed) and run with custom benchmark arguments
./hsm bench --format=text --scenarios=ping_pong,deep --libs=hsm,vanilla_switch

# Build (if needed) and run the hsm_* tests (default filter) on a debug build
./hsm test

# Run tests on a release build instead
./hsm test --release

# Build (if needed) and run CTest with custom arguments (build-type inferred
# from the presence of --debug/--release or defaulting to debug)
./hsm test --debug -R some_other_pattern

# Use an alternative build directory
BUILD_DIR=cmake-build-debug ./hsm bench --format=text
```

The script will configure CMake with `-DBUILD_BENCH=ON -DBUILD_TESTS=ON` and a
per-subcommand build type (`Release` for `bench`, `Debug` for `test`), build
what is needed, and then run either the `hsm_bench` binary or `ctest` from
the chosen build directory depending on the subcommand.


## Examples

Run all scenarios and all libraries with defaults:

```sh
./bench/hsm_bench
```

Run only `ping_pong` and `deep` scenarios for `hsm` and `vanilla_switch` backends:

```sh
./bench/hsm_bench \
  --scenarios=ping_pong,deep \
  --libs=hsm,vanilla_switch
```

Run text-only output (no CSV/JSON) with fewer iterations for a quick smoke check:

```sh
./bench/hsm_bench --format=text --iterations=10000
```

Write snapshots to a custom directory:

```sh
./bench/hsm_bench --output-prefix=/tmp/hsm-bench-snapshots
```