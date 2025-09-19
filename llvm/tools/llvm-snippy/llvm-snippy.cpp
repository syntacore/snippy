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
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <sstream>
#include <string>

namespace llvm {
namespace snippy {

extern cl::OptionCategory Options;

// TODO: All options should be read into a struct all passed around through
// config. There are some options that are necessary to bootstrap the config so
// they'd have to be left in the driver.
#define GEN_SNIPPY_OPTIONS_DEF
#include "SnippyDriverOptions.inc"
#undef GEN_SNIPPY_OPTIONS_DEF

// YAML file with memory layout and histogram
static cl::opt<std::string> LayoutFile(cl::Positional, cl::desc("<layout>"),
                                       cl::cat(Options), cl::init(""));

static cl::list<std::string> AdditionalLayoutFiles(cl::Positional,
                                                   cl::desc("<sub-layouts>..."),
                                                   cl::cat(Options));

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
static Config readSnippyConfig(LLVMState &State, RegPoolWrapper &RP,
                               const OpcodeCache &OpCC,
                               const DebugOptions &DebugOpts) {
  auto &Ctx = State.getCtx();
  auto ParseWithPlugin = isParsingWithPluginEnabled();

  IncludePreprocessor IPP(LayoutFile.getValue(), getExtraIncludeDirsForLayout(),
                          Ctx);
  mergeFiles(IPP, Ctx);
  if (DebugOpts.DumpPreprocessedConfig)
    outs() << IPP.getPreprocessed() << "\n";

  return Config(IPP, RP, State, GeneratorPluginFile.getValue(),
                GeneratorPluginParserFile.getValue(), OpCC, ParseWithPlugin);
}

static void dumpConfigIfNeeded(const Config &Cfg,
                               const ConfigIOContext &CfgParsingCtx,
                               raw_ostream &OS, const DebugOptions &Opts) {
  if (Opts.DumpLayout)
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

static SelectedTargetInfo getSelectedTargetInfo(const ProgramOptions &Opts) {
  SelectedTargetInfo TargetInfo;
  TargetInfo.MArch = Opts.MArch;
  TargetInfo.Triple = Opts.MTargetTriple;
  TargetInfo.CPU = Opts.CpuName;
  TargetInfo.Features = Opts.MAttr;
  if (!Opts.MTargetTripleSpecified && !Opts.MArchSpecified) {
    TargetInfo.Triple = sys::getProcessTriple();
    if (!Opts.CpuNameSpecified)
      TargetInfo.CPU = sys::getHostCPUName();
  }
  return TargetInfo;
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
void dumpIfNecessary(const OpcodeCache &OpCC, const DebugOptions &DebugOpts) {
  if (DebugOpts.DumpOptions) {
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

static void printNoLayoutHint(LLVMContext &Ctx, const DebugOptions &Opts) {
  // NOTE: Some options like list-opcode-names are useful on their own
  // and unnecessary notices just clutter the output. It's reasonable to just
  // skip the notice hint altogether for those.
  //
  // Dumping options is really helpful to see the YAML format for them, so
  // we skip printing the hint as well.
  auto ShouldSkipHint =
      ListOpcodeNames.getNumOccurrences() || Opts.DumpOptionsSpecified;

  if (ShouldSkipHint)
    return;

  snippy::notice(
      WarningName::NotAWarning, Ctx, "Nothing to do",
      "Please specify path to <layout> file as a positional argument for "
      "instruction generation");
}

void generateMain() {
  initializeLLVMAll();
  readSnippyOptionsIfNeeded();
  auto ProgramOpts = copyOptionsToProgramOptions();
  auto ExpectedState = LLVMState::create(getSelectedTargetInfo(ProgramOpts));
  if (!ExpectedState)
    snippy::fatal(ExpectedState.takeError());
  auto &State = ExpectedState.get();
  checkOptions(State.getCtx());
  OpcodeCache OpCC(State.getSnippyTarget(), State.getInstrInfo(),
                   State.getSubtargetInfo());
  auto DebugOpts = copyOptionsToDebugOptions();
  dumpIfNecessary(OpCC, DebugOpts);

  if (!LayoutFile.getNumOccurrences()) {
    printNoLayoutHint(State.getCtx(), DebugOpts);
    return;
  }

  std::vector<RegPool> RegPools;
  RegPoolWrapper RPW(State.getSnippyTarget(), State.getRegInfo(), RegPools);

  auto Cfg = readSnippyConfig(State, RPW, OpCC, DebugOpts);
  if (DebugOpts.Verbose)
    outs() << "Used seed: " << Cfg.ProgramCfg->Seed << '\n';

  ConfigIOContext CfgParsingContext{
      OpCC,
      RPW,
      State,
  };
  dumpConfigIfNeeded(Cfg, CfgParsingContext, outs(), DebugOpts);
  FlowGenerator Flow{std::move(Cfg), OpCC, std::move(RegPools),
                     getOutputFileBasename()};
  auto Result = Flow.generate(State, DebugOpts);
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
