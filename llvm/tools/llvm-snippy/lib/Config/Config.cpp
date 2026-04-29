//===-- Config.cpp ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <algorithm>

#include "snippy/Config/Branchegram.h"
#include "snippy/Config/BurstGram.h"
#include "snippy/Config/CallGraphLayout.h"
#include "snippy/Config/Config.h"
#include "snippy/Config/FunctionDescriptions.h"
#include "snippy/Config/ImmediateHistogram.h"
#include "snippy/Config/MemInitializationMode.h"
#include "snippy/Config/MemoryScheme.h"
#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/Config/RegisterAccess.h"
#include "snippy/Config/Selfcheck.h"
#include "snippy/GeneratorUtils/LLVMState.h"
#include "snippy/GeneratorUtils/RegisterPool.h"
#include "snippy/Support/DiagnosticInfo.h"
#include "snippy/Support/Options.h"
#include "snippy/Support/Utils.h"
#include "snippy/Support/YAMLHistogram.h"
#include "snippy/Target/Target.h"
#include "llvm/CodeGen/MIRYamlMapping.h"
#include "llvm/MC/MCRegisterInfo.h"

#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/YAMLTraits.h"
#include <istream>
#include <sstream>
#include <variant>

#define DEBUG_TYPE "snippy-layout-config"

namespace llvm {

LLVM_SNIPPY_OPTION_DEFINE_ENUM_OPTION_YAML_NO_DECL(
    snippy::MemInitMode, snippy::MemInitModeEnumOption)

LLVM_SNIPPY_OPTION_DEFINE_ENUM_OPTION_YAML_NO_DECL(
    snippy::SelfcheckMode, snippy::SelfcheckModeEnumOption)

using namespace snippy;

void yaml::ScalarEnumerationTraits<BurstMode>::enumeration(yaml::IO &IO,
                                                           BurstMode &BMode) {
  IO.enumCase(BMode, "basic", BurstMode::Basic);
  IO.enumCase(BMode, "store", BurstMode::StoreBurst);
  IO.enumCase(BMode, "load", BurstMode::LoadBurst);
  IO.enumCase(BMode, "load-store", BurstMode::LoadStoreBurst);
  IO.enumCase(BMode, "mixed", BurstMode::MixedBurst);
  IO.enumCase(BMode, "custom", BurstMode::CustomBurst);
}

template <> struct yaml::MappingTraits<BurstGramData> {
  enum class GroupKinds { Groupings, BaseRegisterGroups };

  template <GroupKinds Kind> struct NormalizedOpcodeGroups final {
    std::vector<SList> OpcodeGroups;

    NormalizedOpcodeGroups(yaml::IO &) {}

    NormalizedOpcodeGroups(
        yaml::IO &IO,
        const std::optional<BurstGramData::OpcodeGroupsTy> &Denorm) {
      if (Denorm.has_value()) {
        void *Ctx = IO.getContext();
        assert(Ctx && "To parse or output BurstGram provide ConfigIOContext as "
                      "context for yaml::IO");
        const auto &OpCC = static_cast<const ConfigIOContext *>(Ctx)->OpCC;
        transform(*Denorm, std::back_inserter(OpcodeGroups),
                  [&OpCC](const auto &Set) {
                    SList Res;
                    transform(Set, std::back_inserter(Res),
                              [&OpCC](auto Val) -> std::string {
                                return std::string{OpCC.name(Val)};
                              });
                    return Res;
                  });
      }
    }

    BurstGramData::OpcodeGroupsTy
    getKindDependentGroups(const BurstGramData::OpcodeGroupsTy &Denorm,
                           const OpcodeHistogram &Hist,
                           const OpcodeCache &OpCC) const {
      if constexpr (Kind == GroupKinds::Groupings)
        return {};
      else {
        assert(Kind == GroupKinds::BaseRegisterGroups);
        auto IsNotContainedInAnyGroup = [&Denorm](auto Opcode) {
          return all_of(Denorm, [&Opcode](const auto &Group) {
            return !Group.count(Opcode);
          });
        };
        auto NotSpecifiedOpcodes = make_filter_range(
            make_first_range(Hist.topOpcodes()), IsNotContainedInAnyGroup);
        return {BurstGramData::UniqueOpcodesTy(NotSpecifiedOpcodes.begin(),
                                               NotSpecifiedOpcodes.end())};
      }
    }

    std::optional<BurstGramData::OpcodeGroupsTy> denormalize(yaml::IO &IO) {
      if (OpcodeGroups.empty())
        return std::nullopt;
      BurstGramData::OpcodeGroupsTy Denorm;
      void *Ctx = IO.getContext();
      assert(Ctx && "To parse or output BurstGram provide ConfigIOContext as "
                    "context for yaml::IO");
      const auto &CfgCtx = *static_cast<const ConfigIOContext *>(Ctx);
      const auto &OpCC = CfgCtx.OpCC;
      const auto &Hist = CfgCtx.Histogram;
      transform(
          OpcodeGroups, std::back_inserter(Denorm), [&OpCC](const auto &Vec) {
            std::set<unsigned> Res;
            transform(Vec, std::inserter(Res, Res.end()),
                      [OpCC](const std::string &Name) {
                        auto Opt = OpCC.code(Name);
                        if (!Opt.has_value()) {
                          std::string Msg = "Unknown instruction \"" + Name +
                                            "\" in burst configuration";
                          snippy::fatal(StringRef(Msg));
                        }
                        return *Opt;
                      });
            return Res;
          });
      append_range(Denorm, getKindDependentGroups(Denorm, Hist, OpCC));
      return Denorm;
    }
  };

  static void mapping(yaml::IO &IO, BurstGramData &Burst) {
    IO.mapRequired("min-size", Burst.MinSize);
    IO.mapRequired("max-size", Burst.MaxSize);
    IO.mapRequired("mode", Burst.Mode);
    yaml::MappingNormalization<NormalizedOpcodeGroups<GroupKinds::Groupings>,
                               std::optional<BurstGramData::OpcodeGroupsTy>>
        Keys(IO, Burst.Groupings);
    IO.mapOptional("groupings", Keys->OpcodeGroups);
    yaml::MappingNormalization<
        NormalizedOpcodeGroups<GroupKinds::BaseRegisterGroups>,
        std::optional<BurstGramData::OpcodeGroupsTy>>
        BaseRegsNorm(IO, Burst.BaseRegisterGroups);
    IO.mapOptional("base-register-groups", BaseRegsNorm->OpcodeGroups);
  }

  static std::string validate(yaml::IO &IO, BurstGramData &Burst) {
    if (Burst.MinSize > Burst.MaxSize)
      return "Max size of burst group should be greater than min size.";
    if (Burst.Mode == BurstMode::Basic && (Burst.MaxSize > 0))
      return "Min and max burst group sizes should be 0 with \"basic\" mode";
    if (Burst.Mode != BurstMode::Basic && Burst.MaxSize == 0)
      return "Burst max size should be greater than 0";
    if (Burst.Mode != BurstMode::CustomBurst && Burst.Groupings.has_value())
      return "Groupings can be specified only with custom burst mode";
    if (Burst.Mode == BurstMode::CustomBurst && !Burst.Groupings)
      return "Custom burst mode was specified but groupings are not provided";
    if (Burst.Mode == BurstMode::CustomBurst && Burst.Groupings->empty())
      return "Custom burst mode was specified but groupings are empty";
    if (Burst.Mode == BurstMode::CustomBurst &&
        any_of(*Burst.Groupings,
               [](const auto &Group) { return Group.empty(); }))
      return "Burst grouping can't be empty";
    return std::string();
  }
};

} // namespace llvm

namespace llvm {
namespace snippy {

extern cl::OptionCategory Options;
cl::OptionCategory ProgramOptionsCategory("Snippy Program Level Options");
cl::OptionCategory
    MemInitOptionsCategory("Snippy Memory Initialization options");
cl::OptionCategory
    RegInitOptionsCategory("Snippy Registers Initialization options");

cl::OptionCategory DebugOptionsCategory("Snippy debug options");

cl::OptionCategory ModelOptionsCategory("Snippy Model Options");

cl::OptionCategory
    InstrGenOptionsCategory("Snippy Instruction Generation Options");

#define GEN_SNIPPY_OPTIONS_DEF
#include "SnippyConfigOptions.inc"
#undef GEN_SNIPPY_OPTIONS_DEF

bool isExternal(const FunctionDescs &Funcs, StringRef Name) {
  auto &Descs = Funcs.Descs;
  auto Found =
      llvm::find_if(Descs, [&](auto &Desc) { return Desc.Name == Name; });
  assert(Found != Descs.end());
  return Found->External;
}

bool hasExternalCallee(const FunctionDescs &FuncDescs,
                       const FunctionDesc &Func) {
  assert(llvm::count_if(FuncDescs.Descs,
                        [&](auto &Desc) { return Desc.Name == Func.Name; }));
  return llvm::any_of(Func.Callees, [&](StringRef Name) {
    return isExternal(FuncDescs, Name);
  });
}

struct IncludeParsingWrapper final {
  std::vector<std::string> Includes;
};

static std::optional<unsigned> findRegisterByName(const SnippyTarget &SnippyTgt,
                                                  const MCRegisterInfo &RI,
                                                  StringRef Name) {
  for (auto &RC : RI.regclasses()) {
    auto RegIdx = std::find_if(RC.begin(), RC.end(), [&Name, &RI](auto &Reg) {
      return Name == RI.getName(Reg);
    });
    if (RegIdx != RC.end())
      return *RegIdx;
  }
  return SnippyTgt.findRegisterByName(Name);
}

static std::unordered_set<unsigned>
getRegistersMatchedByRegex(Regex &RegEx, const MCRegisterInfo &MRI) {
  std::unordered_set<unsigned> MatchedRegs;
  for (auto &RC : MRI.regclasses()) {
    auto Matched = llvm::make_filter_range(
        RC, [&RegEx, &MRI](auto &R) { return RegEx.match(MRI.getName(R)); });
    llvm::copy(Matched, std::inserter(MatchedRegs, MatchedRegs.end()));
  }
  return MatchedRegs;
}
} // namespace snippy

struct RegisterAccessNormalization final {
  struct RegisterAccess final {
    std::string Name;
    AccessMaskBits Acc;
  };
  std::vector<RegisterAccess> Data;
  RegisterAccessNormalization(yaml::IO &Io) {}
  RegisterAccessNormalization(yaml::IO &Io, RegisterAccessConfig &Cfg) {
    void *Ctx = Io.getContext();
    assert(Ctx &&
           "To parse or output RegisterAccessConfig provide ConfigIOContext as "
           "context for yaml::IO");
    auto &State = static_cast<const ConfigIOContext *>(Ctx)->State;
    auto &MRI = State.getRegInfo();
    llvm::transform(Cfg, std::back_inserter(Data), [&MRI](auto &RegAcc) {
      auto &&[Reg, Acc] = RegAcc;
      return RegisterAccess{MRI.getName(Reg), static_cast<uint32_t>(Acc)};
    });
  }
  RegisterAccessConfig denormalize(yaml::IO &Io) {
    void *Ctx = Io.getContext();
    assert(Ctx &&
           "To parse or output RegisterAccessConfig provide ConfigIOContext as "
           "context for yaml::IO");
    auto &State = static_cast<const ConfigIOContext *>(Ctx)->State;
    auto &MRI = State.getRegInfo();

    RegisterAccessConfig Res;
    for (auto &&Reg : Data) {
      auto RegisterRegEx = createWholeWordMatchRegex(Reg.Name);
      if (auto Err = RegisterRegEx.takeError())
        snippy::fatal(formatv("Illegal register regex: \"{0}\": {1}", Reg.Name,
                              toString(std::move(Err))));
      auto MatchedRegs =
          snippy::getRegistersMatchedByRegex(*RegisterRegEx, MRI);
      if (MatchedRegs.empty())
        snippy::fatal(
            "Invalid register reservation regex",
            formatv("No registers were matched by regex \"{0}\"", Reg.Name));
      for (auto R : MatchedRegs)
        Res.try_emplace(R, static_cast<AccessMaskBit>(Reg.Acc.value));
    }
    return Res;
  }
};

} // namespace llvm
LLVM_YAML_IS_SEQUENCE_VECTOR(llvm::RegisterAccessNormalization::RegisterAccess)
namespace llvm {

template <> struct yaml::ScalarBitSetTraits<AccessMaskBits> {
#ifdef LLVM_SNIPPY_ACCESS_MASK_DESC
#error LLVM_SNIPPY_ACCESS_MASK_DESC is already defined
#endif
#define LLVM_SNIPPY_ACCESS_MASK_DESC(NAME, VAL)                                \
  Io.bitSetCase(Val, #NAME, static_cast<uint32_t>(AccessMaskBit::NAME));

  static void bitset(yaml::IO &Io, AccessMaskBits &Val) {
    if (!Io.outputting()) {
      LLVM_SNIPPY_ACCESS_MASKS
    } else {
      Io.bitSetCase(Val, AccessMaskNameOf<AccessMaskBit::PrimaryR>.data(),
                    static_cast<uint32_t>(AccessMaskBit::PrimaryR));
      Io.bitSetCase(Val, AccessMaskNameOf<AccessMaskBit::PrimaryW>.data(),
                    static_cast<uint32_t>(AccessMaskBit::PrimaryW));
      Io.bitSetCase(Val, AccessMaskNameOf<AccessMaskBit::SupportR>.data(),
                    static_cast<uint32_t>(AccessMaskBit::SupportR));
      Io.bitSetCase(Val, AccessMaskNameOf<AccessMaskBit::SupportW>.data(),
                    static_cast<uint32_t>(AccessMaskBit::SupportW));
    }
  }
#undef LLVM_SNIPPY_ACCESS_MASK_DESC
};

template <>
struct yaml::CustomMappingTraits<RegisterAccessNormalization::RegisterAccess> {
  static void inputOne(yaml::IO &Io, StringRef Key,
                       RegisterAccessNormalization::RegisterAccess &Elem) {
    Elem.Name = Key.str();
    Io.mapRequired(Key.data(), Elem.Acc);
  }

  static void output(yaml::IO &Io,
                     RegisterAccessNormalization::RegisterAccess &Elem) {
    Io.mapRequired(Elem.Name.data(), Elem.Acc);
  }
};

template <> struct yaml::MappingTraits<IncludeParsingWrapper> {
  static void mapping(yaml::IO &IO, snippy::IncludeParsingWrapper &IPW) {
    IO.mapOptional("include", IPW.Includes);
  }
};

template <> struct yaml::ScalarEnumerationTraits<ImmHistOpcodeSettings::Kind> {
  static void enumeration(IO &IO, ImmHistOpcodeSettings::Kind &K) {
    IO.enumCase(K, "uniform", ImmHistOpcodeSettings::Kind::Uniform);
  }
};

struct ImmHistOpcodeSettingsNorm final {
  ImmHistOpcodeSettings::Kind Kind;
  ImmediateHistogramSequence Seq;
  ImmHistOperands Map;
  yaml::IO &IO;

  ImmHistOpcodeSettingsNorm(yaml::IO &IO) : IO(IO) {}
};

struct ImmHistOpcodeSettingsNormalization final {
  ImmHistOpcodeSettingsNorm Data;

  ImmHistOpcodeSettingsNormalization(yaml::IO &IO) : Data(IO) {}

  ImmHistOpcodeSettingsNormalization(yaml::IO &IO,
                                     const ImmHistOpcodeSettings &Denorm)
      : Data(IO) {
    Data.Kind = Denorm.getKind();
    if (Denorm.isSequence())
      Data.Seq = Denorm.getSequence();
    else if (Denorm.isPerOperand())
      Data.Map = Denorm.getOperandsMap();
  }
  ImmHistOpcodeSettings denormalize(yaml::IO &) {
    switch (Data.Kind) {
    case ImmHistOpcodeSettings::Kind::Custom:
      return ImmHistOpcodeSettings(Data.Seq);
    case ImmHistOpcodeSettings::Kind::Uniform:
      return ImmHistOpcodeSettings();
    case ImmHistOpcodeSettings::Kind::Operands:
      return ImmHistOpcodeSettings(Data.Map);
    }
    llvm_unreachable("Unknown opcode settings kind");
  }
};

template <> struct yaml::PolymorphicTraits<ImmHistOpcodeSettingsNorm> {
  static yaml::NodeKind getKind(const ImmHistOpcodeSettingsNorm &Info) {
    switch (Info.Kind) {
    case ImmHistOpcodeSettings::Kind::Uniform:
      return NodeKind::Scalar;
    case ImmHistOpcodeSettings::Kind::Custom:
      return NodeKind::Sequence;
    case ImmHistOpcodeSettings::Kind::Operands:
      return NodeKind::Map;
    }
    llvm_unreachable("Unknown kind in ImmHistOpcodeSettings");
  }

  static ImmHistOpcodeSettings::Kind &
  getAsScalar(ImmHistOpcodeSettingsNorm &Info) {
    if (!Info.IO.outputting())
      Info.Kind = snippy::ImmHistOpcodeSettings::Kind::Uniform;
    return Info.Kind;
  }

  static ImmediateHistogramSequence &
  getAsSequence(ImmHistOpcodeSettingsNorm &Info) {
    if (!Info.IO.outputting())
      Info.Kind = snippy::ImmHistOpcodeSettings::Kind::Custom;
    return Info.Seq;
  }

  static ImmHistOperands &getAsMap(ImmHistOpcodeSettingsNorm &Info) {
    if (!Info.IO.outputting())
      Info.Kind = snippy::ImmHistOpcodeSettings::Kind::Operands;
    return Info.Map;
  }
};

template <> struct yaml::CustomMappingTraits<ImmHistConfigForRegEx> {
  static void inputOne(IO &IO, StringRef Key, ImmHistConfigForRegEx &Info) {
    yaml::MappingNormalization<ImmHistOpcodeSettingsNormalization,
                               ImmHistOpcodeSettings>
        Norm(IO, Info.Data);
    IO.mapRequired(Key.data(), Norm->Data);
    Info.Expr = Key.str();
  }

  static void output(IO &IO, ImmHistConfigForRegEx &Info) {
    yaml::MappingNormalization<ImmHistOpcodeSettingsNormalization,
                               ImmHistOpcodeSettings>
        Norm(IO, Info.Data);
    IO.mapRequired(Info.Expr.c_str(), Norm->Data);
  }
};

LLVM_SNIPPY_YAML_IS_SEQUENCE_ELEMENT(ImmHistConfigForRegEx,
                                     /* not a flow */ false);

template <> struct yaml::MappingTraits<ImmediateHistogramRegEx> {
  static void mapping(IO &IO, ImmediateHistogramRegEx &IH) {
    IO.mapRequired("opcodes", IH.Exprs);
  }
};

struct ImmediateHistogramNorm final {
  enum class Kind { Sequence, RegEx };
  ImmediateHistogramSequence Seq;
  ImmediateHistogramRegEx RegEx;
  Kind HistKind = Kind::Sequence;
  yaml::IO &IO;

  ImmediateHistogramNorm(yaml::IO &IO) : IO(IO) {}
};

struct ImmediateHistogramNormalization final {
  ImmediateHistogramNorm Data;

  ImmediateHistogramNormalization(yaml::IO &IO) : Data(IO) {}

  ImmediateHistogramNormalization(yaml::IO &IO, const ImmediateHistogram &Hist)
      : Data(IO) {
    if (Hist.holdsAlternative<ImmediateHistogramSequence>()) {
      Data.Seq = Hist.get<ImmediateHistogramSequence>();
      Data.HistKind = ImmediateHistogramNorm::Kind::Sequence;
    } else if (Hist.holdsAlternative<ImmediateHistogramRegEx>()) {
      Data.RegEx = Hist.get<ImmediateHistogramRegEx>();
      Data.HistKind = ImmediateHistogramNorm::Kind::RegEx;
    } else
      llvm_unreachable("Unknown immediate histogram kind");
  }

  ImmediateHistogram denormalize(yaml::IO &) {
    if (Data.HistKind == ImmediateHistogramNorm::Kind::RegEx)
      return ImmediateHistogram(Data.RegEx);
    if (Data.HistKind == ImmediateHistogramNorm::Kind::Sequence)
      return ImmediateHistogram(Data.Seq);
    llvm_unreachable("Unknown immediate histogram kind");
  }
};

template <> struct yaml::PolymorphicTraits<ImmediateHistogramNorm> {
  static yaml::NodeKind getKind(const ImmediateHistogramNorm &Hist) {
    if (Hist.HistKind == ImmediateHistogramNorm::Kind::RegEx)
      return yaml::NodeKind::Map;
    if (Hist.HistKind == ImmediateHistogramNorm::Kind::Sequence)
      return yaml::NodeKind::Sequence;
    llvm_unreachable("Unknown immediate histogram kind");
  }

  static ImmediateHistogramRegEx &getAsMap(ImmediateHistogramNorm &Hist) {
    Hist.HistKind = ImmediateHistogramNorm::Kind::RegEx;
    return Hist.RegEx;
  }

  static ImmediateHistogramSequence &
  getAsSequence(ImmediateHistogramNorm &Hist) {
    Hist.HistKind = ImmediateHistogramNorm::Kind::Sequence;
    return Hist.Seq;
  }

  static int &getAsScalar(ImmediateHistogramNorm &Info) {
    Info.IO.setError("Immediate histogram should be either sequence or map. "
                     "But scalar was encountered.");
    snippy::fatal("Failed to parse configuration file.");
  }
};

// Reserve global state registers so they won't be corrupted when we call
// external function.
static void reserveGlobalStateRegisters(RegPoolWrapper &RP,
                                        const SnippyTarget &Tgt) {
  auto Regs = Tgt.getGlobalStateRegs();
  for (auto Reg : Regs) {
    RP.addReserved(Reg, AccessMaskBit::RW);
    DEBUG_WITH_TYPE(
        "snippy-regpool",
        (dbgs() << "Reserved Because of external callee:\n", RP.print(dbgs())));
  }
}

static std::unordered_set<unsigned>
parseReservedRegisters(const SnippyTarget &Tgt, const MCRegisterInfo &MRI,
                       ArrayRef<RegexOption> RegList) {
  std::unordered_set<unsigned> Reserved;
  for (auto [RegName, Regex] : RegList) {
    if (Regex::isLiteralERE(RegName)) {
      // Not a RegEx
      auto Reg = findRegisterByName(Tgt, MRI, RegName);
      if (!Reg)
        snippy::fatal(formatv("Illegal register name {0}"
                              " is specified in --reserved-regs-list",
                              RegName));
      Reserved.insert(*Reg);
      continue;
    }
    // RegName is a RegEx
    auto RegisterRegEx = createWholeWordMatchRegex(RegName);
    if (auto Err = RegisterRegEx.takeError())
      snippy::fatal(formatv("Illegal register regex: \"{0}\": {1}", RegName,
                            toString(std::move(Err))));

    auto MatchedRegs = getRegistersMatchedByRegex(*RegisterRegEx, MRI);
    if (MatchedRegs.empty())
      snippy::fatal(
          formatv("No registers were matched by regex \"{0}\"", RegName));
    Reserved.merge(std::move(MatchedRegs));
  }
  return Reserved;
}

static RegisterAccessConfig
deriveRegisterAccessesFromReservedList(const SnippyTarget &Tgt,
                                       const MCRegisterInfo &MRI,
                                       const ProgramOptions &Opts) {
  RegisterAccessConfig Res;
  auto Reserved =
      parseReservedRegisters(Tgt, MRI, Opts.ReservedRegsList.value());
  for (auto Reg : Reserved) {
    SmallVector<Register> PhysRegs;
    Tgt.getPhysRegsFromUnit(Reg, MRI, PhysRegs);
    llvm::transform(PhysRegs, std::inserter(Res, Res.end()), [](auto Reg) {
      return std::make_pair(Reg, AccessMaskBit::PrimaryRW);
    });
  }
  return Res;
}

// We want to spill certain global register (e.g. Thread Pointer and Global
// Pointer) to memory instead of stack as we want to spill and reload them
// several times throughout the program and we won't be able to do that if we
// spill them to stack.
static std::vector<MCRegister> getRegsToSpillToMem(const SnippyTarget &Tgt,
                                                   const Config &Cfg) {
  if (!Cfg.PassCfg.hasExternalCallees() ||
      !Cfg.ProgramCfg.hasSectionToSpillGlobalRegs())
    return {};
  return Tgt.getGlobalStateRegs();
}

static std::vector<MCRegister>
parseSpilledRegistersOption(const RegPoolWrapper &RP, const SnippyTarget &Tgt,
                            const MCRegisterInfo &RI, LLVMContext &Ctx,
                            const ProgramOptions &Opts) {
  std::vector<MCRegister> SpilledRegs;
  for (auto &&RegName : Opts.SpilledRegisterList.value()) {
    auto Reg = findRegisterByName(Tgt, RI, RegName);
    if (!Reg)
      snippy::fatal(formatv("Illegal register name {0}"
                            " is specified in --spilled-regs-list",
                            RegName));

    if (RP.isReserved(Reg.value()))
      snippy::fatal(formatv("Register \"{0}\" cannot be spilled, because it is "
                            "explicitly reserved.\n",
                            RegName));
    SpilledRegs.push_back(Reg.value());
  }
  return SpilledRegs;
}

static void generateSPRelativeInstrsError(StringRef RedefineSP) {
  snippy::fatal(
      "Incompatible options",
      "When the stack pointer is redefined to '" + Twine(RedefineSP) +
          "', generation of "
          "SP-relative instructions is not supported. Redefine it with "
          "`redefine-sp` option or remove SP-relative instructions from the "
          "histogram.");
}
namespace {

struct RegChoice {
  enum class Opt { Reg, Any, ABI, AnyNotABI };

  Opt get() const { return Val; }

  std::pair<StringRef, MCRegister> getSpecific() const {
    assert(RegVal.has_value() && "No specifc reg specified");
    auto &&[Name, Reg] = *RegVal;
    return std::make_pair(StringRef(Name), Reg);
  }

  MCRegister getSpecificReg() const {
    assert(RegVal.has_value() && "No specifc reg specified");
    return RegVal->second;
  }

  std::optional<MCRegister> getSpecificRegIfCan() const {
    if (Val == Opt::Reg)
      return RegVal->second;
    return std::nullopt;
  }

  RegChoice(StringRef ABIName, StringRef OptionVal, StringRef OptionName,
            const SnippyTarget &Tgt, const MCRegisterInfo &RI) {
    if (OptionVal == ABIName) {
      Val = Opt::ABI;
      return;
    }
    if (OptionVal == "any") {
      Val = Opt::Any;
      return;
    }
    StringRef PrefixAnyNot = "any-not-";
    if (OptionVal.starts_with(PrefixAnyNot)) {
      auto ExpectABIName = OptionVal;
      ExpectABIName.consume_front(PrefixAnyNot);
      if (ExpectABIName != ABIName) {
        snippy::fatal(
            formatv("\"{0}\" passed to {1} is invalid, did you mean '{2}{3}'",
                    OptionVal, OptionName, PrefixAnyNot, ABIName));
      }
      Val = Opt::AnyNotABI;
      return;
    }
    StringRef PrefixReg = "reg::";
    if (!OptionVal.starts_with(PrefixReg)) {
      snippy::fatal(formatv("\"{0}\", passed to {1} is not valid option value",
                            OptionVal, OptionName));
    }
    auto RegName = OptionVal;
    RegName.consume_front(PrefixReg);
    auto RegV = findRegisterByName(Tgt, RI, RegName);
    if (!RegV)
      snippy::fatal(formatv("Illegal register name {0}"
                            " is specified in {1}",
                            RegName, OptionName));
    Val = Opt::Reg;
    RegVal = std::make_pair(std::string(RegName), *RegV);
  }

private:
  Opt Val;
  std::optional<std::pair<std::string, MCRegister>> RegVal;
};

} // namespace

static MCRegister chooseRA(const RegPoolWrapper &RP, const SnippyTarget &Tgt,
                           const MCRegisterInfo &RI, const Config &Cfg,
                           const RegChoice &RAChoice,
                           std::optional<MCRegister> SP, bool FollowTargetABI,
                           ArrayRef<MCRegister> SpilledToStack) {
  SmallVector<unsigned, 3> CallOpcodes;
  llvm::copy_if(Cfg.Histogram.uniqueOpcodes(), std::back_inserter(CallOpcodes),
                [&](auto &&Opcode) { return Tgt.isCall(Opcode); });
  auto RA = Tgt.getReturnAddress();
  bool CanUseABI = true;
  const auto &RARegClass = Tgt.getRegClassSuitableForRA(std::nullopt, RI);
  auto NotAvailableForSomeCalls = [&](auto &&Reg) {
    return llvm::any_of(CallOpcodes, [&](auto &&Opcode) {
      auto &RC = Tgt.getRegClassSuitableForRA(Opcode, RI);
      return !RC.contains(Reg);
    });
  };
  auto IsCalleeSavedInABIMode = [&](auto &&Reg) {
    return FollowTargetABI &&
           (llvm::count(SpilledToStack, Reg) || Reg == Tgt.getStackPointer());
  };
  auto Filter = [&](auto Reg) {
    return !RARegClass.contains(Reg) || (!CanUseABI && Reg == RA) ||
           NotAvailableForSomeCalls(Reg) || (SP && (*SP == Reg)) ||
           IsCalleeSavedInABIMode(Reg);
  };

  switch (RAChoice.get()) {
  case RegChoice::Opt::ABI:
    return RA;
  case RegChoice::Opt::AnyNotABI:
    CanUseABI = false;
    LLVM_FALLTHROUGH;
  case RegChoice::Opt::Any:
    if (CanUseABI && FollowTargetABI)
      return RA;
    return RP.getAvailableRegister("return address", RI, RARegClass, Filter);
  case RegChoice::Opt::Reg: {
    auto [RegStr, Reg] = RAChoice.getSpecific();
    if (Filter(Reg))
      snippy::fatal(
          formatv("Register {0} specified in --redefine-ra is not suitable "
                  "for return address redefinition",
                  RegStr));
    return Reg;
  }
  }
  llvm_unreachable("Unhandled choice");
}

static std::pair<MCRegister, MCRegister> configureSPandRA(
    const RegPoolWrapper &RP, const SnippyTarget &Tgt, const MCRegisterInfo &RI,
    std::vector<MCRegister> &SpilledToStack, LLVMContext &Ctx, Config &Cfg,
    const ProgramOptions &Opts, bool HasSPRelativeInstrs) {
  auto SP = Tgt.getStackPointer();
  auto RA = Tgt.getReturnAddress();
  bool FollowTargetABI = Cfg.ProgramCfg.FollowTargetABI;
  bool StaticStack = Cfg.ProgramCfg.StaticStack;
  std::string RedefineSP = Opts.RedefineSP;
  std::string RedefineRA = Opts.RedefineRA;
  auto SPChoice = RegChoice("SP", RedefineSP, "--redefine-sp", Tgt, RI);
  auto RAChoice = RegChoice("RA", RedefineRA, "--redefine-ra", Tgt, RI);
  auto NotABIAndNotAny = [](auto &Choice) {
    return Choice.get() != RegChoice::Opt::ABI &&
           Choice.get() != RegChoice::Opt::Any;
  };
  bool BothSame = SPChoice.get() == RegChoice::Opt::Reg &&
                  RAChoice.get() == RegChoice::Opt::Reg &&
                  SPChoice.getSpecificReg() == RAChoice.getSpecificReg();
  bool BothRA = SPChoice.get() == RegChoice::Opt::Reg &&
                SPChoice.getSpecificReg() == RA &&
                RAChoice.get() == RegChoice::Opt::ABI;
  bool BothSP = RAChoice.get() == RegChoice::Opt::Reg &&
                RAChoice.getSpecificReg() == SP &&
                SPChoice.get() == RegChoice::Opt::ABI;

  if (BothSame || BothRA || BothSP)
    snippy::fatal(
        "Cannot assign stack pointer and return address to same register");

  if (!StaticStack && HasSPRelativeInstrs && Opts.RedefineSP.isSpecified() &&
      !NotABIAndNotAny(SPChoice))
    generateSPRelativeInstrsError(RedefineSP);

  // Choose RA first.
  auto RealRA =
      chooseRA(RP, Tgt, RI, Cfg, RAChoice, SPChoice.getSpecificRegIfCan(),
               FollowTargetABI, SpilledToStack);
  // Add redefined RA to spill list to be able to exit from snippy function
  // correctly as required by target abi. We still want to spill target return
  // address register as abi may require it to be preserved. This is also
  // guaranteed in case if preserve-ra is specified.
  if (((FollowTargetABI && (RealRA != RA)) || Opts.PreserveRA.value()) &&
      !llvm::count(SpilledToStack, RealRA))
    SpilledToStack.push_back(RealRA);

  if (FollowTargetABI) {
    if (NotABIAndNotAny(SPChoice) || NotABIAndNotAny(RAChoice))
      snippy::warn(
          WarningName::InconsistentOptions, Ctx,
          "When using --honor-target-abi and --redefine-sp/ra=" +
              Twine(RedefineSP) +
              " options together, target ABI may not be preserved in case of "
              "traps",
          "use these options in combination only for valid code generation");
    else
      return std::make_pair(SP, RA);
  }

  if (SPChoice.get() == RegChoice::Opt::ABI)
    return std::make_pair(SP, RealRA);

  MCRegister RealSP = MCRegister::NoRegister;
  bool CanUseSP = StaticStack || (SPChoice.get() != RegChoice::Opt::AnyNotABI &&
                                  !HasSPRelativeInstrs);
  const auto &SPRegClass = Tgt.getRegClassSuitableForSP(RI);

  auto FullFilter = [&](auto Reg) {
    return !SPRegClass.contains(Reg) || Reg == RealRA ||
           (!CanUseSP && Reg == SP) ||
           (!FollowTargetABI && llvm::any_of(SpilledToStack, [Reg](auto SpReg) {
             return SpReg == Reg;
           }));
  };

  switch (SPChoice.get()) {
  case RegChoice::Opt::Reg: {
    auto [RegStr, Reg] = SPChoice.getSpecific();

    if (!StaticStack && Reg == SP && HasSPRelativeInstrs)
      generateSPRelativeInstrsError(RedefineSP);

    if (FullFilter(Reg))
      snippy::fatal(
          formatv("Register {0} specified in --redefine-sp is not suitable "
                  "for stack pointer redefinition",
                  RegStr));

    RealSP = Reg;
    break;
  }
  case RegChoice::Opt::Any:
  case RegChoice::Opt::AnyNotABI:
    RealSP =
        RP.getAvailableRegister("stack pointer", RI, SPRegClass, FullFilter);
    break;
  default:
    llvm_unreachable("unhandled case");
  }

  // We need to spill SP if it is not used as intended
  // and honor-target-abi is specified and also remove RealSP from SpilledRegs
  // list if it is in it
  if (FollowTargetABI && (RealSP != SP)) {
    llvm::erase(SpilledToStack, RealSP);
    SpilledToStack.push_back(SP);
  }

  return std::make_pair(RealSP, RealRA);
}

static std::vector<std::string> parseModelPluginList(const ModelOptions &Opts) {
  std::vector<std::string> CoSimModelPluginFilesList;
  if (Opts.ModelPluginFile.value() == "None" &&
      !Opts.CoSimModelPluginFilesList.value().empty())
    snippy::fatal(formatv("--cosim-model-plugins"
                          " can only be used when --model-plugin"
                          " is provided and is not None"));
  std::vector<std::string> Ret{Opts.ModelPluginFile};
  copy(Opts.CoSimModelPluginFilesList.value(), std::back_inserter(Ret));
  erase(Ret, "None");

  return Ret;
}

static bool codeLayoutIsBig(const CodeLayoutConfig &CodeLayout,
                            const SnippyTarget &Tgt) {
  auto &Addresses = CodeLayout.Ranges;
  assert(!Addresses.empty());
  auto Starts = llvm::map_range(Addresses, [](auto &R) { return R.Start; });
  auto MinStart = *std::min_element(Starts.begin(), Starts.end());
  auto Finishes =
      llvm::map_range(Addresses, [](auto &R) { return R.Start + R.Size; });
  auto MaxFinish = *std::max_element(Finishes.begin(), Finishes.end());
  assert(MaxFinish > MinStart);
  auto MaxDist = MaxFinish - MinStart;
  return !Tgt.fitsUncondBranch(MaxDist);
}

static unsigned long long
seedOptToValue(StringRef SeedStr, StringRef SeedType = "instructions seed",
               StringRef Warning =
                   "no instructions seed specified, using auto-generated one") {
  if (SeedStr.empty()) {
    auto SeedValue =
        std::chrono::system_clock::now().time_since_epoch().count();
    snippy::warn(WarningName::SeedNotSpecified, Warning, Twine(SeedValue));
    return SeedValue;
  }

  unsigned long long SeedValue;
  if (getAsUnsignedInteger(SeedStr, /* Radix */ 10, SeedValue))
    snippy::fatal(
        formatv("Provided {0} is not convertible to numeric value.", SeedType));
  return SeedValue;
}

static std::optional<unsigned> getExpectedNumInstrs(StringRef NumAsString) {
  if (NumAsString == "all")
    return {};
  int Value;
  if (!to_integer(NumAsString, Value, /*base*/ 10))
    snippy::fatal("num-instrs get not a number or all");
  if (Value < 0)
    snippy::fatal("num-instrs get negative number");
  return Value;
}

static MemorySeedTy hashMemorySeed(uint64_t MemorySeed, LLVMContext &Ctx) {
  static_assert(sizeof(MemorySeedTy) == 4,
                "This hash function is created for 32-bit memory seed");
  auto BytesArr = std::vector<std::byte>{};
  convertNumberToBytesArray(MemorySeed, std::back_inserter(BytesArr));
  assert(BytesArr.size() == sizeof(MemorySeed));
  auto SeedUpperPart = convertBytesToNumber<MemorySeedTy>(
      BytesArr.begin(), BytesArr.begin() + sizeof(MemorySeedTy));
  auto SeedBottomPart = convertBytesToNumber<MemorySeedTy>(
      BytesArr.begin() + sizeof(MemorySeedTy), BytesArr.end());
  if (SeedUpperPart != 0u) {
    MemorySeedTy FinalSeed = SeedUpperPart ^ SeedBottomPart;
    if (FinalSeed == 0)
      FinalSeed = SeedBottomPart | 1;
    snippy::notice(WarningName::NotAWarning, Ctx,
                   "memory seed value is too big, "
                   "so it has been hashed",
                   "new memory seed: " + Twine(FinalSeed));
    return FinalSeed;
  }
  return SeedBottomPart;
}

static std::optional<MemorySeedTy>
getMemorySeed(LLVMContext &Ctx, uint64_t Seed, StringRef MemorySeed,
              MemInitMode InitializeMemory, Config &Cfg) {
  // TODO: move this checks to final config verification
  if (InitializeMemory == MemInitMode::NoInit &&
      (!MemorySeed.empty() && MemorySeed != "none"))
    snippy::fatal("Specify memory init mode in order to use memory seed");

  if (isDuringRuntime(InitializeMemory) &&
      !Cfg.PassCfg.RegistersConfig.InitializeRegs)
    snippy::fatal(formatv("runtime memory initialization may be performed "
                          "only with init-regs-in-elf option"));

  if (isSeedProhibited(InitializeMemory) && MemorySeed != "none")
    snippy::fatal(
        formatv("memory seed is prohibited in {0} mode", InitializeMemory));

  if (isSeedProhibited(InitializeMemory))
    return std::nullopt;

  if (InitializeMemory == MemInitMode::NoInit)
    return std::nullopt;

  if (isSeedOptional(InitializeMemory) && MemorySeed == "none")
    return std::nullopt;

  uint64_t MemSeed;
  if (MemorySeed.empty() || MemorySeed == "random") {
    MemSeed = seedOptToValue(
        "", "memory seed",
        "random memory seed specified, using auto-generated one");
  } else if (MemorySeed == "none") {
    MemSeed = Seed;
    auto Name = MemInitModeEnumOption::toString(InitializeMemory);
    assert(Name.has_value());
    snippy::warn(
        WarningName::SeedNotSpecified,
        formatv(
            "memory seed \"none\" specified for mode \"{0}\" that requires it",
            *Name),
        "using instructions seed");
  } else {
    MemSeed = seedOptToValue(MemorySeed, "");
  }

  return hashMemorySeed(MemSeed, Ctx);
}

static std::optional<std::string> getMemoryFile(LLVMContext &Ctx,
                                                MemInitMode InitializeMemory,
                                                StringRef MemoryFile) {
  if (!isFileInit(InitializeMemory) && MemoryFile != "none")
    snippy::fatal(formatv("init-memory-file option needs memory init mode "
                          "to be specified as file init"));

  if (isFileInit(InitializeMemory) && MemoryFile == "none")
    snippy::notice(WarningName::NotAWarning, Ctx,
                   "init-memory-file option hasn't been specified",
                   "using default file: mem_state.bin");

  return isFileInit(InitializeMemory)
             ? std::make_optional<std::string>(MemoryFile == "none"
                                                   ? StringRef("mem_state.bin")
                                                   : MemoryFile)
             : std::nullopt;
}

static unsigned getSelfcheckPeriod(StringRef Selfcheck) {
  if (Selfcheck == "none")
    return 0;

  if (Selfcheck.empty())
    return 1;

  unsigned long long SelfcheckPeriod = 0;
  if (getAsUnsignedInteger(Selfcheck, /* Radix */ 10, SelfcheckPeriod))
    snippy::fatal(
        "Value of selfcheck option is not convertible to numeric one.");
  assert(isUInt<sizeof(unsigned) * CHAR_BIT>(SelfcheckPeriod));
  return SelfcheckPeriod;
}
unsigned long long initializeRandomEngine(uint64_t SeedValue) {
  RandEngine::init(SeedValue);
  return SeedValue;
}

static bool getStaticStackValue(Config &Cfg, const OpcodeCache &OpCC,
                                const ProgramOptions &Opts) {
  if (Opts.StaticStack.isSpecified() && !Opts.StaticStack)
    return false;
  auto &ProgCfg = Cfg.ProgramCfg;
  auto NumPrimaryInstrs =
      getExpectedNumInstrs(copyOptionsToInstrGenOptions().NumInstrs.value());
  auto HasStackSection =
      ProgCfg.Sections.hasSection(SectionsDescriptions::StackSectionName);
  auto StaticStack = !Opts.FollowTargetABI && HasStackSection &&
                     !ProgCfg.ExternalStack &&
                     !Cfg.PassCfg.hasExternalCallees() &&
                     ProgCfg.PreserveCallerSavedGroups.empty() &&
                     NumPrimaryInstrs && !Cfg.isLoopGenerationPossible(OpCC);
  // If the option is not provided, then its value is auto-detected.
  if (!Opts.StaticStack.isSpecified())
    return StaticStack;
  if (Opts.FollowTargetABI)
    snippy::fatal(
        "Incompatible options",
        "When --honor-target-abi is enabled, option -enable-static-stack "
        "is not supported.");
  if (!HasStackSection)
    snippy::fatal(
        "Incompatible options",
        "When section 'stack' is not provided, option -enable-static-stack "
        "is not supported.");
  if (ProgCfg.ExternalStack)
    snippy::fatal(
        "Incompatible options",
        "When external stack is provided, option -enable-static-stack "
        "is not supported.");
  if (Cfg.PassCfg.hasExternalCallees())
    snippy::fatal(
        "Incompatible options",
        "When external functions are provided, option -enable-static-stack "
        "is not supported.");
  if (!ProgCfg.PreserveCallerSavedGroups.empty())
    snippy::fatal("Incompatible options",
                  "When PreserveCallerSavedGroups is not empty, option "
                  "-enable-static-stack "
                  "is not supported.");
  if (!NumPrimaryInstrs)
    snippy::fatal(
        "Incompatible options",
        "When -num-instrs=all is specified, option -enable-static-stack "
        "is not supported.");
  if (Cfg.isLoopGenerationPossible(OpCC))
    snippy::fatal(
        "Incompatible options",
        "When loop generation is possible, option -enable-static-stack "
        "is not supported.");
  return true;
}

static void normalizeProgramLevelOptions(Config &Cfg, LLVMState &State,
                                         RegPoolWrapper &RP,
                                         const OpcodeCache &OpCC,
                                         std::optional<unsigned long long> Seed,
                                         const ProgramOptions &Opts) {
  auto &ProgCfg = Cfg.ProgramCfg;
  ProgCfg.ABIName = Opts.ABI;
  ProgCfg.FollowTargetABI = Opts.FollowTargetABI;
  ProgCfg.PreserveCallerSavedGroups = Opts.PreserveCallerSavedRegs;
  ProgCfg.MangleExportedNames = Opts.MangleExportedNames;
  ProgCfg.EntryPointName = Opts.EntryPointName;
  ProgCfg.ExternalStack =
      Opts.ExternalStack ||
      (ProgCfg.FollowTargetABI && !ProgCfg.hasInternalStackSection());
  ProgCfg.StaticStack = getStaticStackValue(Cfg, OpCC, Opts);
  ProgCfg.InitialRegYamlFile = Opts.InitialRegisterDataFile;
  // Here we don't use Seed.value_or() because we don't want seedOptToValue to
  // be called at all if Seed was provided
  ProgCfg.Seed = Seed.has_value() ? *Seed : seedOptToValue(Opts.Seed.value());
  // FIXME: RandomEngine initialization should be moved out of Config as well
  // as most of the stuff below
  initializeRandomEngine(ProgCfg.Seed);

  auto &Ctx = State.getCtx();
  const auto &Tgt = State.getSnippyTarget();
  const auto &RI = State.getRegInfo();
  if (!ProgCfg.hasSectionToSpillGlobalRegs() &&
      Cfg.PassCfg.hasExternalCallees())
    reserveGlobalStateRegisters(RP, Tgt);
  if (Opts.ReservedRegsList.isSpecified()) {
    if (!Cfg.PassCfg.RegisterAccess.empty())
      snippy::fatal("Incompatible options",
                    "Cannot use 'register-access' config and "
                    "'reserved-regs-list' options at the same time.");
    Cfg.PassCfg.RegisterAccess =
        deriveRegisterAccessesFromReservedList(Tgt, RI, Opts);
  }
  if (ProgCfg.FollowTargetABI && ProgCfg.ExternalStack) {
    Cfg.PassCfg.RegisterAccess[Tgt.getStackPointer()] = AccessMaskBit::RW;
  }
  for (auto &[Reg, Acc] : Cfg.PassCfg.RegisterAccess) {
    RP.addReserved(Reg, Acc);
    DEBUG_WITH_TYPE("snippy-regpool",
                    (dbgs() << "Reserved with option:\n", RP.print(dbgs())));
  }
  if (Cfg.PassCfg.CodeLayout) {
    auto HasCalls = Cfg.Histogram.hasCallInstrs(OpCC, Tgt);
    if (HasCalls) {
      if (!Cfg.PassCfg.Branches.LoopCounters.UseStack.has_value()) {
        Cfg.PassCfg.Branches.LoopCounters.UseStack = true;
        snippy::warn(
            WarningName::LoopCountersOnStack, Ctx,
            "'place-on-stack' option for loop counters implicitly enabled",
            "code layout and calls in common require such a behaviour");
      } else if (!Cfg.PassCfg.Branches.LoopCounters.UseStack.value()) {
        snippy::fatal("Incompatible options",
                      "When code-layout and calls provided, generation of "
                      "loops without spilling counters is not supported");
      }
    }
  }
  auto RegsSpilledToStack = parseSpilledRegistersOption(RP, Tgt, RI, Ctx, Opts);
  auto RegsSpilledToMem = getRegsToSpillToMem(Tgt, Cfg);
  bool HasSPRelativeInstrs = Cfg.Histogram.hasSPRelativeInstrs(OpCC, Tgt);
  if (ProgCfg.FollowTargetABI) {
    if (HasSPRelativeInstrs && !Opts.RedefineSP.isSpecified())
      snippy::fatal(
          "Incompatible options",
          "When --honor-target-abi is enabled, generation of "
          "SP-relative instructions is not supported. You can provide "
          "`redefine-sp` option to make a generation process possible");

    if (!RegsSpilledToStack.empty())
      snippy::warn(WarningName::InconsistentOptions, Ctx,
                   "--spilled-regs-list is ignored",
                   "--honor-target-abi is enabled.");
    RegsSpilledToStack.clear();
    auto ABIPreserved = Tgt.getCalleeSavedRegs(State.getSubtargetInfo());
    // Global Regs will be spilled separately as we need to spill them to
    // Memory, not stack.
    llvm::copy_if(
        ABIPreserved, std::back_inserter(RegsSpilledToStack),
        [&](auto Reg) { return !llvm::is_contained(RegsSpilledToMem, Reg); });
  }

  std::tie(ProgCfg.StackPointer, ProgCfg.ReturnAddress) = configureSPandRA(
      RP, Tgt, RI, RegsSpilledToStack, Ctx, Cfg, Opts, HasSPRelativeInstrs);
  llvm::copy(RegsSpilledToStack, std::back_inserter(ProgCfg.SpilledToStack));
  llvm::copy(RegsSpilledToMem, std::back_inserter(ProgCfg.SpilledToMem));
}
static void normalizeMemInitOptions(Config &Cfg, LLVMState &State,
                                    const MemInitOptions &Opts) {
  auto &ProgCfg = Cfg.ProgramCfg;
  auto &MemInitCfg = ProgCfg.MemoryCfg;
  MemInitCfg.InitializationMode = Opts.InitializeMemory;
  auto MemorySeedVal =
      getMemorySeed(State.getCtx(), ProgCfg.Seed, Opts.MemorySeed.value(),
                    InitializeMemory, Cfg);
  MemInitCfg.MemorySeed = MemorySeedVal;
  MemInitCfg.MemoryFile = getMemoryFile(
      State.getCtx(), Opts.InitializeMemory.value(), Opts.MemoryFile.value());
  MemInitCfg.SkipRuntimeMemInit = Opts.SkipRuntimeMemInit;
  MemInitCfg.ExternalMemInitRoutine = Opts.ExternalMemInitRoutine;
  auto NoRuntimeError = [](auto OptionName) {
    snippy::fatal(formatv(
        "Option '{0}' requires runtime memory init to be enabled", OptionName));
  };
  if (!isDuringRuntime(MemInitCfg.InitializationMode) &&
      MemInitCfg.SkipRuntimeMemInit)
    NoRuntimeError(SkipRuntimeMemInit.ArgStr);
  if (!isDuringRuntime(MemInitCfg.InitializationMode) &&
      MemInitCfg.ExternalMemInitRoutine)
    NoRuntimeError(ExternalMemInitRoutine.ArgStr);
}

static void normalizeRegInitOptions(Config &Cfg, LLVMState &State,
                                    const RegInitOptions &Opts) {
  auto &RegsCfg = Cfg.PassCfg.RegistersConfig;
  RegsCfg.InitializeRegs = Opts.InitRegsInElf;
  if ((Opts.DumpInitialRegisters.value() == "none" && Verbose) ||
      Opts.DumpInitialRegisters.value().empty()) {
    // if verbose, but no file was specified - use hardcoded default path
    RegsCfg.InitialStateOutputYaml = "initial_registers_state.yml";
  } else if (Opts.DumpInitialRegisters.value() != "none") {
    RegsCfg.InitialStateOutputYaml = Opts.DumpInitialRegisters;
  }

  if ((Opts.DumpResultingRegisters.value() == "none" &&
       (Verbose && Cfg.PassCfg.ModelPluginConfig.runOnModel())) ||
      Opts.DumpResultingRegisters.value().empty()) {
    // if verbose, but no file was specified - use hardcoded default path
    RegsCfg.FinalStateOutputYaml = "registers_state.yml";
  } else if (Opts.DumpResultingRegisters.value() != "none") {
    RegsCfg.FinalStateOutputYaml = Opts.DumpResultingRegisters;
  }
  // TODO: move this check away.
  if (Opts.ValuegramOperandsRegsInitOutputs.isSpecified() &&
      !Opts.ValueGramRegsDataFile.isSpecified())
    snippy::fatal("Incompatible options",
                  "-valuegram-operands-regs-init-outputs available only if "
                  "-valuegram-operands-regs specified");
  if (Opts.ValueGramRegsDataFile.isSpecified() && Cfg.Histogram.hasPatterns())
    snippy::fatal("Usage of valuegram-operands-regs option with specified "
                  "histogram-patterns is prohibited");

  if (Opts.ValueGramRegsDataFile.isSpecified()) {
    Cfg.DefFlowConfig.Valuegram.emplace();
    Cfg.DefFlowConfig.Valuegram->RegsHistograms =
        loadRegistersFromYaml(Opts.ValueGramRegsDataFile.value());
    Cfg.DefFlowConfig.Valuegram->ValuegramOperandsRegsInitOutputs =
        ValuegramOperandsRegsInitOutputs;
  }
}

static Error normalizeInstrGenOptions(Config &Cfg, LLVMState &State,
                                      const InstrGenOptions &Opts) {
  auto &PassCfg = Cfg.PassCfg;
  auto &InstrsCfg = PassCfg.InstrsGenerationConfig;
  auto NumPrimaryInstrs = getExpectedNumInstrs(Opts.NumInstrs.value());
  InstrsCfg.RunMachineInstrVerifier = Opts.VerifyMachineInstrs;
  InstrsCfg.ChainedRXSorted = Opts.ChainedRXSorted;
  InstrsCfg.ChainedRXSectionsFill = Opts.ChainedRXSectionsFill;
  if (Opts.ChainedRXChunkSize)
    InstrsCfg.ChainedRXChunkSize = Opts.ChainedRXChunkSize;

  if (Opts.ChainedRXChunkSize && !NumPrimaryInstrs)
    snippy::fatal(State.getCtx(),
                  "Cannot use '" + Twine(ChainedRXChunkSize.ArgStr) +
                      "' option",
                  "num-instrs is set to 'all'");
  if (Opts.ChainedRXChunkSize && !InstrsCfg.ChainedRXSectionsFill)
    snippy::warn(WarningName::InconsistentOptions, State.getCtx(),
                 "'" + Twine(ChainedRXChunkSize.ArgStr) + "' is ignored",
                 "pass 'chained-rx-sections-fill' to enable it");
  bool MayNeedRelocatedJumps =
      PassCfg.CodeLayout &&
      codeLayoutIsBig(*PassCfg.CodeLayout, State.getSnippyTarget());
  InstrsCfg.NeedsRelocations = MayNeedRelocatedJumps;
  const auto &Tgt = State.getSnippyTarget();
  InstrsCfg.NumInstrs = NumPrimaryInstrs;
  // According to documentation:
  // last-instr not specified - using default one for the target
  // last-instr empty - no last instruction (handled further)
  // last-instr specified - use it as a last instruction
  InstrsCfg.LastInstr =
      Opts.LastInstr.isSpecified() ? Opts.LastInstr : Tgt.getDefaultLastInstr();

  auto &TrackCfg = Cfg.CommonPolicyCfg->TrackCfg;
  TrackCfg.BTMode = Opts.Backtrack;
  TrackCfg.AddressVH = Opts.AddressVHOpt;

  // FIXME: we should create a special routine for tracking duplicates
  if (TrackCfg.Selfcheck && Opts.Selfcheck.isSpecified())
    return createStringError(inconvertibleErrorCode(),
                             "'selfcheck' has been specified both as an option "
                             "and as a configuration field");

  if (TrackCfg.Selfcheck)
    return Error::success();

  if (auto Period = getSelfcheckPeriod(Opts.Selfcheck.value())) {
    auto Mode = Opts.SelfcheckRefValueStorage;
    TrackCfg.Selfcheck = SelfcheckConfig{Mode, Period};

    assert(TrackCfg.Selfcheck);
    auto Err = Tgt.validateSelfcheckConfig(*TrackCfg.Selfcheck,
                                           Cfg.getOpcodeHistogram());
    if (!Err.empty())
      return createStringError(inconvertibleErrorCode(), Err);
  }

  return Error::success();
}

static void normalizeModelOptions(Config &Cfg, LLVMState &State,
                                  const ModelOptions &Opts) {
  auto &ModelCfg = Cfg.PassCfg.ModelPluginConfig;
  ModelCfg.ModelLibraries = parseModelPluginList(Opts);
  ModelCfg.ModelLogPath = Opts.ModelLogPath;
  auto &TFCfg = Cfg.PassCfg.TFOpts;
  TFCfg.LastPC = Opts.LastPC;
  if (Opts.LastPC.isSpecified() && !Opts.TraceSNTFPath.isSpecified())
    snippy::fatal("Can't set last pc", "--trace-SNTF not specified");
  if (!ModelCfg.runOnModel() && (!Opts.TraceSNTFPath.value().empty()))
    snippy::fatal("Can't convert trace to "
                  "SNTF",
                  "--model-plugin set to None");
  TFCfg.TraceSNTFPath =
      !Opts.TraceSNTFPath.value().empty()
          ? std::make_optional<std::string>(Opts.TraceSNTFPath.value())
          : std::nullopt;
}

static void mapOpcodeHistogram(yaml::IO &IO, Config &Info) {
  // Check if Histogram was filled with plugin previously
  if (!Info.Histogram.empty() && !IO.outputting())
    return;

  yaml::MappingNormalization<OpcodeHistogramNormalization, OpcodeHistogram>
      HistNorm(IO, Info.Histogram);
  // FIXME: Remove this copy of all the program options
  auto ProgramOpts = copyOptionsToProgramOptions();
  const auto &DefMainHist = ProgramOpts.DefineMainHistogram.value();
  if (ProgramOpts.DefineMainHistogram.isSpecified() && !DefMainHist.empty())
    HistNorm->DefineMainHist = DefMainHist;
  IO.mapOptional("histogram", HistNorm->OpcHistSeq);
}

void yaml::MappingTraits<Config>::mapping(yaml::IO &IO, Config &Info) {
  IO.mapOptional("sections", Info.ProgramCfg.Sections);
  // Here we call yamlize directly since memory scheme has no top-level key.
  // This could be changed in the future but it'd be a breaking change.
  yaml::MappingTraits<MemoryScheme>::mapping(IO, Info.CommonPolicyCfg->MS);
  IO.mapOptional("branches", Info.PassCfg.Branches);

  mapOpcodeHistogram(IO, Info);

  // Map burst after the opcode histogram, since it may require generating an
  // implicit base‑register group.
  // TODO: get rid of this.
  if (!IO.outputting()) {
    std::optional<BurstGramData> BurstData;
    IO.mapOptional("burst", BurstData);
    if (BurstData) {
      Info.BurstConfig.emplace(*Info.CommonPolicyCfg);
      Info.BurstConfig->Burst = std::move(BurstData).value();
    }
  } else {
    if (Info.BurstConfig)
      IO.mapRequired("burst", Info.BurstConfig->Burst);
  }

  IO.mapOptional("selfcheck", Info.CommonPolicyCfg->TrackCfg.Selfcheck);

  yaml::MappingNormalization<ImmediateHistogramNormalization,
                             ImmediateHistogram>
      ImmHistNorm(IO, Info.CommonPolicyCfg->ImmHistogram);
  IO.mapOptional("imm-hist", ImmHistNorm->Data);

  // TODO: refactor
  auto &CGLayout = Info.PassCfg.CGLayout;
  if (!IO.outputting()) {
    std::optional<FunctionDescs> Tmp;
    IO.mapOptional("call-graph", Tmp);
    if (Tmp) {
      CGLayout.emplace<FunctionDescs>(*std::move(Tmp));
    } else {
      CGLayout.emplace<CallGraphLayout>();
      yaml::MappingTraits<CallGraphLayout>::mapping(
          IO, std::get<CallGraphLayout>(CGLayout));
    }
  } else {
    if (std::holds_alternative<FunctionDescs>(CGLayout)) {
      IO.mapRequired("call-graph", std::get<FunctionDescs>(CGLayout));
    } else {
      yaml::MappingTraits<CallGraphLayout>::mapping(
          IO, std::get<CallGraphLayout>(CGLayout));
    }
  }

  Info.ProgramCfg.TargetConfig->mapConfig(IO);
  IO.mapOptional("fpu-config", Info.CommonPolicyCfg->FPUConfig);
  IO.mapOptional("code-layout", Info.PassCfg.CodeLayout);
  IO.mapOptional("scheduling", Info.PassCfg.Scheduling);
  IO.mapOptional("operands-reinitialization",
                 Info.DefFlowConfig.OperandsReinitialization);
  MappingNormalization<RegisterAccessNormalization, RegisterAccessConfig>
      RegAcc(IO, Info.PassCfg.RegisterAccess);
  IO.mapOptional("register-reservation", RegAcc->Data);
}

std::string yaml::MappingTraits<Config>::validate(yaml::IO &Io, Config &Info) {
  if (Info.PassCfg.CodeLayout && !Info.PassCfg.Branches.unaligned())
    return Twine("Code layout feature is only supported with branches "
                 "alignment set to ")
        .concat(Twine(Branchegram::Unaligned))
        .str();
  void *Ctx = Io.getContext();
  assert(Ctx && "To parse or output Config provide ConfigIOContext as "
                "context for yaml::IO");
  auto &ConfigIOCtx = *static_cast<ConfigIOContext *>(Ctx);
  return Info.CommonPolicyCfg->MS
      .validateSchemes(ConfigIOCtx.State.getCtx(), Info.ProgramCfg.Sections)
      .value_or("");
}

namespace snippy {

static void diagnoseHistogram(LLVMContext &Ctx, const OpcodeCache &OpCC,
                              OpcodeHistogram &Histogram) {
  if (Histogram.size() == 0) {
    snippy::warn(WarningName::InstructionHistogram, Ctx,
                 "Plugin didn't fill histogram",
                 "Generating instructions with only plugin calls");
    return;
  }

  auto InvalidOpcChecker = [OpCC](auto It) {
    return OpCC.desc(It.first) == nullptr;
  };
  const auto &TopOpcodes = Histogram.topOpcodes();
  if (llvm::find_if(TopOpcodes, InvalidOpcChecker) != TopOpcodes.end())
    snippy::fatal("Plugin filled histogram with invalid opcodes");

  auto InvalidWeightsChecker = [](auto It) { return It.second < 0; };
  if (llvm::find_if(TopOpcodes, InvalidWeightsChecker) != TopOpcodes.end())
    snippy::fatal("Plugin filled histogram with negative opcodes weights");
}

ProgramConfig::ProgramConfig(const SnippyTarget &Tgt, StringRef PluginFilename,
                             StringRef PluginInfoFile, const OpcodeCache &OpCC)
    : TargetConfig(Tgt.createTargetConfig()),
      PluginManagerImpl(std::make_unique<PluginManager>()),
      PluginInfoFilename(PluginInfoFile) {
  PluginManagerImpl->loadPluginLib(PluginFilename.str());
}

Config::Config(IncludePreprocessor &IPP, RegPoolWrapper &RP, LLVMState &State,
               ProgramConfig &ProgCfg, const OpcodeCache &OpCC,
               bool ParseWithPlugin)
    : Includes([&IPP] {
        auto IncludesRange = IPP.getIncludes();
        return std::vector(IncludesRange.begin(), IncludesRange.end());
      }()),
      ProgramCfg(ProgCfg),
      CommonPolicyCfg(std::make_unique<CommonPolicyConfig>(ProgramCfg)),
      DefFlowConfig(*CommonPolicyCfg), PassCfg(ProgramCfg) {
  auto &Ctx = State.getCtx();
  if (ParseWithPlugin) {
    ProgramCfg.PluginManagerImpl->parseOpcodes(
        OpCC, ProgramCfg.PluginInfoFilename, Histogram);
    diagnoseHistogram(Ctx, OpCC, Histogram);
  }
}

Expected<Config> Config::create(IncludePreprocessor &IPP, RegPoolWrapper &RP,
                                LLVMState &State, ProgramConfig &ProgCfg,
                                const OpcodeCache &OpCC, bool ParseWithPlugin,
                                std::optional<unsigned long long> Seed) {
  Config Cfg(IPP, RP, State, ProgCfg, OpCC, ParseWithPlugin);
  ConfigIOContext CfgParsingCtx{
      Cfg.Histogram,
      OpCC,
      RP,
      State,
  };

  struct DiagnosticContext {
    IncludePreprocessor &IPP;
    Error ExtraError;
  };

  DiagnosticContext DiagCtx{IPP, Error::success()};

  auto Err = loadYAMLFromBuffer(
      Cfg, IPP.getPreprocessed(),
      [&CfgParsingCtx](auto &Yin) {
        Yin.setAllowUnknownKeys(true);
        Yin.setContext(&CfgParsingCtx);
      },
      [](const auto &Diag, void *Ctx) {
        auto IsDiagAllowed = [](StringRef DiagMsg) {
          auto AllowedKeys = std::array{"options"};
          return any_of(AllowedKeys, [&DiagMsg](auto &&Allowed) {
            return DiagMsg.starts_with((detail::YAMLUnknownKeyStartString +
                                        " '" + StringRef(Allowed) + "'")
                                           .str());
          });
        };
        if (!IsDiagAllowed(Diag.getMessage())) {
          assert(Ctx);
          auto &DiagCtx = *static_cast<DiagnosticContext *>(Ctx);
          auto &IPP = DiagCtx.IPP;
          auto DiagMsg = Diag.getMessage();
          // All diagnostics about unknown keys that are not explicitly allowed
          // should be fatal to prevent silently accepting broken
          // configurations. NOTE: Don't overwrite existing errors because
          // otherwise Error will die with an assertion in the destructor.
          bool IsDisallowedKey =
              DiagMsg.starts_with(detail::YAMLUnknownKeyStartString);
          if (IsDisallowedKey && !DiagCtx.ExtraError)
            DiagCtx.ExtraError = makeFailure(Errc::InvalidArgument, DiagMsg);

          SMDiagnostic NewDiag(
              *Diag.getSourceMgr(), Diag.getLoc(),
              IPP.getCorrespondingLineID(Diag.getLineNo()).FileName,
              IPP.getCorrespondingLineID(Diag.getLineNo()).N,
              Diag.getColumnNo(),
              IsDisallowedKey ? SourceMgr::DK_Error : Diag.getKind(),
              Diag.getMessage(), Diag.getLineContents(), Diag.getRanges());
          NewDiag.print(nullptr, errs());
        }
      },
      DiagCtx);

  if (Err) {
    // Explicitly ignore extra error if failed to read YAML.
    // ExtraError can be appended to the main one and we should probably do so.
    if (DiagCtx.ExtraError)
      consumeError(std::move(DiagCtx.ExtraError));
    return std::move(Err);
  }

  if (DiagCtx.ExtraError)
    return std::move(DiagCtx.ExtraError);

  normalizeProgramLevelOptions(Cfg, State, RP, OpCC, Seed,
                               copyOptionsToProgramOptions());
  normalizeRegInitOptions(Cfg, State, copyOptionsToRegInitOptions());
  normalizeMemInitOptions(Cfg, State, copyOptionsToMemInitOptions());
  normalizeModelOptions(Cfg, State, copyOptionsToModelOptions());
  if ((Err = normalizeInstrGenOptions(Cfg, State,
                                      copyOptionsToInstrGenOptions())))
    snippy::fatal(std::move(Err));
  Cfg.complete(State, OpCC);
  Cfg.validateAll(State, OpCC, RP);
  return Cfg;
}

static void checkMemoryRegions(const SnippyTarget &SnippyTgt,
                               const Config &Cfg) {
  auto Sections = llvm::reverse(Cfg.ProgramCfg.Sections);
  auto ReservedIt = llvm::find_if(Sections, [&SnippyTgt](auto &S) {
    return SnippyTgt.touchesReservedRegion(S);
  });
  if (ReservedIt == Sections.end())
    return;
  auto *Reserved = SnippyTgt.touchesReservedRegion(*ReservedIt);
  std::string ErrBuf;
  llvm::raw_string_ostream SS{ErrBuf};
  SS << "One of layout memory regions interferes with reserved region:\n";
  outputYAMLToStream(*Reserved, SS);
  snippy::fatal(ErrBuf.c_str());
}

static bool hasCallees(const FunctionDesc &FuncDesc) {
  return FuncDesc.Callees.size();
}

static void deleteCallsIfNeeded(
    LLVMState &State, const OpcodeCache &OpCC, OpcodeHistogram &Histogram,
    const std::variant<CallGraphLayout, FunctionDescs> &CGLayout,
    MCRegister RA) {
  auto &Tgt = State.getSnippyTarget();
  auto &RI = State.getRegInfo();
  auto IsCall = [&Tgt](unsigned Opcode) { return Tgt.isCall(Opcode); };
  auto CallsWeight = Histogram.getTopOpcodesWeight(IsCall);
  if (CallsWeight < std::numeric_limits<decltype(CallsWeight)>::epsilon())
    return;
  if (std::abs(Histogram.getTotalWeight() - CallsWeight) <
      std::numeric_limits<decltype(CallsWeight)>::epsilon())
    snippy::fatal(
        "for using calls you need to add to histogram non-call instructions");

  std::visit(OverloadedCallable(
                 [&Histogram, IsCall](const FunctionDescs &Descs) -> void {
                   if (!std::any_of(Descs.Descs.begin(), Descs.Descs.end(),
                                    hasCallees)) {
                     snippy::warn(
                         WarningName::CannotGenerateCalls,
                         "Provided call-graph doesn't allow generation of "
                         "calls as no callees were found",
                         "no calls will be generated");
                     Histogram.eraseTopOpcodes(IsCall);
                   }
                 },
                 [&Histogram, IsCall](const CallGraphLayout &CGLayout) -> void {
                   if (auto NumFunc = CGLayout.FunctionNumber; NumFunc < 2) {
                     snippy::warn(WarningName::CannotGenerateCalls,
                                  "Not enough functions specified to generate "
                                  "calls (required at least 2)",
                                  "-function-number is " + Twine(NumFunc));
                     Histogram.eraseTopOpcodes(IsCall);
                   }
                 }),
             CGLayout);
  for (auto &&CallOpcode :
       make_filter_range(Histogram.uniqueOpcodes(),
                         [&](auto &&Opcode) { return Tgt.isCall(Opcode); })) {
    auto &RARegClass = Tgt.getRegClassSuitableForRA(CallOpcode, RI);
    if (!RARegClass.contains(RA))
      snippy::fatal(llvm::formatv("Call instruction {0} does not support {1} "
                                  "as return address register.",
                                  OpCC.name(CallOpcode), RI.getName(RA)));
  }
}

static void checkBurstGram(LLVMContext &Ctx, const OpcodeHistogram &Histogram,
                           const OpcodeCache &OpCC,
                           const BurstGramData &Burst) {
  if (Burst.Mode != BurstMode::CustomBurst)
    return;
  assert(Burst.Groupings);
  for (auto &&Group : *Burst.Groupings) {
    for (auto Opc : Group) {
      if (!Histogram.contains(Opc))
        snippy::fatal(
            Ctx, "Bad burst config",
            "instruction \"" + OpCC.name(Opc) +
                "\" was specified in burst grouping but not in histogram");
    }
  }
}

static void checkCompatibilityWithValuegramPolicy(const Config &Cfg,
                                                  LLVMContext &Ctx) {
  if (!Cfg.DefFlowConfig.Valuegram)
    return;
  bool FillCodeSectionMode = !Cfg.PassCfg.InstrsGenerationConfig.NumInstrs;
  if (FillCodeSectionMode)
    snippy::fatal(Ctx, "Incompatible options",
                  "When -num-instrs=all is specified, initializing "
                  "registers after each instruction is not supported.");
  if (Cfg.BurstConfig && Cfg.BurstConfig->Burst.Mode != BurstMode::Basic)
    snippy::fatal(
        Ctx, "Incompatible options",
        "Generating bursts and initializing "
        "registers after each instruction is not supported together.");
}

static void checkFPUSettings(Config &Cfg, LLVMContext &Ctx,
                             const SnippyTarget &Tgt, const MCInstrInfo &II) {
  const auto &Histogram = Cfg.Histogram;
  if (llvm::none_of(Histogram.uniqueOpcodes(), [&](auto Opcode) {
        auto &InstrDesc = II.get(Opcode);
        return Tgt.isFloatingPoint(InstrDesc);
      }))
    return;
  auto &FPUConfig = Cfg.CommonPolicyCfg->FPUConfig;
  if (!Cfg.PassCfg.ModelPluginConfig.runOnModel() && FPUConfig.needsModel())
    snippy::fatal(
        "Invalid FPU config",
        Twine("\"")
            .concat(FloatOverwriteModeName<
                    FloatOverwriteMode::IF_MODEL_DETECTED_NAN>)
            .concat("\" overwrite heuristic requires model to be specified"));
}

static void checkGlobalRegsSpillSettings(const SnippyTarget &Tgt,
                                         const MCRegisterInfo &RI,
                                         const Config &Cfg, LLVMContext &Ctx) {
  if (!Cfg.PassCfg.hasExternalCallees() ||
      Cfg.ProgramCfg.hasSectionToSpillGlobalRegs())
    return;
  auto Globals = Tgt.getGlobalStateRegs();
  auto RegNames =
      llvm::map_range(Globals, [&](auto Reg) { return RI.getName(Reg); });
  std::string RegNamesStr;
  raw_string_ostream SS(RegNamesStr);
  SS << "[";
  llvm::interleaveComma(RegNames, SS);
  SS << "]";
  snippy::warn(WarningName::InconsistentOptions, Ctx,
               "External callees were found in call-graph but neither \"" +
                   Twine(SectionsDescriptions::UtilitySectionName) +
                   "\" nor \"" + Twine(SectionsDescriptions::StackSectionName) +
                   "\" sections were found",
               "Implicitly reserving registers: " + Twine(RegNamesStr));
  return;
}

static void checkFullSizeGenerationRequirements(const MCInstrInfo &II,
                                                const SnippyTarget &Tgt,
                                                const OpcodeCache &OpCC,
                                                const Config &Cfg) {
  bool FillCodeSectionMode = !Cfg.PassCfg.InstrsGenerationConfig.NumInstrs;
  if (FillCodeSectionMode && Cfg.Histogram.hasCFInstrs(OpCC))
    snippy::fatal(
        "when -num-instrs=all is specified, branches are not supported");
  if (FillCodeSectionMode && Cfg.Histogram.hasCallInstrs(OpCC, Tgt))
    snippy::fatal("when -num-instrs=all is specified, calls are not supported");

  if (FillCodeSectionMode && Cfg.CommonPolicyCfg->TrackCfg.Selfcheck)
    snippy::fatal(
        "when -num-instrs=all is specified, selfcheck is not supported");
  if (FillCodeSectionMode && Cfg.BurstConfig &&
      Cfg.BurstConfig->Burst.Mode != BurstMode::Basic)
    snippy::fatal(
        "when -num-instrs=all is specified, burst mode is not supported");
}

static size_t getMinimumSelfcheckSize(const Config &Cfg) {
  auto &TrackCfg = Cfg.CommonPolicyCfg->TrackCfg;
  assert(TrackCfg.Selfcheck);

  size_t BlockSize = 2 * ProgramConfig::getSCStride();
  // Note: There are cases when we have some problems for accurate calculating
  // of selcheck section size.
  //       Consequently it can potentially cause overflow of selfcheck
  //       section, So it's better to provide selfcheck section in Layout
  //       explicitly
  return alignTo(Cfg.PassCfg.InstrsGenerationConfig.NumInstrs.value_or(0) *
                     BlockSize / TrackCfg.Selfcheck->Period,
                 ProgramConfig::getPageSize());
}

static void diagnoseSelfcheckSection(LLVMState &State, const Config &Cfg,
                                     size_t MinSize) {
  const auto &Sections = Cfg.ProgramCfg.Sections;
  if (!Sections.hasSection(SectionsDescriptions::SelfcheckSectionName))
    return;
  auto &SelfcheckSection =
      Sections.getSection(SectionsDescriptions::SelfcheckSectionName);
  auto M = SelfcheckSection.M;
  if (!(M.R() && M.W() && !M.X()))
    snippy::fatal(State.getCtx(), "Wrong layout file",
                  "\"" + Twine(SectionsDescriptions::SelfcheckSectionName) +
                      "\" section must be RW");
  auto &SelfcheckSectionSize = SelfcheckSection.Size;
  if (SelfcheckSectionSize < MinSize)
    snippy::fatal(
        State.getCtx(),
        "Cannot use \"" + Twine(SectionsDescriptions::SelfcheckSectionName) +
            "\" section from layout",
        "it does not fit selfcheck data (" + Twine(MinSize) + " bytes)");
  if ((SelfcheckSectionSize !=
       alignTo(SelfcheckSectionSize, ProgramConfig::getPageSize())) ||
      SelfcheckSection.VMA !=
          alignTo(SelfcheckSection.VMA, ProgramConfig::getPageSize()))
    snippy::fatal(State.getCtx(),
                  "Cannot use \"" +
                      Twine(SectionsDescriptions::SelfcheckSectionName) +
                      "\" section from layout",
                  "it has unaligned memory settings");
}

static void
checkValuegramSamplerSettings(const OpcodeValuegramSettings &Settings,
                              unsigned Opcode, const OpcodeCache &OpCC,
                              const LLVMState &State) {
  const auto &InstrInfo = State.getInstrInfo();
  for (auto &&Cfg : Settings) {
    if (Cfg.getKind() != OpcodeValuegramOpcSettingsEntry::EntryKind::Operands)
      continue;
    const auto &OpValues = cast<OpcodeValuegramOperandsEntry>(Cfg.get());
    const auto &InstrDesc = InstrInfo.get(Opcode);
    auto NumOperands = InstrDesc.getNumOperands(),
         NumDefs = InstrDesc.getNumDefs();

    size_t Initializable = llvm::count_if(
        llvm::seq(NumDefs, NumOperands), [&InstrDesc, &State](auto OpIndx) {
          return State.isReinitializableOperand(InstrDesc, OpIndx);
        });
    if (Initializable != OpValues.Values.size())
      snippy::fatal(
          "Invalid opcode valuegram",
          createStringError(
              (std::make_error_code(std::errc::invalid_argument)),
              llvm::formatv(
                  "The number of values is not equal to the number of "
                  "initializable operands for the \"{0}\" opcode. Expected "
                  "{1} values but {2} were specified",
                  InstrInfo.getName(Opcode), std::to_string(Initializable),
                  std::to_string(OpValues.Values.size()))));
  }
}

static void checkOpcodeToSettingsMap(const WeightedOpcToSettingsMaps &Map,
                                     const OpcodeHistogram &Histogram,
                                     const OpcodeCache &OpCC,
                                     const LLVMState &State) {
  const auto &Tgt = State.getSnippyTarget();
  for (auto Opc : Histogram.uniqueOpcodes()) {
    for (auto &&DataSourceMapAndWeight : Map) {
      auto &DataSourceMap = DataSourceMapAndWeight.first;
      assert(DataSourceMap.count(Opc));
      auto &OpcodeSettings = DataSourceMap.getSettingsForOpcode(Opc, OpCC);
      if (!OpcodeSettings.empty()) {
        if (auto E = Tgt.checkOperandsReinitializationForbidden(Opc))
          snippy::fatal("Invalid opcode valuegram", std::move(E));
        if (auto E = Tgt.checkOperandsReinitializationSupported(
                Opc, State.getInstrInfo())) {
          snippy::warn(WarningName::OperandsReinitialization,
                       toString(std::move(E)),
                       "This instruction will be generated as usual.");
          continue;
        }
        checkValuegramSamplerSettings(OpcodeSettings, Opc, OpCC, State);
      }
    }
  }
}

static void reportInvalidIHOperandsNumError(const MCInstrInfo &InstrInfo,
                                            unsigned Opc, size_t ExpectedNum,
                                            size_t RealNum) {
  snippy::fatal(
      "Immediate histogram",
      createStringError(
          (std::make_error_code(std::errc::invalid_argument)),
          llvm::formatv(
              "The number of operands entries is not equal to the number of "
              "immediate operands for the \"{0}\" opcode. Expected "
              "{1} entries but {2} were specified",
              InstrInfo.getName(Opc), ExpectedNum, RealNum)));
}

static void validateImmHist(const OpcodeToImmHistSequenceMap &IHMap,
                            const OpcodeHistogram &H, const OpcodeCache &OpCC,
                            const LLVMState &State) {
  if (IHMap.empty())
    return;
  const auto &Tgt = State.getSnippyTarget();
  const auto &InstrInfo = State.getInstrInfo();

  for (auto Opc : make_first_range(H.opcodeProbabilities())) {
    const auto &Settings = IHMap.getConfigForOpcode(Opc);
    if (!Settings.isPerOperand())
      continue;
    const auto &OperandsMap = Settings.getOperandsMap();
    size_t MapSize = OperandsMap.size();
    size_t TargetSize = Tgt.getNumImmOperands(InstrInfo.get(Opc));
    if (MapSize != TargetSize)
      reportInvalidIHOperandsNumError(InstrInfo, Opc, TargetSize, MapSize);
  }
}

void Config::validateAll(LLVMState &State, const OpcodeCache &OpCC,
                         const RegPoolWrapper &RP) {
  auto &Ctx = State.getCtx();
  auto &Tgt = State.getSnippyTarget();
  auto &TM = State.getTargetMachine();
  auto &CGLayout = PassCfg.CGLayout;
  if (PassCfg.ModelPluginConfig.runOnModel() &&
      (ProgramCfg.InitialRegYamlFile.empty() && !InitRegsInElf))
    snippy::warn(
        WarningName::NonReproducibleExecution,
        formatv("Execution on model without \"{0}\" option enabled will lead "
                "to non-reproducible execution as register will be assumed to "
                "be initialized with random values",
                InitRegsInElf.ArgStr),
        formatv("Enable explicit register initialization with option \"{0}\" "
                "or dump random initial values with option \"{1}\" and suppres "
                "with \"-Wno-error\" option",
                InitRegsInElf.ArgStr, DumpInitialRegisters.ArgStr)

    );
  if (std::holds_alternative<CallGraphLayout>(CGLayout))
    std::get<CallGraphLayout>(CGLayout).validate(Ctx);
  const auto &Sections = ProgramCfg.Sections;
  if (Sections.empty())
    fatal(Ctx, "Incorrect list of sections", "list is empty");

  auto *II = TM.getMCInstrInfo();
  assert(II);

  // General-purpose RW sections are only required when the histogram contains
  // memory-access instructions. Register-only snippets can be generated with
  // RX sections only.
  bool NeedsGeneralRWSection =
      Histogram.getOpcodesProbability(
          [&](unsigned Opcode) { return isLoadStoreInstr(Opcode, *II); }) > 0.0;

  if (Sections.generalRWSections().empty() && NeedsGeneralRWSection)
    fatal(Ctx, "Incorrect list of sections",
          "there are no general purpose RW sections");
  if (std::any_of(Sections.begin(), Sections.end(), [&Sections](auto &S1) {
        return std::count_if(Sections.begin(), Sections.end(),
                             [&S1](auto &S2) { return S2.ID == S1.ID; }) != 1;
      }))
    snippy::fatal(Ctx, "Incorrect list of sections",
                  "List contains duplicate section IDs");
  diagnoseXSections(Ctx, Sections.begin(), Sections.end(),
                    PassCfg.Branches.Alignment);

  if (Sections.size() > 1)
    for (auto SecIt = Sections.begin(); SecIt != std::prev(Sections.end());
         ++SecIt) {
      if (SecIt->interfere(*std::next(SecIt))) {
        std::stringstream SS;
        SS << "section " << SecIt->getName().str() << " and section "
           << std::next(SecIt)->getName().str() << " are interfering";
        snippy::fatal(Ctx, "Incorrect list of sections", SS.str());
      }
    }
  if (!Sections.hasSection(SectionsDescriptions::SelfcheckSectionName) &&
      (CommonPolicyCfg->TrackCfg.Selfcheck &&
       CommonPolicyCfg->TrackCfg.Selfcheck->isSelfcheckSectionRequired()))
    snippy::fatal(Twine("Missing '") +
                      SectionsDescriptions::SelfcheckSectionName +
                      Twine("' section"),
                  "it is required to enable selfcheck");
  if (BurstConfig)
    checkBurstGram(Ctx, Histogram, OpCC, BurstConfig->Burst);
  checkMemoryRegions(Tgt, *this);
  Tgt.checkInstrTargetDependency(Histogram, OpCC, ProgramCfg);
  if (hasTrackingMode())
    Tgt.checkTrackingRestrictions(Histogram);
  checkCompatibilityWithValuegramPolicy(*this, Ctx);

  checkFPUSettings(*this, Ctx, Tgt, *II);
  checkGlobalRegsSpillSettings(State.getSnippyTarget(), State.getRegInfo(),
                               *this, Ctx);
  checkFullSizeGenerationRequirements(State.getInstrInfo(),
                                      State.getSnippyTarget(), OpCC, *this);

  if (!PassCfg.ModelPluginConfig.runOnModel() &&
      !PassCfg.RegistersConfig.FinalStateOutputYaml.empty())
    snippy::fatal("Dump resulting registers can't be done",
                  "dump-registers-yaml option is passed but model-plugin "
                  "is not provided.");

  if (hasCallInstrs(OpCC, Tgt)) {
    auto RA = ProgramCfg.ReturnAddress;
    if (RP.isReserved(RA))
      snippy::fatal(State.getCtx(),
                    "Cannot generate requested call instructions",
                    "return address register is explicitly reserved.");
  }

  if (auto &PreserveGroups = ProgramCfg.PreserveCallerSavedGroups;
      !PreserveGroups.empty()) {
    if (!ProgramCfg.stackEnabled())
      snippy::fatal(Ctx, "Cannot preserve requested caller-saved registers",
                    "no stack space allocated.");

    auto StrClasses = Tgt.getCallerSavedRegGroups();
    SmallVector<std::string, 3> WrongGroups;
    // std::set_difference requires sorted ranges
    llvm::sort(StrClasses);
    llvm::sort(PreserveGroups);
    // erase duplicates
    PreserveGroups.erase(llvm::unique(PreserveGroups), PreserveGroups.end());
    std::set_difference(PreserveGroups.begin(), PreserveGroups.end(),
                        StrClasses.begin(), StrClasses.end(),
                        std::back_inserter(WrongGroups));
    if (!WrongGroups.empty()) {
      StringRef ErrorDesc = WrongGroups.size() == 1
                                ? "is an invalid register group name"
                                : "are invalid register group names";
      snippy::fatal(llvm::formatv("'{0}' {1}. "
                                  "Choose one of the following: [{2}]",
                                  llvm::join(WrongGroups, ", "), ErrorDesc,
                                  llvm::join(StrClasses, ", ")));
    }
    if (!PassCfg.hasExternalCallees())
      snippy::warn(
          WarningName::InconsistentOptions, State.getCtx(),
          llvm::formatv("--{0} is ignored", PreserveCallerSavedRegs.ArgStr),
          "no external callee functions were specified.");
  }

  auto SP = ProgramCfg.StackPointer;
  if (llvm::any_of(ProgramCfg.SpilledToStack,
                   [SP](auto Reg) { return Reg == SP; }))
    snippy::fatal("Stack pointer cannot be spilled. Remove it from "
                  "spill register list.");
  if (!ProgramCfg.stackEnabled()) {
    if (!ProgramCfg.SpilledToStack.empty())
      snippy::fatal(Ctx, "Cannot spill requested registers",
                    "no stack space allocated.");

    auto &CGL = PassCfg.CGLayout;
    if (hasCallInstrs(OpCC, Tgt) &&
        std::visit([](auto &&Layout) { return Layout.getDepth(); }, CGL) > 1)
      snippy::fatal(
          State.getCtx(), "Cannot generate requested call instructions",
          "layout allows calls with depth>=1 but stack space is not provided.");
  }

  if (ProgramCfg.ExternalStack) {
    if (PassCfg.ModelPluginConfig.runOnModel())
      snippy::fatal(Ctx, "Cannot run snippet on model",
                    "external stack was enabled.");
    if (ProgramCfg.Sections.hasSection(
            SectionsDescriptions::StackSectionName)) {
      snippy::warn(WarningName::InconsistentOptions, Ctx,
                   "Section 'stack' will not be used",
                   "external stack was enabled.");
    }
  }
  if (ProgramCfg.Sections.hasSection(
          SectionsDescriptions::SelfcheckSectionName) &&
      CommonPolicyCfg->TrackCfg.Selfcheck)
    diagnoseSelfcheckSection(State, *this, getMinimumSelfcheckSize(*this));
  if (DefFlowConfig.Valuegram.has_value() &&
      DefFlowConfig.OperandsReinitialization.has_value())
    snippy::fatal("Usage of valuegram-operands-regs option with specified "
                  "operands-reinitialization is prohibited");
  if (DefFlowConfig.OperandsReinitialization.has_value())
    checkOpcodeToSettingsMap(DefFlowConfig.OpcodeToORSettingsMap, Histogram,
                             OpCC, State);
  validateImmHist(CommonPolicyCfg->ImmHistMap, Histogram, OpCC, State);
}

void Config::complete(LLVMState &State, const OpcodeCache &OpCC) {
  // FIXME: section sorting must be done internally by ProgramConfig yaml
  // parser.
  std::sort(ProgramCfg.Sections.begin(), ProgramCfg.Sections.end(),
            [](auto &S1, auto &S2) { return S1.VMA < S2.VMA; });

  // Distribute information from unified histogram to different config parts.

  if (BurstConfig) {
    BurstConfig->Burst.convertToCustomMode(Histogram, State.getInstrInfo());
    BurstConfig->Burst.removeUnsupportedOpcodes(State, OpCC);
  }
  CommonPolicyCfg->setupImmHistMap(OpCC, Histogram);
  DefFlowConfig.setupOROpcodeMap(OpCC, Histogram);

  auto UsedInBurst = [&](auto Opc) -> bool {
    if (!BurstConfig.has_value())
      return false;
    auto &BCfg = *BurstConfig;
    auto BurstOpcodes = BCfg.Burst.getAllBurstOpcodes();
    return BurstOpcodes.count(Opc);
  };
  // Data flow histogram.
  auto &DFHistogram = DefFlowConfig.DataFlowHistogram;
  DFHistogram = Histogram;
  deleteCallsIfNeeded(State, OpCC, DFHistogram, PassCfg.CGLayout,
                      ProgramCfg.ReturnAddress);
  auto DFOpcodesToErase = [&](unsigned Opcode) {
    auto *Desc = OpCC.desc(Opcode);
    const auto &Tgt = State.getSnippyTarget();
    assert(Desc);
    return Desc->isBranch() || UsedInBurst(Opcode) ||
           !Tgt.canBeGeneratedAsCommonInstr(*Desc);
  };
  DFHistogram.eraseTopOpcodes(std::move(DFOpcodesToErase));

  // Control flow histogram:
  auto &CFHistogram = PassCfg.BranchOpcodes;
  // CF instructions can only present in the top level of the OpcodeHistogram
  // tree
  CFHistogram.insertTopOpcodes(Histogram.topOpcodes());
  auto CFOpcodesToErase = [&OpCC](unsigned Opcode) {
    auto *Desc = OpCC.desc(Opcode);
    assert(Desc);
    return !Desc->isBranch();
  };
  CFHistogram.eraseTopOpcodes(CFOpcodesToErase);
  if (BurstConfig) {
    auto &BurstWeights = BurstConfig->BurstOpcodeWeights;
    auto &&BurstOpcodes = BurstConfig->Burst.getAllBurstOpcodes();
    auto AllPresentInHistogram =
        llvm::make_filter_range(BurstOpcodes, [&](auto &&Opcode) {
          return Histogram.contains(Opcode);
        });
    llvm::transform(AllPresentInHistogram,
                    std::inserter(BurstWeights, BurstWeights.begin()),
                    [&](auto &&Opcode) {
                      auto FoundOpt = Histogram.find(Opcode);
                      assert(FoundOpt.has_value());
                      return FoundOpt.value();
                    });
  }
}

static auto getContentsFromRelativePath(StringRef ParentDirectory,
                                        StringRef RelativePath) {
  SmallVector<char> Path{ParentDirectory.begin(), ParentDirectory.end()};
  sys::path::append(Path, RelativePath);
  auto SearchLocation = StringRef{Path.data(), Path.size()};
  LLVM_DEBUG(dbgs() << "searching include at: " << SearchLocation << "\n");
  auto Contents = MemoryBuffer::getFile(SearchLocation);
  if (Contents) {
    LLVM_DEBUG(dbgs() << "  include found, contents retrived successfully!\n");
  } else {
    LLVM_DEBUG(dbgs() << "  could not find include at the specified location ("
                      << Contents.getError().message() << ")\n");
  }
  return Contents;
}

using MemBufStrPair = std::pair<std::unique_ptr<MemoryBuffer>, std::string>;

static ErrorOr<MemBufStrPair>
makeBufPathPairOrErr(ErrorOr<std::unique_ptr<MemoryBuffer>> BufOrErr,
                     StringRef Path) {
  if (BufOrErr.getError())
    return BufOrErr.getError();
  return std::make_pair(std::move(BufOrErr.get()), Path.str());
}

static ErrorOr<MemBufStrPair>
makeRelBufPathPairOrErr(ErrorOr<std::unique_ptr<MemoryBuffer>> Contents,
                        StringRef Filename, StringRef ParentPath) {
  SmallString<32> AbsolutePath;
  sys::path::append(AbsolutePath, ParentPath, Filename);
  return makeBufPathPairOrErr(std::move(Contents), AbsolutePath.str().str());
}

static ErrorOr<MemBufStrPair>
getIncludeFileContentsAndPath(StringRef ParentPath,
                              const std::vector<std::string> &ExtraIncludeDirs,
                              StringRef IncludeFilename) {
  LLVM_DEBUG(dbgs() << "processing include: " << IncludeFilename << "\n");
  if (!sys::path::is_relative(IncludeFilename)) {
    LLVM_DEBUG(dbgs() << "include file has an absolute path\n");
    return makeBufPathPairOrErr(MemoryBuffer::getFile(IncludeFilename),
                                IncludeFilename.str());
  }

  LLVM_DEBUG(dbgs() << "include file has a relative path\n");
  auto Contents = getContentsFromRelativePath(ParentPath, IncludeFilename);
  if (Contents)
    return makeRelBufPathPairOrErr(std::move(Contents), IncludeFilename.str(),
                                   ParentPath.str());

  for (const auto &IncludeDir : ExtraIncludeDirs) {
    LLVM_DEBUG(dbgs() << "trying extra include dir: " << IncludeDir << "\n");
    auto Contents = getContentsFromRelativePath(IncludeDir, IncludeFilename);
    if (Contents)
      return makeRelBufPathPairOrErr(std::move(Contents), IncludeFilename.str(),
                                     IncludeDir);
  }

  return make_error_code(errc::no_such_file_or_directory);
}

static std::vector<std::string> getConfigIncludeFiles(StringRef Filename) {
  snippy::IncludeParsingWrapper IPW;
  auto Err = loadYAMLIgnoreUnknownKeys(IPW, Filename);
  if (Err)
    snippy::fatal(toString(std::move(Err)).c_str());
  return IPW.Includes;
}

bool lineIsEmpty(StringRef Line) {
  auto Pos = Line.find_first_not_of(" \t\n");
  return Pos == StringRef::npos;
}

std::string commentIncludes(StringRef Text, unsigned IncludesN) {
  if (IncludesN == 0)
    return Text.str();
  std::string Res;
  raw_string_ostream SS(Res);
  auto StartPos = Text.find("\ninclude:") + 1;
  assert(StartPos != StringRef::npos);
  SS << Text.substr(0, StartPos);
  auto LeftToRead = Text.substr(StartPos).str();
  std::stringstream IS(LeftToRead);
  unsigned Cnt = 0;
  for (std::string Line; std::getline(IS, Line);) {
    if (Cnt < IncludesN + 1) {
      SS << "# " << Line << "\n";
      if (!lineIsEmpty(Line))
        ++Cnt;
    } else
      SS << Line << "\n";
  }
  return Res.substr(0, Res.size() - 1);
}

std::string endLineIfNeeded(StringRef Str) {
  if (Str.empty())
    return "";
  if (Str.back() == '\n')
    return "";
  return "\n";
}

static void checkSubFileContents(StringRef SubFileName, StringRef Contents) {
  if (Contents.find("\ninclude:") != std::string::npos) {
    std::string Msg;
    raw_string_ostream SS(Msg);
    SS << "In file \"" << SubFileName << "\""
       << ": included file cannot contain \"include\" section."
       << "\n";
    snippy::fatal(StringRef(Msg));
  }
}

void IncludePreprocessor::mergeFile(StringRef FileName, StringRef Contents) {
  checkSubFileContents(FileName, Contents);
  std::istringstream IS(Contents.str());
  unsigned LocalIdx = 1; // Line count starts from 1
  for (std::string Line; std::getline(IS, Line); ++LocalIdx) {
    // FileName is passed as a non-owning StringRef. This is intended to
    // avoid copying absolute filepaths for each line
    Lines.emplace_back(LineID{FileName, LocalIdx});
  }
  Text += Contents;
  Text += endLineIfNeeded(Contents);
}

IncludePreprocessor::IncludePreprocessor(
    StringRef Filename, const std::vector<std::string> &IncludeDirs,
    LLVMContext &Ctx)
    : PrimaryFilename(Filename) {
  auto SubFiles = getConfigIncludeFiles(Filename);
  auto ParentDirectoryPath = sys::path::parent_path(Filename);
  for (StringRef IncludeFileName : SubFiles) {
    auto IncludeFileContentsAndPath = getIncludeFileContentsAndPath(
        ParentDirectoryPath, IncludeDirs, IncludeFileName);
    if (!IncludeFileContentsAndPath)
      fatal(Ctx, "Failed to open file \"" + IncludeFileName + "\"",
            IncludeFileContentsAndPath.getError().message());
    auto &&[Contents, Path] = *IncludeFileContentsAndPath;
    auto [InsIter, _] = IncludedFiles.insert(Path);
    mergeFile(*InsIter, Contents->getBuffer());
  }
  auto MemBufOrErr = MemoryBuffer::getFile(Filename);
  if (auto EC = MemBufOrErr.getError(); !MemBufOrErr)
    fatal(Ctx, "Failed to open file \"" + Filename + "\"", EC.message());
  mergeFile(Filename,
            commentIncludes((*MemBufOrErr)->getBuffer(), SubFiles.size()));
}

IncludePreprocessor::IncludePreprocessor(StringRef YAMLText, LLVMContext &Ctx)
    : PrimaryFilename("<in-memory>"), Text(YAMLText.str()) {}

void Config::dump(raw_ostream &OS, const ConfigIOContext &Ctx) const {
  outputYAMLToStream(const_cast<Config &>(*this), OS,
                     [&Ctx = const_cast<ConfigIOContext &>(Ctx)](auto &IO) {
                       IO.setContext(&Ctx);
                     });
}

} // namespace snippy
} // namespace llvm
