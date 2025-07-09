//===-- RegsReservedForLoop.h -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// If conditional branch instr is used as a first terminator in exiting block
/// of loop, there are registers (for example, [CounterReg, LimitReg]) that have
/// to be reserved for all blocks that are dominated by preheader and
/// postdominated by exiting block. Loop's generation algorithm reserves only
/// the minimal required number of registers, so loops by construction require
/// from one to two registers to be used.
///
//===----------------------------------------------------------------------===//

#pragma once

// we store reserved regs in SmallVector
constexpr unsigned CounterRegIdx = 0;
constexpr unsigned LimitRegIdx = 1;

constexpr unsigned MinNumOfReservedRegsForLoop = 1;
constexpr unsigned MaxNumOfReservedRegsForLoop = 2;
