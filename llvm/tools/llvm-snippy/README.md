# LLVM-snippy

LLVM-snippy is a cross-platform random code generator. Generation can be model-based or operate in an unmanaged mode. It supports the generation of loops, function calls, memory patterns, and much more.

## Building and Installing the Generator

In this section, we'll cover the steps for building and installing the llvm-snippy generator.

Suppose you have downloaded the source code into the folder ${SNIPPY_PATH}.

Create a file ${SNIPPY_PATH}/release.cmake.

Also, decide on the installation location. I will denote it as /path/to/install for reference, and will refer to it as ${SNIPPY_INSTALL} from now on.

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

From the ${SNIPPY_PATH} directory, run CMake:

```
> cmake -S llvm -B release/build -G Ninja -C release.cmake
> cmake --build release/build
> cmake --install release/build
```

We haven't open-sourced LIT tests yet. When we do that, there will also be instructions here on how to test the generator.

These steps should get llvm-snippy up and running on your system. If you encounter any issues or have questions, feel free to reach out.

# Quick start guide

In this section, we will create a configuration file, run the generator, and examine the generated results.

## Creating the Configuration File

In this section, we'll guide you through the process of creating a configuration file for llvm-snippy. 

Create a file named layout.yaml with the following content:

```
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

Let's briefly explain the content of the layout.yaml file:

### options

This section allows you to set various command-line options. They can also be set directly in the command line. Here's a breakdown of some options:

- march: Target architecture (e.g., "riscv64-linux-gnu").
- model-plugin: Model plugin to use (e.g., "None").
- num-instrs: Number of instructions to generate.
- o: Base name for the output file.
- init-regs-in-elf: Initialize non-reserved registers in the ELF.

```
march: "riscv64-linux-gnu"
```

The llvm-snippy generator potentially supports a variety of architectures. Currently, the only working backend is RISCV.

```
model-plugin: "None"
```

The llvm-snippy generator also supports various models through a unified interface. Currently, the primary mode is without a model unless you write and integrate your own.

```
num-instrs: 10000
o: "snippet.elf"
```

These options specify the number of instructions and the base name of the output file. You can set num-instrs to "all" and the entire code section will be filled.

```
init-regs-in-elf: true
```

It initializes all non-reserved registers with random values at the beginning of the snippet. This option helps avoid non-determinism due to different register settings before a function call.

### sections

Sections are used to either store code (rx sections) or read and write data (r and rw sections). Full support for rwx sections has not yet been open-sourced.

The section names are arbitrary and conditional—they will be mangled in the final snippet. However, they are useful for understanding the structure.

In the current open-source implementation, there is no distinction between VMA and LMA. The latter is ignored for practical purposes, but it is recommended to specify them as identical for now, just in case.

### histogram

The histogram sets weights for so-called primary instructions. The generator produces two types of instructions—primary ones according to the histogram and auxiliary ones necessary, for example, for memory access or initial register setup. Auxiliary instructions do not participate in probability distribution, and from the histogram perspective, they do not exist.

Weights are relative. A weight of 10.0 means that the primary instruction will occur approximately 10 times more often than an instruction marked with weight 1.0. All weights are summed, and to determine the probability of an instruction, the weight should be divided by the sum of weights.

To see possible instruction names for the histogram, you can use the following command:

```
./llvm-snippy -march=riscv64-linux-gnu -list-opcode-names
```

At the time of open-sourcing, llvm-snippy works with all RISCV instructions available in the LLVM backend.

### access-ranges

Memory access can be defined in several ways. The simplest method is to define an access range. The range includes the start of access, size, and stride. For example, if the start is 0x100000 and the stride is 16, then the valid addresses for access are 0x100000, 0x100010, 0x100020, and so on, up to the size. Additional variability can be introduced through offsets. For instance, if you add first-offset = 0 and last-offset = 2, the valid addresses would be 0x100000, 0x100001, 0x100002, 0x100010, 0x100011, 0x100012, and so forth.

Addresses for load and store instructions are chosen not from sections but from access schemes. Thus, an access scheme can intersect multiple sections or even take addresses completely independently of sections. This is not a bug; it's a feature that allows us to test very interesting scenarios. However, the default use is to apply access schemes to existing sections.

## Running the Generator and Analyzing Results

Now, run snippy.

```
> ${SNIPPY_INSTALL}/llvm-snippy layout.yaml
warning: no instructions seed specified, using auto-generated one: 1703685916769088352
```

You will see two generated files: snippet.elf and snippet.elf.ld

The snippet.elf file is an object file containing the entry point SnippyFunction and both sections that were requested.

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

You can link it with any program that calls this function. Note that the snippet has its own stack in this case. Working with an external stack is also possible but less safe.

To ensure that sections do not change their position during linking, you can see a fragment of the linker script alongside the generated file, which correctly places them in the final program.

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

You can reproduce the same generation by repeating the call with the same seed.

```
> ${SNIPPY_INSTALL}/llvm-snippy --seed=1703685916769088352 layout.yaml
```

Of course, not everything is covered in this quick start. We hope to publish detailed documentation soon. For now, the help command is available.

# Contacts

For questions regarding llvm-snippy and its development, as well as your suggestions, please contact konstantin.vladimirov@syntacore.com

Additionally, merge requests and open issues are welcome.
