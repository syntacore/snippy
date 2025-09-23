//===- RISCVInstrEnumerator.cpp - Generate lists of RISC-V instrs ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/RISCVISAUtils.h"
#include "llvm/TableGen/Error.h"
#include "llvm/TableGen/Record.h"
#include "llvm/TableGen/TableGenBackend.h"

#include "InstructionFeature.h"

using namespace instr_enumerator_tblgen;

static llvm::StringRef getExtensionName(const llvm::Record *R) {
  llvm::StringRef Name = R->getValueAsString("Name");
  Name.consume_front("experimental-");
  return Name;
}

/// This function interates over instruction's predicates and selects only
/// RISC-V extension predicates from them.
/// \param [in] InstructionPredicates
///     Instruction's predicates to iterate over.
/// \return
///     Vector of RISC-V extension predicates
static std::vector<const llvm::Record *>
getExtensionPredicatesFromInstructionPredicates(
    const std::vector<const llvm::Record *> &InstructionPredicates) {
  std::vector<const llvm::Record *> ExtensionPredicates{};
  llvm::copy_if(
      InstructionPredicates, std::back_inserter(ExtensionPredicates),
      [](const llvm::Record *Predicate) {
        if (!Predicate->getValueAsBit("AssemblerMatcherPredicate"))
          return false;

        const llvm::DagInit *Condition =
            Predicate->getValueAsDag("AssemblerCondDag");
        if (Condition->getNumArgs() == 0)
          llvm::PrintFatalError(Predicate->getLoc(),
                                "Condition has zero arguments!");

        return llvm::all_of(Condition->getArgs(), [](const llvm::Init *Arg) {
          return llvm::isa<llvm::DefInit>(Arg) &&
                 llvm::cast<llvm::DefInit>(Arg)->getDef()->isSubClassOf(
                     "RISCVExtension");
        });
      });
  return ExtensionPredicates;
}

/// Parse RISC-V extension predicate.
/// \param [in] ExtensionPredicate
///     Currently RISC-V extension predicate may have one of the 2 condition
///     format:
///       * any_of with several arguments (see HasStdExtAOrZaamo)
///       * all_of with one argument (see HasStdExtA)
/// \return
///     Vector of extension's names related by an any_of relationship (so called
///     ExtensionGroup).
static std::vector<llvm::StringRef> getExtensionGroupFromExtensionPredicate(
    const llvm::Record *ExtensionPredicate) {
  const llvm::DagInit *Condition =
      ExtensionPredicate->getValueAsDag("AssemblerCondDag");

  const llvm::DefInit *Operator =
      llvm::dyn_cast<llvm::DefInit>(Condition->getOperator());
  if (!Operator)
    llvm::PrintFatalError(ExtensionPredicate->getLoc(), "Invalid Operator!");

  llvm::StringRef CombineType = Operator->getDef()->getName();
  if (CombineType == "any_of") {
    std::vector<llvm::StringRef> ExtensionGroup{};
    llvm::transform(Condition->getArgs(), std::back_inserter(ExtensionGroup),
                    [](const llvm::Init *Arg) {
                      return getExtensionName(
                          llvm::cast<llvm::DefInit>(Arg)->getDef());
                    });
    return ExtensionGroup;
  }

  if (CombineType == "all_of") {
    if (Condition->getNumArgs() != 1)
      llvm::PrintFatalError(ExtensionPredicate->getLoc(),
                            "Invalid \"all_of\" condition!");
    return std::vector<llvm::StringRef>{getExtensionName(
        llvm::cast<llvm::DefInit>(Condition->getArg(0))->getDef())};
  }

  llvm::PrintFatalError(
      ExtensionPredicate->getLoc(),
      "Invalid CombineType! (should be \"any_of\" or \"all_of\")");
}

/// Get ExtensionGroups (see GetExtensionGroupFromExtensionPredicate
/// description) for the instruction. \param [in] InstructionPredicates
///     Predicates of the instruction for which we want to get ExtensionGroups,
/// \return
///     Vector of instruction's ExtensionGroups.
static std::vector<std::vector<llvm::StringRef>>
stringifyRISCVExtensionsFromPredicates(
    const std::vector<const llvm::Record *> &InstructionPredicates) {
  std::vector<std::vector<llvm::StringRef>> ExtensionGroups{};
  llvm::transform(
      getExtensionPredicatesFromInstructionPredicates(InstructionPredicates),
      std::back_inserter(ExtensionGroups),
      [](const llvm::Record *ExtensionPredicate) {
        return getExtensionGroupFromExtensionPredicate(ExtensionPredicate);
      });
  return ExtensionGroups;
}

static std::vector<const llvm::Record *>
getRISCVExtensions(const llvm::RecordKeeper &Records) {
  std::vector<const llvm::Record *> Extensions =
      Records.getAllDerivedDefinitionsIfDefined("RISCVExtension");
  return Extensions;
}

static void
printExtensionTable(llvm::raw_ostream &OS,
                    const std::vector<const llvm::Record *> &Extensions) {
  OS << "static constexpr llvm::StringLiteral SupportedExtensions[] = {";

  for (const llvm::Record *R : Extensions)
    OS << "\"" << getExtensionName(R) << "\", ";

  OS << "};\n\n";
}

static void emitRISCVExtensions(const llvm::RecordKeeper &Records,
                                llvm::raw_ostream &OS) {
  std::vector<const llvm::Record *> Extensions = getRISCVExtensions(Records);
  printExtensionTable(OS, Extensions);

  OS << "\nstatic const llvm::DenseMap<llvm::StringRef, "
        "std::vector<llvm::StringRef>> ImpliedExts = {\n";
  for (const llvm::Record *Ext : Extensions) {
    auto ImpliesList = Ext->getValueAsListOfDefs("Implies");
    OS << "    { \"" << getExtensionName(Ext) << "\", { ";

    for (auto *ImpliedExt : Ext->getValueAsListOfDefs("Implies")) {
      if (!ImpliedExt->isSubClassOf("RISCVExtension"))
        continue;

      OS << "\"" << getExtensionName(ImpliedExt) << "\", ";
    }

    OS << "}}, \n";
  }

  OS << "};\n\n";
}

/// Obtains all RISC-V instruction records from TableGen, except of privelleged,
/// vendor-specific and some special ones.
static std::vector<const llvm::Record *>
getRISCVInstructions(const llvm::RecordKeeper &RK) {
  std::vector<const llvm::Record *> NonPseudoInstructions{};
  llvm::copy_if(RK.getAllDerivedDefinitions("RVInstCommon"),
                std::back_inserter(NonPseudoInstructions),
                [](const llvm::Record *Instr) {
                  return !Instr->getValueAsBit("isPseudo") &&
                         !Instr->getValueAsBit("isCodeGenOnly");
                });

  std::vector<const llvm::Record *> ExcludedInstructions =
      RK.getAllDerivedDefinitions("Priv");
  llvm::copy(RK.getAllDerivedDefinitions("Priv_rr"),
             std::back_inserter(ExcludedInstructions));
  llvm::copy(RK.getAllDerivedDefinitions("CSR_ir"),
             std::back_inserter(ExcludedInstructions));
  llvm::copy(RK.getAllDerivedDefinitions("CSR_ii"),
             std::back_inserter(ExcludedInstructions));
  ExcludedInstructions.push_back(RK.getDef("UNIMP"));

  llvm::copy_if(RK.getAllDerivedDefinitions("RVInstCommon"),
                std::back_inserter(ExcludedInstructions),
                [](const llvm::Record *Instr) {
                  return llvm::any_of(Instr->getValueAsListOfDefs("Predicates"),
                                      [](const llvm::Record *Predicate) {
                                        return Predicate->getName().starts_with(
                                            "HasVendor");
                                      });
                });

  std::vector<const llvm::Record *> RISCVInstructions{};

  llvm::sort(NonPseudoInstructions);
  llvm::sort(ExcludedInstructions);
  std::set_difference(NonPseudoInstructions.begin(),
                      NonPseudoInstructions.end(), ExcludedInstructions.begin(),
                      ExcludedInstructions.end(),
                      std::back_inserter(RISCVInstructions));

  return RISCVInstructions;
}

static void emitRISCVExtensionsForInstr(const llvm::Record *Instr,
                                        llvm::raw_ostream &OS) {
  std::vector<std::vector<llvm::StringRef>> ExtensionPredicates =
      stringifyRISCVExtensionsFromPredicates(
          Instr->getValueAsListOfDefs("Predicates"));

  // TODO: Get rid of this!
  if (Instr->getName() == "CLZ") {
    OS << "\"zbb\"";
    return;
  }

  if (ExtensionPredicates.empty()) {
    OS << "\"i\"";
    return;
  }

  for (const auto &Extensions : ExtensionPredicates) {
    for (llvm::StringRef Extension : Extensions)
      OS << "\"" << Extension << "\", ";
  }
}

static void emitRISCVInstructions(std::vector<const llvm::Record *> Instrs,
                                  llvm::raw_ostream &OS) {
  OS << "static std::set<llvm::StringRef> AllRISCVInstructions{ ";
  for (const llvm::Record *Instr : Instrs) {
    OS << "\"" << Instr->getName() << "\", ";
  }
  OS << "};\n";

  OS << "using InstructionInfo = std::pair<llvm::StringLiteral, "
        "std::vector<llvm::StringRef>>;\n";
  OS << "static InstructionInfo InstructionTable[] = {\n";
  for (auto &&Instr : Instrs) {
    OS << "{\"" << Instr->getName() << "\", {";
    emitRISCVExtensionsForInstr(Instr, OS);
    OS << "}},\n";
  }
  OS << "};\n";
}

static bool RISCV32bitFeatureFilter(const llvm::Record *Instr) {
  return !llvm::any_of(Instr->getValueAsListOfDefs("Predicates"),
                       [](const llvm::Record *Predicate) {
                         return Predicate->getName() == "IsRV64";
                       });
}

static bool RISCV64bitFeatureFilter(const llvm::Record *Instr) {
  return !llvm::any_of(Instr->getValueAsListOfDefs("Predicates"),
                       [](const llvm::Record *Predicate) {
                         return Predicate->getName() == "IsRV32";
                       });
}

static const std::vector<InstructionFeature> gRISCVInstrFeatures = {
    InstructionFeature{"memory-access"}.specifyFilter(
        memoryAccessFeatureFilter),

    InstructionFeature{"control-flow"}
        .specifyFilter(controlFlowFeatureFilter)
        .addInstrs("JAL", "JALR"),

    InstructionFeature{"rv32"}.specifyFilter(RISCV32bitFeatureFilter),

    InstructionFeature{"rv64"}.specifyFilter(RISCV64bitFeatureFilter),
};

namespace instr_enumerator_tblgen {

bool emitRISCVInstrEnums(llvm::raw_ostream &OS, const llvm::RecordKeeper &RK) {
  emitRISCVExtensions(RK, OS);

  auto &&Instructions = getRISCVInstructions(RK);
  emitRISCVInstructions(Instructions, OS);

  emitFeatures(Instructions, gRISCVInstrFeatures, OS);

  return false;
}

} // namespace instr_enumerator_tblgen
