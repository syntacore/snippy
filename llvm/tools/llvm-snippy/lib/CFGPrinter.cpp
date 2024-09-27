//===-- CFGPrinter.cpp ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CFGPrinter.h"
#include "InitializePasses.h"
#include "snippy/CreatePasses.h"

#include "snippy/Support/Options.h"

#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "snippy-cfg-printer"
#define PASS_DESC "Snippy CFG Printer"

namespace llvm {
namespace snippy {

extern cl::OptionCategory Options;

static snippy::opt<std::string> CFGBasename("cfg-basename",
                                            cl::desc("Basename for CFG dump."),
                                            cl::cat(Options));

static snippy::opt_list<std::string>
    DumpCFGBefore("dump-cfg-before",
                  cl::desc("Dump CFG before specified passes."),
                  cl::cat(Options), cl::CommaSeparated, cl::Hidden);
static snippy::opt_list<std::string>
    DumpCFGAfter("dump-cfg-after", cl::desc("Dump CFG after specified passes."),
                 cl::cat(Options), cl::CommaSeparated, cl::Hidden);

static snippy::opt<bool>
    DumpCFGBeforeAll("dump-cfg-before-all",
                     cl::desc("Dump CFG before all passes"), cl::cat(Options),
                     cl::Hidden);
static snippy::opt<bool> DumpCFGAfterAll("dump-cfg-after-all",
                                         cl::desc("Dump CFG after all passes"),
                                         cl::cat(Options), cl::Hidden);

static snippy::opt_list<std::string>
    ViewCFGBefore("view-cfg-before",
                  cl::desc("View CFG before specified passes"),
                  cl::cat(Options), cl::CommaSeparated, cl::Hidden);
static snippy::opt_list<std::string>
    ViewCFGAfter("view-cfg-after", cl::desc("View CFG after specified passes"),
                 cl::cat(Options), cl::CommaSeparated, cl::Hidden);

static snippy::opt<bool>
    ViewCFGBeforeAll("view-cfg-before-all",
                     cl::desc("View CFG before all passes"), cl::cat(Options),
                     cl::Hidden);
static snippy::opt<bool> ViewCFGAfterAll("view-cfg-after-all",
                                         cl::desc("View CFG after all passes"),
                                         cl::cat(Options), cl::Hidden);

template <typename R>
static bool shouldDumpCFGBeforeOrAfterPass(StringRef PassID, R &&PassesToDump) {
  return is_contained(std::forward<R>(PassesToDump), PassID);
}

bool shouldDumpCFGBeforePass(StringRef PassID) {
  return DumpCFGBeforeAll ||
         shouldDumpCFGBeforeOrAfterPass(PassID, DumpCFGBefore) ||
         shouldViewCFGBeforePass(PassID);
}

bool shouldDumpCFGAfterPass(StringRef PassID) {
  return DumpCFGAfterAll ||
         shouldDumpCFGBeforeOrAfterPass(PassID, DumpCFGAfter) ||
         shouldViewCFGAfterPass(PassID);
}

template <typename R>
static bool shouldViewCFGBeforeOrAfterPass(StringRef PassID, R &&PassesToView) {
  return is_contained(std::forward<R>(PassesToView), PassID);
}

bool shouldViewCFGBeforePass(StringRef PassID) {
  return ViewCFGBeforeAll ||
         shouldViewCFGBeforeOrAfterPass(PassID, ViewCFGBefore);
}

bool shouldViewCFGAfterPass(StringRef PassID) {
  return ViewCFGAfterAll ||
         shouldViewCFGBeforeOrAfterPass(PassID, ViewCFGAfter);
}

// NOTE: taken from llvm/lib/Support/GraphWriter.cpp:99
static std::string replaceIllegalFilenameChars(std::string Filename,
                                               const char ReplacementChar) {
  std::string IllegalChars =
      is_style_windows(sys::path::Style::native) ? "\\/:?\"<>|" : "/";

  for (char IllegalChar : IllegalChars) {
    std::replace(Filename.begin(), Filename.end(), IllegalChar,
                 ReplacementChar);
  }

  return Filename;
}

static std::string createCFGFilename(const Twine &MFName, StringRef Desc) {
  std::string Filename;
  raw_string_ostream FilenameS(Filename);
  if (CFGBasename.isSpecified()) {
    FilenameS << CFGBasename;
    if (CFGBasename.getValue().back() != '/')
      FilenameS << "_";
  }
  FilenameS << replaceIllegalFilenameChars(MFName.str(), '_') << "-cfg-dump";
  if (!Desc.empty())
    FilenameS << '-' << replaceIllegalFilenameChars(Desc.str(), '_');
  FilenameS << ".dot";
  return Filename;
}

namespace {

class CFGPrinter final : public MachineFunctionPass {
  std::string Desc;
  std::string FilenameDesc;
  bool EnableView = false;

  std::string makeTitle(StringRef FunctionName) const;
  std::string dumpCFG(const MachineFunction &MF) const;

public:
  static char ID;

  CFGPrinter();

  CFGPrinter(const Twine &Desc, bool EnableView);

  CFGPrinter(const Twine &Desc, const Twine &FilenameDesc, bool EnableView);

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnMachineFunction(MachineFunction &MF) override;
};

char CFGPrinter::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::CFGPrinter;

INITIALIZE_PASS(CFGPrinter, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

MachineFunctionPass *createCFGPrinterPass(bool EnableView) {
  return new CFGPrinter(/*Desc=*/"", EnableView);
}

MachineFunctionPass *createCFGPrinterPassBefore(const PassInfo &PI,
                                                bool EnableView) {
  return new CFGPrinter("before " + PI.getPassName(),
                        "before-" + PI.getPassArgument(), EnableView);
}

MachineFunctionPass *createCFGPrinterPassAfter(const PassInfo &PI,
                                               bool EnableView) {
  dbgs() << "Creating CFG printer after ***" << PI.getPassName() << "***\n";
  return new CFGPrinter("after " + PI.getPassName(),
                        "after-" + PI.getPassArgument(), EnableView);
}

namespace snippy {

CFGPrinter::CFGPrinter() : MachineFunctionPass(ID) {}

CFGPrinter::CFGPrinter(const Twine &Desc, bool EnableView)
    : MachineFunctionPass(ID), Desc(Desc.str()), FilenameDesc(Desc.str()),
      EnableView(EnableView) {}

CFGPrinter::CFGPrinter(const Twine &Desc, const Twine &FilenameDesc,
                       bool EnableView)
    : MachineFunctionPass(ID), Desc(Desc.str()),
      FilenameDesc(FilenameDesc.str()), EnableView(EnableView) {}

std::string CFGPrinter::makeTitle(StringRef FunctionName) const {
  return ("CFG for '" + FunctionName + "' function " + Desc).str();
}

std::string CFGPrinter::dumpCFG(const MachineFunction &MF) const {
  auto MFName = MF.getName();
  std::string Title = makeTitle(MFName);

  auto Filename = createCFGFilename(MFName, FilenameDesc);
  std::error_code EC;
  raw_fd_ostream O(Filename, EC);
  if (EC)
    snippy::fatal(MF.getFunction().getContext(),
                  "Error when trying to open file", EC.message());

  llvm::WriteGraph(O, &MF, /* ShortNames */ false, Title);

  return Filename;
}

bool CFGPrinter::runOnMachineFunction(MachineFunction &MF) {
  std::string Filename = dumpCFG(MF);

  if (EnableView && !Filename.empty())
    // NOTE: Currently this function prints logs in errs()
    DisplayGraph(Filename, /* wait */ false, GraphProgram::DOT);

  return false;
}

} // namespace snippy

std::string DOTGraphTraits<const MachineFunction *>::getNodeLabel(
    const MachineBasicBlock *Node, const MachineFunction *Graph) {
  std::string OutStr;
  raw_string_ostream OSS(OutStr);

  if (isSimple()) {
    OSS << printMBBReference(*Node);
    if (const BasicBlock *BB = Node->getBasicBlock())
      OSS << ": " << BB->getName();
  } else {
    Node->print(OSS);
  }

  if (OutStr[0] == '\n')
    OutStr.erase(OutStr.begin());

  // Process string output to make it nicer...
  snippy::replaceAllSubstrs(OutStr, "\n", "\\l");
  return OutStr;
}

} // namespace llvm
