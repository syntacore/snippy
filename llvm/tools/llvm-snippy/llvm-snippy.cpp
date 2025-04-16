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

#include "snippy/Config/Config.h"
#include "snippy/Generator/LLVMState.h"
#include "snippy/Generator/MemoryManager.h"
#include "snippy/Generator/RegisterPool.h"
#include "snippy/Support/DiagnosticInfo.h"
#include "snippy/Support/OpcodeCache.h"
#include "snippy/Support/Options.h"
#include "snippy/Support/Utils.h"
#include "snippy/Support/YAMLUtils.h"
#include "snippy/Target/Target.h"
#include "snippy/Target/TargetSelect.h"
#include "snippy/Version/Version.inc"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/VCSRevision.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"

#include <algorithm>
#include <sstream>
#include <string>

namespace llvm {
namespace snippy {

extern cl::OptionCategory Options;

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

static snippy::opt<bool>
    ListOpcodeNames("list-opcode-names",
                    cl::desc("list available opcode names"), cl::cat(Options));

std::optional<unsigned> getExpectedNumInstrs(StringRef NumAsString) {
  if (NumAsString == "all")
    return {};
  int Value;
  if (!to_integer(NumAsString, Value, /*base*/ 10))
    snippy::fatal("num-instrs get not a number or all");
  if (Value < 0)
    snippy::fatal("num-instrs get negative number");
  return Value;
}

static snippy::opt<bool> DumpMF(
    "dump-mf",
    cl::desc("[Debug] dump final generated machine function in Machine IR"),
    cl::cat(Options));

static snippy::opt<bool> DumpMI(
    "dump-mi",
    cl::desc("[Debug] dump generated machine instructions in Machine IR"),
    cl::cat(Options));

static snippy::opt<bool> DumpCFG("dump-cfg", cl::desc("Dump generated CFG"),
                                 cl::cat(Options));

static snippy::opt<bool> ViewCFG("view-cfg", cl::desc("View generated CFG"),
                                 cl::cat(Options),
                                 cl::callback([](const bool &V) {
                                   if (V)
                                     DumpCFG = true;
                                 }));

static snippy::opt<std::string>
    Seed("seed",
         cl::desc("seed for instruction generation. If the option is not used "
                  "or its value is not set, seed will be generated randomly."),
         cl::cat(Options));

static snippy::opt<std::string>
    OutputFileBasename("o", cl::desc("Override output file base name"),
                       cl::value_desc("filename"), cl::cat(Options));

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

static snippy::opt<bool> DumpOptions("dump-options",
                                     cl::desc("Dump snippy options' values"),
                                     cl::cat(Options), cl::init(false));

static snippy::opt<bool> DumpPreprocessedConfig(
    "E", cl::desc("Dump snippy config after preprocessing it"), cl::init(false),
    cl::cat(Options));

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
      snippy::fatal(toString(std::move(Err)).c_str());
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
static Config readSnippyConfig(LLVMState &State, RegPool &RP,
                               const OpcodeCache &OpCC) {
  auto &Ctx = State.getCtx();
  auto ParseWithPlugin = isParsingWithPluginEnabled();

  IncludePreprocessor IPP(LayoutFile.getValue(), getExtraIncludeDirsForLayout(),
                          Ctx);
  mergeFiles(IPP, Ctx);
  if (DumpPreprocessedConfig)
    outs() << IPP.getPreprocessed() << "\n";

  return Config(IPP, RP, State, GeneratorPluginFile.getValue(),
                GeneratorPluginParserFile.getValue(), OpCC, ParseWithPlugin);
}

static void dumpConfigIfNeeded(const Config &Cfg,
                               const ConfigIOContext &CfgParsingCtx,
                               raw_ostream &OS) {
  if (DumpLayout)
    Cfg.dump(OS, CfgParsingCtx);
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
    snippy::fatal(Ctx, "-plugin-parser option was specified,",
                  "but no generator plugin file was passed");
  checkWarningOptions();
}

static void printNoLayoutHint(LLVMContext &Ctx) {
  // NOTE: Some options like list-opcode-names are useful on their own
  // and unnecessary notices just clutter the output. It's reasonable to just
  // skip the notice hint altogether for those.
  //
  // Dumping options is really helpful to see the YAML format for them, so
  // we skip printing the hint as well.
  auto ShouldSkipHint =
      ListOpcodeNames.getNumOccurrences() || DumpOptions.getNumOccurrences();

  if (ShouldSkipHint)
    return;

  snippy::notice(
      WarningName::NotAWarning, Ctx, "Nothing to do",
      "Please specify path to <layout> file as a positional argument for "
      "instruction generation");
}

static DebugOptions getDebugOptions() {
  DebugOptions DbgCfg;
  DbgCfg.PrintInstrs = DumpMI;
  DbgCfg.PrintMachineFunctions = DumpMF;
  DbgCfg.PrintControlFlowGraph = DumpCFG;
  DbgCfg.ViewControlFlowGraph = ViewCFG;
  return DbgCfg;
}

void generateMain() {
  initializeLLVMAll();
  readSnippyOptionsIfNeeded();
  auto ExpectedState = LLVMState::create(getSelectedTargetInfo());
  if (!ExpectedState)
    snippy::fatal(ExpectedState.takeError());
  auto &State = ExpectedState.get();
  checkOptions(State.getCtx());
  OpcodeCache OpCC(State.getSnippyTarget(), State.getInstrInfo(),
                   State.getSubtargetInfo());
  dumpIfNecessary(OpCC);

  if (!LayoutFile.getNumOccurrences()) {
    printNoLayoutHint(State.getCtx());
    return;
  }

  // init random engine before everything else.
  auto SeedValue = seedOptToValue(Seed.getValue());
  initializeRandomEngine(SeedValue);
  RegPool RP;

  auto Cfg = readSnippyConfig(State, RP, OpCC);
  dumpConfigIfNeeded(Cfg, ConfigIOContext{OpCC, RP, State}, outs());
  FlowGenerator Flow{std::move(Cfg), OpCC, std::move(RP),
                     getOutputFileBasename()};
  auto Result = Flow.generate(State, getDebugOptions());
  saveToFile(Result);
}

} // namespace snippy
} // namespace llvm

int main(int Argc, char **Argv) {
  using namespace llvm;
  if (Argc)
    llvm::snippy::ARGV0 = Argv[0];
  cl::AddExtraVersionPrinter([](raw_ostream &OS) {
    OS << "Snippy version: " LLVM_SNIPPY_VERSION_STRING "\n";
  });
#if defined(LLVM_REVISION)
  cl::AddExtraVersionPrinter(
      [](raw_ostream &OS) { OS << "Revision: " LLVM_REVISION "\n\n"; });
#endif
  cl::ParseCommandLineOptions(Argc, Argv, "");

  snippy::generateMain();
  snippy::reportWarningsSummary();
  return EXIT_SUCCESS;
}
