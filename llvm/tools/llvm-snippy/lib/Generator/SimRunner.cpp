//===-- SimRunner.cpp -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/SimRunner.h"
#include "llvm/Support/Path.h"

namespace llvm {
namespace snippy {
namespace {
Expected<uint64_t> getAddressOfSymbolInImage(StringRef Image,
                                             StringRef SymbolName) {
  auto MemBuf = MemoryBuffer::getMemBuffer(Image, "", false);

  auto &&Bin = object::ObjectFile::createObjectFile(*MemBuf);
  if (!Bin)
    return Bin.takeError();
  auto &&Obj = *Bin;

  auto ExitSimIt = std::find_if(Obj->symbols().begin(), Obj->symbols().end(),
                                [SymbolName](const auto &Sym) {
                                  if (auto Name = Sym.getName())
                                    return *Name == SymbolName;
                                  return false;
                                });
  if (ExitSimIt == Obj->symbols().end())
    return {make_error<Failure>(Twine("no symbol ") + Twine(SymbolName) +
                                Twine(" in image"))};

  auto ExpectedAddress = ExitSimIt->getAddress();
  if (!ExpectedAddress)
    return ExpectedAddress.takeError();

  return *ExpectedAddress;
}
} // namespace

SimRunner::SimRunner(LLVMContext &Ctx, const SnippyTarget &TGT,
                     const TargetSubtargetInfo &Subtarget,
                     SimulationEnvironment SimEnv,
                     ArrayRef<std::string> ModelLibs)
    : Ctx(Ctx) {
  Env = std::make_unique<SimulationEnvironment>(std::move(SimEnv));
  assert(!ModelLibs.empty() && "Model lib list must not be empty");

  for (auto &ModelLibName : ModelLibs) {
    auto CfgCopy = Env->SimCfg;
    // Add model plugin name postfix to secondary plugins' trace log files.
    if (!CfgCopy.TraceLogPath.empty() && ModelLibName != ModelLibs.front())
      CfgCopy.TraceLogPath = (CfgCopy.TraceLogPath + Twine(".") +
                              sys::path::filename(ModelLibName))
                                 .str();
    auto Sim = Interpreter::createSimulatorForTarget(
        TGT, Subtarget, CfgCopy, Env->TgtGenCtx, Env->CallbackHandler.get(),
        ModelLibName);
    CoInterp.emplace_back(
        std::make_unique<Interpreter>(Ctx, *Env, std::move(Sim)));
  }
}

ProgramCounterType SimRunner::run(StringRef Program,
                                  const IRegisterState &InitialRegState) {
  auto StopPC = getAddressOfSymbolInImage(Program, Linker::GetExitSymbolName());
  if (auto E = StopPC.takeError()) {
    auto Err = toString(std::move(E));
    report_fatal_error("[Internal error]: unable to get last instruction PC: " +
                           Twine(Err) + Twine("\nPlease, report a bug"),
                       false);
  }
  for (auto &I : CoInterp) {
    I->setInitialState(InitialRegState);
    I->loadElfImage(Program);
    I->setStopModeByPC(*StopPC);
  }

  checkStates(/* CheckMemory */ true);

  auto &PrimI = getPrimaryInterpreter();
  PrimI.logMessage("#===Simulation Start===\n");

  ProgramCounterType CurPC = PrimI.getPC();

  while (!PrimI.endOfProg()) {
    CurPC = PrimI.getPC();
    auto ExecRes = PrimI.step();
    if (ExecRes == ExecutionResult::FatalError)
      PrimI.reportSimulationFatalError("Primary interpreter step failed");

    for (auto [Num, I] : enumerate(drop_begin(CoInterp))) {
      assert(I.get() != &PrimI);
      if (I->step() == ExecutionResult::FatalError)
        I->reportSimulationFatalError(std::to_string(Num) +
                                      " interpreter step failed");
    }

    if (ExecRes == ExecutionResult::SimulationExit)
      break;

    if (ExecRes != ExecutionResult::Success)
      PrimI.reportSimulationFatalError(
          "Unexpected primary interpreter step result");
    // TODO: add an option to compare memory state after each step
    checkStates(/* CheckMemory */ false);
  }
  checkStates(/* CheckMemory */ true);
  return CurPC;
}

void SimRunner::checkStates(bool CheckMemory) {
  if (CoInterp.size() < 2)
    return;
  auto &PI = getPrimaryInterpreter();
  if (std::any_of(CoInterp.begin(), CoInterp.end(),
                  [&PI, CheckMemory](auto &I) {
                    return !PI.compareStates(*I, CheckMemory);
                  })) {
    std::string MismatchMessage;
    llvm::raw_string_ostream Stream(MismatchMessage);
    for (auto &I : CoInterp)
      I->dumpCurrentRegStateToStream(Stream);
    report_fatal_error(
        "Interpreters states differ :\n" + Twine(MismatchMessage), false);
  }
}

} // namespace snippy
} // namespace llvm
