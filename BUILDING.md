# Building the Project

This file explains how to build and test the project from a normal developer
checkout using CMake Presets.

The project uses CMake and requires out-of-tree builds managed through unified
configuration, build, and test presets. Do not try to configure directly in
the repository root.

The top-level `Makefile` remains as a thin compatibility wrapper over the
default POSIX build layout. The native interface is completely driven via
CMake Presets and CTest.

## Compiler and OS Support

We support builds for machines running recent vintage POSIX operating systems
(Linux, macOS, FreeBSD) and recent vintage Microsoft Windows (Windows 10 or
newer).

Unlike legacy SIMH forks, _we explicitly do not support_ building on obsolete
operating systems or compilers.

Our goal is to maintain a clean, supportable, and modern codebase that can be
preserved into the far future. We do not provide continuous integration for
unmaintainable architectures, nor do we accept patches that complicate the
codebase for legacy toolchains.

The codebase officially targets the C17 standard, though it is verified to
compile seamlessly under configurations ranging from C99 to C23. Compilers
lacking native C11 support are explicitly unsupported.

## Dependencies

### Core Requirements

At minimum, your host environment must have the following installed:

- A modern C compiler toolchain (C11/C17 capable)
- `cmake` (v3.21 or newer required for full preset compatibility)
- `ninja` or system `make`
- `git` and `bison`
- `libpcre2` (Backend for SCP EXPECT routing commands)
- `pkg-config` / `pkgconf` (Required on POSIX hosts)
- `libuuid` / system UUID generation API

### Optional Feature Packages

To compile with full graphics, console editing, and advanced networking
features, ensure the following are available:

- `SDL2` & `SDL2_ttf` — Windowing, raw input, and high-quality text rendering.
- `freetype` — Embedded font glyph rasterization.
- `libpng` & `zlib` — Image loading and compressed file-handling paths.
- `libedit` — Interactive command-line history and editing (POSIX only).
- `libpcap` — Physical/bridged host network interface packet capture.
- `libslirp` — Local NAT networking capabilities (POSIX >= 4.7.0; Windows >= 4.9.0).
- `libvdeplug` — Virtual Distributed Ethernet backend (POSIX only).

### Test Environment

- `cmocka` (1.1.5 or newer) — Host-side C unit-testing framework.
- Python 3 — Driver engine for system integration tests.

### Installing Dependencies via Script (POSIX and Unixen)

The helper utility scripts below are available to automate dependency
setup across standard environments:

```sh
tools/ci/deps/deps.sh linux        # Debian / Ubuntu apt pipeline
tools/ci/deps/deps.sh osx          # macOS Homebrew pipeline
tools/ci/deps/deps.sh macports     # macOS MacPorts pipeline
```

N.B.: The Github and Gitlab CI/CD pipelines use these utility scripts to
install dependencies, so if they work on CI/CD pipelines, they should work for
you!

### Installing Dependencies on Windows

Windows dependencies split into two categories: external utilities and library
dependencies.

- External utilities, such as `cmake`, `git`, `bison` and `python`/`python3`
  should be installed by an appropriate package manager, such as
  [scoop.sh](https://scoop.sh/).

- [`vcpkg`](https://vcpkg.io/) manages library dependencies, and locally
  installs `SDL2`, `SDL2_ttf`, ..., into the build tree during the `cmake`
  build process.

  __BE SURE TO SET `VCPKG_ROOT` in your environment so that `cmake` understands
  the `vcpkg` toolchain file's location.__

---

## The Preset Workflow

The project defines uniform environments via `CMakePresets.json`. This
eliminates the need to remember explicit compiler choices, binary directories,
or toolchain paths.

### 1. Symmetric Environments (Linux & macOS Ninja/Make)

For standard UNIX targets, the configuration, build, and test preset names
match exactly.

```sh
# Step 1: Configure the tree
cmake --preset ninja-release

# Step 2: Build the targets
cmake --build --preset ninja-release
```

*Generated binaries will land inside `build/ninja/bin/Release/`. For debug
 variants, substitute `ninja-debug` (which outputs to
 `build/ninja/bin/Debug/`).*

### 2. Multi-Config Frameworks (macOS Xcode, Windows MSVC, Ninja/Clang)

Multi-configuration generators use a base configuration layout that handles
building distinct targets cleanly.

#### macOS Xcode:
```sh
# Configure the Xcode project spaces
cmake --preset xcode

# Build a specific configuration layout
cmake --build --preset xcode --config Release
```
*Generated binaries land inside `build/xcode/bin/Release/`.*

#### Windows (Visual Studio 2022 / 2026 with vcpkg):

Windows presets explicitly map static compilation configurations via `vcpkg`
toolchains. Because configurations are bound directly to the build presets,
choose the suffix matching your target:

```pwsh
# Configure using the Visual Studio 2022 preset
cmake --preset windows-vs2022

# Compile either the Debug or Release target variants
cmake --build --preset windows-vs2022-release
cmake --build --preset windows-vs2022-debug
```

*Generated binaries will map to `build/vs2022/bin/Release/` or
 `build/vs2022/bin/Debug/` respectively.*


Additionally, for the Ninja and Clang on Windows connoisseur:

```pwsh
# Configure using the Ninja + Clang multi-configuration generators
cmake --preset windows-ninja-multiconfig

# Compile either the Debug, Release or RelWithDebInfo target variants
cmake --build --preset windows-ninja-release
cmake --build --preset windows-ninja-debug
cmake --build --preset windows-ninja-relwithdebinfo
```

---

## Build Directory Structure

The build system uses a unified directory layout that is both platform-agnostic
and generator-aware, making it simple to locate build artifacts regardless of
your development environment.

### Directory Layout

All build artifacts are organized under a consistent hierarchy that separates
CMake build files from runtime executables:

```
build/
├── ninja/              # Ninja CMake build files
│   └── bin/           # Executables
├── vs2022/            # Visual Studio 2022 build files
│   └── bin/           # Executables
├── vs2026/            # Visual Studio 2026 build files
│   └── bin/           # Executables
├── make/              # Unix Makefiles build files
│   └── bin/           # Executables
└── xcode/             # Xcode build files
    └── bin/           # Executables
```

Within each generator's `bin/` directory, executables are organized by build
configuration:

```
build/{generator}/bin/
├── Debug/              # Debug executables
├── Release/            # Release executables
├── RelWithDebInfo/     # Optimized with debug info
└── sanitizers/
    ├── asan/          # Address Sanitizer
    ├── tsan/          # Thread Sanitizer (Linux only)
    ├── msan/          # Memory Sanitizer (Linux only)
    └── ubsan/         # Undefined Behavior Sanitizer (Linux only)
```

### Rationale

This structure provides several advantages:

- **Clean separation:** CMake build files (.vcxproj, CMakeCache.txt, etc.) are
  kept separate from runtime executables in the `bin/` subdirectory.
- **Platform-agnostic:** The same logical path works across operating systems,
  with only the generator name changing based on your toolchain.
- **Generator-aware:** Different build tools (Ninja, Make, Xcode, Visual
  Studio) each have their own namespace, preventing conflicts when switching
  between generators.
- **Simple artifact discovery:** Executables are always under `{generator}/bin/{config}/`,
  making it easy to locate binaries for testing, debugging, or deployment.
- **Configuration isolation:** Each build configuration (Debug, Release, etc.)
  maintains its own artifact directory, eliminating cross-contamination between
  builds.

### Locating Build Artifacts

To find the executables for a specific configuration, navigate to the
appropriate path under `build/{generator}/bin/`. Here are some examples:

**Linux with Ninja:**
```sh
# Release build
./build/ninja/bin/Release/zimh-pdp11

# Debug build with Address Sanitizer
./build/ninja/bin/sanitizers/asan/zimh-vax
```

**Windows with Visual Studio 2022:**
```pwsh
# Release build
.\build\vs2022\bin\Release\zimh-pdp11.exe

# Debug build
.\build\vs2022\bin\Debug\zimh-vax.exe
```

**Windows with Ninja and Clang:**
```pwsh
# Release build
.\build\bin\ninja\release\zimh-pdp11.exe

# RelWithDebInfo build with Address Sanitizer
.\build\bin\ninja\sanitizers\asan\zimh-vax.exe
```

**macOS with Xcode:**
```sh
# Release build
./build/xcode/bin/Release/zimh-pdp11

# Debug build
./build/xcode/bin/Debug/zimh-vax
```

**Linux with Unix Makefiles:**
```sh
# Release build
./build/make/bin/Release/zimh-pdp11

# Debug build with Thread Sanitizer
./build/make/bin/sanitizers/tsan/zimh-vax
```

This consistent structure means you can quickly construct the path to any
binary by knowing just three pieces of information: the generator you're using,
the configuration you built, and the simulator name.

---

## Overriding Preset Variables & Feature Selection

Presets contain default configurations, but they can be fully customized or
trimmed at configuration time by passing standard `-D` compilation arguments.

### Compiling Without Video/Graphics Support

If your target machine lacks `SDL2_ttf` or a full graphical environment, you
can safely strip video capabilities while utilizing your preset:

```sh
cmake --preset ninja-release -DWITH_VIDEO=Off
cmake --build --preset ninja-release
```

### Common Configuration Toggles

- `-DWITH_VIDEO=Off` — Completely disable SDL structural subsystems (Default:
  `On`).
- `-DWITH_NETWORK=Off` — Strip out all external network code stacks (Default:
  `On`).
- `-DWITH_PCAP=Off` — Disable bridge/pcap network capture layers (Default:
  `On`).
- `-DWARNINGS_FATAL=On` — Escalate all compiler warnings into hard build
  errors (Default: `Off`).
- `-DRELEASE_LTO=On` — Force Link-Time Optimization routines on Release
  compilation (Default: `Off`).
- `-DC_DIALIECT={11|17|23|26}` - Set the C compiler's dialect to C11,
  C17, C23 or C26.
- `-DSTD_EXTENSIONS={On|Off}` - Enable C dialect-specific extensions.

  Note: The ZIMH code does not rely on or use dialect-specific
  extensions, as might be avaiable via `gnu11` in the GNU C
  compiler. Consequently, this configuration variable has very little
  effect. It exists for completeness.

---

## Runtime Sanitizers

Preset names ending in `-asan`, `-tsan`, `-msan` and `-ubsan` enable
the runtime address, thread, memory and undefined behavior sanitizers,
respectively. For example:


```bash
# Configure a Ninja RelWithDebInfo build and the address sanitizer:
cmake --preset ninja-relwithdebinfo-asan
cmake --build ninja-relwithdebinfo
```

```pwsh
# Configure Ninja/Clang on Windows with the address sanitizer:
cmake --preset windows-ninja-asan
cmake --preset windows-ninja-relwithdebinfo
```

The corresponding command line configuration variables are:

- `-DSANITIZE_ADDRESS:Bool=On`: Enable the address sanitizer
- `-DSANITIZE_THREAD:Bool=On`: Enable the thread sanitizer
- `-DSANITIZE_MEMORY:Bool=On`: Enable the memory sanitizer
- `-DSANITIZE_UNDEFINED:Bool=On`: Enable the undefined behavior sanitizer

_Windows note:_ You will need to find and copy the
`clang_rt.asan_dynamic-x86_64.dll` library to the
`build/bin/ninja/sanitizers/asan` directory where the Windows Ninja+ASan
binaries are located. Otherwise, you'll get a pop-up about a missing
DLL. There is no static link option for this library.

---

## Selective Compilation Targets

To avoid compiling the entire matrix of historical simulators, you can isolate
compilation to a specific architecture by targeting it explicitly through the
preset engine:

```sh
cmake --build --preset ninja-release --target pdp11
cmake --build --preset ninja-release --target vax
```

### Key Top-Level Targets

- **Standard Simulator Engine Set:** `cmake --build --preset ninja-release`
- **Experimental Simulators:** `--target experimental-simulators`
- **Host Unit Testing Binary Array:** `--target unit-tests`
- **System Integration Testing Framework:** `--target integration-tests`

*Simulator targets are unprefixed during execution, but compiled output
 filenames are standardized with the `zimh-` prefix (e.g., `zimh-pdp11`).*

---

## Running the Test Suites

The project includes pre-configured **Test Presets** that map directly to your
build layouts. These automated presets encapsulate standard execution
optimization flags, automatically running with the following defaults:

- 8 parallel execution threads (`-j 8`)
- Extended output diagnostics on failure (`--output-on-failure`)
- A 5-minute guard timeout per test item (`--timeout 300`)

Once compilation under a preset concludes successfully, invoke `ctest
--preset` passing the corresponding target environment:

```sh
# For symmetric layout environments (Linux, macOS Ninja)
ctest --preset ninja-release
ctest --preset ninja-debug

# For multi-config or asymmetric environments (Windows MSVC)
ctest --preset windows-vs2022-release
ctest --preset windows-vs2022-debug
```

### Fallback Manual Invocation

If you need to bypass a preset configuration to pass custom `ctest` filtering
flags (like running a specific subset of tests via `-R`), direct `ctest` to
the preset's binary directory manually:

```sh
ctest --test-dir build/ninja/Release -R pdp11 -j 4
```

Alternatively, you can route testing passes straight through the build
architecture pipelines:

```sh
cmake --build --preset ninja-release --target unit-tests
```

### Known Environmental Testing Constraints

When operating inside strictly confined containers or restricted sandbox
execution spaces, you may encounter a failure on the following target:

- `zimh-uc15`

This specific simulator relies heavily on internal POSIX shared memory
interfaces. If your environment or execution platform blocks `shm_open`
routines via a sandbox policy, this test will fail. This indicates an
environmental restriction rather than a structural code defect; execution will
succeed normally on native hardware.

## Safe Tree Reconfigurations

CMake aggressively caches values inside build spaces. If you modify
fundamental feature flags (such as toggling `WITH_VIDEO`) or make significant
modifications to your environment, the most reliable approach is to clean the
preset's defined folder and restart:

```sh
# Wipe and reconfigure a standard layout folder
rm -rf build/ninja/Release
cmake --preset ninja-release
```
