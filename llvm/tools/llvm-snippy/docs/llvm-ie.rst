LLVM-IE Documentation
#####################

Overview
========

``llvm-ie`` is a utility tool designed to conveniently extract instructions from various computer architectures supported by LLVM based on specific criteria.

In LLVM, a wide range of backend components—including instructions, registers, and scheduling models—is defined in TableGen (.td) files. The build process compiles these descriptions into the C++ data structures that form the backend's internal representation.

``llvm-ie`` specifically queries the information derived from instruction definitions, which contains rich details such as whether an instruction is a branch, a call, or accesses memory.

Initially, ``llvm-ie`` was created to extract RISC-V instructions belonging to specific RISC-V extensions. However, its functionality was later expanded to include architecture-independent filters and a modular design for adding new architecture backends.

While the tool was originally developed as a helper utility for ``llvm-snippy``, which uses it to eliminate boilerplate code, we believe ``llvm-ie`` can be useful in other scenarios as well.

Running llvm-ie
===============

To use ``llvm-ie``, you must explicitly specify the target backend using the ``-arch`` option.

For example, to list instructions corresponding to the "A" extension for the RISC-V architecture, use the following command:

.. code-block:: bash

   llvm-ie -arch=riscv -riscv-ext "a"

.. important::
    Currently, ``llvm-ie`` only supports the RISC-V backend.

Target-Independent Options
==========================

Currently, ``llvm-ie`` supports the following architecture-independent filters:

*   ``-memory-access``: Filters for instructions that perform memory accesses (loads/stores).
*   ``-control-flow``: Filters for instructions that affect control flow (jumps, branches, calls).

Target-Dependent Options
========================

RISC-V Options
--------------

Currently, ``llvm-ie`` supports the following RISC-V-specific options:

*   ``-riscv-ext``: Filters instructions by RISC-V extensions using expression logic.
*   ``-rv32``: Restricts the output to instructions available in the RV32 base instruction set.
*   ``-rv64``: Restricts the output to instructions available in the RV64 base instruction set.

Extension Expressions
---------------------

The ``-riscv-ext`` option is the most powerful, as it allows you to get instructions belonging to specific RISC-V extensions. This option takes an expression composed of RISC-V extension names as operands and three basic operations:

*   ``+`` : Union (OR)
*   ``-`` : Exclusion (NOT)
*   ``&`` : Intersection (AND)

You can draw an analogy to set algebra: extension names are sets, and the basic operations ``+``, ``-``, and ``&`` correspond to union, set difference, and intersection, respectively.

Operation Notes and Caveats
~~~~~~~~~~~~~~~~~~~~~~~~~~~

1.  **Supported Operations:** Only these three operations (``+``, ``-``, ``&``) are currently supported. Parentheses and other operations are not available.
2.  **Precedence and Syntax:** The ``&`` operator has higher precedence than ``+`` and ``-``. To simplify the parser, the ``&`` operator must be written without spaces between its operands (e.g., ``"c&d"`` instead of ``"c & d"``).
3.  **Parser Behavior:** The current parser is designed to be permissive and may output extra instructions. This was a deliberate decision to avoid accidentally filtering out required instructions. For example, when asking for the "V" extension, ``llvm-ie`` might also include instructions that require both the "V" and "F" extensions to be present.
    To get a more precise output, you can use more complex expressions. For instance, to request the "V" extension but exclude instructions that also require the "F" extension, you could use:

    .. code-block:: bash

       llvm-ie -arch=riscv -riscv-ext "v - f"
       
Usage Examples
==============

.. code-block:: bash

   # Instructions requiring both C and D extensions
   llvm-ie -arch=riscv -riscv-ext "d&c"

   # Vector instructions excluding D and F extensions  
   llvm-ie -arch=riscv -riscv-ext "v - d - f"

   # Compressed instructions that access memory
   llvm-ie -arch=riscv -riscv-ext "c" -memory-access

   # RV32 control-flow instructions from I, M, C, B extensions
   llvm-ie -arch=riscv -riscv-ext "i + m + c + b" -rv32 -control-flow
