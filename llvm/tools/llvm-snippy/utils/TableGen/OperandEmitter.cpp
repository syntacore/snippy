//===-------- OperandGeneratorEmitter.cpp - Generator for Fusion ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===---------------------------------------------------------------------===//

#include "Common/CodeGenTarget.h"
#include "Common/GlobalISel/GlobalISelMatchTable.h"
#include "Common/PredicateExpander.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/TableGen/Error.h"
#include "llvm/TableGen/Record.h"
#include "llvm/TableGen/TableGenBackend.h"
#include <iterator>
#include <vector>

namespace llvm {
namespace snippy {
namespace {

class OperandGeneratorEmitter {
  const RecordKeeper &Records;
  const CodeGenTarget Target;

public:
  struct OperandRenderInfo {
    OperandRenderInfo(std::string Name, size_t NumOperands)
        : RenderName(std::move(Name)), MINumOperands(NumOperands) {}
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
  // method to provide an ability to merge operands
  virtual OperandInfoTy
  processOperands(OperandInfoTy Renders,
                  const CodeGenInstruction &Inst) const = 0;

  virtual bool needImmConstraint(std::string RenderMethod) const = 0;

  virtual void emitImmConstraint(raw_ostream &OS) const = 0;

  virtual bool needRegConstraint(std::string RenderMethod) const = 0;

  virtual void emitRegConstraint(raw_ostream &OS) const = 0;
};

class AArch64OperandEmitter final : public OperandGeneratorEmitter {

  virtual OperandInfoTy
  processOperands(OperandInfoTy Renders,
                  const CodeGenInstruction &Inst) const override {

    for (auto [RenderName, Operand] : llvm::zip_equal(Renders, Inst.Operands)) {
      if (Operand.Rec->isSubClassOf("ComplexPattern")) {
        // NOTE: This renders will produce 2 MachineOperands, but Instrdesc
        // expect only 1 operand
        if (Operand.Name == "Rm_and_shift")
          RenderName = {"addShiftedRegOperands", 1};
        else if (Operand.Name == "Rm_and_extend")
          RenderName = {"addExtendedRegOperands", 1};
      }
    }
    // rewrite pattern matching
    for (auto Render = Renders.begin(); Render != Renders.end();) {
      if (Render->RenderName == "addShifterOperands") {
        // assume addShifterOperands can`t be first method
        assert(Render != Renders.begin());

        if (std::prev(Render)->RenderName == "addImmOperands") {
          std::prev(Render)->RenderName = "addShiftedImmOperands";
          // NOTE: here we erase one renderer, so need to increase MINumOperands
          // to keep total operands count
          std::prev(Render)->MINumOperands += 1;
          Render = Renders.erase(Render);
          continue;
        }
      }
      ++Render;
    }
    return Renders;
  }

  virtual bool needImmConstraint(std::string RenderMethod) const override {
    if (RenderMethod == "addImmOperands")
      return true;

    return false;
  }

  virtual void emitImmConstraint(raw_ostream &OS) const override {}

  virtual bool needRegConstraint(std::string RenderMethod) const override {
    return RenderMethod == "AddRegOperands";
  }

  virtual void emitRegConstraint(raw_ostream &OS) const override {}

public:
  AArch64OperandEmitter(const RecordKeeper &R) : OperandGeneratorEmitter(R) {};
};

class RISCVOperandEmitter final : public OperandGeneratorEmitter {

  virtual OperandInfoTy
  processOperands(OperandInfoTy Renders,
                  const CodeGenInstruction &Inst) const override {
    // No operand processing in RISCV now
    return Renders;
  }

  virtual bool needImmConstraint(std::string RenderMethod) const override {
    if (RenderMethod == "addImmOperands") {
      return true;
    }
    return false;
  }

  virtual void emitImmConstraint(raw_ostream &OS) const override {}

  virtual bool needRegConstraint(std::string RenderMethod) const override {
    return RenderMethod == "AddRegOperands";
  }

  virtual void emitRegConstraint(raw_ostream &OS) const override {}

public:
  RISCVOperandEmitter(const RecordKeeper &R) : OperandGeneratorEmitter(R) {};
};

OperandGeneratorEmitter::OperandInfoTy
OperandGeneratorEmitter::getOperandInfo(const CodeGenInstruction &Inst) {
  OperandInfoTy Result;

  for (const auto &Op : Inst.Operands)
    Result.push_back({resolveRenderMethod(Op), 1});

  return Result;
}

/// Expand an operand into its component operands.
/// Handles both single operands and multi-operand (aggregate) cases.
std::vector<CGIOperandList::OperandInfo>
OperandGeneratorEmitter::expandOperand(const CGIOperandList::OperandInfo &Op) {
  std::vector<CGIOperandList::OperandInfo> OperandList;
  const DagInit *MIOI = Op.MIOperandInfo;

  if (!MIOI || MIOI->getNumArgs() == 0) {
    // Single operand (possibly anonymous).
    OperandList.push_back(Op);
  } else {
    // Multi-operand case (e.g. register pairs).
    for (unsigned j = 0; j < Op.MINumOperands; ++j) {
      OperandList.push_back(Op);
      OperandList.back().Rec = cast<DefInit>(MIOI->getArg(j))->getDef();
    }
  }

  return OperandList;
}

/// Resolve the render method string for a given operand.
/// This is the main part to add new corner cases
std::string OperandGeneratorEmitter::resolveRenderMethod(
    const CGIOperandList::OperandInfo &Operand) {
  const Record *OpR = Operand.Rec;
  const Init *RMName = nullptr;

  if (OpR->isSubClassOf("Operand") || OpR->isSubClassOf("RegisterOperand")) {
    if (const auto *DI =
            dyn_cast<DefInit>(OpR->getValueInit("ParserMatchClass"))) {
      const Record *ParserMatchClass = DI->getDef();
      OpR = ParserMatchClass;
      RMName = ParserMatchClass->getValueInit("RenderMethod");
    } else if (OpR->isSubClassOf("RegisterOperand")) {
      return "addRegOperands"; // if RegisterOperand has no parserMatchClass it
                               // could be crated as simple Reg
    } else {
      // TODO: Basically think that ISA provides implicit render methods
      assert(OpR->getValue("Name"));
      return "add" + std::string(OpR->getValueAsString("Name")) + "Operands";
    }

  } else if (OpR->isSubClassOf("RegisterClass")) {
    return "addRegOperands";

  } else if (OpR->isSubClassOf("AsmOperandClass")) {
    if (OpR->getName() == "unknown")
      assert(0);
    RMName = OpR->getValueInit("RenderMethod");

  } else {
    llvm_unreachable("Unexpected operand type in GetOperandInfo");
  }

  // Resolve RenderMethod field.
  if (const auto *SI = dyn_cast<StringInit>(RMName))
    return SI->getValue().str();

  assert(isa<UnsetInit>(RMName) && "Unexpected non-string RenderMethod!");
  return "add" + std::string(OpR->getValueAsString("Name")) + "Operands";
}

void OperandGeneratorEmitter::emitCaseForInst(raw_ostream &OS,
                                              const CodeGenInstruction &Inst) {

  // Resolve operand renderers once.
  OperandInfoTy Renders = getOperandInfo(Inst);
  Renders = processOperands(Renders, Inst);

  OS << "  case " << Inst.Namespace << "::" << Inst.TheDef->getName()
     << ": {\n";
  unsigned Logical = 0;
  for (const auto &Render : Renders) {
    // TODO: support multiple MIOperands in one logical
    CGIOperandList::OperandInfo Op = Inst.Operands[Logical];

    if (needRegConstraint(Render.RenderName)) {
      // TODO: Add assertion on Op type
      emitRegConstraint(OS);
    }

    if (needImmConstraint(Render.RenderName)) {
      emitImmConstraint(OS);
    }

    // Default: call whatever render the table asked for.
    if (!Render.RenderName.empty()) {
      OS << Render.RenderName << "(Preselected.slice(" << Logical << ", "
         << Render.MINumOperands << "));\n";
    } else {
      llvm_unreachable(llvm::formatv("No render method for Inst: {0}",
                                     Inst.TheDef->getName())
                           .str()
                           .c_str());
    }
    Logical += Render.MINumOperands;
  }
  assert(Logical == Inst.Operands.size());
  OS << "    break;\n";
  OS << "  }\n";
}

void OperandGeneratorEmitter::emitDispatcher(raw_ostream &OS) {
  OS << "/// addOperands - generated dispatcher that renders all operands\n";
  OS << "virtual void "
        "generateOperands(llvm::ArrayRef<planning::PreselectedOpInfo> "
        "Preselected) override "
        "{\n";
  OS << "  switch (InstrDesc.getOpcode()) {\n";
  for (const CodeGenInstruction *I : Target.getInstructions()) {
    if (I->Namespace != Target.getInstNamespace())
      continue;
    if (I->isPseudo)
      continue;
    emitCaseForInst(OS, *I);
  }

  OS << "  default: llvm_unreachable(\"Undefined instruction\");\n";
  OS << "  }\n";
  OS << "}\n\n";
}

void OperandGeneratorEmitter::run(raw_ostream &OS) {
  // Emit file header.
  emitSourceFileHeader("Operand Generators", OS);

  const std::string &TargetName = std::string(Target.getName());
  emitDispatcher(OS);
}
} // namespace
bool emitAArch64Operands(llvm::raw_ostream &OS,
                         const llvm::RecordKeeper &Records) {
  AArch64OperandEmitter Generator(Records);
  Generator.run(OS);
  return false;
}
bool emitRISCVOperands(llvm::raw_ostream &OS,
                       const llvm::RecordKeeper &Records) {
  RISCVOperandEmitter Generator(Records);
  Generator.run(OS);
  return false;
}
} // namespace snippy
} // namespace llvm
