//===-- RISCVSimulator.cpp --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <string>

#include <RISCVModel/RVM.hpp>

#include "snippy/Config/RegisterHistogram.h"
#include "snippy/Simulator/Targets/RISCV.h"
#include "snippy/Simulator/Transactions.h"
#include "snippy/Support/DiagnosticInfo.h"
#include "snippy/Support/DynLibLoader.h"
#include "snippy/Support/Options.h"
#include "snippy/Support/RandUtil.h"

#include "Common.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#define DEBUG_TYPE "snippy-riscv-sim"

namespace llvm {

#define GET_REGISTER_MATCHER
#include "RISCVGenAsmMatcher.inc"
#undef GET_REGISTER_MATCHER

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
    snippy::fatal("Too many programm registers are specified "
                  "in the reg state YAML");
  return true;
}

void RISCVRegisterState::loadFromYamlFile(StringRef YamlFile,
                                          WarningsT &WarningsArr,
                                          const SnippyTarget *Tgt) {
  auto RH = loadRegistersFromYaml(YamlFile);

  constexpr auto DefaultPC = 0ul;
  PC = DefaultPC;
  if (hasPCInRH(RH))
    WarningsArr.emplace_back("PC value from YAML has been ignored");

  std::array<StringRef, 4> AllowedRegClasses = {"X", "F", "V", "P"};
  checkRegisterClasses(RH.Registers, AllowedRegClasses, Tgt);
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

void RISCVRegisterState::saveAsYAMLFile(raw_ostream &OS) const {
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
  auto RegSerObj = AllRegisters()
                       .addRegisterGroup("X", XRegsMasked)
                       .addRegisterGroup("F", FRegsMasked)
                       .addRegisterGroup("V", VLEN, VRegs);
  if (!DumpRegsWithoutPC)
    RegSerObj.addRegisterGroup("P", PC);
  RegSerObj.saveAsYAML(OS);
}

bool RISCVRegisterState::operator==(const IRegisterState &Another) const {
  auto &Casted = static_cast<const RISCVRegisterState &>(Another);

  return XRegs == Casted.XRegs && FRegs == Casted.FRegs &&
         VRegs == Casted.VRegs;
}

RegSizeInBytes RISCVRegisterState::getRegSizeInBytes(Register Reg,
                                                     unsigned XLen,
                                                     unsigned VLEN) {
  return static_cast<RegSizeInBytes>(
      getRegBitWidth(Reg, XLen * RISCV_CHAR_BIT, VLEN) / RISCV_CHAR_BIT);
}

uint64_t RISCVRegisterState::getMaxRegValueForSize(RegSizeInBytes Size) {
  if (Size == RegSizeInBytes::Reg4Bytes)
    return std::numeric_limits<uint32_t>::max();
  return std::numeric_limits<uint64_t>::max();
}

uint64_t RISCVRegisterState::getMaxRegValueForSize(Register Reg, unsigned XLen,
                                                   unsigned VLEN) {
  auto SizeInBytes = getRegSizeInBytes(Reg, XLen, VLEN);
  return getMaxRegValueForSize(SizeInBytes);
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
    auto ExecRes = ModelState.executeInstr();
    switch (ExecRes) {
    case MODEL_SUCCESS:
      return ExecutionResult::Success;
    case MODEL_EXCEPTION:
      return ExecutionResult::FatalError;
    case MODEL_FINISH:
      return ExecutionResult::SimulationExit;
    }

    llvm_unreachable("unknown model result of instruction execution");
  }

  void setStopModeByPC(ProgramCounterType PC) override {
    ModelState.setStopMode(STOP_BY_PC);
    ModelState.setStopPC(PC);
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
    snippy::fatal("Impossible to read the register from the "
                  "Simulator. Invalid RegID for the RISCV target");
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
    snippy::fatal("Impossible to set the register from the "
                  "Simulator. Invalid RegID for the RISCV target");
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

  void dumpSystemRegistersState(raw_ostream &OS) const override {
    OS << "MEPC:   0x" << utohexstr(ModelState.readCSRReg(RVM_CSR_MEPC))
       << "\n";
    OS << "MCAUSE: 0x" << utohexstr(ModelState.readCSRReg(RVM_CSR_MCAUSE))
       << "\n";
    OS << "MTVAL:  0x" << utohexstr(ModelState.readCSRReg(RVM_CSR_MTVAL))
       << "\n";
  }

  bool supportsCallbacks() const override {
    return ModelState.getVTable()->queryCallbackSupportPresent();
  }
};

struct SimulatorIsaInfo {
  bool Is64Bit;
  RVMExtDescriptor Ext;
};

// TODO: auto-generate this?
static void addMisaBits(RVMExtDescriptor &Ext,
                        const RISCVSubtarget &Subtarget) {
  auto &MisaBits = Ext.MisaExt;
  if (Subtarget.hasStdExtA())
    MisaBits[RVM_MISA_A] = true;
  if (Subtarget.hasStdExtC())
    MisaBits[RVM_MISA_C] = true;
  if (Subtarget.hasStdExtD())
    MisaBits[RVM_MISA_D] = true;
  if (Subtarget.hasStdExtF())
    MisaBits[RVM_MISA_F] = true;
  if (Subtarget.hasStdExtM())
    MisaBits[RVM_MISA_M] = true;
  if (Subtarget.hasStdExtV())
    MisaBits[RVM_MISA_V] = true;
}

// TODO: auto-generate this?
static void addZextBits(RVMExtDescriptor &Ext,
                        const RISCVSubtarget &Subtarget) {
  auto &ZextBits = Ext.ZExt;
  if (Subtarget.hasStdExtZicsr())
    ZextBits[RVM_ZEXT_ICSR] = true;
  if (Subtarget.hasStdExtZifencei())
    ZextBits[RVM_ZEXT_IFENCEI] = true;
  if (Subtarget.hasStdExtZicond())
    ZextBits[RVM_ZEXT_ICOND] = true;
  if (Subtarget.hasStdExtZicbom())
    ZextBits[RVM_ZEXT_ICBOM] = true;
  if (Subtarget.hasStdExtZicboz())
    ZextBits[RVM_ZEXT_ICBOZ] = true;
  if (Subtarget.hasStdExtZicntr())
    ZextBits[RVM_ZEXT_ICNTR] = true;
  if (Subtarget.hasStdExtZicbop())
    ZextBits[RVM_ZEXT_ICBOP] = true;
  if (Subtarget.hasStdExtZimop())
    ZextBits[RVM_ZEXT_IMOP] = true;
  if (Subtarget.hasStdExtZihpm())
    ZextBits[RVM_ZEXT_IHPM] = true;
  if (Subtarget.hasStdExtZihintntl())
    ZextBits[RVM_ZEXT_IHINTNTL] = true;
  if (Subtarget.hasStdExtZihintpause())
    ZextBits[RVM_ZEXT_IHINTPAUSE] = true;
  if (Subtarget.hasStdExtZicfiss())
    ZextBits[RVM_ZEXT_ICFISS] = true;
  if (Subtarget.hasStdExtZicfilp())
    ZextBits[RVM_ZEXT_ICFILP] = true;
  if (Subtarget.hasStdExtZaamo())
    ZextBits[RVM_ZEXT_AAMO] = true;
  if (Subtarget.hasStdExtZabha())
    ZextBits[RVM_ZEXT_ABHA] = true;
  if (Subtarget.hasStdExtZacas())
    ZextBits[RVM_ZEXT_ACAS] = true;
  if (Subtarget.hasStdExtZcmp())
    ZextBits[RVM_ZEXT_CMP] = true;
  if (Subtarget.hasStdExtZcmt())
    ZextBits[RVM_ZEXT_CMT] = true;
  if (Subtarget.hasStdExtZfa())
    ZextBits[RVM_ZEXT_FA] = true;
  if (Subtarget.hasStdExtZfbfmin())
    ZextBits[RVM_ZEXT_FBFMIN] = true;
  if (Subtarget.hasStdExtZfinx())
    ZextBits[RVM_ZEXT_FINX] = true;
  if (Subtarget.hasStdExtZdinx())
    ZextBits[RVM_ZEXT_DINX] = true;
  if (Subtarget.hasStdExtZhinx())
    ZextBits[RVM_ZEXT_HINX] = true;
  if (Subtarget.hasStdExtZhinxmin())
    ZextBits[RVM_ZEXT_HINXMIN] = true;
  if (Subtarget.hasStdExtZba())
    ZextBits[RVM_ZEXT_BA] = true;
  if (Subtarget.hasStdExtZbb())
    ZextBits[RVM_ZEXT_BB] = true;
  if (Subtarget.hasStdExtZbc())
    ZextBits[RVM_ZEXT_BC] = true;
  if (Subtarget.hasStdExtZbs())
    ZextBits[RVM_ZEXT_BS] = true;
  if (Subtarget.hasStdExtZfh())
    ZextBits[RVM_ZEXT_FH] = true;
  if (Subtarget.hasStdExtZfhmin())
    ZextBits[RVM_ZEXT_FHMIN] = true;
  if (Subtarget.hasStdExtZbkb())
    ZextBits[RVM_ZEXT_BKB] = true;
  if (Subtarget.hasStdExtZbkc())
    ZextBits[RVM_ZEXT_BKC] = true;
  if (Subtarget.hasStdExtZbkx())
    ZextBits[RVM_ZEXT_BKX] = true;
  if (Subtarget.hasStdExtZknd())
    ZextBits[RVM_ZEXT_KND] = true;
  if (Subtarget.hasStdExtZkne())
    ZextBits[RVM_ZEXT_KNE] = true;
  if (Subtarget.hasStdExtZknh())
    ZextBits[RVM_ZEXT_KNH] = true;
  if (Subtarget.hasStdExtZksed())
    ZextBits[RVM_ZEXT_KSED] = true;
  if (Subtarget.hasStdExtZksh())
    ZextBits[RVM_ZEXT_KSH] = true;
  if (Subtarget.hasStdExtZkr())
    ZextBits[RVM_ZEXT_KR] = true;
  if (Subtarget.hasStdExtZkn())
    ZextBits[RVM_ZEXT_KN] = true;
  if (Subtarget.hasStdExtZks())
    ZextBits[RVM_ZEXT_KS] = true;
  if (Subtarget.hasStdExtZk())
    ZextBits[RVM_ZEXT_K] = true;
  if (Subtarget.hasStdExtZkt())
    ZextBits[RVM_ZEXT_KT] = true;
  if (Subtarget.hasStdExtZvbb())
    ZextBits[RVM_ZEXT_VBB] = true;
  if (Subtarget.hasStdExtZvbc())
    ZextBits[RVM_ZEXT_VBC] = true;
  // zvkb is subset of zvbb. Do not set this flag
  // when zvbb is already set.
  if (Subtarget.hasStdExtZvkb() && !Subtarget.hasStdExtZvbb())
    ZextBits[RVM_ZEXT_VKB] = true;
  if (Subtarget.hasStdExtZvfbfmin())
    ZextBits[RVM_ZEXT_VFBFMIN] = true;
  if (Subtarget.hasStdExtZvfbfwma())
    ZextBits[RVM_ZEXT_VFBFWMA] = true;
  if (Subtarget.hasStdExtZvfh())
    ZextBits[RVM_ZEXT_VFH] = true;
  if (Subtarget.hasStdExtZvfhmin())
    ZextBits[RVM_ZEXT_VFHMIN] = true;
}

static void addXextBits(RVMExtDescriptor &Ext, const RISCVSubtarget &ST) {
  [[maybe_unused]] auto &XExt = Ext.XExt;
}

static SimulatorIsaInfo deriveSimulatorIsaInfo(const RISCVSubtarget &ST) {
  SimulatorIsaInfo IsaInfo = {};
  IsaInfo.Ext.ZExtSize = sizeof(IsaInfo.Ext.ZExt);
  IsaInfo.Ext.XExtSize = sizeof(IsaInfo.Ext.XExt);
  addMisaBits(IsaInfo.Ext, ST);
  addZextBits(IsaInfo.Ext, ST);
  addXextBits(IsaInfo.Ext, ST);
  IsaInfo.Is64Bit = ST.is64Bit();
  return IsaInfo;
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
    snippy::fatal(
        formatv("could figure out interface version of the model ({0}) symbol",
                InterfaceVersionSymbol));
  unsigned CurrentVersion = *InterfaceVersionAddress;
  if (CurrentVersion != ExpectedVersion)
    snippy::fatal(formatv(
        "unexpected model interface version detected! Got {0}, expecting {1}",
        CurrentVersion, ExpectedVersion));
  return;
}

static const rvm::RVM_FunctionPointers &
getSimulatorEntryPoint(llvm::snippy::DynamicLibrary &ModelLib) {
  checkModelInterfaceVersion(ModelLib, RVMAPI_CURRENT_INTERFACE_VERSION);
  const auto *VTable = reinterpret_cast<const rvm::RVM_FunctionPointers *>(
      ModelLib.getAddressOfSymbol(SimulatorEntryPointSymbol));
  if (!VTable)
    snippy::fatal(
        formatv("could not find entry point <{0}> to create simulator",
                SimulatorEntryPointSymbol));
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

std::unique_ptr<SimulatorInterface> createRISCVSimulator(
    llvm::snippy::DynamicLibrary &ModelLib, const SimulationConfig &Cfg,
    RVMCallbackHandler *CallbackHandler, const RISCVSubtarget &Subtarget,
    unsigned VLENB, bool EnableMisalignedAccess) {
  const auto &VTable = getSimulatorEntryPoint(ModelLib);

  auto SimInfo = deriveSimulatorIsaInfo(Subtarget);
  LLVM_DEBUG(dbgs() << "Model::isa_string: "
                    << rvm::create_isa_string(SimInfo.Ext, SimInfo.Is64Bit)
                    << "\n");

  auto StateBuilder = rvm::State::Builder(&VTable);

  if (EnableMisalignedAccess)
    StateBuilder.enableMisalignedAccess();

  // For legacy reasons, we allow to configure zero-size
  // regions. However model implementation is allowed to raise
  // an error for such regions, so filter them out here.
  auto NonEmptyRegions = llvm::make_filter_range(
      Cfg.MemoryRegions, [](auto &&Region) { return Region.Size; });
  for (auto &&Region : NonEmptyRegions)
    StateBuilder.addMemoryRegion(Region.Start, Region.Size,
                                 Region.Name.c_str());

  if (SimInfo.Is64Bit)
    StateBuilder.setRV64Isa();
  else
    StateBuilder.setRV32Isa();

  StateBuilder.setExtensions(SimInfo.Ext);

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

  auto ModelState = StateBuilder.build();
  auto Sim = std::make_unique<SnippyRISCVSimulator>(std::move(ModelState));
  auxSimInit(Subtarget, *Sim);
  return Sim;
}

} // namespace snippy
} // namespace llvm
