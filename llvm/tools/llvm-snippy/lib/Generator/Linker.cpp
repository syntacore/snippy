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

namespace {

using FilePathT = SmallString<20>;

static void checkError(const std::error_code &ECode, StringRef What = "") {
  if (Error E = errorCodeToError(ECode))
    report_fatal_error(Twine(What) + ": " + ECode.message(), false);
}

static std::string link(StringRef LLD, StringRef LinkerScript, bool Relocatable,
                        std::vector<FilePathT> ObjectFilesPaths) {
  int FD;
  FilePathT OutPath;

  checkError(sys::fs::createTemporaryFile("tmp-out", "elf", FD, OutPath),
             "Failed to create temporary file");

  auto LLDCommands = std::vector<StringRef>{LLD};
  std::copy(ObjectFilesPaths.begin(), ObjectFilesPaths.end(),
            std::back_inserter(LLDCommands));
  if (Relocatable)
    LLDCommands.push_back("-r");

  LLDCommands.insert(LLDCommands.end(),
                     {"-o", OutPath, "--script", LinkerScript});

  auto RetCode = sys::ExecuteAndWait(LLD, LLDCommands);
  if (RetCode)
    report_fatal_error("lld returned non-zero status", false);

  uint64_t OutSize;
  checkError(sys::fs::file_size(OutPath, OutSize),
             "Could not read temporary file size");

  constexpr auto MAX_SUPPORTED_SIZE = 1ull << 32;
  if (OutSize > MAX_SUPPORTED_SIZE)
    report_fatal_error("file size > 4Gb not supported");

  checkError(sys::fs::openFileForRead(OutPath, FD),
             "Could not open temporary file");

  auto NH = sys::fs::convertFDToNativeFile(FD);
  std::string Ret;
  Ret.resize(OutSize + 1);
  auto ExpRead = sys::fs::readNativeFile(NH, {Ret.data(), Ret.size()});
  if (!ExpRead || ExpRead.get() != OutSize)
    report_fatal_error("Failed on temporary file read", false);

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
      report_fatal_error(
          Twine("Could not find ld.lld: ") +
              (PathSpecified
                   ? StringRef("Please, specify it's path via "
                               "--lld-path=<path to ld.lld> or copy it to "
                               "llvm-snippy installation directory.")
                   : StringRef("Could not find it in the specified path.")),
          false);
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

static void reportSectionInterfereError(const Linker::OutputSectionT &Added,
                                        const Linker::OutputSectionT &Existing,
                                        StringRef What) {
  std::string MessageBuf;
  raw_string_ostream SS{MessageBuf};
  SS << "Extra added section " << Added.Name << ":\n" << Added.Desc;
  SS << What << " with existing section " << Existing.Name << ":\n"
     << Existing.Desc;
  report_fatal_error(MessageBuf.c_str(), false);
}

template <typename SecT, typename PredT>
static auto findFirstSection(SecT &&Sections, PredT &&Pred) {
  return std::find_if(Sections.begin(), Sections.end(),
                      [&Pred](auto &&Section) {
                        return std::invoke(Pred, Section.OutputSection.Desc);
                      });
}

} // namespace

Linker::OutputSectionT::OutputSectionT(const SectionDesc &Desc)
    : Desc(Desc), Name(getUniqueSectionName(Desc)) {}

void Linker::calculateMemoryRegion() {
  assert(!Sections.empty() && "Sections must not be empty here");
  MemoryRegion.first = Sections.front().OutputSection.Desc.VMA;
  MemoryRegion.second = Sections.back().OutputSection.Desc.VMA +
                        Sections.back().OutputSection.Desc.Size;
}

Linker::Linker(LLVMContext &Ctx, const SectionsDescriptions &Sects,
               StringRef MN)
    : MangleName(MN) {
  std::transform(Sects.begin(), Sects.end(), std::back_inserter(Sections),
                 [](auto &SectionDesc) {
                   return SectionEntry{OutputSectionT{SectionDesc}, {}};
                 });
  std::sort(Sections.begin(), Sections.end(), [](auto &LSE, auto &RSE) {
    return LSE.OutputSection.Desc.VMA < RSE.OutputSection.Desc.VMA;
  });
  calculateMemoryRegion();
  auto DefaultCodeSection = findFirstSection(
      Sections, [](const SectionDesc &Desc) { return Desc.M.X(); });

  if (DefaultCodeSection != Sections.end())
    DefaultCodeSection->InputSections.emplace_back(kDefaultTextSectionName);

  auto ROMSection = findFirstSection(Sections, [](const SectionDesc &S) {
    return S.M.R() && !S.M.W() && !S.M.X();
  });

  if (ROMSection != Sections.end())
    ROMSection->InputSections.emplace_back(kDefaultRODataSectionName);
}

bool Linker::hasOutputSectionFor(StringRef SectionName) const {
  return std::any_of(
      Sections.begin(), Sections.end(), [&SectionName](auto &Section) {
        return llvm::any_of(Section.InputSections, [&SectionName](auto &&Name) {
          return Name == SectionName;
        });
      });
}

Linker::OutputSectionT
Linker::getOutputSectionFor(StringRef SectionName) const {
  assert(hasOutputSectionFor(SectionName));

  auto Section = std::find_if(
      Sections.begin(), Sections.end(), [&SectionName](auto &Section) {
        return llvm::any_of(Section.InputSections, [&SectionName](auto &&Name) {
          return Name == SectionName;
        });
      });
  return Section->OutputSection;
}

std::string Linker::getOutputNameForDesc(const SectionDesc &Desc) const {
  auto Section = std::find_if(Sections.begin(), Sections.end(),
                              [&Desc](const SectionEntry &Section) {
                                return Section.OutputSection.Desc == Desc;
                              });
  assert(Section != Sections.end() && "Got unknown section");
  assert(!Section->OutputSection.Name.empty());
  return Section->OutputSection.Name;
}

void Linker::addInputSectionForDescr(SectionDesc OutputSectionDesc,
                                     StringRef InpSectName) {
  auto SectIt =
      std::find_if(Sections.begin(), Sections.end(),
                   [&OutputSectionDesc](const SectionEntry &Sect) {
                     return Sect.OutputSection.Desc == OutputSectionDesc;
                   });
  assert(SectIt != Sections.end() &&
         "Can't find current section in the output sections");
  SectIt->InputSections.emplace_back(InpSectName);
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

void Linker::addSection(const SectionDesc &Section,
                        StringRef InputSectionName) {
  OutputSectionT NewSection{Section};

  auto FoundInterfere =
      std::find_if(Sections.begin(), Sections.end(), [&Section](auto &SE) {
        return SE.OutputSection.Desc.interfere(Section);
      });
  if (FoundInterfere != Sections.end())
    reportSectionInterfereError(NewSection, FoundInterfere->OutputSection,
                                "Interferes");

  auto FoundIndexInterfere =
      std::find_if(Sections.begin(), Sections.end(), [&Section](auto &SE) {
        return SE.OutputSection.Desc.ID == Section.ID;
      });

  if (FoundIndexInterfere != Sections.end())
    reportSectionInterfereError(NewSection, FoundInterfere->OutputSection,
                                "Shares same id");

  auto FoundPlace =
      std::find_if(Sections.begin(), Sections.end(), [&Section](auto &SE) {
        return SE.OutputSection.Desc.VMA > Section.VMA;
      });

  Sections.insert(FoundPlace,
                  SectionEntry{NewSection, {std::string{InputSectionName}}});
  calculateMemoryRegion();
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

std::string Linker::createLinkerScript(bool Export) const {
  std::string ScriptText;
  llvm::raw_string_ostream STS{ScriptText};

  std::string MemoryRegionName =
      MangleName.empty() ? "SNIPPY" : "SNIPPY_" + MangleName;
  STS << "MEMORY {\n"
      << "  " << MemoryRegionName << " (rwx) : ORIGIN = " << MemoryRegion.first
      << ", LENGTH = " << (MemoryRegion.second - MemoryRegion.first) << "\n";

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

    STS << "  " << OutSectionName << " " << SE.OutputSection.Desc.VMA;

    if (SE.InputSections.empty())
      STS << " (NOLOAD) ";

    STS << ": {\n";
    if (Export) {
      STS << "  KEEP(*(" << OutSectionName << "))\n";
    } else {
      if (SE.InputSections.empty()) {
        STS << "  PROVIDE(" << OutSectionName << "_start_ = .);\n";
        STS << "  . +=" << SE.OutputSection.Desc.Size << ";\n";
        STS << "  PROVIDE(" << OutSectionName << "_end_ = .);\n";
      } else {
        for (auto &&InputSectionName : SE.InputSections)
          STS << "  KEEP(*(" << InputSectionName << "))\n";
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
  return createLinkerScript(/*Export*/ true);
}

std::string Linker::run(ObjectFilesList ObjectFilesToLink,
                        bool Relocatable) const {
  assert(ObjectFilesToLink.size() > 0 && "Linker needs at least one image");

  auto ObjectFilesPaths = std::vector<FilePathT>{};
  for (auto &Objectfile : ObjectFilesToLink)
    ObjectFilesPaths.push_back(writeDataToDisk(Objectfile));

  auto InternalLinkerScript = createLinkerScript(/*Export*/ false);
  auto LinkerScriptPath = writeDataToDisk(InternalLinkerScript);

  auto LLDExe = findLLD();
  auto FinalImage =
      link(LLDExe, LinkerScriptPath, Relocatable, ObjectFilesPaths);

  sys::fs::remove(LinkerScriptPath);
  std::for_each(ObjectFilesPaths.begin(), ObjectFilesPaths.end(),
                [](const FilePathT &ImagePath) { sys::fs::remove(ImagePath); });

  return FinalImage;
}

StringRef Linker::GetExitSymbolName() { return "__snippy_exit"; }

} // namespace snippy
} // namespace llvm
