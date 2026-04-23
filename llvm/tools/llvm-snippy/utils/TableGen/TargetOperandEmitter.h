//===-- TargetOperandEmitter.h ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_TARGETOPERANDEMITTER_H
#define LLVM_CODEGEN_TARGETOPERANDEMITTER_H

#include "Common/CodeGenTarget.h"
#include "Common/PredicateExpander.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/TableGen/Error.h"
#include "llvm/TableGen/Record.h"
#include "llvm/TableGen/TableGenBackend.h"
#include <vector>

namespace llvm {
namespace snippy {

class OperandGeneratorEmitter {
  const RecordKeeper &Records;
  const CodeGenTarget Target;

public:
  struct OperandRenderInfo {
    OperandRenderInfo(std::string name, size_t NumOperands)
        : RenderName(std::move(name)), MINumOperands(NumOperands) {}
    std::string RenderName;
    size_t MINumOperands;
  };
  typedef std::vector<OperandRenderInfo> OperandInfoTy;
  typedef std::vector<OperandInfoTy> OperandInfoListTy;

  OperandGeneratorEmitter(const RecordKeeper &R) : Records(R), Target(R) {}
  virtual ~OperandGeneratorEmitter() = default;
  OperandInfoTy getOperandInfo(const CodeGenInstruction &Inst);
  std::string resolveRenderMethod(const CGIOperandList::OperandInfo &Operand);
  std::vector<CGIOperandList::OperandInfo>
  expandOperand(const CGIOperandList::OperandInfo &Op);

  void run(raw_ostream &OS);

private:
  static bool hasInit(const Record *R, StringRef Field) {
    if (!R)
      return false;
    if (auto *V = R->getValue(Field))
      return !isa<UnsetInit>(V->getValue());
    return false;
  }

  void emitPrologue(raw_ostream &OS);
  void emitOperandHelpers(raw_ostream &OS);
  void emitDispatcher(raw_ostream &OS);
  void emitCaseForInst(raw_ostream &OS, const CodeGenInstruction &Inst);

  // platform specific methods
  //  method to provide an ability to merge operands
  virtual OperandInfoTy
  processOperands(OperandInfoTy Renders,
                  const CodeGenInstruction &Inst) const = 0;

  virtual bool needImmConstraint(std::string RenderMethod) const = 0;

  virtual void emitImmConstraint(raw_ostream &OS) const = 0;

  virtual bool needRegConstraint(std::string RenderMethod) const = 0;

  virtual void emitRegConstraint(raw_ostream &OS) const = 0;
};

} // namespace snippy
} // namespace llvm

#endif // LLVM_CODEGEN_TARGETOPERANDEMITTER_H
