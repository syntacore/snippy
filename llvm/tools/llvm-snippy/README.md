# LLVM-snippy

LLVM-snippy is a cross-platform random code generator. Generation can be model-based or operate in an unmanaged mode. LLVM-snippy supports loop generation, function calls, memory patterns, and much more.

## Building and Installing Generator

In this section, we will cover the steps for building and installing LLVM-snippy.

1. Download the source code. For the presentation purposes, we assume that you download it to the `${SNIPPY_PATH}` directory.
2. In `${SNIPPY_PATH}`, create the `release.cmake` file.
3. Select the installation location. We will denote it as `/path/to/install` for reference, and will refer to it as `${SNIPPY_INSTALL}` from now on.

   ```
   set(CMAKE_BUILD_TYPE RELEASE CACHE STRING "")
   set(CMAKE_INSTALL_PREFIX "/path/to/install" CACHE STRING "")
   set(CMAKE_C_COMPILER clang CACHE STRING "")
   set(CMAKE_CXX_COMPILER clang++ CACHE STRING "")
   set(LLVM_ENABLE_LLD ON CACHE BOOL "")
   set(LLVM_ENABLE_PROJECTS
     lld
     CACHE STRING "")
   set(CLANG_BUILD_EXAMPLES OFF CACHE BOOL "")
   set(LLVM_BUILD_TESTS OFF CACHE BOOL "")
   set(LLVM_ENABLE_ASSERTIONS ON CACHE BOOL "")
   set(LLVM_OPTIMIZED_TABLEGEN ON CACHE BOOL "")
   set(LLVM_CCACHE_BUILD ON CACHE BOOL "")
   set(LLVM_TARGETS_TO_BUILD
     RISCV CACHE STRING "")
   set(CLANG_ENABLE_STATIC_ANALYZER OFF CACHE BOOL "")
   set(CLANG_ENABLE_ARCMT OFF CACHE BOOL "")
   set(LLVM_BUILD_SNIPPY ON CACHE BOOL "")
   ```

4. From the `${SNIPPY_PATH}` directory, run CMake:
   
   ```
   > cmake -S llvm -B release/build -G Ninja -C release.cmake
   > cmake --build release/build
   > cmake --install release/build
   ```

## Testing

You can test LLVM-snippy by running LIT tests located in `llvm/test/tools/llvm-snippy`. These tests are based on a common LLVM infrastructure that includes llvm-lit and FileCheck. For the additional information about the LLVM testing infrastructure, refer to [LLVM Testing Infrastructure Guide](https://llvm.org/docs/TestingGuide.html).

### Prerequisites

To run tests, verify you have:

 * LLVM-snippy built
 * Python 3.6 or later 
 * POSIX mandatory utilities (for example, grep, cat, etc.)

### Running LLVM-snippy Tests
Use the following command:

```
> cmake --build release/build/ --target check-llvm-tools-llvm-snippy
```

Alternatively, you can run llvm-lit directly:

```
> python3 ${SNIPPY_BUILD}/bin/llvm-lit llvm/test/tools/llvm-snippy/
```

We expect that all tests pass with the `passed`, `unsupported`, or `xfail` statuses. If some of the tests fail for you, let us know by [creating an issue](#contributing-to-llvm-snippy).

Once done, LLVM-snippy should be up and running on your system. If you encounter any issues or have questions, feel free to reach out.

# Quick Start Guide

In this section, we will:

1. [Create a configuration file](#creating-configuration-file). 
2. [Run the generator and analyze the generated results](#running-generator-and-analyzing-results).

## Creating Configuration File

Configuration that you run through LLVM-snippy can consist of one or more YAML files with configuration details provided in specific sections. For the presentation purposes, you will use one configuration file `layout.yaml`.

Create a YAML file `layout.yaml` with the following content:

```yaml
options:
  march: "riscv64-linux-gnu"
  model-plugin: "None"
  num-instrs: 10000
  o: "snippet.elf"
  init-regs-in-elf: true
  honor-target-abi: true
  stack-size: 1024
  last-instr: "RET"

sections:
  - name:      text
    VMA:       0x210000
    SIZE:      0x100000
    LMA:       0x210000
    ACCESS:    rx
  - name:      data
    VMA:       0x100000
    SIZE:      0x100000
    LMA:       0x100000
    ACCESS:    rw

histogram:
    - [ADD, 1.0]
    - [ADDI, 1.0]
    - [SUB, 1.0]
    - [SRA, 1.0]
    - [SRAI, 1.0]
    - [SRL, 1.0]
    - [SRLI, 1.0]
    - [SLL, 1.0]
    - [SLLI, 1.0]
    - [AND, 1.0]
    - [ANDI, 1.0]
    - [OR, 1.0]
    - [ORI, 1.0]
    - [XOR, 1.0]
    - [XORI, 1.0]
    - [LW, 10.0]
    - [SW, 10.0]

access-ranges:
   - start: 0x100000
     size: 0x10000
     stride: 16
     first-offset: 0
     last-offset: 2
```

This way, the `layout.yaml` file contains the following parts:

- [`options`](#options)
- [`sections`](#sections)
- [`histogram`](#histogram)
- [`access-ranges`](#access-ranges)

See the chapters that follow for brief descriptions of each part.

### options

In `options`, you provide configuration details for the generator run. 

>Note: You can also set options directly in the command line. 
 
Following is a breakdown of some options:

- `march`: Target architecture. For example: `riscv64-linux-gnu`.
  
   The LLVM-snippy generator potentially supports a variety of architectures. Currently, the only working backend is RISC-V.
- `model-plugin`: Hardware model plugin you want to use. For example: `None` (this disables snippet execution on a model).

  Though LLVM-snippy supports various models through a unified interface, currently, the primary mode is without a model unless you write and integrate your own.

- `num-instrs`: Number of instructions to generate.

  To get an entire code section filled, set `num-instrs` to `all`: it fills the executable section with instructions and generates them until the target executable section is full.
- `o`: Base name for the output file.
- `init-regs-in-elf`: Parameter that initializes non-reserved registers in the .elf file. If you set it to `true`, it initializes all non-reserved registers with random values at the beginning of the snippet. This option helps avoid non-determinism due to different register settings before a function call.

### sections

`sections` provides a list of sections for the output file with specific details for each entry.

Sections are used to either store code (`rx` sections) or read and write data (`r` and `rw` sections). Full support for `rwx` sections has not yet been open-sourced.

The section names are arbitrary and conditional—they will be mangled in the final snippet. However, they are useful for understanding the structure.

In the current open-source implementation, there is no distinction between VMA and LMA. The latter is ignored for practical purposes, but it is recommended to specify them as identical for now, just in case.

### histogram

`hustogram` covers a list of instructions you want snippy to use and their relative weight. A histogram only sets weights for so-called primary instructions.

LLVM-snippy generates two types of instructions: 

- Primary. They are generated according to the histogram. 
- Auxiliary. They are required, for example, for memory access or initial register setup. Auxiliary instructions do not participate in probability distribution, and from the histogram perspective, they do not exist.

Weights are relative. A weight of `10.0` means that the primary instruction will occur approximately 10 times more often than an instruction marked with weight `1.0`. All weights are summed, and to determine the probability of an instruction, the weight must be divided by the sum of the weights.

To see possible instruction names for the histogram, you can use the following command:

```
./llvm-snippy -march=riscv64-linux-gnu -list-opcode-names
```

At the time of publication, open-source LLVM-snippy works with all RISC-V instructions available in the LLVM backend.

### access-ranges

You can define memory access in several ways. The simplest method is to define an access range. 

An access range includes the start of access, size, and stride. For example, if the start is `0x100000` and the stride is `16`, then the valid addresses for the access are `0x100000`, `0x100010`, `0x100020`, and so on, up to the size. 

You can also introduce additional variability through offsets. For example, if you set `first-offset: 0` and `last-offset: 2`, the valid addresses would be `0x100000`, `0x100001`, `0x100002`, `0x100010`, `0x100011`, `0x100012`, and so on.

Addresses for load and store instructions are selected not from sections, but from access schemes. So, an access scheme can intersect multiple sections or even take addresses completely independently of the sections. This is not a bug: it's a feature that allows us to test particularly interesting scenarios. However, the default use is to apply access schemes to existing sections.

## Running Generator and Analyzing Results

### Running LLVM-snippy

Once done with the configuration file, you can run snippy based on it.

```
> ${SNIPPY_INSTALL}/llvm-snippy layout.yaml
warning: no instructions seed specified, using auto-generated one: 1703685916769088352
```

### Reviewing Generation Results 

As a result, you see two generated files: `snippet.elf` and `snippet.elf.ld`.

`snippet.elf` is an object file that contains the entry point `SnippyFunction` and both sections that you requested via the `sections` part of the configuration file:

```
Sections:
Idx Name             Size      VMA               LMA               File off  Algn
  0 .snippy.stack.rw 00000400  0000000000000000  0000000000000000  00000040  2**0
                  ALLOC, READONLY
  1 .snippy.data.rw  00100000  0000000000000000  0000000000000000  00000040  2**0
                  ALLOC, READONLY
  2 .snippy.text.rx  00015438  0000000000000000  0000000000000000  00000040  2**2
                  CONTENTS, ALLOC, LOAD, READONLY, CODE
```

The code is located in the code section.

```
    5 Disassembly of section .snippy.text.rx:
    6
    7 0000000000000000 <SnippyFunction>:
    8        0: 00100e37            lui t3,0x100
    9        4: ff0e0e1b            addiw t3,t3,-16
   10        8: 002e3023            sd  sp,0(t3)
   11        c: 00100137            lui sp,0x100
   12       10: ff01011b            addiw sp,sp,-16
   13       14: ff010113            addi  sp,sp,-16
...
21777    15424: 01010113            addi  sp,sp,16
21778    15428: 00100537            lui a0,0x100
21779    1542c: ff05051b            addiw a0,a0,-16
21780    15430: 00053103            ld  sp,0(a0)
21781    15434: 00008067            ret
```

You can link it with any program that calls this function. Note that the snippet has its own stack in this case. Working with an external stack is also possible, but less safe.

To verify that sections do not change their positions during linking, you can see a fragment of the linker script alongside the generated file, which correctly places them in the final program:

```
MEMORY {
  SNIPPY (rwx) : ORIGIN = 1047552, LENGTH = 2229248
}
SECTIONS {
  .snippy.stack.rw 1047552 (NOLOAD) : {
  KEEP(*(.snippy.stack.rw))
} >SNIPPY
  .snippy.data.rw 1048576 (NOLOAD) : {
  KEEP(*(.snippy.data.rw))
} >SNIPPY
  .snippy.text.rx 2162688: {
  KEEP(*(.snippy.text.rx))
} >SNIPPY
}
```
### Reproducing Generation 

You can reproduce the same generation by repeating the call with the same seed used in the initial run:

```
> ${SNIPPY_INSTALL}/llvm-snippy --seed=1703685916769088352 layout.yaml
```

### Calling Help

Of course, not everything is covered in this quick start. We hope to publish a more detailed documentation soon. For now, you can see all available snippy parameters and options in its embedded help. To open it, run:

```
> ${SNIPPY_INSTALL}/llvm-snippy --help
```

# Contributing to LLVM-snippy

Thank you for showing interest in contributing to LLVM-snippy. There are several ways you can help to make the generator better.

## Bug Reports

We want to know about all LLVM-snippy bugs: segmentation faults, poor or incorrect diagnostics, lack of randomization in the generated test, incorrectly working feature, etc. If you think that you have encountered a bug, file an issue on GitHub. Make sure that you provide the minimum description, reproduction and HEAD commit hash in the issue.

## Bug Fixes

We appreciate your desire to improve LLVM-snippy. You can start working on any open unassigned issue. A comment in the issue is enough to show that you have started working on it. If you do not have bandwidth to work on the issue already assigned to you, let other contributes know by leaving a comment in the issue, so it can be reassigned.

If the fix requires design discussion, create an [RFC](#driving-a-major-feature).

Regardless of the fix type, we encourage you to provide a short summary in the issue comments before creating a PR.

## Driving Major Features

If you want to introduce a major change or implement a new feature in LLVM-snippy, create an RFC first. We would like to keep LLVM-snippy community informed about major changes. We should also reach consensus on all technical and design decisions before any significant work is done.

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

These changes require a major release version change and must be discussed in prior. Please, create an [RFC](#driving-a-major-feature) for such changes.

## Submitting Patches

When your patch is ready for review, create a pull request. We ask you to follow these rules:
1. Adhere to the [LLVM Coding Standards](https://llvm.org/docs/CodingStandards.html) where applicable.
2. Run `git-clang-format` on your changes.
3. Remove any unrelated changes from your patch.
4. Add at least one lit-test if possible.
5. Make a single commit.
6. Create a PR from your fork or use a branch that starts from `users/<username>/`.

You can request a review by mentioning people in the PR comments. Best candidates for review are developers who have contributed in the area your patch concerns. Keep in mind that the usual review period is one week, so do not ping more often.

Once your patch is reviewed and approved, you can merge the change. If you do not have the rights, let the maintainers know, so they could merge the change on your behalf.

Feel free to participate in any review you are interested in.

# Contacts

Use [GitHub issues and pull-requests](#contributing-to-llvm-snippy) to interact with the LLVM-snippy community, make suggestions and ask questions.
