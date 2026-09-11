# Unified Threading

This project's objective is unified threading and polling, where the option to
use threads for Ethernet I/O and TMXR multiplexors is chosen at runtime, not at
compile time.

This eliminates the conditional compilation forests with `SIM_ASYNCH_IO`,
`USE_READER_THREAD` and friends. For the emulated Ethernet, specifically,
eliminating `USE_READER_THREAD` unifies the API between the single thread and
multi-threaded code, where both code models utilize the same code and API.

The net effects of unified threading are:

- `pthreads` is now integral to the build process.
- Fewer special SIMH core library builds. All libraries build with thread
  support. `SIM_ASYNCH_IO`, `USE_READER_THREAD` and other preprocessor
  constants are no longer necessary.
- NetBSD uniprocessor: This was a test case raised by Rhialto, where ZIMH's
  uVAX 3900 executed on a single core. _If executing on a single core system,
  turn off threads (set `sim_async_available` to false.)_

## TODO:

- Remove `ETH_THREADING_AVAILABLE` preprocessor conditional code.
- Remove `SIM_ASYNCH_IO` preprocessor conditional code.
- Add `bool sim_async_available` as a global that determines if threads are
  available, i.e., executing on a multicore system. If on a uniprocessor
  system, this will be set to `false` and should gate `sim_async_enabled`.
  - Ensure that `sim_asynch_enabled` cannot be set if `sim_async_available` is
    `false`.
  - Use functions to return the `sim_async_available` value.
- Refactor `sim_defs.h` AIO macros as functions in the `aio_support` library.
  - Eventually, merge `aio_support` into the core SIMH libraries.
- Check `pdp11_xq.{c,h}` for residual `USE_READER_THREAD` and `SIM_ASYNCH_IO`
  code.
- Globally check for residual `SIM_ASYNCH_IO` conditional code.
