//===-- llvm-snippy.cpp -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Model checking asm snippet generation
///
//===----------------------------------------------------------------------===//

#include "lib/FlowGenerator.h"

#include "snippy/Config/BurstGram.h"
#include "snippy/Config/Config.h"
#include "snippy/Config/ImmediateHistogram.h"
#include "snippy/Config/MemoryScheme.h"
#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/Config/SerDesUtils.h"
#include "snippy/Generator/GeneratorSettings.h"
#include "snippy/Generator/LLVMState.h"
#include "snippy/Generator/RegisterPool.h"
#include "snippy/Support/DiagnosticInfo.h"
#include "snippy/Support/OpcodeCache.h"
#include "snippy/Support/Options.h"
#include "snippy/Support/Utils.h"
#include "snippy/Support/YAMLUtils.h"
#include "snippy/Target/Target.h"
#include "snippy/Target/TargetSelect.h"

#include "llvm/ADT/Twine.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/VCSRevision.h"
#include "llvm/TargetParser/Host.h"

#include <algorithm>
#include <sstream>
#include <string>

namespace llvm {
namespace snippy {

cl::OptionCategory Options("llvm-snippy options");

// Unlike clang, march here is not something like rv64gc (provided by attrs and
// CPU). Instead it is something like target (try riscv64-linux-gnu).
static snippy::opt<std::string>
    MArch("march", cl::desc("target architecture"),
          cl::value_desc("one of suppported Archs: RISCV, etc.."),
          cl::cat(Options), cl::init(""));

static snippy::opt<std::string>
    CpuName("mcpu", cl::desc("cpu name to use, leave empty to autodetect"),
            cl::cat(Options), cl::init(""));

static snippy::opt<std::string> MAttr(
    "mattr", cl::desc("comma-separated list of target architecture features"),
    cl::value_desc("+feature1,-feature2,..."), cl::cat(Options), cl::init(""));

static snippy::opt<std::string> ABI("mabi",
                                    cl::desc("custom abi for output elf-files"),
                                    cl::cat(Options), cl::init(""));

static snippy::opt<std::string> LastInstr(
    "last-instr",
    cl::desc(
        "custom choice of the last instruction. Use 'RET' to emit return."),
    cl::cat(Options), cl::init("EBREAK"));
// YAML file with memory layout and histogram
static cl::opt<std::string> LayoutFile(cl::Positional, cl::desc("<layout>"),
                                       cl::cat(Options), cl::init(""));

static cl::list<std::string> AdditionalLayoutFiles(cl::Positional,
                                                   cl::desc("<sub-layouts>..."),
                                                   cl::cat(Options));

static snippy::opt_list<std::string> LayoutIncludeDirectories(
    "layout-include-dir", cl::CommaSeparated,
    cl::desc("extra directory where to look for include files"),
    cl::cat(Options));

static snippy::opt<std::string>
    InitialRegisterDataFile("initial-regs",
                            cl::desc("file for initial registers state"),
                            cl::cat(Options), cl::init(""));
cl::alias InitialRegisterYamlFile("initial-regs-yaml",
                                  cl::desc("Alias for -initial-regs"),
                                  cl::aliasopt(InitialRegisterDataFile));

static snippy::opt_list<std::string> ReservedRegisterList(
    "reserved-regs-list", cl::CommaSeparated,
    cl::desc("list of registers that shall not be used in snippet code"),
    cl::cat(Options));

static snippy::opt_list<std::string>
    SpilledRegisterList("spilled-regs-list", cl::CommaSeparated,
                        cl::desc("list of registers that shall be spilled "
                                 "before snippet execution and restored after"),
                        cl::cat(Options));

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

static snippy::opt<bool>
    ListOpcodeNames("list-opcode-names",
                    cl::desc("list available opcode names"), cl::cat(Options));

static bool RequestForInitialStateDump = false;
static snippy::opt<std::string> DumpInitialRegisters(
    "dump-initial-registers-yaml", cl::ValueOptional,
    cl::desc("Request dump of an initial register file state"),
    cl::value_desc("filename"), cl::init("initial_registers_state.yml"),
    cl::cat(Options), cl::callback([](const std::string &) {
      RequestForInitialStateDump = true;
    }));

static bool RequestForFinalStateDump = false;
static snippy::opt<std::string> DumpResultingRegisters(
    "dump-registers-yaml", cl::ValueOptional,
    cl::desc("Request dump of the file register file state"),
    cl::value_desc("filename"), cl::init("registers_state.yml"),
    cl::cat(Options),
    cl::callback([](const std::string &) { RequestForFinalStateDump = true; }));

static snippy::opt<std::string>
    NumInstrs("num-instrs", cl::desc("number of generating instructions"),
              cl::cat(Options), cl::init("10"));

std::optional<unsigned> getExpectedNumInstrs(StringRef NumAsString) {
  if (NumAsString == "all")
    return {};
  int Value;
  if (!to_integer(NumAsString, Value, /*base*/ 10))
    report_fatal_error("num-instrs get not a number or all", false);
  if (Value < 0)
    report_fatal_error("num-instrs get negative number", false);
  return Value;
}

static snippy::opt<bool> DumpCFG("dump-cfg",
                                 cl::desc("[Debug] dump generated CFG"),
                                 cl::cat(Options));

static snippy::opt<bool> DumpMF(
    "dump-mf",
    cl::desc("[Debug] dump final generated machine function in Machine IR"),
    cl::cat(Options));

static snippy::opt<bool>
    Backtrack("backtrack",
              cl::desc("Enable backtracking facilities so as not to generate "
                       "erroneous code, e.g. division by zero"),
              cl::cat(Options), cl::init(false));

static snippy::opt<std::string>
    SelfCheck("selfcheck",
              cl::desc("Enable full selfcheck or partial selfcheck with "
                       "number N (means each N instructions)"),
              cl::cat(Options), cl::ValueOptional, cl::init(""));

static snippy::opt<bool> StrictMemoryScheme(
    "strict-memory-schemes",
    cl::desc("Raise error instead of warning when potentially invalid memory "
             "scheme is encountered."),
    cl::cat(Options), cl::init(false));

static snippy::opt<bool> DumpMI(
    "dump-mi",
    cl::desc("[Debug] dump generated machine instructions in Machine IR"),
    cl::cat(Options));

} // namespace snippy

namespace snippy {

static snippy::opt<std::string>
    Seed("seed",
         cl::desc("seed for instruction generation. If the option is not used "
                  "or its value is not set, seed will be generated randomly."),
         cl::cat(Options));

static snippy::opt<std::string>
    OutputFileBasename("o", cl::desc("Override output file base name"),
                       cl::value_desc("filename"), cl::cat(Options));

static snippy::opt<std::string>
    EntryPointName("entry-point", cl::desc("Override entry point name"),
                   cl::value_desc("label name"), cl::cat(Options),
                   cl::init("SnippyFunction"));

static snippy::opt<std::string> ModelPluginFile(
    "model-plugin",
    cl::desc("Primary model plugin to use for snippet generation"
             "Use =None to disable snippet execution on the model."),
    cl::value_desc("filename"), cl::cat(Options), cl::init("libRISCVModel.so"));

static snippy::opt_list<std::string>
    CoSimModelPluginFilesList("cosim-model-plugins", cl::CommaSeparated,
                              cl::desc("Comma separated list of hardware model "
                                       "plugins to use for co-simulation"),
                              cl::cat(Options));

static snippy::opt<std::string>
    GeneratorPluginFile("generator-plugin",
                        cl::desc("Plugin for custom instruction generation."
                                 "Use =None to generate instructions "
                                 "with build-in histogram."
                                 "(=None - default value)"),
                        cl::value_desc("filename"), cl::cat(Options),
                        cl::init("None"));

static snippy::opt<std::string> GeneratorPluginParserFile(
    "plugin-info-file",
    cl::desc("File with info to parse with plugin generator. "
             "Use =None to parse histogram with build-in "
             "Snippy pareser."),
    cl::value_desc("filename"), cl::cat(Options), cl::init("None"));

static snippy::opt<std::string>
    MemAddrGeneratorFile("address-generator-plugin",
                         cl::desc("Plugin for custom addreses generation."
                                  "Use =None to generate addresses "
                                  "with build-in randomizer."
                                  "(=None - default value)"),
                         cl::value_desc("filename"), cl::cat(Options),
                         cl::init("None"));

static snippy::opt<std::string> MemAddrGeneratorInfoFile(
    "address-plugin-info-file",
    cl::desc("File with info for addresses generator. "
             "Use =None if plugin doesn't need additional info."
             "(=None - default value)"),
    cl::value_desc("filename"), cl::cat(Options), cl::init("None"));

static snippy::opt<BurstMode> BurstModeOpt(
    "memory-access-mode",
    cl::desc("Controls mode of load/store instructions generation:"),
    cl::values(clEnumValN(BurstMode::Basic, "basic",
                          "Memory access instructions will not be grouped"),
               clEnumValN(BurstMode::StoreBurst, "store-burst",
                          "Group only store instructions"),
               clEnumValN(BurstMode::LoadBurst, "load-burst",
                          "Group only load instructions"),
               clEnumValN(BurstMode::MixedBurst, "mixed-burst",
                          "Group memory access instructions"),
               clEnumValN(BurstMode::LoadStoreBurst, "load-store-burst",
                          "Group memory access instructions, but do not mix "
                          "loads and stores")),
    cl::init(BurstMode::Basic), cl::cat(Options));

static snippy::opt<unsigned> BurstGroupThresholdOpt(
    "memory-access-burst-group-size",
    cl::desc("The size of a burst group of memory access instructions."),
    cl::cat(Options), cl::init(1));

static snippy::opt<bool> VerifyMachineInstrs(
    "verify-mi",
    cl::desc("Enables verification of generated machine instructions."),
    cl::cat(Options), cl::init(false));

static snippy::opt<bool>
    DumpLayout("dump-layout",
               cl::desc("Dump the whole snippy configuration YAML"),
               cl::init(false), cl::cat(Options));

static snippy::opt<bool> Verbose("verbose", cl::desc("Show verbose output."),
                                 cl::init(false), cl::cat(Options),
                                 cl::callback([](const bool &V) {
                                   if (V) {
                                     DumpLayout = true;
                                     DumpMF = true;
                                     DumpMI = true;
                                   }
                                 }));

static snippy::opt<bool>
    AddressVHOpt("enable-address-value-hazards",
                 cl::desc("form address values based on dynamic register "
                          "values(backtracking needs to be enabled)"),
                 cl::cat(Options), cl::init(false));

static snippy::opt<bool> DumpOptions("dump-options",
                                     cl::desc("Dump snippy options' values"),
                                     cl::cat(Options), cl::init(false));

static snippy::opt<bool> DumpPreprocessedConfig(
    "E", cl::desc("Dump snippy config after preprocessing it"), cl::init(false),
    cl::cat(Options));

static ExitOnError ExitOnErr("llvm-snippy error: ");

// Used to construct search paths for dynamically-loaded plugins
static std::string ARGV0;

static std::string getOutputFileBasename() {
  SmallVector<char> OutputFile;
  if (OutputFileBasename.getValue().empty()) {
    auto &LF = LayoutFile.getValue();
    OutputFile.assign(LF.begin(), LF.end());
  } else {
    auto &OFB = OutputFileBasename.getValue();
    OutputFile.assign(OFB.begin(), OFB.end());
  }
  return std::string{OutputFile.begin(), OutputFile.end()};
}

static void checkMemoryRegions(const SnippyTarget &SnippyTgt,
                               const Config &Cfg) {
  SectionDesc const *Reserved = nullptr;
  auto &Sections = Cfg.Sections;
  Reserved = std::accumulate(Sections.begin(), Sections.end(), Reserved,
                             [&SnippyTgt](auto *Res, auto &Section) {
                               auto *Touches =
                                   SnippyTgt.touchesReservedRegion(Section);
                               if (Touches)
                                 return Touches;
                               else
                                 return Res;
                             });
  if (Reserved) {
    std::string ErrBuf;
    llvm::raw_string_ostream SS{ErrBuf};
    SS << "One of layout memory regions interferes with reserved region:\n";
    Reserved->dump(SS);
    report_fatal_error(ErrBuf.c_str(), false);
  }
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
    report_fatal_error(
        "for using calls you need to add to histogram non-call instructions",
        false);
}

static void checkFullSizeGenerationRequirements(const MCInstrInfo &II,
                                                const SnippyTarget &Tgt,
                                                const Config &Cfg,
                                                bool FillCodeSectionMode,
                                                unsigned SelfCheckPeriod) {
  if (FillCodeSectionMode &&
      Cfg.Histogram.getOpcodesWeight([&II](unsigned Opcode) {
        auto &Desc = II.get(Opcode);
        return Desc.isBranch();
      }) > 0.0)
    report_fatal_error(
        "when -num-instr=all is specified, branches are not supported", false);
  if (FillCodeSectionMode &&
      Cfg.Histogram.getOpcodesWeight(
          [&Tgt](unsigned Opcode) { return Tgt.isCall(Opcode); }) > 0.0)
    report_fatal_error(
        "when -num-instr=all is specified, calls are not supported", false);

  if (FillCodeSectionMode && SelfCheckPeriod)
    report_fatal_error(
        "when -num-instr=all is specified, selfcheck is not supported", false);
  if (FillCodeSectionMode && Cfg.Burst.Data->Mode != BurstMode::Basic)
    report_fatal_error(
        "when -num-instr=all is specified, burst mode is not supported", false);
}

static void setBurstGramIfNeeded(BurstGram &BGram,
                                 const ConfigIOContext &ParsingCtx) {
  if (BurstModeOpt.isSpecified() || BurstGroupThresholdOpt.isSpecified()) {
    if (BGram)
      fatal(ParsingCtx.Ctx,
            "Attempt to specify burst config through the command line options",
            "burst configuration was already found in config file");
    BGram.Data = BurstGramData();
    if (BurstModeOpt.isSpecified())
      BGram.Data->Mode = BurstModeOpt;
    if (BurstGroupThresholdOpt.isSpecified())
      BGram.Data->MinSize = BGram.Data->MaxSize = BurstGroupThresholdOpt;
  }
  if (!BGram)
    BGram.Data = BurstGramData();
}

static auto getExtraIncludeDirsForLayout() {
  std::vector<std::string> Result;
  std::copy(LayoutIncludeDirectories.begin(), LayoutIncludeDirectories.end(),
            std::back_inserter(Result));
  return Result;
}

static bool generateWithPlugin() { return GeneratorPluginFile != "None"; }
static bool isParsingWithPluginEnabled() {
  return GeneratorPluginParserFile != "None";
}

static unsigned long long
seedOptToValue(StringRef SeedStr, StringRef SeedType = "instructions seed",
               StringRef Warning = "no instructions seed specified,"
                                   " using auto-generated one: ") {
  if (SeedStr.empty()) {
    auto SeedValue =
        std::chrono::system_clock::now().time_since_epoch().count();
    llvm::errs() << "warning: " << Warning << SeedValue << "\n";
    return SeedValue;
  }

  unsigned long long SeedValue;
  if (getAsUnsignedInteger(SeedStr, /* Radix */ 10, SeedValue))
    report_fatal_error("Provided " + SeedType +
                           " is not convertible to numeric value.",
                       false);
  return SeedValue;
}

static unsigned getSelfcheckPeriod() {
  if (!SelfCheck.isSpecified())
    return 0;

  if (SelfCheck.getValue().empty())
    return 1;

  unsigned long long SelfCheckPeriod = 0;
  if (getAsUnsignedInteger(SelfCheck.getValue(), /* Radix */ 10,
                           SelfCheckPeriod))
    report_fatal_error(
        "Value of selfcheck option is not convertible to numeric one.", false);
  assert(isUInt<sizeof(unsigned) * CHAR_BIT>(SelfCheckPeriod));
  return SelfCheckPeriod;
}

static std::string
deriveDefaultableOptionValue(bool ExtractValue,
                             const snippy::opt<std::string> &Opt) {
  if (!ExtractValue)
    return {};
  auto OptValue = Opt.getValue();
  if (OptValue.empty())
    OptValue = Opt.getDefault().getValue();
  return OptValue;
}

std::optional<unsigned> findRegisterByName(const SnippyTarget &SnippyTgt,
                                           const MCRegisterInfo &RI,
                                           StringRef Name) {
  for (auto &RC : RI.regclasses()) {
    auto RegIdx = std::find_if(RC.begin(), RC.end(), [&Name, &RI](auto &Reg) {
      return Name.equals(RI.getName(Reg));
    });
    if (RegIdx != RC.end())
      return *RegIdx;
  }
  return SnippyTgt.findRegisterByName(Name);
}
std::vector<std::string> parseModelPluginList() {
  if ((!ModelPluginFile.isSpecified() ||
       ModelPluginFile.getValue() == "None") &&
      CoSimModelPluginFilesList.size())
    report_fatal_error("--" + CoSimModelPluginFilesList.ArgStr +
                       " can only be used when --" + ModelPluginFile.ArgStr +
                       " is provided and is not None");

  std::vector<std::string> Ret{ModelPluginFile.getValue()};
  copy(CoSimModelPluginFilesList, std::back_inserter(Ret));
  erase_value(Ret, "None");

  return Ret;
}

static void parseReservedRegistersOption(RegPool &RP, const SnippyTarget &Tgt,
                                         const MCRegisterInfo &RI) {
  for (auto &&RegName : ReservedRegisterList) {
    auto Reg = findRegisterByName(Tgt, RI, RegName);
    if (!Reg)
      report_fatal_error("Illegal register name " + Twine(RegName) +
                             " is specified in --" +
                             ReservedRegisterList.ArgStr,
                         false);
    RP.addReserved(Reg.value(), AccessMaskBit::GRW);
    DEBUG_WITH_TYPE("snippy-regpool",
                    (dbgs() << "Reserved with option:\n", RP.print(dbgs())));
  }
}

static std::vector<unsigned>
parseSpilledRegistersOption(RegPool &RP, const SnippyTarget &Tgt,
                            const MCRegisterInfo &RI, LLVMContext &Ctx) {
  std::vector<unsigned> SpilledRegs;

  for (auto &&RegName : SpilledRegisterList) {
    auto Reg = findRegisterByName(Tgt, RI, RegName);
    if (!Reg)
      report_fatal_error("Illegal register name " + Twine(RegName) +
                             " is specified in --" + SpilledRegisterList.ArgStr,
                         false);

    if (RP.isReserved(Reg.value()))
      report_fatal_error(
          "Register \"" + Twine(RegName) +
              "\" cannot be spilled, because it is explicitly reserved.\n",
          false);
    SpilledRegs.push_back(Reg.value());
  }

  if (FollowTargetABI) {
    if (!SpilledRegs.empty())
      snippy::warn(WarningName::InconsistentOptions, Ctx,
                   "--" + Twine(SpilledRegisterList.ArgStr) + " is ignored",
                   "--" + Twine(FollowTargetABI.ArgStr) + " is enabled.");
    SpilledRegs.clear();
    auto ABIPreserved = Tgt.getRegsPreservedByABI();

    std::transform(ABIPreserved.begin(), ABIPreserved.end(),
                   std::back_inserter(SpilledRegs),
                   [](auto Reg) { return Reg; });
  }
  return SpilledRegs;
}

static void reportWarningsSummary() {
  const auto &Warnings = snippy::SnippyDiagnosticInfo::fetchReportedWarnings();
  if (Warnings.empty())
    return;
  errs() << "_______\n";
  errs() << "  Test Generation resulted in the following warnings:\n";
  for (const auto &[Message, Count] : Warnings)
    errs() << "    * " << Message << " (x" << Count << ")\n";
}

static void readSnippyOptionsIfNeeded() {
  if (LayoutFile.getNumOccurrences()) {
    OptionsMappingWrapper OMWP;
    auto Err = loadYAMLIgnoreUnknownKeys(OMWP, LayoutFile.getValue());
    if (Err)
      report_fatal_error(toString(std::move(Err)).c_str(), false);
  }
}

static std::string readFile(StringRef Filename, LLVMContext &Ctx) {
  auto MemBufOrErr = MemoryBuffer::getFile(Filename);
  if (auto EC = MemBufOrErr.getError(); !MemBufOrErr)
    fatal(Ctx, "Failed to open file \"" + Filename + "\"", EC.message());
  return (*MemBufOrErr)->getBuffer().str();
}

static void mergeFiles(IncludePreprocessor &IPP, LLVMContext &Ctx) {
  for (const auto &AdditionalLayout : AdditionalLayoutFiles) {
    IPP.mergeFile(AdditionalLayout, readFile(AdditionalLayout, Ctx));
  }
}

static Config readSnippyConfig(LLVMContext &Ctx, const SnippyTarget &Tgt,
                               const OpcodeCache &OpCC) {
  ConfigIOContext CfgParsingCtx{OpCC, Ctx, Tgt};
  auto ParseWithPlugin = isParsingWithPluginEnabled();
  IncludePreprocessor IPP(LayoutFile.getValue(), getExtraIncludeDirsForLayout(),
                          Ctx);
  auto IncludeFiles = [&IPP] {
    auto IncludesRange = IPP.getIncludes();
    return std::vector(IncludesRange.begin(), IncludesRange.end());
  }();
  Config Cfg(Tgt, GeneratorPluginFile.getValue(),
             GeneratorPluginParserFile.getValue(), OpCC, ParseWithPlugin, Ctx,
             IncludeFiles);
  mergeFiles(IPP, Ctx);
  if (DumpPreprocessedConfig)
    outs() << IPP.getPreprocessed() << "\n";
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
    fatal(Ctx, "Failed to parse file \"" + LayoutFile.getValue() + "\"",
          toString(std::move(Err)));
  return Cfg;
}

static void dumpConfigIfNeeded(const Config &Cfg,
                               const ConfigIOContext &CfgParsingCtx,
                               raw_ostream &OS) {
  if (DumpLayout) {
    Cfg.dump(OS, CfgParsingCtx);
    // FIXME: Currently sections can't be serialized through YAML
    OS << "Sections:\n";
    for (auto &&S : Cfg.Sections) {
      S.dump(OS);
      OS << "\n";
    }
    Cfg.MS.dump();
  }
}

static void convertToCustomBurstMode(const OpcodeHistogram &Histogram,
                                     const MCInstrInfo &II,
                                     BurstGramData &Burst) {
  if (Burst.Mode == BurstMode::CustomBurst || Burst.Mode == BurstMode::Basic)
    return;
  assert(!Burst.Groupings &&
         "Groupings are specified but burst mode is not \"custom\"");
  Burst.Groupings = BurstGramData::GroupingsTy();
  auto &Groupings = *Burst.Groupings;
  auto CopyFirstIfSatisfies = [&Histogram](auto &Cont, auto &&Cond) {
    copy_if(make_first_range(Histogram), std::inserter(Cont, Cont.end()), Cond);
  };
  auto Group = BurstGramData::UniqueOpcodesTy{};
  switch (Burst.Mode) {
  default:
    llvm_unreachable("Unknown Burst Mode");
  case BurstMode::LoadBurst:
    CopyFirstIfSatisfies(Group, [&II](auto Opc) {
      return II.get(Opc).mayLoad() && !II.get(Opc).mayStore();
    });
    break;
  case BurstMode::StoreBurst:
    CopyFirstIfSatisfies(Group,
                         [&II](auto Opc) { return II.get(Opc).mayStore(); });
    break;
  case BurstMode::LoadStoreBurst:
    CopyFirstIfSatisfies(Group,
                         [&II](auto Opc) { return II.get(Opc).mayStore(); });
    Groupings.push_back(Group);
    Group.clear();
    CopyFirstIfSatisfies(Group, [&II](auto Opc) {
      return II.get(Opc).mayLoad() && !II.get(Opc).mayStore();
    });
    break;
  case BurstMode::MixedBurst:
    CopyFirstIfSatisfies(Group, [&II](auto Opc) {
      return II.get(Opc).mayStore() || II.get(Opc).mayLoad();
    });
    break;
  }
  Burst.Groupings->push_back(Group);
  Burst.Mode = BurstMode::CustomBurst;
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

static void checkConfig(const Config &Cfg, const SnippyTarget &Tgt,
                        LLVMContext &Ctx, const OpcodeCache &OpCC) {
  Cfg.CGLayout.validate(Ctx);
  if (Cfg.Sections.empty())
    fatal(Ctx, "Incorrect list of sections", "list is empty");
  if (Cfg.Sections.generalRWSections().empty())
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
  for (auto &&Section : Cfg.Sections) {
    auto FoundSameIDString =
        std::find_if(Cfg.Sections.begin(), Cfg.Sections.end(),
                     [&Section](auto &&AnotherSec) {
                       return Section.ID != AnotherSec.ID &&
                              Section.getIDString() == AnotherSec.getIDString();
                     });
    if (FoundSameIDString != Cfg.Sections.end()) {
      auto IDString = Section.getIDString();
      snippy::fatal(Ctx, "Incorrect list of sections",
                    "List contains both numbered section #" + Twine(IDString) +
                        " and named section with same name.");
    }
  }

  if (std::any_of(Cfg.Sections.begin(), Cfg.Sections.end(), [&Cfg](auto &S1) {
        return std::count_if(Cfg.Sections.begin(), Cfg.Sections.end(),
                             [&S1](auto &S2) { return S2.ID == S1.ID; }) != 1;
      }))
    snippy::fatal(Ctx, "Incorrect list of sections",
                  "List contains duplicate section IDs");
  diagnoseXSections(Ctx, Cfg.Sections.begin(), Cfg.Sections.end());

  if (Cfg.Sections.size() < 2)
    return;
  for (auto SecIt = Cfg.Sections.begin();
       SecIt != std::prev(Cfg.Sections.end()); ++SecIt) {
    if (SecIt->interfere(*std::next(SecIt))) {
      std::stringstream SS;
      SS << "section " << SecIt->getIDString() << " and section "
         << std::next(SecIt)->getIDString() << " are interfering";
      snippy::fatal(Ctx, "Incorrect list of sections", SS.str());
    }
  }
  checkBurstGram(Ctx, Cfg.Histogram, OpCC, *Cfg.Burst.Data);
  checkCallRequirements(Tgt, Cfg.Histogram);
  checkMemoryRegions(Tgt, Cfg);
  Tgt.checkInstrTargetDependency(Cfg.Histogram);
  Cfg.MS.validateSchemes(Ctx, Cfg.Sections, StrictMemoryScheme);
}

static std::string getMemPlugin() {
  if (MemAddrGeneratorFile == "None")
    return {""};
  return MemAddrGeneratorFile;
}

static std::string getMemPluginInfo() {
  auto FileName = std::string{MemAddrGeneratorInfoFile};
  if (MemAddrGeneratorInfoFile == "None")
    FileName = "";
  if (!FileName.empty() && getMemPlugin().empty())
    report_fatal_error("Addresses generator plugin info file "
                       "may be used only with " +
                           Twine(MemAddrGeneratorFile.ArgStr) + "Option",
                       false);
  return FileName;
}

// Function to do all the necessary operations on Config after reading it
// from YAML.
static void completeConfig(Config &Cfg, LLVMState &State,
                           const OpcodeCache &OpCC) {
  auto TargetAlignment =
      State.getTargetMachine().createDataLayout().getPointerABIAlignment(
          0 /* address space */);
  Cfg.MS.fillBaseAccessesIfNeeded(Cfg.Sections, TargetAlignment);
  setBurstGramIfNeeded(Cfg.Burst, ConfigIOContext{OpCC, State.getCtx(),
                                                  State.getSnippyTarget()});
  Cfg.MS.updateMemoryBank();
  std::sort(Cfg.Sections.begin(), Cfg.Sections.end(),
            [](auto &S1, auto &S2) { return S1.VMA < S2.VMA; });
  convertToCustomBurstMode(Cfg.Histogram, State.getInstrInfo(),
                           *Cfg.Burst.Data);
  Cfg.MS.setAddrPlugin({getMemPlugin(), getMemPluginInfo()});
  checkConfig(Cfg, State.getSnippyTarget(), State.getCtx(), OpCC);
}

static void initializeLLVMAll() {
  InitializeAllTargetInfos();
  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  InitializeAllAsmParsers();
  InitializeAllDisassemblers();
  InitializeAllSnippyTargets();
}

static SelectedTargetInfo getSelectedTargetInfo() {
  SelectedTargetInfo TargetInfo;
  if (MArch.getValue().empty()) {
    // select current target
    TargetInfo.Triple = sys::getProcessTriple();
    TargetInfo.CPU = CpuName.getValue().empty() ? sys::getHostCPUName().str()
                                                : CpuName.getValue();
  } else {
    TargetInfo.Triple = MArch.getValue();
    TargetInfo.CPU = CpuName.getValue();
  }
  TargetInfo.Features = MAttr.getValue();
  return TargetInfo;
}

unsigned long long initializeRandomEngine(unsigned long long SeedValue) {
  if (Verbose)
    outs() << "Used seed: " << SeedValue << "\n";
  RandEngine::init(SeedValue);
  return SeedValue;
}

GeneratorSettings createGeneratorConfig(LLVMState &State, Config &&Cfg,
                                        RegPool &RP) {
  auto OutputFilename = getOutputFileBasename();
  auto SelfCheckPeriod = getSelfcheckPeriod();
  auto NumPrimaryInstrs = getExpectedNumInstrs(NumInstrs.getValue());
  bool FillCodeSectionMode = !NumPrimaryInstrs;
  auto SeedValue = seedOptToValue(Seed.getValue());
  initializeRandomEngine(SeedValue);
  auto Models = parseModelPluginList();
  bool RunOnModel = !Models.empty();
  parseReservedRegistersOption(RP, State.getSnippyTarget(), State.getRegInfo());
  auto SpilledRegs = parseSpilledRegistersOption(
      RP, State.getSnippyTarget(), State.getRegInfo(), State.getCtx());
  std::string DumpPathInitialState = deriveDefaultableOptionValue(
      RequestForInitialStateDump || Verbose, DumpInitialRegisters);
  std::string DumpPathFinalState = deriveDefaultableOptionValue(
      RequestForFinalStateDump || Verbose, DumpResultingRegisters);
  checkFullSizeGenerationRequirements(State.getInstrInfo(),
                                      State.getSnippyTarget(), Cfg,
                                      FillCodeSectionMode, SelfCheckPeriod);

  return GeneratorSettings(
      ABI, OutputFilename,
      TrackingOptions{Backtrack, SelfCheckPeriod, AddressVHOpt},
      DebugOptions{DumpMI, DumpMF, DumpCFG},
      LinkerOptions{ExternalStackOpt, MangleExportedNames,
                    std::move(EntryPointName.getValue())},
      ModelPluginOptions{RunOnModel, std::move(Models)},
      InstrsGenerationOptions{VerifyMachineInstrs, ChainedRXSectionsFill,
                              ChainedRXSorted, NumPrimaryInstrs,
                              std::move(LastInstr)},
      RegistersOptions{
          InitRegsInElf, InitialRegisterDataFile.getValue(),
          std::move(DumpPathInitialState), std::move(DumpPathFinalState),
          SmallVector<unsigned>(SpilledRegs.begin(), SpilledRegs.end())},
      std::move(Cfg));
}

static FlowGenerator createFlowGenerator(Config &&Cfg, LLVMState &State,
                                         const OpcodeCache &OpCC) {
  RegPool RP;
  auto GenSettings = createGeneratorConfig(State, std::move(Cfg), RP);
  return FlowGenerator(std::move(GenSettings), OpCC, std::move(RP));
}

static void saveToFile(const GeneratorResult &Result) {
  auto OutputFilename = getOutputFileBasename();
  auto ElfFile = addExtensionIfRequired(OutputFilename, ".elf");
  writeFile(ElfFile, Result.SnippetImage);
  if (!Result.LinkerScript.empty()) {
    auto LinkerScriptFilename = addExtensionIfRequired(OutputFilename, ".ld");
    writeFile(LinkerScriptFilename, Result.LinkerScript);
  }
}

// Function to place call of every "dump" method that does not need Config
void dumpIfNecessary(const OpcodeCache &OpCC) {
  if (DumpOptions) {
    OptionsMappingWrapper OMWP;
    outputYAMLToStream(OMWP, outs());
  }
  if (ListOpcodeNames)
    OpCC.dump();
}

void checkOptions(LLVMContext &Ctx) {
  auto GenerateWithPluginMode = generateWithPlugin();
  auto ParseWithPlugin = isParsingWithPluginEnabled();
  if (ParseWithPlugin && !GenerateWithPluginMode)
    fatal(Ctx, "-plugin-parser option was specified,",
          "but no generator plugin file was passed");
}

void generateMain() {
  initializeLLVMAll();
  readSnippyOptionsIfNeeded();
  auto State = LLVMState(getSelectedTargetInfo());
  checkOptions(State.getCtx());
  OpcodeCache OpCC(State.getSnippyTarget(), State.getInstrInfo(),
                   State.getSubtargetInfo());
  dumpIfNecessary(OpCC);
  if (LayoutFile.getValue().empty()) {
    errs() << "Please specify --layout=<file> for instruction generation\n";
    return;
  }
  auto Cfg = readSnippyConfig(State.getCtx(), State.getSnippyTarget(), OpCC);
  completeConfig(Cfg, State, OpCC);
  dumpConfigIfNeeded(
      Cfg, ConfigIOContext{OpCC, State.getCtx(), State.getSnippyTarget()},
      outs());
  auto Flow = createFlowGenerator(std::move(Cfg), State, OpCC);
  auto Result = Flow.generate(State);
  saveToFile(Result);
}

} // namespace snippy
} // namespace llvm

int main(int Argc, char **Argv) {
  using namespace llvm;
  if (Argc)
    llvm::snippy::ARGV0 = Argv[0];
#if defined(LLVM_REVISION)
  cl::AddExtraVersionPrinter(
      [](raw_ostream &OS) { OS << "Revision: " LLVM_REVISION "\n\n"; });
#endif
  cl::ParseCommandLineOptions(Argc, Argv, "");

  snippy::ExitOnErr.setExitCodeMapper(
      [](const Error &Err) { return EXIT_FAILURE; });

  snippy::generateMain();
  snippy::reportWarningsSummary();
  return EXIT_SUCCESS;
}
