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
// FIXME: remove dependency on Generator library
#include "snippy/Generator/LLVMState.h"
#include "snippy/Generator/MemoryManager.h"
#include "snippy/Generator/RegisterPool.h"
#include "snippy/Generator/SelfcheckMode.h"
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
    snippy::SelfcheckRefValueStorageType,
    snippy::SelfcheckRefValueStorageEnumOption)

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

// Legacy options

static snippy::opt<std::string> LastInstr(
    "last-instr",
    cl::desc(
        "custom choice of the last instruction. Use 'RET' to emit return."),
    cl::cat(Options), cl::init("EBREAK"));

static snippy::opt<std::string>
    InitialRegisterDataFile("initial-regs",
                            cl::desc("file for initial registers state"),
                            cl::cat(Options), cl::init(""));
snippy::alias
    InitialRegisterYamlFile("initial-regs-yaml",
                            cl::desc("Alias for -initial-regs"),
                            snippy::aliasopt(InitialRegisterDataFile));

static snippy::opt<std::string> ValuegramRegsDataFile(
    "valuegram-operands-regs",
    cl::desc("Set values in operands registers before each instruction "
             "according to the file. It supported only if a number of "
             "instructions are generated."),
    cl::cat(Options), cl::init(""));

static snippy::opt<bool> ValuegramOperandsRegsInitOutputs(
    "valuegram-operands-regs-init-outputs",
    cl::desc("turning on initialization of destination registers before "
             "each non-service instruction. Available only if specified "
             "-valuegram-operands-regs."),
    cl::Hidden, cl::init(false));

static snippy::opt_list<std::string> ReservedRegisterList(
    "reserved-regs-list", cl::CommaSeparated,
    cl::desc("list of registers that shall not be used in snippet code"),
    cl::cat(Options));

static snippy::opt_list<std::string>
    SpilledRegisterList("spilled-regs-list", cl::CommaSeparated,
                        cl::desc("list of registers that shall be spilled "
                                 "before snippet execution and restored after"),
                        cl::cat(Options));

static snippy::opt<std::string>
    RedefineSP("redefine-sp",
               cl::desc("Specify the reg to use as a stack pointer"),
               cl::cat(Options), cl::init("any"));

static snippy::opt<bool> FollowTargetABI(
    "honor-target-abi",
    cl::desc("Automatically spill registers that are required to be preserved "
             "by snippet execution to follow ABI calling conventions."),
    cl::cat(Options), cl::init(false));

static snippy::opt<bool> ChainedRXSectionsFill(
    "chained-rx-sections-fill",
    cl::desc("Span the generated code across all provided RX sections. "
             "When disabled only one of all provided RX section is used to "
             "generate code to."),
    cl::cat(Options), cl::init(false));

static snippy::opt<size_t> ChainedRXChunkSize(
    "chained-rx-chunk-size",
    cl::desc("Slice main function in blocks of specified size, distribute over "
             "rx sections and randomly link them in list."),
    cl::cat(Options), cl::init(0u));

static snippy::opt<bool>
    ChainedRXSorted("chained-rx-sorted",
                    cl::desc("Sort RX sections by their ID alphabetically when "
                             "generating chain execution routine."),
                    cl::cat(Options), cl::init(false));

static snippy::opt<bool> ExternalStackOpt(
    "external-stack",
    cl::desc(
        "Snippy will assume that stack pointer is pre-initialized externally"),
    cl::cat(Options), cl::init(false));

static snippy::opt<bool> InitRegsInElf(
    "init-regs-in-elf",
    cl::desc("include registers initialization in final elf file"),
    cl::cat(Options));

static snippy::opt<bool> MangleExportedNames(
    "mangle-exported-names",
    cl::desc("Enable mangling of exported symbols and section names using "
             "snippet main function name."),
    cl::cat(Options), cl::init(false));

static snippy::opt<std::string> DumpInitialRegisters(
    "dump-initial-registers-yaml", cl::ValueOptional,
    cl::desc("Request dump of an initial register file state"),
    cl::value_desc("filename"), cl::init("none"), cl::cat(Options),
    cl::callback([](const std::string &Str) {
      if (Str.empty())
        DumpInitialRegisters = Str;
    }));

static snippy::opt<std::string> DumpResultingRegisters(
    "dump-registers-yaml", cl::ValueOptional,
    cl::desc("Request dump of the file register file state"),
    cl::value_desc("filename"), cl::init("none"), cl::cat(Options),
    cl::callback([](const std::string &Str) {
      if (Str.empty())
        DumpResultingRegisters = Str;
    }));

static snippy::opt<std::string>
    NumInstrs("num-instrs", cl::desc("number of generating instructions"),
              cl::cat(Options), cl::init("10"));

static snippy::opt<bool>
    Backtrack("backtrack",
              cl::desc("Enable backtracking facilities so as not to generate "
                       "erroneous code, e.g. division by zero"),
              cl::cat(Options), cl::init(false));

static snippy::opt<std::string>
    SelfCheck("selfcheck",
              cl::desc("Enable full selfcheck or partial selfcheck with "
                       "number N (means each N instructions)"),
              cl::cat(Options), cl::ValueOptional, cl::init("none"));

static snippy::opt<SelfcheckRefValueStorageType>
    SelfcheckRefValueStorage("selfcheck-ref-value-storage",
                             cl::desc("Option to define "
                                      "how to get reference selfcheck values"),
                             SelfcheckRefValueStorageEnumOption::getClValues(),
                             cl::init(SelfcheckRefValueStorageType::Code),
                             cl::cat(Options));

static snippy::opt<std::string>
    EntryPointName("entry-point", cl::desc("Override entry point name"),
                   cl::value_desc("label name"), cl::cat(Options),
                   cl::init("SnippyFunction"));

static snippy::opt<std::string> ModelPluginFile(
    "model-plugin",
    cl::desc("Primary model plugin to use for snippet generation"
             "Use 'None' to disable snippet execution on the model."),
    cl::value_desc("alias|filename"), cl::cat(Options), cl::init("None"));

static snippy::opt_list<std::string>
    CoSimModelPluginFilesList("cosim-model-plugins", cl::CommaSeparated,
                              cl::desc("Comma separated list of hardware model "
                                       "plugins to use for co-simulation"),
                              cl::cat(Options));

static snippy::opt<bool> VerifyMachineInstrs(
    "verify-mi",
    cl::desc("Enables verification of generated machine instructions."),
    cl::cat(Options), cl::init(false));

static snippy::opt<bool>
    AddressVHOpt("enable-address-value-hazards",
                 cl::desc("form address values based on dynamic register "
                          "values(backtracking needs to be enabled)"),
                 cl::cat(Options), cl::init(false));

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
static void reserveGlobalStateRegisters(RegPool &RP, const SnippyTarget &Tgt) {
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

static void parseReservedRegistersOption(RegPool &RP, const SnippyTarget &Tgt,
                                         const MCRegisterInfo &RI,
                                         yaml::IO &IO) {
  std::vector<std::string> ReservedRegisterList;
  IO.mapRequired("reserved-regs-list", ReservedRegisterList);
  for (auto &&RegName : ReservedRegisterList) {
    auto Reg = findRegisterByName(Tgt, RI, RegName);
    if (!Reg)
      snippy::fatal(formatv("Illegal register name {0}"
                            " is specified in --reserved-regs-list",
                            RegName));
    llvm::for_each(Tgt.getPhysRegsFromUnit(Reg.value(), RI),
                   [&RP](auto SimpleReg) {
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
parseSpilledRegistersOption(const RegPool &RP, const SnippyTarget &Tgt,
                            const MCRegisterInfo &RI, LLVMContext &Ctx,
                            yaml::IO &IO) {
  std::vector<std::string> SpilledRegisterList;
  IO.mapRequired("spilled-regs-list", SpilledRegisterList);
  std::vector<MCRegister> SpilledRegs;

  for (auto &&RegName : SpilledRegisterList) {
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

static MCRegister getRealStackPointer(const RegPool &RP,
                                      const SnippyTarget &Tgt,
                                      const MCRegisterInfo &RI,
                                      std::vector<MCRegister> &SpilledToStack,
                                      LLVMContext &Ctx, Config &Cfg,
                                      yaml::IO &IO) {
  auto SP = Tgt.getStackPointer();
  std::string RedefineSP;
  bool FollowTargetABI = Cfg.ProgramCfg->FollowTargetABI;
  IO.mapRequired("redefine-sp", RedefineSP);
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
        RP.getAvailableRegister("stack pointer", FullFilter, RI, SPRegClass);
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

static std::vector<std::string> parseModelPluginList(yaml::IO &IO) {
  std::string ModelPluginFile;
  std::vector<std::string> CoSimModelPluginFilesList;
  IO.mapRequired("model-plugin", ModelPluginFile);
  IO.mapRequired("cosim-model-plugins", CoSimModelPluginFilesList);
  if (ModelPluginFile == "None" && !CoSimModelPluginFilesList.empty())
    snippy::fatal(formatv("--cosim-model-plugins"
                          " can only be used when --model-plugin"
                          " is provided and is not None"));
  std::vector<std::string> Ret{ModelPluginFile};
  copy(CoSimModelPluginFilesList, std::back_inserter(Ret));
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

static unsigned getSelfcheckPeriod(StringRef SelfCheck) {
  if (SelfCheck == "none")
    return 0;

  if (SelfCheck.empty())
    return 1;

  unsigned long long SelfCheckPeriod = 0;
  if (getAsUnsignedInteger(SelfCheck, /* Radix */ 10, SelfCheckPeriod))
    snippy::fatal(
        "Value of selfcheck option is not convertible to numeric one.");
  assert(isUInt<sizeof(unsigned) * CHAR_BIT>(SelfCheckPeriod));
  return SelfCheckPeriod;
}
unsigned long long initializeRandomEngine(uint64_t SeedValue) {
  RandEngine::init(SeedValue);
  return SeedValue;
}

void yaml::MappingTraits<ConfigCLOptionsMapper>::mapping(
    yaml::IO &IO, ConfigCLOptionsMapper &Mapper) {
  void *Ctx = IO.getContext();
  assert(Ctx && "To parse or output Config provide ConfigIOContext as "
                "context for yaml::IO");
  auto &ConfigIOCtx = *static_cast<ConfigIOContext *>(Ctx);
  auto &State = ConfigIOCtx.State;
  auto &RP = ConfigIOCtx.RP;
  auto &ProgCfg = *Mapper.Cfg.ProgramCfg;
  IO.mapRequired("mabi", ProgCfg.ABIName);
  IO.mapRequired("honor-target-abi", ProgCfg.FollowTargetABI);
  IO.mapRequired("mangle-exported-names", ProgCfg.MangleExportedNames);
  IO.mapRequired("entry-point", ProgCfg.EntryPointName);
  IO.mapRequired("external-stack", ProgCfg.ExternalStack);
  IO.mapRequired("initial-regs", ProgCfg.InitialRegYamlFile);
  std::string Seed, MemorySeed, MemoryFile;
  SelfcheckRefValueStorageType SelfcheckRefValueStorage;

  IO.mapRequired("seed", Seed);
  if (!IO.outputting()) {
    ProgCfg.Seed = seedOptToValue(Seed);
    // FIXME: RandomEngine initialization should be moved out of Config as well
    // as most of the stuff below
    initializeRandomEngine(ProgCfg.Seed);
  }
  if (!ProgCfg.hasSectionToSpillGlobalRegs() &&
      Mapper.Cfg.PassCfg.hasExternalCallees())
    reserveGlobalStateRegisters(RP, State.getSnippyTarget());
  parseReservedRegistersOption(RP, State.getSnippyTarget(), State.getRegInfo(),
                               IO);

  auto RegsSpilledToStack = parseSpilledRegistersOption(
      RP, State.getSnippyTarget(), State.getRegInfo(), State.getCtx(), IO);
  auto RegsSpilledToMem =
      getRegsToSpillToMem(State.getSnippyTarget(), Mapper.Cfg);

  if (ProgCfg.FollowTargetABI) {
    if (!RegsSpilledToStack.empty())
      snippy::warn(WarningName::InconsistentOptions, State.getCtx(),
                   "--spilled-regs-list is ignored",
                   "--honor-target-abi is enabled.");
    RegsSpilledToStack.clear();
    auto ABIPreserved = State.getSnippyTarget().getRegsPreservedByABI();
    // Global Regs will be spilled separately as we need to spill them to
    // Memory, not stack.
    llvm::copy_if(
        ABIPreserved, std::back_inserter(RegsSpilledToStack),
        [&](auto Reg) { return !llvm::is_contained(RegsSpilledToMem, Reg); });
  }

  auto StackPointer =
      getRealStackPointer(RP, State.getSnippyTarget(), State.getRegInfo(),
                          RegsSpilledToStack, State.getCtx(), Mapper.Cfg, IO);
  ProgCfg.StackPointer = StackPointer;
  llvm::copy(RegsSpilledToStack, std::back_inserter(ProgCfg.SpilledToStack));
  llvm::copy(RegsSpilledToMem, std::back_inserter(ProgCfg.SpilledToMem));

  auto &PassCfg = Mapper.Cfg.PassCfg;

  auto &ModelCfg = PassCfg.ModelPluginConfig;
  auto Models = parseModelPluginList(IO);
  ModelCfg.ModelLibraries = std::move(Models);
  auto &InstrsCfg = PassCfg.InstrsGenerationConfig;
  std::string NumInstrs;
  IO.mapRequired("num-instrs", NumInstrs);
  auto NumPrimaryInstrs = getExpectedNumInstrs(NumInstrs);
  IO.mapRequired("verify-mi", InstrsCfg.RunMachineInstrVerifier);
  IO.mapRequired("chained-rx-sorted", InstrsCfg.ChainedRXSorted);
  IO.mapRequired("chained-rx-sections-fill", InstrsCfg.ChainedRXSectionsFill);
  IO.mapRequired("chained-rx-sections-fill", InstrsCfg.ChainedRXSectionsFill);
  size_t ChunkSize;
  static const char *ChunkOptName = "chained-rx-chunk-size";
  IO.mapRequired(ChunkOptName, ChunkSize);
  if (ChunkSize)
    InstrsCfg.ChainedRXChunkSize = ChunkSize;

  if (ChunkSize && !NumPrimaryInstrs)
    snippy::fatal(State.getCtx(),
                  "Cannot use '" + Twine(ChunkOptName) + "' option",
                  "num-instr is set to 'all'");
  if (ChunkSize && !InstrsCfg.ChainedRXSectionsFill)
    snippy::warn(WarningName::InconsistentOptions, State.getCtx(),
                 "'" + Twine(ChunkOptName) + "' is ignored",
                 "pass 'chained-rx-sections-fill' to enable it");
  InstrsCfg.NumInstrs = NumPrimaryInstrs;
  IO.mapRequired("last-instr", InstrsCfg.LastInstr);

  bool Verbose = false;
  IO.mapRequired("verbose", Verbose);

  auto &RegsCfg = PassCfg.RegistersConfig;
  IO.mapRequired("init-regs-in-elf", RegsCfg.InitializeRegs);
  std::string DumpRegsInit, DumpRegsFinal;
  IO.mapRequired("dump-initial-registers-yaml", DumpRegsInit);
  IO.mapRequired("dump-registers-yaml", DumpRegsFinal);
  if ((DumpRegsInit == "none" && Verbose) || DumpRegsInit.empty()) {
    // if verbose, but no file was specified - use hardcoded default path
    RegsCfg.InitialStateOutputYaml = "initial_registers_state.yml";
  } else if (DumpRegsInit != "none") {
    RegsCfg.InitialStateOutputYaml = DumpRegsInit;
  }

  if ((DumpRegsFinal == "none" && (Verbose && ModelCfg.runOnModel())) ||
      DumpRegsFinal.empty()) {
    // if verbose, but no file was specified - use hardcoded default path
    RegsCfg.FinalStateOutputYaml = "registers_state.yml";
  } else if (DumpRegsFinal != "none") {
    RegsCfg.FinalStateOutputYaml = DumpRegsFinal;
  }

  auto &TrackCfg = Mapper.Cfg.CommonPolicyCfg->TrackCfg;
  IO.mapRequired("backtrack", TrackCfg.BTMode);
  IO.mapRequired("enable-address-value-hazards", TrackCfg.AddressVH);
  std::string SelfCheck;
  IO.mapRequired("selfcheck", SelfCheck);
  TrackCfg.SelfCheckPeriod = getSelfcheckPeriod(SelfCheck);
  std::string ValuegramRegsDataFile;
  bool ValuegramOperandsRegsInitOutputs;
  IO.mapRequired("valuegram-operands-regs", ValuegramRegsDataFile);
  IO.mapRequired("valuegram-operands-regs-init-outputs",
                 ValuegramOperandsRegsInitOutputs);
  // TODO: move this check away.
  if (ValuegramOperandsRegsInitOutputs && ValuegramRegsDataFile.empty())
    snippy::fatal("Incompatible options",
                  "-valuegram-operands-regs-init-outputs available only if "
                  "-valuegram-operands-regs specified");

  if (!ValuegramRegsDataFile.empty()) {
    Mapper.Cfg.DefFlowConfig.Valuegram.emplace();
    Mapper.Cfg.DefFlowConfig.Valuegram->RegsHistograms =
        loadRegistersFromYaml(ValuegramRegsDataFile);
    Mapper.Cfg.DefFlowConfig.Valuegram->ValuegramOperandsRegsInitOutputs =
        ValuegramOperandsRegsInitOutputs;
  }
}

void yaml::MappingTraits<Config>::mapping(yaml::IO &IO, Config &Info) {
  IO.mapOptional("sections", Info.ProgramCfg->Sections);
  // Here we call yamlize directly since memory scheme has no top-level key.
  // This could be changed in the future but it'd be a breaking change.
  yaml::MappingTraits<MemoryScheme>::mapping(IO, Info.CommonPolicyCfg->MS);
  IO.mapOptional("branches", Info.PassCfg.Branches);
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
Config::Config(IncludePreprocessor &IPP, RegPool &RP, LLVMState &State

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
          auto &IPP = *static_cast<IncludePreprocessor *>(Ctx);
          SMDiagnostic NewDiag(
              *Diag.getSourceMgr(), Diag.getLoc(),
              IPP.getCorrespondingLineID(Diag.getLineNo()).FileName,
              IPP.getCorrespondingLineID(Diag.getLineNo()).N,
              Diag.getColumnNo(), Diag.getKind(), Diag.getMessage(),
              Diag.getLineContents(), Diag.getRanges());
          NewDiag.print(nullptr, errs());
        }
      },
      IPP);
  if (Err)
    snippy::fatal(Ctx,
                  "Failed to parse file \"" + IPP.getPrimaryFilename() + "\"",
                  toString(std::move(Err)));
  // Append command line options.
  // Due to legacy some config parameters were fetched externally from options.
  // ConfigCLOptionsMapper is made to replace that.
  std::string CLOptYaml;
  raw_string_ostream OS{CLOptYaml};
  outputYAMLToStream(OptionsStorage::instance(), OS);
  ConfigCLOptionsMapper OptMapper{*this};
  Err = loadYAMLFromBuffer(
      OptMapper, CLOptYaml,
      [&CfgParsingCtx](auto &Yin) {
        Yin.setAllowUnknownKeys(true);
        Yin.setContext(&CfgParsingCtx);
      },
      [](const auto &Diag, void *Ctx) {
        // No diagnostics.
      },
      IPP);
  if (Err)
    snippy::fatal(toString(std::move(Err)));

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
  unsigned SelfCheckPeriod = Cfg.CommonPolicyCfg->TrackCfg.SelfCheckPeriod;
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

  if (FillCodeSectionMode && SelfCheckPeriod)
    snippy::fatal(
        "when -num-instr=all is specified, selfcheck is not supported");
  if (FillCodeSectionMode && Cfg.BurstConfig &&
      Cfg.BurstConfig->Burst.Mode != BurstMode::Basic)
    snippy::fatal(
        "when -num-instr=all is specified, burst mode is not supported");
}

static size_t getMinimumSelfcheckSize(const Config &Cfg) {
  auto &TrackCfg = Cfg.CommonPolicyCfg->TrackCfg;
  assert(TrackCfg.SelfCheckPeriod);

  size_t BlockSize = 2 * ProgramConfig::getSCStride();
  // Note: There are cases when we have some problems for accurate calculating
  // of selcheck section size.
  //       Consequently it can potentially cause overflow of selfcheck
  //       section, So it's better to provide selfcheck section in Layout
  //       explicitly
  return alignTo(Cfg.PassCfg.InstrsGenerationConfig.NumInstrs.value_or(0) *
                     BlockSize / TrackCfg.SelfCheckPeriod,
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
                         const RegPool &RP) {
  auto &Ctx = State.getCtx();
  auto &Tgt = State.getSnippyTarget();
  auto &TM = State.getTargetMachine();
  auto &CGLayout = PassCfg.CGLayout;
  if (PassCfg.ModelPluginConfig.runOnModel() && !InitRegsInElf)
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
      CommonPolicyCfg->TrackCfg.SelfCheckPeriod)
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
      CommonPolicyCfg->TrackCfg.SelfCheckPeriod)
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
