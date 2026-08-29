# LLVM-snippy

LLVM-snippy is a cross-platform random code generator. Generation can be model-based or operate in an unmanaged mode. LLVM-snippy supports loop generation, function calls, memory patterns, and much more.

> [!IMPORTANT]
> Full documentation is available [here](https://llvm-snippy.github.io/llvm-snippy/).

## Building and Installing Generator

This section describes how to build and install LLVM-snippy.

### Dependencies

To build LLVM-snippy you need the following tools:

- CMake (>= 3.20)
- Clang
- Ninja (recommended)
- Python >= 3.8
- ccache (optional but recommended; disable with `-DLLVM_CCACHE_BUILD=OFF`)
- Sphinx (optional; disable with `-DLLVM_ENABLE_SPHINX=OFF`)
- [RVMI](https://github.com/LLVM-Snippy/rvmi) – RISC‑V Model Interface library
- [riscv-isa-sim](https://github.com/LLVM-Snippy/riscv-isa-sim) – Spike RISC‑V ISA simulator (LLVM‑snippy fork)

The last two dependencies are provided as part of the LLVM‑snippy Nix expressions and can be built automatically.
If you prefer a fully manual CMake build, you must build them separately and pass the appropriate paths to CMake (see instructions below).

### Build using Nix (recommended)

The simplest way to build LLVM‑snippy is to use Nix. From the root of the repository run:

```bash
nix-build llvm/tools/llvm-snippy -A llvm-snippy
```

This command builds LLVM‑snippy together with all required dependencies (RVMI, riscv‑isa‑sim, etc.) and places the result in the Nix store. A symlink `result` will be created in the current directory pointing to the build output.

### Build with CMake (manual)

If you need to develop LLVM‑snippy or build it outside of Nix, follow these steps.
You will need to obtain RVMI and riscv‑isa‑sim. The easiest way is to build them using the Nix expressions included in this repository (see below).
Alternatively, you can build them manually by cloning the repositories and following their own build instructions. In that case, you need to provide the correct paths to CMake:
- For RVMI, set `PKG_CONFIG_LIBDIR` (or `PKG_CONFIG_PATH`) to the directory containing `rvmi.pc` (typically `<rvmi-install>/lib/pkgconfig`).
- For riscv‑isa‑sim (Spike), pass `-DRISCVModelSpike_DIR=<path-to-spike-install>/lib` to CMake.

#### 1. Prepare dependencies with Nix

**Build RVMI**

```bash
nix-build llvm/tools/llvm-snippy -A rvmi.dev
```

The command will print a path like `/nix/store/…-rvmi-…-dev` at the end.
Append `lib/pkgconfig` to that path and export it as `PKG_CONFIG_LIBDIR` so CMake can locate RVMI via pkg‑config:

```bash
export PKG_CONFIG_LIBDIR=/nix/store/…-rvmi-…-dev/lib/pkgconfig
```

**Build riscv‑isa‑sim (Spike)**

```bash
nix-build llvm/tools/llvm-snippy -A riscv-isa-sim
```

The command will print a path like `/nix/store/…-riscv-isa-sim-…-dev` at the end.
You will need this path (with `/lib` appended) for the CMake variable `RISCVModelSpike_DIR`.

#### 2. Run CMake

From the `${SNIPPY_PATH}` directory (the root of the LLVM‑snippy checkout) run:

```bash
cmake -S llvm -B build --preset=snippy_basic \
      -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_ENABLE_SPHINX=OFF \
      -DLLVM_CCACHE_BUILD=ON \
      -DRISCVModelSpike_DIR=/nix/store/…-riscv-isa-sim-…-dev/lib
```

Adjust the value of `RISCVModelSpike_DIR` to the actual path printed by the `nix-build` command for riscv‑isa‑sim (or to the location where you installed it manually).
Optionally, you can also pass `-DLLVM_ENABLE_SPHINX=ON` if you want to build documentation (requires Sphinx).

#### 3. Build LLVM‑snippy

```bash
cmake --build build --target llvm-snippy llvm-ie
```

The generated binaries will be placed under `build/bin/`.

## Testing

You can test LLVM-snippy by running LIT tests located in `llvm/test/tools/llvm-snippy`. These tests are based on a common LLVM infrastructure that includes `llvm-lit` and `FileCheck`. For additional information about the LLVM testing infrastructure, refer to the [LLVM Testing Infrastructure Guide](https://llvm.org/docs/TestingGuide.html).

### Dependencies

To run tests, verify you have:

- LLVM-snippy built
- POSIX mandatory utilities (e.g., `grep`, `cat`, etc.)

### Running LLVM-snippy Tests

The test suite should be run twice: once without a model (default) and once with the Spike model enabled. This ensures coverage of both the unmanaged generation mode and the model‑based mode.

First, run the tests without a model:

```bash
cmake --build build/ -t check-llvm-tools-llvm-snippy
```

Then, run the tests with the Spike model enabled:

```bash
LIT_OPTS=-Dsnippy-test-model=spike cmake --build build/ -t check-llvm-tools-llvm-snippy
```

Alternatively, you can run `llvm-lit` directly (adjust the options similarly if you need to test with Spike):

```bash
python3 build/bin/llvm-lit llvm/test/tools/llvm-snippy/
```

We expect that all tests pass with the `passed`, `unsupported`, or `xfail` statuses. If some tests fail for you, let us know by [creating an issue](#contributing-to-llvm-snippy).

Once done, LLVM-snippy should be up and running on your system. If you encounter any issues or have questions, feel free to reach out.

# Contributing to LLVM-snippy

Thank you for showing interest in contributing to LLVM-snippy. There are several ways you can help to make the generator better.

## Bug Reports

We want to know about all LLVM-snippy bugs: segmentation faults, poor or incorrect diagnostics, lack of randomization in the generated test, incorrectly working feature, etc. If you think that you have encountered a bug, file an issue on GitHub. Make sure that you provide the minimum description, reproduction steps, and HEAD commit hash in the issue.

## Bug Fixes

We appreciate your desire to improve LLVM-snippy. You can start working on any open unassigned issue. A comment in the issue is enough to show that you have started working on it. If you do not have bandwidth to work on the issue already assigned to you, let other contributors know by leaving a comment in the issue so it can be reassigned.

If the fix requires design discussion, create an [RFC](#driving-a-major-feature).

Regardless of the fix type, we encourage you to provide a short summary in the issue comments before creating a PR.

## Driving Major Features

If you want to introduce a major change or implement a new feature in LLVM-snippy, create an RFC first. We would like to keep the LLVM-snippy community informed about major changes. We should also reach consensus on all technical and design decisions before any significant work is done.

There is no template for an RFC, so feel free to create it on your own. A good RFC contains:

- Overview
- Proposal
- Pros and cons
- Implementation steps

## Backward Compatibility

We try to keep backward compatibility in LLVM-snippy for our customers. Backward incompatible changes include, but are not limited to:

- Removing an existing option
- Changing an input configuration format
- Amending the signatures of entry functions or globals

These changes require a major release version change and must be discussed in advance. Please create an [RFC](#driving-a-major-feature) for such changes.

## Submitting Patches

When your patch is ready for review, create a pull request. We ask you to follow these rules:

1. Adhere to the [LLVM Coding Standards](https://llvm.org/docs/CodingStandards.html) where applicable.
2. Run `git-clang-format` on your changes.
3. Remove any unrelated changes from your patch.
4. Add at least one lit-test at `llvm/test/tools/llvm-snippy` if possible.
5. Add unit tests at `llvm/unittests/tools/llvm-snippy` if you can.
6. Make a single commit.
7. Create a PR from your fork, or use a branch that starts with `users/<username>/`.

You can request a review by mentioning people in the PR comments. Best candidates for review are developers who have contributed in the area your patch concerns. Keep in mind that the usual review period is one week, so do not ping more often.

Once your patch is reviewed and approved, you can merge the change. If you do not have the rights, let the maintainers know so they can merge the change on your behalf.

Feel free to participate in any review you are interested in.

# Contacts

Use [GitHub issues and pull requests](#contributing-to-llvm-snippy) to interact with the LLVM-snippy community, make suggestions, and ask questions.
