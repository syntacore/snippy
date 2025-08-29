//===-- Config.cpp ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/Config.h"
#include "snippy/Config/Branchegram.h"
#include "snippy/Config/BurstGram.h"
#include "snippy/Config/MemoryScheme.h"
#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/Config/Selfcheck.h"
// FIXME: remove dependency on Generator library
#include "snippy/Generator/LLVMState.h"
#include "snippy/Generator/MemoryManager.h"
#include "snippy/Generator/RegisterPool.h"
#include "snippy/Support/Options.h"
#include "snippy/Support/YAMLHistogram.h"
#include "snippy/Target/Target.h"

#include "llvm/Support/Errc.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/YAMLTraits.h"
#include <istream>
#include <sstream>
#include <variant>

#define DEBUG_TYPE "snippy-layout-config"

namespace llvm {

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
  struct NormalizedGroupings final {
    std::vector<SList> Groupings;

    NormalizedGroupings(yaml::IO &) {}

    NormalizedGroupings(
        yaml::IO &IO, const std::optional<BurstGramData::GroupingsTy> &Denorm) {
      if (Denorm.has_value()) {
        void *Ctx = IO.getContext();
        assert(Ctx && "To parse or output BurstGram provide ConfigIOContext as "
                      "context for yaml::IO");
        const auto &OpCC = static_cast<const ConfigIOContext *>(Ctx)->OpCC;
        transform(*Denorm, std::back_inserter(Groupings),
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

    std::optional<BurstGramData::GroupingsTy> denormalize(yaml::IO &IO) {
      BurstGramData::GroupingsTy Denorm;
      void *Ctx = IO.getContext();
      assert(Ctx && "To parse or output BurstGram provide ConfigIOContext as "
                    "context for yaml::IO");
      const auto &OpCC = static_cast<const ConfigIOContext *>(Ctx)->OpCC;
      transform(
          Groupings, std::back_inserter(Denorm), [&OpCC](const auto &Vec) {
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
      if (Denorm.empty())
        return std::nullopt;
      return Denorm;
    }
  };

  static void mapping(yaml::IO &IO, BurstGramData &Burst) {
    IO.mapRequired("min-size", Burst.MinSize);
    IO.mapRequired("max-size", Burst.MaxSize);
    IO.mapRequired("mode", Burst.Mode);
    yaml::MappingNormalization<NormalizedGroupings,
                               std::optional<BurstGramData::GroupingsTy>>
        Keys(IO, Burst.Groupings);
    IO.mapOptional("groupings", Keys->Groupings);
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

LLVM_SNIPPY_YAML_INSTANTIATE_HISTOGRAM_IO(snippy::OpcodeHistogramDecodedEntry);
LLVM_SNIPPY_YAML_IS_HISTOGRAM_DENORM_ENTRY(snippy::OpcodeHistogramDecodedEntry)

namespace snippy {

extern cl::OptionCategory Options;
cl::OptionCategory ProgramOptionsCategory("Snippy Program Level Options");
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
} // namespace snippy

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
  ImmHistOpcodeSettings::Kind Kind = ImmHistOpcodeSettings::Kind::Custom;
  ImmediateHistogramSequence Seq;
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
  }
  ImmHistOpcodeSettings denormalize(yaml::IO &) {
    if (Data.Kind == ImmHistOpcodeSettings::Kind::Custom)
      return ImmHistOpcodeSettings(Data.Seq);
    if (Data.Kind == ImmHistOpcodeSettings::Kind::Uniform)
      return ImmHistOpcodeSettings();
    llvm_unreachable("Unknown opcode settings kind");
  }
};

template <> struct yaml::PolymorphicTraits<ImmHistOpcodeSettingsNorm> {
  static yaml::NodeKind getKind(const ImmHistOpcodeSettingsNorm &Info) {
    if (Info.Kind == ImmHistOpcodeSettings::Kind::Uniform)
      return NodeKind::Scalar;
    if (Info.Kind == ImmHistOpcodeSettings::Kind::Custom)
      return NodeKind::Sequence;
    llvm_unreachable("Unknown map value kind in ImmHistOpcodeSettings");
  }

  static ImmHistOpcodeSettings::Kind &
  getAsScalar(ImmHistOpcodeSettingsNorm &Info) {
    return Info.Kind;
  }

  static ImmediateHistogramSequence &
  getAsSequence(ImmHistOpcodeSettingsNorm &Info) {
    return Info.Seq;
  }

  static ImmediateHistogramSequence &getAsMap(ImmHistOpcodeSettingsNorm &Info) {
    Info.IO.setError("Immediate histogram opcode setting should be either "
                     "sequence or scalar. But map was encountered.");
    snippy::fatal("Failed to parse configuration file.");
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

static void parseReservedRegistersOption(RegPoolWrapper &RP,
                                         const SnippyTarget &Tgt,
                                         const MCRegisterInfo &RI,
                                         const ProgramOptions &Opts) {
  for (auto &&RegName : Opts.ReservedRegsList) {
    auto Reg = findRegisterByName(Tgt, RI, RegName);
    if (!Reg)
      snippy::fatal(formatv("Illegal register name {0}"
                            " is specified in --reserved-regs-list",
                            RegName));
    SmallVector<Register> PhysRegs;
    Tgt.getPhysRegsFromUnit(Reg.value(), RI, PhysRegs);
    llvm::for_each(PhysRegs, [&RP](auto SimpleReg) {
      RP.addReserved(SimpleReg, AccessMaskBit::GRW);
    });
    DEBUG_WITH_TYPE("snippy-regpool",
                    (dbgs() << "Reserved with option:\n", RP.print(dbgs())));
  }
}

// We want to spill certain global register (e.g. Thread Pointer and Global
// Pointer) to memory instead of stack as we want to spill and reload them
// several times throughout the program and we won't be able to do that if we
// spill them to stack.
static std::vector<MCRegister> getRegsToSpillToMem(const SnippyTarget &Tgt,
                                                   const Config &Cfg) {
  if (!Cfg.PassCfg.hasExternalCallees() ||
      !Cfg.ProgramCfg->hasSectionToSpillGlobalRegs())
    return {};
  return Tgt.getGlobalStateRegs();
}

static std::vector<MCRegister>
parseSpilledRegistersOption(const RegPoolWrapper &RP, const SnippyTarget &Tgt,
                            const MCRegisterInfo &RI, LLVMContext &Ctx,
                            const ProgramOptions &Opts) {
  std::vector<MCRegister> SpilledRegs;
  for (auto &&RegName : Opts.SpilledRegisterList) {
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

static MCRegister getRealStackPointer(const RegPoolWrapper &RP,
                                      const SnippyTarget &Tgt,
                                      const MCRegisterInfo &RI,
                                      std::vector<MCRegister> &SpilledToStack,
                                      LLVMContext &Ctx, Config &Cfg,
                                      const ProgramOptions &Opts) {
  auto SP = Tgt.getStackPointer();
  bool FollowTargetABI = Cfg.ProgramCfg->FollowTargetABI;
  std::string RedefineSP = Opts.RedefineSP;
  if (FollowTargetABI) {
    if (RedefineSP != "any" && RedefineSP != "SP")
      snippy::warn(
          WarningName::InconsistentOptions, Ctx,
          "When using --honor-target-abi and --redefine-sp=" +
              Twine(RedefineSP) +
              " options together, target ABI may not be preserved in case of "
              "traps",
          "use these options in combination only for valid code generation");
    else
      RedefineSP = "SP";
  }

  if (RedefineSP == "SP")
    return SP;

  MCRegister RealSP = MCRegister::NoRegister;
  bool CanUseSP = !(RedefineSP == "any-not-SP");
  const auto &SPRegClass = Tgt.getRegClassSuitableForSP(RI);
  auto BasicFilter = Tgt.filterSuitableRegsForStackPointer();

  auto FullFilter = [&](auto Reg) {
    return std::invoke(BasicFilter, Reg) || (!CanUseSP && Reg == SP) ||
           (!FollowTargetABI && llvm::any_of(SpilledToStack, [Reg](auto SpReg) {
             return SpReg == Reg;
           }));
  };

  std::string RegPrefix = "reg::";
  if (RedefineSP.rfind(RegPrefix, 0) != std::string::npos) {
    auto RegStr = RedefineSP.substr(RegPrefix.size());
    auto Reg = findRegisterByName(Tgt, RI, RegStr);
    if (!Reg)
      snippy::fatal(formatv("Illegal register name {0}"
                            " is specified in --redefine-sp",
                            RegStr));

    if (RP.isReserved(Reg.value()))
      snippy::fatal(
          formatv("Register {0} cannot redefine stack pointer, because it is "
                  "explicitly reserved.\n",
                  RegStr));

    if (FullFilter(Reg.value()))
      snippy::fatal(
          formatv("Register {0} specified in --redefine-sp is not suitable "
                  "for stack pointer redefinition",
                  RegStr));

    RealSP = Reg.value();
  } else if (RedefineSP == "any" || RedefineSP == "any-not-SP") {
    RealSP =
        RP.getAvailableRegister("stack pointer", RI, SPRegClass, FullFilter);
  } else {
    snippy::fatal(
        formatv("\"{0}\", passed to --redefine-sp is not valid option value",
                RedefineSP));
  }

  // We need to spill SP if it is not used as intended
  // and honor-target-abi is specified and also remove RealSP from SpilledRegs
  // list if it is in it
  if (FollowTargetABI && (RealSP != SP)) {
    llvm::erase(SpilledToStack, RealSP);
    SpilledToStack.push_back(SP);
  }

  return RealSP;
}

static std::vector<std::string> parseModelPluginList(const ModelOptions &Opts) {
  std::vector<std::string> CoSimModelPluginFilesList;
  if (Opts.ModelPluginFile == "None" && !Opts.CoSimModelPluginFilesList.empty())
    snippy::fatal(formatv("--cosim-model-plugins"
                          " can only be used when --model-plugin"
                          " is provided and is not None"));
  std::vector<std::string> Ret{Opts.ModelPluginFile};
  copy(Opts.CoSimModelPluginFilesList, std::back_inserter(Ret));
  erase(Ret, "None");

  return Ret;
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

static void normalizeProgramLevelOptions(Config &Cfg, LLVMState &State,
                                         RegPoolWrapper &RP,
                                         const ProgramOptions &Opts) {
  auto &ProgCfg = *Cfg.ProgramCfg;
  ProgCfg.ABIName = Opts.ABI;
  ProgCfg.FollowTargetABI = Opts.FollowTargetABI;
  ProgCfg.PreserveCallerSavedGroups = Opts.PreserveCallerSavedRegs;
  ProgCfg.MangleExportedNames = Opts.MangleExportedNames;
  ProgCfg.EntryPointName = Opts.EntryPointName;
  ProgCfg.ExternalStack = Opts.ExternalStack;
  ProgCfg.InitialRegYamlFile = Opts.InitialRegisterDataFile;
  ProgCfg.Seed = seedOptToValue(Opts.Seed);
  // FIXME: RandomEngine initialization should be moved out of Config as well
  // as most of the stuff below
  initializeRandomEngine(ProgCfg.Seed);
  if (!ProgCfg.hasSectionToSpillGlobalRegs() &&
      Cfg.PassCfg.hasExternalCallees())
    reserveGlobalStateRegisters(RP, State.getSnippyTarget());
  parseReservedRegistersOption(RP, State.getSnippyTarget(), State.getRegInfo(),
                               Opts);
  auto RegsSpilledToStack = parseSpilledRegistersOption(
      RP, State.getSnippyTarget(), State.getRegInfo(), State.getCtx(), Opts);
  auto RegsSpilledToMem = getRegsToSpillToMem(State.getSnippyTarget(), Cfg);
  if (ProgCfg.FollowTargetABI) {
    if (!RegsSpilledToStack.empty())
      snippy::warn(WarningName::InconsistentOptions, State.getCtx(),
                   "--spilled-regs-list is ignored",
                   "--honor-target-abi is enabled.");
    RegsSpilledToStack.clear();
    auto ABIPreserved =
        State.getSnippyTarget().getRegsPreservedByABI(State.getSubtargetInfo());
    // Global Regs will be spilled separately as we need to spill them to
    // Memory, not stack.
    llvm::copy_if(
        ABIPreserved, std::back_inserter(RegsSpilledToStack),
        [&](auto Reg) { return !llvm::is_contained(RegsSpilledToMem, Reg); });
  }

  ProgCfg.StackPointer =
      getRealStackPointer(RP, State.getSnippyTarget(), State.getRegInfo(),
                          RegsSpilledToStack, State.getCtx(), Cfg, Opts);
  llvm::copy(RegsSpilledToStack, std::back_inserter(ProgCfg.SpilledToStack));
  llvm::copy(RegsSpilledToMem, std::back_inserter(ProgCfg.SpilledToMem));
}

static void normalizeRegInitOptions(Config &Cfg, LLVMState &State,
                                    const RegInitOptions &Opts) {
  auto &RegsCfg = Cfg.PassCfg.RegistersConfig;
  RegsCfg.InitializeRegs = Opts.InitRegsInElf;
  if ((Opts.DumpInitialRegisters == "none" && Verbose) ||
      Opts.DumpInitialRegisters.empty()) {
    // if verbose, but no file was specified - use hardcoded default path
    RegsCfg.InitialStateOutputYaml = "initial_registers_state.yml";
  } else if (Opts.DumpInitialRegisters != "none") {
    RegsCfg.InitialStateOutputYaml = Opts.DumpInitialRegisters;
  }

  if ((Opts.DumpResultingRegisters == "none" &&
       (Verbose && Cfg.PassCfg.ModelPluginConfig.runOnModel())) ||
      Opts.DumpResultingRegisters.empty()) {
    // if verbose, but no file was specified - use hardcoded default path
    RegsCfg.FinalStateOutputYaml = "registers_state.yml";
  } else if (Opts.DumpResultingRegisters != "none") {
    RegsCfg.FinalStateOutputYaml = Opts.DumpResultingRegisters;
  }
  // TODO: move this check away.
  if (Opts.ValuegramOperandsRegsInitOutputsSpecified &&
      !Opts.ValueGramRegsDataFileSpecified)
    snippy::fatal("Incompatible options",
                  "-valuegram-operands-regs-init-outputs available only if "
                  "-valuegram-operands-regs specified");

  if (Opts.ValueGramRegsDataFileSpecified) {
    Cfg.DefFlowConfig.Valuegram.emplace();
    Cfg.DefFlowConfig.Valuegram->RegsHistograms =
        loadRegistersFromYaml(Opts.ValueGramRegsDataFile);
    Cfg.DefFlowConfig.Valuegram->ValuegramOperandsRegsInitOutputs =
        ValuegramOperandsRegsInitOutputs;
  }
}

static Error normalizeInstrGenOptions(Config &Cfg, LLVMState &State,
                                      const InstrGenOptions &Opts) {
  auto &PassCfg = Cfg.PassCfg;
  auto &InstrsCfg = PassCfg.InstrsGenerationConfig;
  auto NumPrimaryInstrs = getExpectedNumInstrs(Opts.NumInstrs);
  InstrsCfg.RunMachineInstrVerifier = Opts.VerifyMachineInstrs;
  InstrsCfg.ChainedRXSorted = Opts.ChainedRXSorted;
  InstrsCfg.ChainedRXSectionsFill = Opts.ChainedRXSectionsFill;
  if (Opts.ChainedRXChunkSize)
    InstrsCfg.ChainedRXChunkSize = Opts.ChainedRXChunkSize;

  if (Opts.ChainedRXChunkSize && !NumPrimaryInstrs)
    snippy::fatal(State.getCtx(),
                  "Cannot use '" + Twine(ChainedRXChunkSize.ArgStr) +
                      "' option",
                  "num-instr is set to 'all'");
  if (Opts.ChainedRXChunkSize && !InstrsCfg.ChainedRXSectionsFill)
    snippy::warn(WarningName::InconsistentOptions, State.getCtx(),
                 "'" + Twine(ChainedRXChunkSize.ArgStr) + "' is ignored",
                 "pass 'chained-rx-sections-fill' to enable it");
  InstrsCfg.NumInstrs = NumPrimaryInstrs;
  InstrsCfg.LastInstr = Opts.LastInstr;

  auto &TrackCfg = Cfg.CommonPolicyCfg->TrackCfg;
  TrackCfg.BTMode = Opts.Backtrack;
  TrackCfg.AddressVH = Opts.AddressVHOpt;

  // FIXME: we should create a special routine for tracking duplicates
  if (TrackCfg.Selfcheck && Opts.SelfcheckSpecified)
    return createStringError(inconvertibleErrorCode(),
                             "'selfcheck' has been specified both as an option "
                             "and as a configuration field");

  if (TrackCfg.Selfcheck)
    return Error::success();

  if (auto Period = getSelfcheckPeriod(Opts.Selfcheck)) {
    auto Mode = Opts.SelfcheckRefValueStorage;
    TrackCfg.Selfcheck = SelfcheckConfig{Mode, Period};
  }
  return Error::success();
}

static void normalizeModelOptions(Config &Cfg, LLVMState &State,
                                  const ModelOptions &Opts) {
  auto &ModelCfg = Cfg.PassCfg.ModelPluginConfig;
  ModelCfg.ModelLibraries = parseModelPluginList(Opts);
}

void yaml::MappingTraits<Config>::mapping(yaml::IO &IO, Config &Info) {
  IO.mapOptional("sections", Info.ProgramCfg->Sections);
  // Here we call yamlize directly since memory scheme has no top-level key.
  // This could be changed in the future but it'd be a breaking change.
  yaml::MappingTraits<MemoryScheme>::mapping(IO, Info.CommonPolicyCfg->MS);
  IO.mapOptional("branches", Info.PassCfg.Branches);
  IO.mapOptional("selfcheck", Info.CommonPolicyCfg->TrackCfg.Selfcheck);
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

  YAMLHistogramIO<OpcodeHistogramDecodedEntry> HistIO(Info.Histogram);
  IO.mapOptional("histogram", HistIO);

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

  Info.ProgramCfg->TargetConfig->mapConfig(IO);
  IO.mapOptional("fpu-config", Info.CommonPolicyCfg->FPUConfig);
}

std::string yaml::MappingTraits<Config>::validate(yaml::IO &Io, Config &Info) {
  void *Ctx = Io.getContext();
  assert(Ctx && "To parse or output Config provide ConfigIOContext as "
                "context for yaml::IO");
  auto &ConfigIOCtx = *static_cast<ConfigIOContext *>(Ctx);
  return Info.CommonPolicyCfg->MS
      .validateSchemes(ConfigIOCtx.State.getCtx(), Info.ProgramCfg->Sections)
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
  if (std::find_if(Histogram.begin(), Histogram.end(), InvalidOpcChecker) !=
      Histogram.end())
    snippy::fatal("Plugin filled histogram with invalid opcodes");

  auto InvalidWeightsChecker = [](auto It) { return It.second < 0; };
  if (std::find_if(Histogram.begin(), Histogram.end(), InvalidWeightsChecker) !=
      Histogram.end())
    snippy::fatal("Plugin filled histogram with negative opcodes weights");
}

ProgramConfig::ProgramConfig(const SnippyTarget &Tgt, StringRef PluginFilename)
    : TargetConfig(Tgt.createTargetConfig()),
      PluginManagerImpl(std::make_unique<PluginManager>()) {}
Config::Config(IncludePreprocessor &IPP, RegPoolWrapper &RP, LLVMState &State

               ,
               StringRef PluginFilename, StringRef PluginInfoFilename,
               const OpcodeCache &OpCC, bool ParseWithPlugin)
    : Includes([&IPP] {
        auto IncludesRange = IPP.getIncludes();
        return std::vector(IncludesRange.begin(), IncludesRange.end());
      }()),
      ProgramCfg(std::make_unique<ProgramConfig>(State.getSnippyTarget(),
                                                 PluginFilename)),
      CommonPolicyCfg(std::make_unique<CommonPolicyConfig>(*ProgramCfg)),
      DefFlowConfig(*CommonPolicyCfg), PassCfg(*ProgramCfg) {
  auto &PluginManager = *ProgramCfg->PluginManagerImpl;
  PluginManager.loadPluginLib(PluginFilename.str());
  auto &Ctx = State.getCtx();

  if (ParseWithPlugin) {
    PluginManager.parseOpcodes(OpCC, PluginInfoFilename.str(),
                               std::inserter(Histogram, Histogram.begin()));
    diagnoseHistogram(Ctx, OpCC, Histogram);
  }
  ConfigIOContext CfgParsingCtx{OpCC, RP, State};

  struct DiagnosticContext {
    IncludePreprocessor &IPP;
    Error ExtraError;
  };

  DiagnosticContext DiagCtx{IPP, Error::success()};

  auto Err = loadYAMLFromBuffer(
      *this, IPP.getPreprocessed(),
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

  auto ReportError = [&Ctx, &IPP](auto Err) {
    snippy::fatal(Ctx,
                  "Failed to parse file \"" + IPP.getPrimaryFilename() + "\"",
                  toString(std::move(Err)));
  };

  if (Err)
    ReportError(std::move(Err));

  if (DiagCtx.ExtraError)
    ReportError(std::move(DiagCtx.ExtraError));

  normalizeProgramLevelOptions(*this, State, RP, copyOptionsToProgramOptions());
  normalizeRegInitOptions(*this, State, copyOptionsToRegInitOptions());
  normalizeModelOptions(*this, State, copyOptionsToModelOptions());
  if ((Err = normalizeInstrGenOptions(*this, State,
                                      copyOptionsToInstrGenOptions())))
    snippy::fatal(std::move(Err));
  complete(State, OpCC);
  validateAll(State, OpCC, RP);
}

static void checkMemoryRegions(const SnippyTarget &SnippyTgt,
                               const Config &Cfg) {
  auto Sections = llvm::reverse(Cfg.ProgramCfg->Sections);
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

static void checkCallRequirements(const SnippyTarget &Tgt,
                                  const OpcodeHistogram &Histogram) {
  bool hasCalls = Histogram.getOpcodesWeight([&Tgt](unsigned Opcode) {
    return Tgt.isCall(Opcode);
  }) > 0.0;
  bool hasNonCalls = Histogram.getOpcodesWeight([&Tgt](unsigned Opcode) {
    return !Tgt.isCall(Opcode);
  }) > 0.0;
  if (hasCalls && !hasNonCalls)
    snippy::fatal(
        "for using calls you need to add to histogram non-call instructions");
}

static void checkBurstGram(LLVMContext &Ctx, const OpcodeHistogram &Histogram,
                           const OpcodeCache &OpCC,
                           const BurstGramData &Burst) {
  if (Burst.Mode != BurstMode::CustomBurst)
    return;
  assert(Burst.Groupings);
  for (auto &&Group : *Burst.Groupings) {
    for (auto Opc : Group) {
      if (!Histogram.count(Opc))
        warn(WarningName::BurstMode, Ctx,
             "Instruction \"" + OpCC.name(Opc) +
                 "\" was specified in burst grouping but not in histogram",
             "Instruction won't be generated");
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
                  "When -num-instr=all is specified, initializing "
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
  if (llvm::none_of(llvm::make_first_range(Histogram), [&](auto Opcode) {
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
      Cfg.ProgramCfg->hasSectionToSpillGlobalRegs())
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
                                                const Config &Cfg) {
  bool FillCodeSectionMode = !Cfg.PassCfg.InstrsGenerationConfig.NumInstrs;
  if (FillCodeSectionMode &&
      Cfg.Histogram.getOpcodesWeight([&II](unsigned Opcode) {
        auto &Desc = II.get(Opcode);
        return Desc.isBranch();
      }) > 0.0)
    snippy::fatal(
        "when -num-instr=all is specified, branches are not supported");
  if (FillCodeSectionMode &&
      Cfg.Histogram.getOpcodesWeight(
          [&Tgt](unsigned Opcode) { return Tgt.isCall(Opcode); }) > 0.0)
    snippy::fatal("when -num-instr=all is specified, calls are not supported");

  if (FillCodeSectionMode && Cfg.CommonPolicyCfg->TrackCfg.Selfcheck)
    snippy::fatal(
        "when -num-instr=all is specified, selfcheck is not supported");
  if (FillCodeSectionMode && Cfg.BurstConfig &&
      Cfg.BurstConfig->Burst.Mode != BurstMode::Basic)
    snippy::fatal(
        "when -num-instr=all is specified, burst mode is not supported");
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
  auto &Sections = Cfg.ProgramCfg->Sections;
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

void Config::validateAll(LLVMState &State, const OpcodeCache &OpCC,
                         const RegPoolWrapper &RP) {
  auto &Ctx = State.getCtx();
  auto &Tgt = State.getSnippyTarget();
  auto &TM = State.getTargetMachine();
  auto &CGLayout = PassCfg.CGLayout;
  if (PassCfg.ModelPluginConfig.runOnModel() &&
      (ProgramCfg->InitialRegYamlFile.empty() && !InitRegsInElf))
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
  const auto &Sections = ProgramCfg->Sections;
  if (Sections.empty())
    fatal(Ctx, "Incorrect list of sections", "list is empty");
  if (Sections.generalRWSections().empty())
    fatal(Ctx, "Incorrect list of sections",
          "there are no general purpose RW sections");
  // Folowing check is for situations like this:
  //
  //
  // sections:
  //    - no: 1
  //      VMA: 0x1000
  //      SIZE: 0x1000
  //      LMA: 0x1000
  //      ACCESS: rx
  //    - name: 1
  //      VMA: 0x2000
  //      SIZE: 0x1000
  //      LMA: 0x1000
  //      ACCESS: rw
  //
  // -------------
  //
  // Technically those are different IDs, because one is int(1) and another is
  // string("1"). But those IDs have identical getIDString() output which is
  // disallowed.
  for (auto &&Section : Sections) {
    auto FoundSameIDString = std::find_if(
        Sections.begin(), Sections.end(), [&Section](auto &&AnotherSec) {
          return Section.ID != AnotherSec.ID &&
                 Section.getIDString() == AnotherSec.getIDString();
        });
    if (FoundSameIDString != Sections.end()) {
      auto IDString = Section.getIDString();
      snippy::fatal(Ctx, "Incorrect list of sections",
                    "List contains both numbered section #" + Twine(IDString) +
                        " and named section with same name.");
    }
  }

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
        SS << "section " << SecIt->getIDString() << " and section "
           << std::next(SecIt)->getIDString() << " are interfering";
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
  checkCallRequirements(Tgt, Histogram);
  checkMemoryRegions(Tgt, *this);
  Tgt.checkInstrTargetDependency(Histogram);
  checkCompatibilityWithValuegramPolicy(*this, Ctx);

  auto *II = TM.getMCInstrInfo();
  assert(II);
  checkFPUSettings(*this, Ctx, Tgt, *II);
  checkGlobalRegsSpillSettings(State.getSnippyTarget(), State.getRegInfo(),
                               *this, Ctx);
  checkFullSizeGenerationRequirements(State.getInstrInfo(),
                                      State.getSnippyTarget(), *this);

  if (!PassCfg.ModelPluginConfig.runOnModel() &&
      !PassCfg.RegistersConfig.FinalStateOutputYaml.empty())
    snippy::fatal("Dump resulting registers can't be done",
                  "dump-registers-yaml option is passed but model-plugin "
                  "is not provided.");

  if (hasCallInstrs(OpCC, Tgt)) {
    const auto &RI = State.getRegInfo();
    auto RA = RI.getRARegister();
    if (RP.isReserved(RA))
      snippy::fatal(State.getCtx(),
                    "Cannot generate requested call instructions",
                    "return address register is explicitly reserved.");
  }

  if (auto &PreserveGroups = ProgramCfg->PreserveCallerSavedGroups;
      !PreserveGroups.empty()) {
    if (!ProgramCfg->stackEnabled())
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

  auto SP = ProgramCfg->StackPointer;
  if (llvm::any_of(ProgramCfg->SpilledToStack,
                   [SP](auto Reg) { return Reg == SP; }))
    snippy::fatal("Stack pointer cannot be spilled. Remove it from "
                  "spill register list.");
  if (!ProgramCfg->stackEnabled()) {
    if (!ProgramCfg->SpilledToStack.empty())
      snippy::fatal(Ctx, "Cannot spill requested registers",
                    "no stack space allocated.");

    auto &CGL = PassCfg.CGLayout;
    if (hasCallInstrs(OpCC, Tgt) &&
        std::visit([](auto &&Layout) { return Layout.getDepth(); }, CGL) > 1)
      snippy::fatal(
          State.getCtx(), "Cannot generate requested call instructions",
          "layout allows calls with depth>=1 but stack space is not provided.");
  }

  if (ProgramCfg->ExternalStack) {
    if (PassCfg.ModelPluginConfig.runOnModel())
      snippy::fatal(Ctx, "Cannot run snippet on model",
                    "external stack was enabled.");
    if (ProgramCfg->Sections.hasSection(
            SectionsDescriptions::StackSectionName)) {
      snippy::warn(WarningName::InconsistentOptions, Ctx,
                   "Section 'stack' will not be used",
                   "external stack was enabled.");
    }
  }
  if (ProgramCfg->Sections.hasSection(
          SectionsDescriptions::SelfcheckSectionName) &&
      CommonPolicyCfg->TrackCfg.Selfcheck)
    diagnoseSelfcheckSection(State, *this, getMinimumSelfcheckSize(*this));
}

void Config::complete(LLVMState &State, const OpcodeCache &OpCC) {
  // FIXME: section sorting must be done internally by ProgramConfig yaml
  // parser.
  std::sort(ProgramCfg->Sections.begin(), ProgramCfg->Sections.end(),
            [](auto &S1, auto &S2) { return S1.VMA < S2.VMA; });

  // Distribute information from unified histogram to different config parts.

  if (BurstConfig)
    BurstConfig->Burst.convertToCustomMode(Histogram, State.getInstrInfo());
  CommonPolicyCfg->setupImmHistMap(OpCC, Histogram);

  // Data flow histogram.

  auto UsedInBurst = [&](auto Opc) -> bool {
    if (!BurstConfig.has_value())
      return false;
    auto &BCfg = *BurstConfig;
    auto BurstOpcodes = BCfg.Burst.getAllBurstOpcodes();
    return BurstOpcodes.count(Opc);
  };
  auto &DFHistogram = DefFlowConfig.DataFlowHistogram;
  std::copy_if(Histogram.begin(), Histogram.end(),
               std::inserter(DFHistogram, DFHistogram.end()),
               [&](const auto &Hist) {
                 auto *Desc = OpCC.desc(Hist.first);
                 assert(Desc);
                 return Desc->isBranch() == false && !UsedInBurst(Hist.first);
               });

  // Control flow histogram:
  auto &CFHistogram = PassCfg.BranchOpcodes;
  std::copy_if(Histogram.begin(), Histogram.end(),
               std::inserter(CFHistogram, CFHistogram.end()),
               [&OpCC](const auto &Hist) {
                 auto *Desc = OpCC.desc(Hist.first);
                 assert(Desc);
                 return Desc->isBranch();
               });
  if (BurstConfig) {
    auto &BurstWeights = BurstConfig->BurstOpcodeWeights;
    auto &&BurstOpcodes = BurstConfig->Burst.getAllBurstOpcodes();
    auto AllPresentInHistogram = llvm::make_filter_range(
        BurstOpcodes, [&](auto &&Opcode) { return Histogram.count(Opcode); });
    llvm::transform(AllPresentInHistogram,
                    std::inserter(BurstWeights, BurstWeights.begin()),
                    [&](auto &&Opcode) {
                      auto Found = Histogram.find(Opcode);
                      assert(Found != Histogram.end());
                      return *Found;
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

void Config::dump(raw_ostream &OS, const ConfigIOContext &Ctx) const {
  outputYAMLToStream(const_cast<Config &>(*this), OS,
                     [&Ctx = const_cast<ConfigIOContext &>(Ctx)](auto &IO) {
                       IO.setContext(&Ctx);
                     });
}

} // namespace snippy
} // namespace llvm
