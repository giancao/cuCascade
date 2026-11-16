# AGENTS.md

cuCascade is a C++20/CUDA library for tiered (GPU, host, disk) memory management and data
representation, plus an optional async I/O layer (local and object-store datasources, prefetching
cache). Each library is built as `<name>_shared` and `<name>_static`, with strict layering
(top depends on bottom):

| Library | Sources | Depends on |
| --- | --- | --- |
| `cucascade_cudf` (`CUCASCADE_BUILD_CUDF`) | `src/cudf`, `include/cucascade/cudf` | io, core, libcudf |
| `cucascade_io` (`CUCASCADE_BUILD_IO`) | `src/io`, `include/cucascade/{io,exec}` | core, kvikIO (`CUCASCADE_USE_KVIKIO`) and its engine plugins (liburing, OpenSSL) |
| `cucascade` (core) | `src/{memory,data,cuda}`, `include/cucascade/{memory,data,cuda,utils,log}`, `include/cucascade/*.hpp` | topology, RMM, CUDA runtime |
| `cucascade_topology_discovery` | `src/memory/topology_discovery.cpp`, `src/log` | NVML; never a link-time `libcuda` dependency |

Keep cudf types and headers out of core and io. The disk tier stores an opaque `int32_t` type id
that the cudf converters translate. DISK spaces read and write their files through an `io::ioctx`
(`make_disk_io_context`), so a cudf build requires `CUCASCADE_BUILD_IO`.

## Commands

Tooling and environments come from pixi (`pixi.toml`); lint tool versions are pinned in
`.pre-commit-config.yaml`.

- Build: `pixi run build` (release, `build/release`) or `pixi run build-debug` (`build/debug`).
  Libraries, tests and benchmarks are built by default.
- Rebuild one target: `pixi run cmake --build build/release --target cucascade_tests`
- Test everything: `pixi run test` (`ctest` in `build/release`; it does not build first).
- Run a subset: each test binary is one CTest test, so filter with Catch2 tags instead, e.g.
  `./build/release/test/cucascade_cudf_tests "[data_batch]"` (`--list-tags` to discover them).
  Binaries run without `pixi run`.
- Lint and format: `pixi run lint` (pre-commit; hooks rewrite files in place). Run one hook with
  `pixi run pre-commit run clang-format --files <paths>`; don't use another clang-format version.
- Benchmarks: `./build/release/benchmark/cucascade_benchmarks --benchmark_filter=<regex>` (the
  full suite is slow). Compare runs with
  `python scripts/compare_benchmarks.py --baseline a.json --current b.json` (`--benchmark_out`
  files).
- Stream-ordering checks: `pixi run compute-sanitizer ./build/release/test/<binary> "<tag>"`.
- Other environments: `pixi run -e <env> <task>` with `cuda-{12,13}-{nightly,stable}`; `default`
  is `cuda-13-nightly`. A build dir keeps the compilers and libcudf of the env that first
  configured it, and the `build`/`test` tasks always use `build/release`, so give other envs their
  own dir: `pixi run -e cuda-13-stable bash -c "cmake --preset release -B build/cuda-13-stable &&
  cmake --build build/cuda-13-stable"`.
- kvikIO is the `kvikio` git submodule (`git submodule update --init kvikio`), built from source
  as `libkvikio.so` when `CUCASCADE_USE_KVIKIO` is ON (the default). Configure fails unless it is
  the same RAPIDS release as libcudf. The submodule is on 26.10, so use a `*-stable` env. The io
  library requires kvikIO (`CUCASCADE_BUILD_IO=ON` with `CUCASCADE_USE_KVIKIO=OFF` fails at
  configure), so the nightly envs, `default` included, can only build with
  `-DCUCASCADE_BUILD_IO=OFF -DCUCASCADE_USE_KVIKIO=OFF`. To build another checkout, set
  `CUCASCADE_KVIKIO_SOURCE_DIR`; set it to an empty string to link an installed kvikIO, which
  must be the POC fork (upstream kvikIO lacks the `kvikio/exec` and `kvikio/io` headers).
- The io engines live in the kvikIO submodule and are re-exported here under their cuCascade
  names: `cucascade::exec::{semi_future,promise,future,try_t,invocable,admission_control}`,
  `exec::thread_util`, the request and error types, `byte_range`, `object_identity`, `io_utils`,
  `tiered_queue`, `slot_pool`, the URI parser, `object_store_config`, and everything under
  `io/uring/`, `io/cufile/` (except the per-path GDS probe), `io/rest/` and `io/tls/`.
  `templated_ioctx` is glue over kvikIO's `reactor_dispatch`. Change these in the kvikIO
  submodule (`kvikio/cpp/{include/kvikio,src}/{exec,io}`), commit there first, then bump the
  submodule here. `CUCASCADE_USE_ABSEIL_INVOCABLE` and the moodycamel namespace are passed on to
  the kvikIO build and must agree with it.
- What is left of the io layer here: `ioctx` with the `fs_cache` and metadata store,
  `engine_ioctx` (the facade over a kvikIO engine) and its thin subclasses `uring_ioctx`,
  `cufile_ioctx`, `rest_ioctx`, the staging source over a HOST memory resource, NIC and storage
  discovery, the datasource registry, and s3rdma.
- A host chooses a backend only through `io_config::backend` (`io_backend`, with
  `string_to_enum` / `enum_to_string` for its config spelling) and asks the registry which
  context serves local files (`io_context_registry::local_type()`). What an engine offers beyond
  the engine interface (bucket listing, the `uring` gauges) is reached with
  `engine_api<Api>(ctx)` (`engine_ioctx.hpp`), by interface; hosts should not cast an `ioctx` to
  `uring_ioctx` or `rest_ioctx`.
- Each engine is a kvikIO plugin built next to `libkvikio.so`: `libkvikio_engine_uring.so`,
  `libkvikio_engine_cufile_reactor.so`, `libkvikio_engine_uring_http.so`. The cuCascade libraries
  do not link them; the factories select an engine by name in kvikIO's registry
  (`make_uring_ioctx_factory` falls back to the built-in `cufile_posix` engine only when the plugin cannot be loaded or io_uring is not allowed in the process; `make_rest_ioctx_factory` falls back to `curl` only when the plugin cannot be loaded. An engine that exists but cannot be created yields no context).
  Tests and benchmarks that use reactor, transport or TLS classes directly, or build an ioctx
  from a reactor context, link the plugin target (`kvikio::engine_uring`,
  `kvikio::engine_cufile_reactor`, `kvikio::engine_uring_http`); `make_reactor_context` wraps a
  HOST memory resource for them.
- `cucascade::io::{rest,tls,http,net}` are namespaces with a using-directive for kvikIO's
  (`io/rest/forward.hpp`), so qualified names resolve to kvikIO's declarations. Do not declare a
  nested namespace there whose name kvikIO also uses (`detail`, `s3`): it would hide kvikIO's.
  The io test binary force-includes `forward.hpp` because the tests of the REST layers include
  kvikIO's private headers (`"io/rest/..."`, `"io/tls/..."`, found through the plugin target).
- Engines stage only through the injected staging source (the HOST space), except the cuFile
  reactors: each owns a fixed 16 MiB page-locked scratch. That is a fixed overhead outside the HOST
  budget, reported by `io_config::fixed_pinned_overhead_bytes()` for hosts that account for it.
- A staging source reports exhaustion in its own type: `host_staging_source` makes it an
  `rmm::out_of_memory`, which hosts retry on.
- Running a kvikIO gtest from a standalone kvikIO build needs
  `LD_PRELOAD=<build>/libkvikio.so`, or the conda copy of kvikIO is loaded instead.

## Before you finish

- The build passes. Code in `src/` and `benchmark/` compiles with `-Werror -Wconversion
  -Wsign-conversion`; use `static_cast` for narrowing or sign changes. Tests and header-only code
  (e.g. `data_repository.hpp`) are not compiled with these flags here, but downstream users build
  them with `-Werror`, so keep them warning-clean by hand.
- The relevant test binary passes, then `pixi run test`.
- `pixi run lint` is clean.
- CI builds all four `cuda-{12,13}-{nightly,stable}` envs and runs tests on a single sm_75 GPU.
  Code must compile in all four and must not require a newer GPU at runtime. When touching cudf
  or RMM APIs, also build `cuda-13-stable`.
- `pixi.lock` is gitignored and CI re-solves every run, so nightly packages drift. If the nightly
  env fails in code you did not touch, check whether `cuda-13-stable` builds before debugging it.
- Multi-GPU tests are tagged `[.multi-device]` (hidden) or skip with fewer than two GPUs, so CI
  never runs them. For cross-device changes run `<binary> "[multi-device]"` on a multi-GPU
  machine, or say that they were not run.

## Where things go

- Source, test and benchmark files are listed explicitly (no globs) in `src/*/CMakeLists.txt`,
  `test/CMakeLists.txt` and `benchmark/CMakeLists.txt`. The directory does not decide the
  library: `topology_discovery.cpp` and `src/log` build into `cucascade_topology_discovery` (so
  every library links the logger), `src/exec` into `cucascade_io`.
- Tests go in the binary whose dependencies they need, regardless of directory:
  `cucascade_tests` (core), `cucascade_io_tests`, `cucascade_cudf_tests` (anything using cudf),
  `cucascade_topology_discovery_tests`. Tests needing an optional layer go in the matching
  `if(CUCASCADE_BUILD_CUDF)` / `if(CUCASCADE_BUILD_IO)` block.
- Topology discovery must load without `libcuda`: reach the driver only through the lazy `dlopen`
  wrapper in `topology_discovery.cpp`, and keep passive `discover()` from loading it. CTest
  enforces both.
- Public headers must be self-contained (include what they use). Topology headers must also build
  with `CUCASCADE_TOPOLOGY_ONLY=ON` (no RMM or cudf).
- Version differences: include `cucascade/cuda/stream.hpp` for `::cuda::stream_ref`, put cudf
  shims in `src/cudf/cudf_compat.hpp`, and guard helpers used by only one `#if` branch so the
  other build doesn't fail on unused functions.
- The io/exec layer is shared with Sirius, which embeds cuCascade. Keep Sirius, DuckDB and cudf
  types out of it, use the `cucascade::io::concurrent_queue` aliases instead of `moodycamel::`,
  and use only the parts of `exec::invocable` that `absl::AnyInvocable` also supports
  (`CUCASCADE_USE_ABSEIL_INVOCABLE`).
- libcudf loads `libkvikio.so` too, and a process maps one library per soname, so the kvikIO built
  here also serves libcudf; the environment's own copy must not be the one loaded. cuCascade puts
  its build directory first in the runtime search path of its binaries and makes `libkvikio.so` a
  direct dependency of everything linking the io library; the `[kvikio]` test fails if another
  copy is loaded. A host linking cuCascade statically inherits the direct dependency but has to
  order the search path of its own binaries itself, with `CUCASCADE_KVIKIO_LIBRARY_DIR`. It also
  has to add cuCascade before any package whose CMake config imports kvikIO (libcuvs does), or
  configure fails; later `find_package(kvikio)` calls are served by this build.

## Invariants that are easy to break

- Batches exist only as `shared_ptr` from `data_batch::make`. State is `idle`, `read_only`
  (shared) or `mutable_locked` (exclusive), entered via `to_read_only()` / `to_mutable()` (or
  `try_to_*`). Destroying the mutable accessor or the last read-only accessor returns the batch to
  `idle`; `data_batch::to_idle(std::move(accessor))` releases one accessor and hands back the
  batch. There is no upgrade or downgrade; calling `to_mutable()` while the same thread holds an
  accessor deadlocks.
- Host-side batch locks do not order GPU work. Construct a `gpu_table_representation` with the
  stream that wrote it (a null stream means "writer unknown" and forces device-wide syncs), and
  call `record_writer_event(stream)` after later writes. Readers use `wait_for_writer(stream)`;
  built-in converters do this themselves. A reader holding a `read_only_data_batch` calls
  `record_reader_event(stream)` before releasing it so later mutation waits for its reads.
- Change tiers with `mutable_data_batch::convert_to`, not `set_data`: `set_data` destroys the old
  representation immediately, without synchronizing or recording a writer event.
- The converter registry starts empty. `register_builtin_converters()`
  (`cucascade/cudf/builtin_converters.hpp`) adds the GPU/host/disk converters; registering a pair
  twice throws.
- `disk_data_representation` deletes its file on destruction, and its `clone()` always throws;
  materialize to another tier through a converter.
- The DISK converters hand the io context whole blocks only (`disk_block_size()`, 4 KiB): buffers
  start on block boundaries and the partial last block of each is padded with zeros in host
  memory (`disk_edge_blocks`). Do not write a partial block: an io stack using O_DIRECT would
  write it through the page cache next to blocks it writes directly, and mixing the two on one
  file can leave stale zero pages in the page cache that a later buffered read returns (seen on
  XFS, kernel 6.11). `[disk][io][layout]` checks the slices.
- A repository's ordering mode (`ORDERED` / `UNORDERED`) is set by its first insertion, applies to
  all partitions and is never reset; mixing `add_data_batch` and `add_data_batch_at_position`
  throws. New methods follow the public-locks / protected `*_unlocked` hook pattern. Pops and
  lookups never lock or transition the returned batch.
- Don't allocate in destructors or other `noexcept` paths, and allocate before submitting async
  GPU or I/O work so a `bad_alloc` cannot strand it. Test such paths with
  `test/utils/allocation_counter.hpp`.
- In destructors and `noexcept` paths use `CUCASCADE_ASSERT_CUDA_SUCCESS` (reports failures to
  the log sink; debug builds also assert) and swallow filesystem errors; elsewhere use
  `CUCASCADE_CUDA_TRY` / `CUCASCADE_FAIL` from `include/cucascade/error.hpp`.
- `CUCASCADE_LOG_*` arguments are not evaluated below the runtime threshold (`warn` until a host
  installs a sink) or `CUCASCADE_MIN_LOG_LEVEL`, so keep side effects out of them. A host sink
  may call back into cuCascade, so log after releasing internal locks where possible.

## Tests

- Test binaries install an RMM pool on the GPU; set `CUCASCADE_TEST_GPU_POOL_BYTES` to shrink it
  when the GPU is shared or binaries run concurrently.
- Tests that change environment variables or need fresh CUDA/NVML state run in a child process
  (`test/utils/child_test_runner.hpp`).
- Tests must fail rather than hang: bound every wait with a timeout and release latches on all
  exit paths, including exceptions.

## Conventions not enforced by tooling

- New files copy the SPDX/Apache-2.0 header from a neighbouring file of the same type, with the
  current year.
- Namespace form (`namespace cucascade { namespace memory {` vs `namespace cucascade::io {`),
  accessor naming (`get_` or not), enum naming and doc-comment style (`/** */` vs `///`) vary
  across modules and files; match the neighbouring code. `data` types live directly in
  `namespace cucascade`. Members use a leading underscore (`_mutex`).
- Public headers get Doxygen comments; mark getters `[[nodiscard]]`.
- Annotate performance-relevant entry points with `CUCASCADE_FUNC_RANGE()` (`error.hpp`) or a
  named `nvtx3::scoped_range_in<libcucascade_domain>` (`nvtx.hpp`).
- Commits and PR titles use Conventional Commits with an optional scope (`feat(io): ...`,
  `fix(cudf): ...`); mark breaking public API changes with `!`. PRs are squash-merged with the
  PR title and description as the commit message.
- CI runs on `pull-request/<N>` branches that copy-pr-bot creates; drafts are not synced, and
  untrusted commits need a maintainer's `/ok to test`. Never push `pull-request/*` yourself.

## Docs

`docs/README.md` indexes the design docs. Where a doc disagrees with the code, the code is right.
