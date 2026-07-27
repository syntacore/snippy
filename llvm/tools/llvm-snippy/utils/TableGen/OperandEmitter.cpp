//===-------- OperandEmitter.cpp - Table-driven operand generator ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===---------------------------------------------------------------------===//

#include "Common/CodeGenTarget.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Regex.h"
#include "llvm/TableGen/Error.h"
#include "llvm/TableGen/Record.h"
#include "llvm/TableGen/TableGenBackend.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace llvm {
namespace snippy {
namespace {

static std::string sanitizeMethodToEnumName(llvm::StringRef RenderMethod) {
  llvm::StringRef S = RenderMethod;
  if (S.starts_with("add"))
    S = S.drop_front(3);
  if (S.ends_with("Operands"))
    S = S.drop_back(8);
  else if (S.ends_with("Operand"))
    S = S.drop_back(7);
  if (S.empty())
    return "Unknown";

  std::string Result = S.str();
  static const llvm::Regex R("[<>:, ]");
  std::string New;
  while ((New = R.sub("_", Result)) != Result)
    Result = std::move(New);

  llvm::StringRef Trimmed = Result;
  Trimmed = Trimmed.ltrim('_').rtrim('_');
  return Trimmed.empty() ? "Unknown" : Trimmed.str();
}

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
  using OperandInfoTy = std::vector<OperandRenderInfo>;

  // Maps render method string -> enum value name
  using RenderKindMap = std::map<std::string, std::string>;
  // Ordered list of (method string, enum name) pairs (insertion order)
  using RenderKindList = std::vector<std::pair<std::string, std::string>>;

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

  // Collect all unique render methods across target instructions (post-merge).
  // Also caches per-instruction OperandInfoTy for reuse in later emit steps.
  void
  collectRenderKinds(RenderKindMap &MethodToEnum, RenderKindList &OrderedKinds,
                     std::map<std::string, OperandInfoTy> &InstrRenderCache);

  void emitRenderKindEnum(raw_ostream &OS, StringRef TgtName,
                          const RenderKindList &OrderedKinds);

  void emitStructDefs(raw_ostream &OS, StringRef TgtName);

  void emitPerInstrArrays(raw_ostream &OS, StringRef TgtName,
                          const RenderKindMap &MethodToEnum,
                          const std::map<std::string, OperandInfoTy> &Cache);

  void emitIndexTable(raw_ostream &OS, StringRef TgtName,
                      const std::map<std::string, OperandInfoTy> &Cache);

  void emitGenerateOperandsFunc(raw_ostream &OS, StringRef TgtName,
                                const RenderKindList &OrderedKinds);

  // Target-specific: merge/reorder operands after initial resolution.
  virtual OperandInfoTy processOperands(OperandInfoTy Renders,
                                        const CodeGenInstruction &) const {
    return Renders;
  }
};

class AArch64OperandEmitter final : public OperandGeneratorEmitter {
  OperandInfoTy processOperands(OperandInfoTy Renders,
                                const CodeGenInstruction &Inst) const override {
    for (auto [RenderInfo, Operand] : llvm::zip_equal(Renders, Inst.Operands)) {
      if (Operand.Rec->isSubClassOf("ComplexPattern")) {
        // NOTE: This renders will produce 2 MachineOperands, but Instrdesc
        // expect only 1 operand
        if (Operand.Name == "Rm_and_shift")
          RenderInfo = {"addShiftedRegOperands", 1};
        else if (Operand.Name == "Rm_and_extend")
          RenderInfo = {"addExtendedRegOperands", 1};
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

public:
  explicit AArch64OperandEmitter(const RecordKeeper &R)
      : OperandGeneratorEmitter(R) {}
};

class RISCVOperandEmitter final : public OperandGeneratorEmitter {
public:
  explicit RISCVOperandEmitter(const RecordKeeper &R)
      : OperandGeneratorEmitter(R) {}
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
      const Record *PMC = DI->getDef();
      OpR = PMC;
      RMName = PMC->getValueInit("RenderMethod");
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
    llvm_unreachable("Unexpected operand type in resolveRenderMethod");
  }

  // Resolve RenderMethod field.
  if (const auto *SI = dyn_cast<StringInit>(RMName))
    return SI->getValue().str();

  assert(isa<UnsetInit>(RMName) && "Unexpected non-string RenderMethod!");
  return "add" + std::string(OpR->getValueAsString("Name")) + "Operands";
}

void OperandGeneratorEmitter::collectRenderKinds(
    RenderKindMap &MethodToEnum, RenderKindList &OrderedKinds,
    std::map<std::string, OperandInfoTy> &InstrRenderCache) {
  std::set<std::string> UsedEnumNames;
  for (const CodeGenInstruction *I : Target.getInstructions()) {
    if (I->Namespace != Target.getInstNamespace())
      continue;
    if (I->isPseudo)
      continue;

    OperandInfoTy Renders = getOperandInfo(*I);
    Renders = processOperands(Renders, *I);
    InstrRenderCache[I->TheDef->getName().str()] = Renders;

    for (const auto &Render : Renders) {
      if (MethodToEnum.count(Render.RenderName))
        continue;
      std::string EnumName = sanitizeMethodToEnumName(Render.RenderName);
      std::string FinalName = EnumName;
      unsigned Suffix = 2;
      while (UsedEnumNames.count(FinalName))
        FinalName = EnumName + std::to_string(Suffix++);
      UsedEnumNames.insert(FinalName);
      MethodToEnum[Render.RenderName] = FinalName;
      OrderedKinds.push_back({Render.RenderName, FinalName});
    }
  }
}

void OperandGeneratorEmitter::emitRenderKindEnum(raw_ostream &OS,
                                                 StringRef TgtName,
                                                 const RenderKindList &Kinds) {
  OS << "enum class " << TgtName << "RenderKind : uint8_t {\n";
  for (const auto &[Method, Name] : Kinds)
    OS << "  " << Name << ", // " << Method << "\n";
  OS << "};\n\n";
}

void OperandGeneratorEmitter::emitStructDefs(raw_ostream &OS,
                                             StringRef TgtName) {
  OS << "struct " << TgtName << "OpDesc {\n";
  OS << "  " << TgtName << "RenderKind Kind;\n";
  OS << "  uint8_t MINumOps;\n";
  OS << "};\n\n";

  OS << "struct " << TgtName << "InstrOpEntry {\n";
  OS << "  const " << TgtName << "OpDesc *Descs;\n";
  OS << "  uint8_t NumDescs;\n";
  OS << "  bool IsValid;\n";
  OS << "};\n\n";
}

void OperandGeneratorEmitter::emitPerInstrArrays(
    raw_ostream &OS, StringRef TgtName, const RenderKindMap &MethodToEnum,
    const std::map<std::string, OperandInfoTy> &Cache) {
  for (const auto &[InstName, Renders] : Cache) {
    if (Renders.empty())
      continue;
    OS << "static const " << TgtName << "OpDesc " << TgtName << "_" << InstName
       << "_descs[] = {\n";
    for (const auto &R : Renders) {
      OS << "  {" << TgtName << "RenderKind::" << MethodToEnum.at(R.RenderName)
         << ", " << R.MINumOperands << "},\n";
    }
    OS << "};\n";
  }
  OS << "\n";
}

void OperandGeneratorEmitter::emitIndexTable(
    raw_ostream &OS, StringRef TgtName,
    const std::map<std::string, OperandInfoTy> &Cache) {
  const auto &Instrs = Target.getInstructions();
  const size_t NumInstr = Instrs.size();

  OS << "static const " << TgtName << "InstrOpEntry " << TgtName << "_OpTable["
     << NumInstr << "] = {\n";

  for (const CodeGenInstruction *I : Instrs) {
    const bool InTargetNS = (I->Namespace == Target.getInstNamespace());
    if (!InTargetNS || I->isPseudo) {
      OS << "  {nullptr, 0, false},\n";
      continue;
    }
    auto It = Cache.find(I->TheDef->getName().str());
    assert(It != Cache.end() && "Instruction missing from render cache");
    const OperandInfoTy &Renders = It->second;
    if (Renders.empty()) {
      OS << "  {nullptr, 0, true},\n";
    } else {
      OS << "  {" << TgtName << "_" << I->TheDef->getName() << "_descs, "
         << Renders.size() << ", true},\n";
    }
  }
  OS << "};\n\n";
}

void OperandGeneratorEmitter::emitGenerateOperandsFunc(
    raw_ostream &OS, StringRef TgtName, const RenderKindList &OrderedKinds) {
  OS << "inline void " << TgtName << "OpndGenerator::generateOperands(\n";
  OS << "    llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {\n";
  OS << "  const " << TgtName << "InstrOpEntry &Entry =\n";
  OS << "      " << TgtName << "_OpTable[InstrDesc.getOpcode()];\n";
  OS << "  assert(Entry.IsValid &&\n";
  OS << "         \"Pseudo or non-" << TgtName.str()
     << " instruction in operand generator\");\n";
  OS << "  unsigned Logical = 0;\n";
  OS << "  for (unsigned I = 0; I < Entry.NumDescs; ++I) {\n";
  OS << "    const " << TgtName << "OpDesc &D = Entry.Descs[I];\n";
  OS << "    auto Slice = Preselected.slice(Logical, D.MINumOps);\n";
  OS << "    switch (D.Kind) {\n";
  for (const auto &[Method, EnumName] : OrderedKinds) {
    OS << "    case " << TgtName << "RenderKind::" << EnumName << ":\n";
    OS << "      " << Method << "(Slice);\n";
    OS << "      break;\n";
  }
  OS << "    default: llvm_unreachable(\"Unknown RenderKind\");\n";
  OS << "    }\n";
  OS << "    Logical += D.MINumOps;\n";
  OS << "  }\n";
  OS << "}\n";
}

void OperandGeneratorEmitter::run(raw_ostream &OS) {
  emitSourceFileHeader("Operand Tables", OS);

  const std::string TgtName = std::string(Target.getName());

  RenderKindMap MethodToEnum;
  RenderKindList OrderedKinds;
  std::map<std::string, OperandInfoTy> InstrRenderCache;
  collectRenderKinds(MethodToEnum, OrderedKinds, InstrRenderCache);

  emitRenderKindEnum(OS, TgtName, OrderedKinds);
  emitStructDefs(OS, TgtName);
  emitPerInstrArrays(OS, TgtName, MethodToEnum, InstrRenderCache);
  emitIndexTable(OS, TgtName, InstrRenderCache);
  emitGenerateOperandsFunc(OS, TgtName, OrderedKinds);
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
