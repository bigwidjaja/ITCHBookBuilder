# Phase 0: Setup

[Back to the plan](../ITCH%20Book%20Builder%20Project%20Plan%20and%20Assignment.md)

#### Step 0: Environment and skeleton (S)

**Goal.** A Linux machine and an empty project that builds, tests, and runs in CI.

**Learn.** CMake targets, static libraries, Debug vs Release, apt, bash, GitHub Actions.

**Tasks**

1. Install Ubuntu 24.04 (UTM VM or native), then the toolchain:

   ```
   sudo apt update
   sudo apt install build-essential cmake ninja-build gdb git valgrind \
     python3-dev python3-venv clang-format hyperfine \
     linux-tools-common linux-tools-$(uname -r)
   ```
2. Create the repo layout from Part 1 and push it to GitHub.
3. Write `CMakeLists.txt`: C++20, a static library `itchbook_core`, an executable `itchbook` that links it, and a test executable using GoogleTest through `FetchContent`.
4. Add an option `ITCHBOOK_SANITIZE` that adds `-fsanitize=address,undefined -fno-omit-frame-pointer` to compile and link flags.
5. Make the CLI take a path and print the file size (`std::filesystem::file_size`).
6. Write `scripts/get_data.sh`: download the sample file, decompress it, and create `data/small.itch`.
7. Add `.github/workflows/ci.yml`: configure, build Debug, and run `ctest` on `ubuntu-latest`.

**Deliverable.** One trivial test passing locally and in CI.

**Checkpoint.** These all work:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build
./build/itchbook data/small.itch
```

**Hint.** Put all real code in the library from day one. The CLI, tests, benchmarks, and Python module all link the same library.

Next: [Phase 1: Correct (v1)](phase-1-correct.md)
