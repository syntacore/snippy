//===-- Linker.cpp ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/Linker.h"
#include "snippy/Generator/GlobalsPool.h"
#include "snippy/Support/Options.h"
#include "snippy/Support/Utils.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

namespace llvm {
namespace snippy {

extern cl::OptionCategory Options;

static snippy::opt<std::string>
    LLDDirOpt("lld-dir",
              cl::desc("Path to ld.lld directory being used for linking. "
                       "Defaulting to directory of llvm-snippy installation."),
              cl::cat(Options), cl::init(""));

static snippy::opt<bool> DumpPHDRSDef(
    "enable-phdrs-definition",
    cl::desc("If enabled, snippy will define all programm headers in linker "
             "script that are used by sections in layout."
             "Note: when disabled section are still placed to specified "
             "segments, however user must provide definitions of"
             "that segments themself."),
    cl::cat(Options), cl::init(false));

static snippy::opt<bool>
    LinkerUseHex("linker-use-hex",
                 cl::desc("Make linker script use hexadecimal numbers in "
                          "linker script for better readability"),
                 cl::cat(Options), cl::init(false));

namespace {

using FilePathT = SmallString<20>;

static void checkError(const std::error_code &ECode, StringRef What = "") {
  if (Error E = errorCodeToError(ECode))
    snippy::fatal(Twine(What) + ": " + ECode.message());
}

static std::string link(StringRef LLD, StringRef LinkerScript, bool Relocatable,
                        std::vector<FilePathT> ObjectFilesPaths,
                        bool DisableRelaxations, bool Shared) {
  int FD;
  FilePathT OutPath;

  checkError(sys::fs::createTemporaryFile("tmp-out", "elf", FD, OutPath),
             "Failed to create temporary file");

  auto LLDCommands = std::vector<StringRef>{LLD};
  std::copy(ObjectFilesPaths.begin(), ObjectFilesPaths.end(),
            std::back_inserter(LLDCommands));
  assert(!Relocatable || !Shared);
  if (Relocatable)
    LLDCommands.push_back("-r");
  if (Shared)
    LLDCommands.push_back("-shared");

  LLDCommands.insert(LLDCommands.end(),
                     {"-o", OutPath, "--script", LinkerScript});
  if (DisableRelaxations)
    LLDCommands.push_back("--no-relax");

  auto RetCode = sys::ExecuteAndWait(LLD, LLDCommands);
  if (RetCode)
    snippy::fatal("lld returned non-zero status");

  uint64_t OutSize;
  checkError(sys::fs::file_size(OutPath, OutSize),
             "Could not read temporary file size");

  constexpr auto MAX_SUPPORTED_SIZE = 1ull << 32;
  if (OutSize > MAX_SUPPORTED_SIZE)
    snippy::fatal("file size > 4Gb not supported");

  checkError(sys::fs::openFileForRead(OutPath, FD),
             "Could not open temporary file");

  auto NH = sys::fs::convertFDToNativeFile(FD);
  std::string Ret;
  Ret.resize(OutSize + 1);
  auto ExpRead = sys::fs::readNativeFile(NH, {Ret.data(), Ret.size()});
  if (!ExpRead || ExpRead.get() != OutSize)
    snippy::fatal("Failed on temporary file read");

  sys::fs::remove(OutPath);
  return Ret;
}

// Creates temporary file with unique name and writes Data in it.
// Filename is returned on success.
static auto writeDataToDisk(StringRef Data) {

  FilePathT FilePath;
  int FD;

  checkError(sys::fs::createTemporaryFile("", "tmp", FD, FilePath),
             "Could not create temporary file");

  raw_fd_ostream Stream(FD, true /*ShouldClose*/);
  Stream.write(Data.data(), Data.size());
  if (Stream.has_error())
    checkError(Stream.error(), "Failed on file write");

  return FilePath;
}

static StringRef findLLD() {
  static SmallString<128> Found;

  if (Found.empty()) {
    decltype(Found) LLDDir;
    auto PathSpecified = !LLDDirOpt.getValue().empty();
    if (!PathSpecified) {
      static int Dummy = 0;
      const char *ptr = nullptr;
      auto SnippyExec = sys::fs::getMainExecutable(ptr, &Dummy);
      auto ParentDir = sys::path::parent_path(SnippyExec);
      LLDDir = ParentDir;
    } else {
      LLDDir = LLDDirOpt.getValue();
    }

    auto LLDExeE = sys::findProgramByName("ld.lld", {LLDDir});
    if (!LLDExeE) {
      snippy::fatal(
          Twine("Could not find ld.lld: ") +
          (PathSpecified
               ? StringRef("Please, specify it's path via "
                           "--lld-path=<path to ld.lld> or copy it to "
                           "llvm-snippy installation directory.")
               : StringRef("Could not find it in the specified path.")));
    }
    Found = LLDExeE.get();
  }

  return Found;
}

static std::string getUniqueSectionName(const SectionDesc &Desc) {
  std::string Buf;
  llvm::raw_string_ostream StringStream{Buf};
  StringStream << "." << Desc.getIDString() << ".";
  Desc.M.dump(StringStream);

  return Buf;
}

template <typename SecT, typename PredT>
static auto findFirstSection(SecT &&Sections, PredT &&Pred) {
  return std::find_if(Sections.begin(), Sections.end(),
                      [&Pred](auto &&Section) {
                        return std::invoke(Pred, Section.OutputSection.Desc);
                      });
}

} // namespace

Linker::NamedOutputSection::NamedOutputSection(const SectionDesc &Desc)
    : Desc(Desc), Name(getUniqueSectionName(Desc)) {}

void Linker::calculateMemoryRegion() {
  if (Sections.empty())
    return;
  MemoryRegion.first = Sections.front().OutputSection.Desc.VMA;
  MemoryRegion.second = Sections.back().OutputSection.Desc.VMA +
                        Sections.back().OutputSection.Desc.Size;
}

Linker::Linker(LLVMContext &Ctx, const SectionsDescriptions &Sects,
               StringRef MN)
    : MangleName(MN), MemoryRegion{0, 0} {
  std::transform(Sects.begin(), Sects.end(), std::back_inserter(Sections),
                 [](auto &SectionDesc) {
                   return SectionEntry{NamedOutputSection{SectionDesc}, {}};
                 });
  std::sort(Sections.begin(), Sections.end(), [](auto &LSE, auto &RSE) {
    return LSE.OutputSection.Desc.VMA < RSE.OutputSection.Desc.VMA;
  });
  calculateMemoryRegion();
  auto DefaultCodeSection = findFirstSection(
      Sections, [](const SectionDesc &Desc) { return Desc.M.X(); });

  if (DefaultCodeSection != Sections.end())
    DefaultCodeSection->InputSections.push_back({kDefaultTextSectionName});
  auto ROMSection = findFirstSection(Sections, [](const SectionDesc &S) {
    return S.M.R() && !S.M.W() && !S.M.X();
  });

  if (ROMSection != Sections.end())
    ROMSection->InputSections.push_back({kDefaultRODataSectionName});
}

bool Linker::LinkedSections::hasOutputSectionFor(StringRef SectionName) const {
  return std::any_of(begin(), end(), [&SectionName](auto &Section) {
    return llvm::any_of(Section.InputSections, [&SectionName](auto &&S) {
      return S.Name == SectionName;
    });
  });
}

const Linker::NamedOutputSection &
Linker::LinkedSections::getOutputSectionFor(StringRef SectionName) const {
  auto S = std::find_if(begin(), end(), [SectionName](auto &Section) {
    return llvm::any_of(Section.InputSections, [SectionName](auto &S) {
      return S.Name == SectionName;
    });
  });
  assert(S != end());
  return S->OutputSection;
}

std::string
Linker::LinkedSections::getOutputNameForDesc(const SectionDesc &Desc) const {
  auto Entry = getOutputSectionImpl(Desc);
  assert(Entry != end() && "Got unknown section");
  assert(!Entry->OutputSection.Name.empty());
  return Entry->OutputSection.Name;
}

void Linker::LinkedSections::addInputSectionFor(const SectionDesc &Desc,
                                                StringRef InSectName) {
  auto SectIt = getOutputSectionImpl(Desc);
  assert(SectIt != end() &&
         "Can't find current section in the output sections");
  SectIt->InputSections.push_back({InSectName.str()});
}

std::string Linker::getMangledName(StringRef SectionName) const {
  return (".snippy" + Twine(MangleName.empty() ? "" : ".") + MangleName +
          SectionName)
      .str();
}

std::string Linker::getMangledFunctionName(StringRef FuncName) const {
  return ("__" + MangleName + Twine(MangleName.empty() ? "" : "_") + FuncName)
      .str();
}

std::vector<std::string> Linker::collectPhdrInfo() const {
  std::unordered_set<std::string> Phdrs;
  for (auto &&SE : Sections) {
    if (!SE.OutputSection.Desc.hasPhdr())
      continue;
    Phdrs.emplace(SE.OutputSection.Desc.getPhdr());
  }
  std::vector<std::string> Ret;
  std::copy(Phdrs.begin(), Phdrs.end(), std::back_inserter(Ret));
  return Ret;
}

static std::string utostr(uint64_t Val) {
  if (LinkerUseHex)
    return "0x" + llvm::utohexstr(Val);
  return std::to_string(Val);
}

Expected<std::string> Linker::createLinkerScriptImpl(bool Shared) const {
  std::string ScriptText;
  llvm::raw_string_ostream STS{ScriptText};

  auto ConvertToPhdrFlags = [](const auto &Mask) {
    auto MInt = static_cast<int>(Mask);
    // Convert snippy access flag into elf's segment flag:
    // 012    012
    // RWX -> XWR
    return ((MInt & 0x1) << 2u) | (MInt & 0x2) | ((MInt & 0x4) >> 2u);
  };
  auto FindExtraAvailableAddressRegion =
      [](auto &&MainRegion,
         size_t Size) -> Expected<std::pair<size_t, size_t>> {
    using AddrT = decltype(MainRegion.first);
    auto MakeNoSpaceError = []() {
      return makeFailure(Errc::InvalidConfiguration,
                         "failed to allocate extra space for dynamic sections "
                         "- please reconfigure sections");
    };
    if (MainRegion.first == 0x0 &&
        MainRegion.second == std::numeric_limits<AddrT>::max()) {
      return MakeNoSpaceError();
    }
    if (std::numeric_limits<AddrT>::max() - MainRegion.second >= Size)
      return std::make_pair(MainRegion.second, MainRegion.second + Size);
    if (MainRegion.first >= Size)
      return std::make_pair(MainRegion.first - Size, MainRegion.first);
    return MakeNoSpaceError();
  };
  STS << "MEMORY {\n";
  for (auto &&I : iota_range(1u, 8u, /* Inclusive */ false)) {
    auto AccStr = AccMask(I).toString();
    STS << "  RAM_" << AccStr << " (" << AccStr
        << ") : ORIGIN = " << utostr(MemoryRegion.first)
        << ", LENGTH = " << utostr(MemoryRegion.second - MemoryRegion.first);
  }
  if (Shared) {
    auto ExtraRegion =
        FindExtraAvailableAddressRegion(MemoryRegion, /* Size */ 0x10000);
    if (ExtraRegion)
      STS << " EXTRA_RAM (rwx) : ORIGIN = " << utostr(ExtraRegion->first)
          << ", LENGTH = " << utostr(ExtraRegion->second - ExtraRegion->first);
    else
      return ExtraRegion.takeError();
  }
  STS << "}\n\n";
  STS << "PHDRS\n";
  STS << "{\n";
  for (auto &SE : Sections) {
    auto OutSectionName = getMangledName(SE.OutputSection.Name);
    STS << OutSectionName << " PT_LOAD FLAGS ("
        << ConvertToPhdrFlags(SE.OutputSection.Desc.M) << ");\n";
  }
  if (Shared) {
    STS << "EXTRA_SEG PT_LOAD ;\n";
    STS << "EXTRA_SEG_DYN PT_DYNAMIC ;\n";
  }
  STS << "}\n";

  STS << "SECTIONS {\n";
  for (auto &SE : Sections) {
    auto OutSectionName = getMangledName(SE.OutputSection.Name);

    STS << "  " << OutSectionName << " " << utostr(SE.OutputSection.Desc.VMA);

    if (SE.InputSections.empty())
      STS << " (NOLOAD) ";

    STS << ": {\n";
    if (SE.InputSections.empty()) {
      STS << "  PROVIDE(" << OutSectionName << "_start_ = .);\n";
      STS << "  . +=" << utostr(SE.OutputSection.Desc.Size) << ";\n";
      STS << "  PROVIDE(" << OutSectionName << "_end_ = .);\n";
    } else {
      for (auto &&InputSection : SE.InputSections) {
        STS << "  KEEP(*(" << InputSection.Name << "))\n";
      }
    }

    STS << "} >RAM_" << SE.OutputSection.Desc.M.toString() << " :"
        << OutSectionName;
    STS << "\n";
  }
  // These sections are impliciltly added if shared object is linked.
  if (Shared) {
    constexpr std::array<const char *, 4> ExtraSections{".dynsym", ".hash",
                                                        ".gnu.hash", ".dynstr"};
    for (auto &&ExtraSec : ExtraSections) {
      STS << "  " << ExtraSec << " : {\n";
      STS << "  *(" << ExtraSec << ")\n";
      STS << "  } >EXTRA_RAM :EXTRA_SEG\n";
    }
    // .dynamic section is special - it must be put in DYNAMIC segment.
    STS << "  .dynamic : {\n";
    STS << "  *(.dynamic)\n";
    STS << "  } >EXTRA_RAM :EXTRA_SEG_DYN\n";
  }
  STS << "}\n";
  return ScriptText;
}

std::string Linker::createLinkerScriptImplLegacy(bool Export) const {
  std::string ScriptText;
  llvm::raw_string_ostream STS{ScriptText};
  std::string MemoryRegionName =
      MangleName.empty() ? "SNIPPY" : "SNIPPY_" + MangleName;
  STS << "MEMORY {\n"
      << "  " << MemoryRegionName
      << " (rwx) : ORIGIN = " << utostr(MemoryRegion.first)
      << ", LENGTH = " << utostr(MemoryRegion.second - MemoryRegion.first)
      << "\n";

  STS << "}\n";
  auto Phdrs = collectPhdrInfo();
  if (!Phdrs.empty() && (!Export || DumpPHDRSDef)) {
    STS << "PHDRS\n";
    STS << "{\n";
    for (auto &&Header : Phdrs) {
      STS << Header << " PT_LOAD ;\n";
    }
    STS << "}\n";
  }
  STS << "SECTIONS {\n";
  for (auto &SE : Sections) {
    auto OutSectionName = getMangledName(SE.OutputSection.Name);

    STS << "  " << OutSectionName << " " << utostr(SE.OutputSection.Desc.VMA);

    if (SE.InputSections.empty())
      STS << " (NOLOAD) ";

    STS << ": {\n";
    if (Export) {
      STS << "  KEEP(*(" << OutSectionName << "))\n";
    } else {
      if (SE.InputSections.empty()) {
        STS << "  PROVIDE(" << OutSectionName << "_start_ = .);\n";
        STS << "  . +=" << utostr(SE.OutputSection.Desc.Size) << ";\n";
        STS << "  PROVIDE(" << OutSectionName << "_end_ = .);\n";
      } else {
        for (auto &&InputSection : SE.InputSections) {
          STS << "  KEEP(*(" << InputSection.Name << "))\n";
        }
      }
    }

    STS << "} >" << MemoryRegionName;
    if (SE.OutputSection.Desc.hasPhdr()) {
      STS << " :" << SE.OutputSection.Desc.getPhdr();
    }
    STS << "\n";
  }

  STS << "}";
  return ScriptText;
}

std::string Linker::generateLinkerScript() const {
  return createLinkerScriptImplLegacy(/* Export */ true);
}
Expected<std::string> Linker::run(ObjectFilesList ObjectFilesToLink,
                                  bool Shared) const {
  assert(ObjectFilesToLink.size() > 0 && "Linker needs at least one image");
  auto ObjectFilesPaths = std::vector<FilePathT>{};
  for (auto &Objectfile : ObjectFilesToLink)
    ObjectFilesPaths.push_back(writeDataToDisk(Objectfile));
  auto EInternalLinkerScript = createLinkerScriptImpl(Shared);
  if (!EInternalLinkerScript)
    return EInternalLinkerScript.takeError();
  auto LinkerScriptPath = writeDataToDisk(*EInternalLinkerScript);

  auto LLDExe = findLLD();
  auto FinalImage = link(LLDExe, LinkerScriptPath, /* Relocatable */ false,
                         ObjectFilesPaths, /* NoRelax */ true, Shared);

  sys::fs::remove(LinkerScriptPath);
  std::for_each(ObjectFilesPaths.begin(), ObjectFilesPaths.end(),
                [](const FilePathT &ImagePath) { sys::fs::remove(ImagePath); });

  return FinalImage;
}
std::string Linker::runLegacy(ObjectFilesList ObjectFilesToLink,
                              bool Relocatable, bool DisableRelaxations) const {
  assert(ObjectFilesToLink.size() > 0 && "Linker needs at least one image");
  auto ObjectFilesPaths = std::vector<FilePathT>{};
  for (auto &Objectfile : ObjectFilesToLink)
    ObjectFilesPaths.push_back(writeDataToDisk(Objectfile));
  auto InternalLinkerScript = createLinkerScriptImplLegacy(/*Export*/ false);
  auto LinkerScriptPath = writeDataToDisk(InternalLinkerScript);

  auto LLDExe = findLLD();
  auto FinalImage =
      link(LLDExe, LinkerScriptPath, Relocatable, ObjectFilesPaths,
           DisableRelaxations, /* shared */ false);

  sys::fs::remove(LinkerScriptPath);
  std::for_each(ObjectFilesPaths.begin(), ObjectFilesPaths.end(),
                [](const FilePathT &ImagePath) { sys::fs::remove(ImagePath); });

  return FinalImage;
}

StringRef Linker::getExitSymbolName() { return "__snippy_exit"; }

} // namespace snippy
} // namespace llvm
