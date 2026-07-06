//===------- AddMetadataSectionPass.cpp -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InitializePasses.h"

#include "snippy/AddMetadataSectionPass.h"
#include "snippy/Config/Config.h"
#include "snippy/Config/ConfigIOContext.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Support/YAMLUtils.h"
#include "snippy/Version/Version.inc"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/VCSRevision.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "snippy-add-metadata-section"
#define PASS_DESC "Add Metadata Section"

namespace llvm {
namespace snippy {
namespace {

class AddMetadataSection final : public ModulePass {
public:
  static char ID;

  AddMetadataSection() : ModulePass(ID) {}

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnModule(Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<GeneratorContextWrapper>();
    ModulePass::getAnalysisUsage(AU);
  }

private:
  std::string collectMetadataString() const;
  void createNoteSection(Module &M, StringRef NoteData) const;

  static void appendU32(raw_ostream &OS, uint32_t Value);
};

char AddMetadataSection::ID = 0;

std::string AddMetadataSection::collectMetadataString() const {
  std::string Info;

  raw_string_ostream OS(Info);

  OS << "Snippy Version: " << LLVM_SNIPPY_VERSION_STRING << "\n";
#if defined(LLVM_REVISION)
  OS << "Revision: " << LLVM_REVISION << "\n";
#else
  OS << "Revision: unknown\n";
#endif

  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &SnippyConfig = SGCtx.getConfig();
  auto &ProgCtx = SGCtx.getProgramContext();

  auto &State = ProgCtx.getLLVMState();
  auto &OpCC = ProgCtx.getOpcodeCache();
  auto RPW = ProgCtx.getRegisterPool();
  auto &ProgCfg = SnippyConfig.ProgramCfg;

  ConfigIOContext IOCtx{SnippyConfig.Histogram, OpCC, RPW, State
  };

  std::string ConfigStr;
  raw_string_ostream ConfigOS(ConfigStr);

  outputYAMLToStream(SnippyConfig, ConfigOS,
                     [&IOCtx](auto &IO) { IO.setContext(&IOCtx); });

  ConfigOS.flush();

  OS << "Seed: " << ProgCfg.Seed << "\n";
  OS << "Entry Point: " << ProgCfg.EntryPointName << "\n";
  OS << ConfigStr;

  OS.flush();
  return Info;
}

void AddMetadataSection::appendU32(raw_ostream &OS, uint32_t Value) {
  char Buffer[4];
  llvm::support::endian::write32le(Buffer, Value);
  OS.write(Buffer, sizeof(Buffer));
}

// Creates an ELF note section (.note.snippy) containing the provided metadata.
//
// ELF Note Section Format (per ELF specification):
//   - uint32_t NameSize: Size of note name field (including null terminator)
//   - uint32_t DescSize: Size of note descriptor field
//   - uint32_t Type: Note type (architecture-specific)
//   - uint8_t Name[NameSize + padding]: Note name string (null-terminated)
//   - uint8_t Desc[DescSize + padding]: Note descriptor data
//   - Both Name and Desc fields are padded to 4-byte boundaries
//
// This implementation adds the following metadata to the ELF file:
//   - Snippy version string (from Version.inc)
//   - LLVM revision (if available)
//   - Random seed used for generation
//   - Entry point name
//   - Complete YAML configuration (histogram, register pools, memory scheme,
//   etc.)
//
// The note section is readable by tools like llvm-objcopy and llvm-readelf for
// verification and debugging of generation parameters.
void AddMetadataSection::createNoteSection(Module &M,
                                           StringRef NoteData) const {
  auto &Ctx = M.getContext();
  auto &DL = M.getDataLayout();

  Align MaxAlign = DL.getABITypeAlign(Type::getInt8Ty(Ctx));

  std::string NoteBuffer;
  raw_string_ostream OS(NoteBuffer);

  const std::string NoteName = "Snippy";
  auto NameSize = static_cast<uint32_t>(NoteName.size());
  auto DescSize = static_cast<uint32_t>(NoteData.size());

  uint32_t NoteType = ELF::NT_ARCH;

  appendU32(OS, NameSize);
  appendU32(OS, DescSize);
  appendU32(OS, NoteType);

  OS << NoteName;
  OS << '\0';
  uint32_t NamePad = llvm::alignTo(NameSize, 4u) - NameSize;
  OS << std::string(NamePad, '\0');

  OS << NoteData;

  uint32_t DescPad = llvm::alignTo(DescSize, 4u) - DescSize;
  OS << std::string(DescPad, '\0');

  OS.flush();

  ArrayType *NoteTypeArray =
      ArrayType::get(Type::getInt8Ty(Ctx), NoteBuffer.size());

  auto *NoteGV =
      new GlobalVariable(M, NoteTypeArray, true, GlobalValue::PrivateLinkage,
                         ConstantDataArray::getString(Ctx, NoteBuffer, false),
                         "__snippy_metadata.note");

  NoteGV->setSection(".note.snippy");
  NoteGV->setAlignment(MaxAlign);
  NoteGV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
}

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::AddMetadataSection;

INITIALIZE_PASS_BEGIN(AddMetadataSection, DEBUG_TYPE, PASS_DESC, false, true)
INITIALIZE_PASS_DEPENDENCY(GeneratorContextWrapper)
INITIALIZE_PASS_END(AddMetadataSection, DEBUG_TYPE, PASS_DESC, false, true)

namespace llvm {

ModulePass *createAddMetadataSectionPass() { return new AddMetadataSection; }

namespace snippy {

bool AddMetadataSection::runOnModule(Module &M) {
  // Check if note section already exists to avoid duplicates
  if (M.getGlobalVariable("__snippy_metadata.note"))
    return false;

  std::string MetadataStr = collectMetadataString();

  LLVM_DEBUG(dbgs() << "Adding metadata section with content:\n"
                    << MetadataStr << "\n");

  createNoteSection(M, MetadataStr);

  return true;
}

} // namespace snippy
} // namespace llvm
