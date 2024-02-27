//===-- RISCVSimulator.cpp --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <string>

#include <RISCVModel/RVM.hpp>

#include "snippy/Config/SerDesUtils.h"
#include "snippy/Simulator/Targets/RISCV.h"
#include "snippy/Simulator/Transactions.h"
#include "snippy/Support/DiagnosticInfo.h"
#include "snippy/Support/DynLibLoader.h"
#include "snippy/Support/Options.h"

#include "Common.h"

#define DEBUG_TYPE "snippy-riscv-sim"

namespace llvm {
namespace snippy {

cl::OptionCategory RISCVSimOptions("RISCV simulator options");

static snippy::opt<bool> DumpRegsWithoutPC(
    "exclude-pc-from-dump",
    cl::desc("dumps program counter register with other regs"),
    cl::cat(RISCVSimOptions), cl::init(false), cl::Hidden);

template <typename IntegerType, typename ContainerType>
ContainerType applyMask(const ContainerType &Vect, IntegerType Mask) {
  ContainerType Result;
  std::transform(Vect.begin(), Vect.end(), std::back_inserter(Result),
                 [=](const auto &Item) { return Item & Mask; });
  return Result;
}

static bool hasPCInRH(const RegistersWithHistograms &RH) {
  constexpr auto PCRegClassPrefix = "P";
  const auto &ClassValues = RH.Registers.ClassValues;
  auto PCRegClassIt = std::find_if(ClassValues.begin(), ClassValues.end(),
                                   [](const RegisterClassValues &CV) {
                                     return CV.RegType == PCRegClassPrefix;
                                   });
  if (PCRegClassIt == ClassValues.end())
    return false;

  auto NumFound =
      std::distance(PCRegClassIt->Values.begin(), PCRegClassIt->Values.end());
  assert(NumFound > 0);

  if (NumFound > 1)
    report_fatal_error("Too many programm registers are specified "
                       "in the reg state YAML",
                       false);
  return true;
}

void RISCVRegisterState::loadFromYamlFile(StringRef YamlFile,
                                          WarningsT &WarningsArr) {
  auto RH = loadRegistersFromYaml(YamlFile);

  constexpr auto DefaultPC = 0ul;
  PC = DefaultPC;
  if (hasPCInRH(RH))
    WarningsArr.emplace_back("PC value from YAML has been ignored");

  std::array<StringRef, 4> AllowedRegClasses = {"X", "F", "V", "P"};
  checkRegisterClasses(RH.Registers, AllowedRegClasses);
  checkRegisterClasses(RH.Histograms, AllowedRegClasses);

  auto NumXRegs = XRegs.size();
  getRegisterGroup(RH, NumXRegs, "X", XRegSize * RISCV_CHAR_BIT, XRegs);
  if (XRegs.size() != NumXRegs) {
    XRegs.resize(NumXRegs);
    uniformlyFillXRegs();
  }
  XRegs[0] = 0;

  auto NumFRegs = FRegs.size();
  getRegisterGroup(RH, NumFRegs, "F", FRegSize * RISCV_CHAR_BIT, FRegs);
  if (FRegs.size() != NumFRegs) {
    FRegs.resize(NumFRegs);
    uniformlyFillFRegs();
  }

  auto NumVRegs = VRegs.size();
  getRegisterGroup(RH, NumVRegs, "V", VLEN, VRegs);
  if (VRegs.size() != NumVRegs) {
    VRegs.resize(NumVRegs);
    uniformlyFillVRegs();
  }
}

void RISCVRegisterState::saveAsYAMLFile(StringRef Filename) const {
  // Under some circumstances rv32 models can behave little bit wierdly:
  // namely, they can unexpectedly update high part of 64-bit storage for
  // 32-bit register. So we have to manually truncate the state to avoid
  // discrepancies between different models
  auto XRegsMasked = XRegs;
  if (XRegSize == Reg4Bytes)
    XRegsMasked = applyMask(XRegsMasked, std::numeric_limits<uint32_t>::max());
  auto FRegsMasked = FRegs;
  if (FRegSize == Reg4Bytes)
    FRegsMasked = applyMask(FRegsMasked, std::numeric_limits<uint32_t>::max());

  auto RegSerObj = RegisterSerialization()
                       .addRegisterGroup("X", XRegsMasked)
                       .addRegisterGroup("F", FRegsMasked)
                       .addRegisterGroup("V", VLEN, VRegs);
  if (!DumpRegsWithoutPC)
    RegSerObj.addRegisterGroup("P", PC);
  RegSerObj.saveAsYAML(Filename);
}

bool RISCVRegisterState::operator==(const IRegisterState &Another) const {
  auto &Casted = static_cast<const RISCVRegisterState &>(Another);

  return XRegs == Casted.XRegs && FRegs == Casted.FRegs &&
         VRegs == Casted.VRegs;
}

void RISCVRegisterState::uniformlyFillXRegs() {
  assert(!XRegs.empty());
  uniformlyFill<uint64_t>(MutableArrayRef<uint64_t>(XRegs).slice(1), 0,
                          getMaxRegValueForSize(XRegSize));
}

void RISCVRegisterState::uniformlyFillFRegs() {
  if (!FRegs.empty())
    uniformlyFill<uint64_t>(FRegs, 0, getMaxRegValueForSize(FRegSize));
}

void RISCVRegisterState::uniformlyFillVRegs() {
  for (auto &VReg : VRegs)
    VReg = RandEngine::genAPInt(VLEN);
}

class SnippyRISCVSimulator final
    : public CommonSimulatorImpl<rvm::State, SnippyRISCVSimulator, RVMXReg,
                                 RVMFReg> {
  unsigned VLEN = 0;
  RegSizeInBytes XRegSize = Reg4Bytes;
  RegSizeInBytes FRegSize = Reg4Bytes;

public:
  SnippyRISCVSimulator(rvm::State &&MS) : CommonSimulatorImpl(std::move(MS)) {}

  ExecutionResult executeInstr() override {
    if (ModelState.executeInstr() == 0)
      return ExecutionResult::Success;
    return ExecutionResult::FatalError;
  }

  llvm::APInt readReg(llvm::Register Reg) const override {
    auto RegSize = getRegBitWidth(Reg, XRegSize * RISCV_CHAR_BIT, VLEN);
    auto RegIdx = regToIndex(Reg);
    if (Reg >= RISCV::X0 && Reg <= RISCV::X31)
      return llvm::APInt(RegSize,
                         ModelState.readXReg(static_cast<RVMXReg>(RegIdx)));
    if ((Reg >= RISCV::F0_D && Reg <= RISCV::F31_D) ||
        (Reg >= RISCV::F0_F && Reg <= RISCV::F31_F) ||
        (Reg >= RISCV::F0_H && Reg <= RISCV::F31_H))
      return llvm::APInt(RegSize,
                         ModelState.readFReg(static_cast<RVMFReg>(RegIdx)));
    if (Reg >= RISCV::V0 && Reg <= RISCV::V31) {
      llvm::SmallVector<uint64_t> Val(RegSize / 64);
      ModelState.readVReg(static_cast<RVMVReg>(RegIdx),
                          reinterpret_cast<char *>(Val.data()), RegSize);
      return llvm::APInt(RegSize, Val);
    }
    llvm::report_fatal_error("Impossible to read the register from the "
                             "Simulator. Invalid RegID for the RISCV target",
                             false);
  }

  void setReg(llvm::Register Reg, const APInt &NewValue) override {
    auto RegIdx = regToIndex(Reg);
    if (Reg >= RISCV::X0 && Reg <= RISCV::X31) {
      setGPR(RegIdx, NewValue.getZExtValue());
      return;
    }
    if ((Reg >= RISCV::F0_D && Reg <= RISCV::F31_D) ||
        (Reg >= RISCV::F0_F && Reg <= RISCV::F31_F) ||
        (Reg >= RISCV::F0_H && Reg <= RISCV::F31_H)) {
      setFPR(RegIdx, NewValue.getZExtValue());
      return;
    }
    if (Reg >= RISCV::V0 && Reg <= RISCV::V31) {
      setVPR(RegIdx, NewValue);
      return;
    }
    llvm::report_fatal_error("Impossible to set the register from the "
                             "Simulator. Invalid RegID for the RISCV target",
                             false);
  }

  void saveState(IRegisterState &Output) const override {
    auto &Regs = static_cast<RISCVRegisterState &>(Output);
    Regs.PC = readPC();
    for (size_t XRegNo = 0; XRegNo < Regs.XRegs.size(); ++XRegNo)
      Regs.XRegs[XRegNo] = readGPR(XRegNo);
    for (size_t FRegNo = 0; FRegNo < Regs.FRegs.size(); ++FRegNo)
      Regs.FRegs[FRegNo] = readFPR(FRegNo);

    if (Regs.VRegs.empty())
      return;

    for (size_t VRegNo = 0; VRegNo < Regs.VRegs.size(); ++VRegNo)
      Regs.VRegs[VRegNo] = readVPR(VRegNo);
  }

  void setState(const IRegisterState &Input) override {
    const auto &Regs = static_cast<const RISCVRegisterState &>(Input);
    setPC(Regs.PC);
    XRegSize = Regs.XRegSize;
    FRegSize = Regs.FRegSize;
    VLEN = Regs.VLEN;

    for (size_t XRegNo = 0; XRegNo < Regs.XRegs.size(); ++XRegNo)
      setGPR(XRegNo, Regs.XRegs[XRegNo]);
    for (size_t FRegNo = 0; FRegNo < Regs.FRegs.size(); ++FRegNo)
      setFPR(FRegNo, Regs.FRegs[FRegNo]);

    if (Regs.VRegs.empty())
      return;

    for (size_t VRegNo = 0; VRegNo < Regs.VRegs.size(); ++VRegNo)
      setVPR(VRegNo, Regs.VRegs[VRegNo]);
  }

  VectorRegisterType readVPR(unsigned RegID) const override {
    SmallVector<uint64_t, 2> Data(VLEN / RISCV_CHAR_BIT);
    ModelState.readVReg(static_cast<RVMVReg>(RegID),
                        reinterpret_cast<char *>(Data.data()),
                        VLEN / RISCV_CHAR_BIT);
    return VectorRegisterType(VLEN, Data);
  }

  void setVPR(unsigned RegID, const VectorRegisterType &NewValue) override {
    assert(NewValue.getBitWidth() == VLEN);
    ModelState.setVReg(static_cast<RVMVReg>(RegID),
                       reinterpret_cast<const char *>(NewValue.getRawData()),
                       NewValue.getBitWidth() / RISCV_CHAR_BIT);
  }

  bool supportsCallbacks() const override {
    return ModelState.getVTable()->queryCallbackSupportPresent();
  }
};

struct SimulatorIsaInfo {
  bool Is64Bit;
  uint64_t MisaBits;
  uint64_t Zext;
};

// TODO: auto-generate this?
static uint64_t deriveMisaBits(const RISCVSubtarget &Subtarget) {
  uint64_t MisaBits = 0u;
  if (Subtarget.hasStdExtA())
    MisaBits |= RVM_MISA_A;
  if (Subtarget.hasStdExtC())
    MisaBits |= RVM_MISA_C;
  if (Subtarget.hasStdExtD())
    MisaBits |= RVM_MISA_D;
  if (Subtarget.hasStdExtF())
    MisaBits |= RVM_MISA_F;
  if (Subtarget.hasStdExtM())
    MisaBits |= RVM_MISA_M;
  if (Subtarget.hasStdExtV())
    MisaBits |= RVM_MISA_V;
  return MisaBits;
}

// TODO: auto-generate this?
static uint64_t deriveZextBits(const RISCVSubtarget &Subtarget) {
  uint64_t ZextBits = 0u;
  if (Subtarget.hasStdExtZba())
    ZextBits |= RVM_ZEXT_BA;
  if (Subtarget.hasStdExtZbb())
    ZextBits |= RVM_ZEXT_BB;
  if (Subtarget.hasStdExtZbc())
    ZextBits |= RVM_ZEXT_BC;
  if (Subtarget.hasStdExtZbs())
    ZextBits |= RVM_ZEXT_BS;
  if (Subtarget.hasStdExtZfh())
    ZextBits |= RVM_ZEXT_FH;
  return ZextBits;
}

static SimulatorIsaInfo deriveSimulatorIsaInfo(const RISCVSubtarget &ST) {
  return {ST.is64Bit(), deriveMisaBits(ST), deriveZextBits(ST)};
}

static void auxSimInit(const RISCVSubtarget &Subtarget,
                       SnippyRISCVSimulator &Simulator) {
  auto &LLSim = Simulator.getLLImpl();
  auto MSTATUS_CSR = LLSim.readCSRReg(RVM_CSR_MSTATUS);
  auto DEFAULT_MSTATUS = MSTATUS_CSR;
  // NOTE: our generator focuses on creating snippets that can be run in
  // user mode. Some extensions (like F), require some CSRs
  // to be programmed - this can be done only from privileged mode.
  // The code below ensures that appropriate CSRs are
  // set to a value that allows using instructions from such extenstions
  // without raising an interrupt (using "DPI" calls to simulator)
  if (Subtarget.hasStdExtV()) {
    // MSTATUS.VS = Initial
    MSTATUS_CSR |= (1 << RVM_MSTATUS_VS_FIELD_OFFSET);
  }
  if (Subtarget.hasStdExtF() || Subtarget.hasStdExtD()) {
    // MSTATUS.FS = Initial
    MSTATUS_CSR |= (1 << RVM_MSTATUS_FS_FIELD_OFFSET);
  }

  errs() << "NOTE: adjusting MSTATUS: 0x" << Twine::utohexstr(DEFAULT_MSTATUS)
         << "->" << Twine::utohexstr(MSTATUS_CSR) << "\n";

  LLSim.setCSRReg(RVM_CSR_MSTATUS, MSTATUS_CSR);
}

#define D_STRINGIFY(S) #S
#define D_XSTRINGIFY(S) D_STRINGIFY(S)
static constexpr const char *SimulatorEntryPointSymbol =
    D_XSTRINGIFY(RVMAPI_ENTRY_POINT_SYMBOL);
static constexpr const char *InterfaceVersionSymbol =
    D_XSTRINGIFY(RVMAPI_VERSION_SYMBOL);
#undef D_XSTRINGIFY
#undef D_STRINGIFY

static void checkModelInterfaceVersion(llvm::snippy::DynamicLibrary &ModelLib,
                                       unsigned ExpectedVersion) {
  const auto *InterfaceVersionAddress = reinterpret_cast<const unsigned char *>(
      ModelLib.getAddressOfSymbol(InterfaceVersionSymbol));
  if (!InterfaceVersionAddress)
    report_fatal_error("could figure out interface version of the model (" +
                           Twine(InterfaceVersionSymbol) + " symbol",
                       false);
  unsigned CurrentVersion = *InterfaceVersionAddress;
  if (CurrentVersion != ExpectedVersion)
    report_fatal_error("unexpected model interface version detected! Got " +
                           Twine(CurrentVersion) + ", expecting " +
                           Twine(ExpectedVersion),
                       false);
  return;
}

static const rvm::RVM_FunctionPointers &
getSimulatorEntryPoint(llvm::snippy::DynamicLibrary &ModelLib) {
  checkModelInterfaceVersion(ModelLib, RVMAPI_CURRENT_INTERFACE_VERSION);
  const auto *VTable = reinterpret_cast<const rvm::RVM_FunctionPointers *>(
      ModelLib.getAddressOfSymbol(SimulatorEntryPointSymbol));
  if (!VTable)
    report_fatal_error("could not find entry point <" +
                           Twine(SimulatorEntryPointSymbol) +
                           "> to create simulator",
                       false);
  return *VTable;
}

void MemUpdateCallback(RVMCallbackHandler *H, uint64_t Addr, const char *Data,
                       size_t Size) {
  assert(H);
  for (auto &&Observer : H->getObservers())
    Observer->memUpdateNotification(Addr, Data, Size);
}

void XRegUpdateCallback(RVMCallbackHandler *H, RVMXReg Reg, RVMRegT Value) {
  assert(H);
  for (auto &&Observer : H->getObservers())
    Observer->xregUpdateNotification(Reg, Value);
}

void FRegUpdateCallback(RVMCallbackHandler *H, RVMFReg Reg, RVMRegT Value) {
  assert(H);
  for (auto &&Observer : H->getObservers())
    Observer->fregUpdateNotification(Reg, Value);
}

void VRegUpdateCallback(RVMCallbackHandler *H, RVMVReg Reg, const char *Data,
                        size_t Size) {
  assert(H);
  for (auto &&Observer : H->getObservers())
    Observer->vregUpdateNotification(Reg, {Data, Size});
}

void PCUpdateCallback(RVMCallbackHandler *H, uint64_t PC) {
  assert(H);
  for (auto &&Observer : H->getObservers())
    Observer->PCUpdateNotification(PC);
}

unsigned getCustomExtensionsBits(const RISCVSubtarget &ST) {
  unsigned Res = 0;
  return Res;
}

std::unique_ptr<SimulatorInterface> createRISCVSimulator(
    llvm::snippy::DynamicLibrary &ModelLib, const SimulationConfig &Cfg,
    RVMCallbackHandler *CallbackHandler, const RISCVSubtarget &Subtarget,
    unsigned VLENB, bool EnableMisalignedAccess) {
  const auto &VTable = getSimulatorEntryPoint(ModelLib);

  auto RomStart =
      std::min(std::accumulate(Cfg.ProgSections.begin(), Cfg.ProgSections.end(),
                               std::numeric_limits<size_t>::max(),
                               [](auto Start, auto &Section) {
                                 return std::min(Start, Section.Start);
                               }),
               Cfg.RomStart);
  auto RomEnd = std::max(
      std::accumulate(Cfg.ProgSections.begin(), Cfg.ProgSections.end(), 0ul,
                      [](auto End, auto &Section) {
                        return std::max(End, Section.Start + Section.Size);
                      }),
      Cfg.RomStart + Cfg.RomSize);
  auto SimInfo = deriveSimulatorIsaInfo(Subtarget);
  LLVM_DEBUG(dbgs() << "Model::RV64 = " << SimInfo.Is64Bit << "\n");
  LLVM_DEBUG(dbgs() << "Model::MISA = 0x" << utohexstr(SimInfo.MisaBits)
                    << "\n");
  LLVM_DEBUG(dbgs() << "Model::Zext = 0x" << utohexstr(SimInfo.Zext) << "\n");

  auto StateBuilder = rvm::State::Builder(&VTable);

  if (EnableMisalignedAccess)
    StateBuilder.enableMisalignedAccess();

  StateBuilder.setRomStart(RomStart);
  StateBuilder.setRomSize(RomEnd - RomStart);

  StateBuilder.setRamStart(Cfg.RamStart);
  StateBuilder.setRamSize(Cfg.RamSize);

  if (SimInfo.Is64Bit)
    StateBuilder.setRV64Isa();
  else
    StateBuilder.setRV32Isa();

  StateBuilder.setMisa(SimInfo.MisaBits);
  StateBuilder.setZext(SimInfo.Zext);

  if (VLENB)
    StateBuilder.setVLEN(VLENB);

  StateBuilder.setLogPath(Cfg.TraceLogPath);

  if (CallbackHandler) {
    StateBuilder.registerCallbackHandler(CallbackHandler);
    StateBuilder.registerMemUpdateCallback(MemUpdateCallback);
    StateBuilder.registerXRegUpdateCallback(XRegUpdateCallback);
    StateBuilder.registerFRegUpdateCallback(FRegUpdateCallback);
    StateBuilder.registerVRegUpdateCallback(VRegUpdateCallback);
    StateBuilder.registerPCUpdateCallback(PCUpdateCallback);
  }

  StateBuilder.setXext(getCustomExtensionsBits(Subtarget));

  auto ModelState = StateBuilder.build();
  auto Sim = std::make_unique<SnippyRISCVSimulator>(std::move(ModelState));
  auxSimInit(Subtarget, *Sim);
  return Sim;
}

} // namespace snippy
} // namespace llvm
