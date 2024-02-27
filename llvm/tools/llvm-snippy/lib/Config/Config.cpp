//===-- Config.cpp ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/Config.h"
#include "snippy/Config/Branchegram.h"
#include "snippy/Config/BurstGram.h"
#include "snippy/Config/MemoryScheme.h"
#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/Generator/BurstMode.h"
#include "snippy/Support/Options.h"
#include "snippy/Support/YAMLHistogram.h"
#include "snippy/Target/Target.h"

#include "llvm/Support/Errc.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/YAMLTraits.h"
#include <istream>
#include <sstream>
#include <variant>

#define DEBUG_TYPE "snippy-layout-config"

namespace llvm {
using namespace snippy;

void yaml::ScalarEnumerationTraits<BurstMode>::enumeration(yaml::IO &IO,
                                                           BurstMode &BMode) {
  IO.enumCase(BMode, "basic", BurstMode::Basic);
  IO.enumCase(BMode, "store", BurstMode::StoreBurst);
  IO.enumCase(BMode, "load", BurstMode::LoadBurst);
  IO.enumCase(BMode, "load-store", BurstMode::LoadStoreBurst);
  IO.enumCase(BMode, "mixed", BurstMode::MixedBurst);
  IO.enumCase(BMode, "custom", BurstMode::CustomBurst);
}

template <> struct yaml::MappingTraits<BurstGramData> {
  struct NormalizedGroupings final {
    std::vector<SList> Groupings;

    NormalizedGroupings(yaml::IO &) {}

    NormalizedGroupings(
        yaml::IO &IO, const std::optional<BurstGramData::GroupingsTy> &Denorm) {
      if (Denorm.has_value()) {
        void *Ctx = IO.getContext();
        assert(Ctx && "To parse or output BurstGram provide ConfigIOContext as "
                      "context for yaml::IO");
        const auto &OpCC = static_cast<const ConfigIOContext *>(Ctx)->OpCC;
        transform(*Denorm, std::back_inserter(Groupings),
                  [&OpCC](const auto &Set) {
                    SList Res;
                    transform(Set, std::back_inserter(Res),
                              [&OpCC](auto Val) -> std::string {
                                return std::string{OpCC.name(Val)};
                              });
                    return Res;
                  });
      }
    }

    std::optional<BurstGramData::GroupingsTy> denormalize(yaml::IO &IO) {
      BurstGramData::GroupingsTy Denorm;
      void *Ctx = IO.getContext();
      assert(Ctx && "To parse or output BurstGram provide ConfigIOContext as "
                    "context for yaml::IO");
      const auto &OpCC = static_cast<const ConfigIOContext *>(Ctx)->OpCC;
      transform(
          Groupings, std::back_inserter(Denorm), [&OpCC](const auto &Vec) {
            std::set<unsigned> Res;
            transform(Vec, std::inserter(Res, Res.end()),
                      [OpCC](const std::string &Name) {
                        auto Opt = OpCC.code(Name);
                        if (!Opt.has_value()) {
                          std::string Msg = "Unknown instruction \"" + Name +
                                            "\" in burst configuration";
                          report_fatal_error(StringRef(Msg), false);
                        }
                        return *Opt;
                      });
            return Res;
          });
      if (Denorm.empty())
        return std::nullopt;
      return Denorm;
    }
  };

  static void mapping(yaml::IO &IO, BurstGramData &Burst) {
    IO.mapRequired("min-size", Burst.MinSize);
    IO.mapRequired("max-size", Burst.MaxSize);
    IO.mapRequired("mode", Burst.Mode);
    yaml::MappingNormalization<NormalizedGroupings,
                               std::optional<BurstGramData::GroupingsTy>>
        Keys(IO, Burst.Groupings);
    IO.mapOptional("groupings", Keys->Groupings);
  }

  static std::string validate(yaml::IO &IO, BurstGramData &Burst) {
    if (Burst.MinSize > Burst.MaxSize)
      return "Max size of burst group should be greater than min size.";
    if (Burst.Mode == BurstMode::Basic && (Burst.MaxSize > 0))
      return "Min and max burst group sizes should be 0 with \"basic\" mode";
    if (Burst.Mode != BurstMode::Basic && Burst.MaxSize == 0)
      return "Burst max size should be greater than 0";
    if (Burst.Mode != BurstMode::CustomBurst && Burst.Groupings.has_value())
      return "Groupings can be specified only with custom burst mode";
    if (Burst.Mode == BurstMode::CustomBurst && !Burst.Groupings)
      return "Custom burst mode was specified but groupings are not provided";
    if (Burst.Mode == BurstMode::CustomBurst && Burst.Groupings->empty())
      return "Custom burst mode was specified but groupings are empty";
    if (Burst.Mode == BurstMode::CustomBurst &&
        any_of(*Burst.Groupings,
               [](const auto &Group) { return Group.empty(); }))
      return "Burst grouping can't be empty";
    return std::string();
  }
};

template <> struct yaml::MappingTraits<snippy::BurstGram> {
  static void mapping(yaml::IO &IO, snippy::BurstGram &Burst) {
    IO.mapOptional("burst", Burst.Data);
  }
};

LLVM_SNIPPY_YAML_INSTANTIATE_HISTOGRAM_IO(snippy::OpcodeHistogramDecodedEntry);
LLVM_SNIPPY_YAML_IS_HISTOGRAM_DENORM_ENTRY(snippy::OpcodeHistogramDecodedEntry)

namespace snippy {
struct SectionAttrs {
  std::variant<int, std::string> ID{0};
  size_t VMA;
  size_t Size;
  size_t LMA;
  std::string Access;

  SectionAttrs(yaml::IO &) {}

  SectionAttrs(yaml::IO &IO, const SectionDesc &Desc)
      : ID(Desc.ID), VMA(Desc.VMA), Size(Desc.Size), LMA(Desc.LMA) {
    auto SS = raw_string_ostream(Access);
    Desc.M.dump(SS);
  }

  SectionDesc denormalize(yaml::IO &IO) {
    return SectionDesc(ID, VMA, Size, LMA, Access.c_str());
  }
};

struct IncludeParsingWrapper final {
  std::vector<std::string> Includes;
};
} // namespace snippy

template <> struct yaml::MappingTraits<IncludeParsingWrapper> {
  static void mapping(yaml::IO &IO, snippy::IncludeParsingWrapper &IPW) {
    IO.mapOptional("include", IPW.Includes);
  }
};

template <> struct yaml::MappingTraits<snippy::SectionAttrs> {
  static void mapping(yaml::IO &IO, snippy::SectionAttrs &Info) {
    std::optional<int> NumberFromNo;
    std::optional<std::string> NumberFromName;
    IO.mapOptional("no", NumberFromNo);
    IO.mapOptional("name", NumberFromName);
    if (!NumberFromNo && !NumberFromName)
      report_fatal_error("There is a section in the layout file that does not "
                         "have 'no' key, nor 'name'.",
                         false);
    if (NumberFromNo && NumberFromName)
      report_fatal_error("There is a section in the layout file that has both "
                         "'no' and 'name' keys.",
                         false);
    if (NumberFromNo)
      Info.ID = *NumberFromNo;
    else
      Info.ID = *NumberFromName;
    IO.mapRequired("VMA", Info.VMA);
    IO.mapRequired("SIZE", Info.Size);
    IO.mapRequired("LMA", Info.LMA);
    IO.mapRequired("ACCESS", Info.Access);
  }
};

template <>
void yaml::yamlize(IO &IO, snippy::SectionDesc &Desc, bool, EmptyContext &Ctx) {
  MappingNormalization<snippy::SectionAttrs, snippy::SectionDesc> MappingNorm(
      IO, Desc);
  yamlize(IO, *(MappingNorm.operator->()), true, Ctx);
}

template <> struct yaml::ScalarEnumerationTraits<ImmHistOpcodeSettings::Kind> {
  static void enumeration(IO &IO, ImmHistOpcodeSettings::Kind &K) {
    IO.enumCase(K, "uniform", ImmHistOpcodeSettings::Kind::Uniform);
  }
};

struct ImmHistOpcodeSettingsNorm final {
  ImmHistOpcodeSettings::Kind Kind = ImmHistOpcodeSettings::Kind::Custom;
  ImmediateHistogramSequence Seq;
  yaml::IO &IO;

  ImmHistOpcodeSettingsNorm(yaml::IO &IO) : IO(IO) {}
};

struct ImmHistOpcodeSettingsNormalization final {
  ImmHistOpcodeSettingsNorm Data;

  ImmHistOpcodeSettingsNormalization(yaml::IO &IO) : Data(IO) {}

  ImmHistOpcodeSettingsNormalization(yaml::IO &IO,
                                     const ImmHistOpcodeSettings &Denorm)
      : Data(IO) {
    Data.Kind = Denorm.getKind();
    if (Denorm.isSequence())
      Data.Seq = Denorm.getSequence();
  }
  ImmHistOpcodeSettings denormalize(yaml::IO &) {
    if (Data.Kind == ImmHistOpcodeSettings::Kind::Custom)
      return ImmHistOpcodeSettings(Data.Seq);
    if (Data.Kind == ImmHistOpcodeSettings::Kind::Uniform)
      return ImmHistOpcodeSettings();
    llvm_unreachable("Unknown opcode settings kind");
  }
};

template <> struct yaml::PolymorphicTraits<ImmHistOpcodeSettingsNorm> {
  static yaml::NodeKind getKind(const ImmHistOpcodeSettingsNorm &Info) {
    if (Info.Kind == ImmHistOpcodeSettings::Kind::Uniform)
      return NodeKind::Scalar;
    if (Info.Kind == ImmHistOpcodeSettings::Kind::Custom)
      return NodeKind::Sequence;
    llvm_unreachable("Unknown map value kind in ImmHistOpcodeSettings");
  }

  static ImmHistOpcodeSettings::Kind &
  getAsScalar(ImmHistOpcodeSettingsNorm &Info) {
    return Info.Kind;
  }

  static ImmediateHistogramSequence &
  getAsSequence(ImmHistOpcodeSettingsNorm &Info) {
    return Info.Seq;
  }

  static ImmediateHistogramSequence &getAsMap(ImmHistOpcodeSettingsNorm &Info) {
    Info.IO.setError("Immediate histogram opcode setting should be either "
                     "sequence or scalar. But map was encountered.");
    report_fatal_error("Failed to parse configuration file.", false);
  }
};

template <> struct yaml::CustomMappingTraits<ImmHistConfigForRegEx> {
  static void inputOne(IO &IO, StringRef Key, ImmHistConfigForRegEx &Info) {
    yaml::MappingNormalization<ImmHistOpcodeSettingsNormalization,
                               ImmHistOpcodeSettings>
        Norm(IO, Info.Data);
    IO.mapRequired(Key.data(), Norm->Data);
    Info.Expr = Key.str();
  }

  static void output(IO &IO, ImmHistConfigForRegEx &Info) {
    yaml::MappingNormalization<ImmHistOpcodeSettingsNormalization,
                               ImmHistOpcodeSettings>
        Norm(IO, Info.Data);
    IO.mapRequired(Info.Expr.c_str(), Norm->Data);
  }
};

LLVM_SNIPPY_YAML_IS_SEQUENCE_ELEMENT(ImmHistConfigForRegEx,
                                     /* not a flow */ false);

template <> struct yaml::MappingTraits<ImmediateHistogramRegEx> {
  static void mapping(IO &IO, ImmediateHistogramRegEx &IH) {
    IO.mapRequired("opcodes", IH.Exprs);
  }
};

struct ImmediateHistogramNorm final {
  enum class Kind { Sequence, RegEx };
  ImmediateHistogramSequence Seq;
  ImmediateHistogramRegEx RegEx;
  Kind HistKind = Kind::Sequence;
  yaml::IO &IO;

  ImmediateHistogramNorm(yaml::IO &IO) : IO(IO) {}
};

struct ImmediateHistogramNormalization final {
  ImmediateHistogramNorm Data;

  ImmediateHistogramNormalization(yaml::IO &IO) : Data(IO) {}

  ImmediateHistogramNormalization(yaml::IO &IO, const ImmediateHistogram &Hist)
      : Data(IO) {
    if (Hist.holdsAlternative<ImmediateHistogramSequence>()) {
      Data.Seq = Hist.get<ImmediateHistogramSequence>();
      Data.HistKind = ImmediateHistogramNorm::Kind::Sequence;
    } else if (Hist.holdsAlternative<ImmediateHistogramRegEx>()) {
      Data.RegEx = Hist.get<ImmediateHistogramRegEx>();
      Data.HistKind = ImmediateHistogramNorm::Kind::RegEx;
    } else
      llvm_unreachable("Unknown immediate histogram kind");
  }

  ImmediateHistogram denormalize(yaml::IO &) {
    if (Data.HistKind == ImmediateHistogramNorm::Kind::RegEx)
      return ImmediateHistogram(Data.RegEx);
    if (Data.HistKind == ImmediateHistogramNorm::Kind::Sequence)
      return ImmediateHistogram(Data.Seq);
    llvm_unreachable("Unknown immediate histogram kind");
  }
};

template <> struct yaml::PolymorphicTraits<ImmediateHistogramNorm> {
  static yaml::NodeKind getKind(const ImmediateHistogramNorm &Hist) {
    if (Hist.HistKind == ImmediateHistogramNorm::Kind::RegEx)
      return yaml::NodeKind::Map;
    if (Hist.HistKind == ImmediateHistogramNorm::Kind::Sequence)
      return yaml::NodeKind::Sequence;
    llvm_unreachable("Unknown immediate histogram kind");
  }

  static ImmediateHistogramRegEx &getAsMap(ImmediateHistogramNorm &Hist) {
    Hist.HistKind = ImmediateHistogramNorm::Kind::RegEx;
    return Hist.RegEx;
  }

  static ImmediateHistogramSequence &
  getAsSequence(ImmediateHistogramNorm &Hist) {
    Hist.HistKind = ImmediateHistogramNorm::Kind::Sequence;
    return Hist.Seq;
  }

  static int &getAsScalar(ImmediateHistogramNorm &Info) {
    Info.IO.setError("Immediate histogram should be either sequence or map. "
                     "But scalar was encountered.");
    report_fatal_error("Failed to parse configuration file.", false);
  }
};

void yaml::MappingTraits<Config>::mapping(yaml::IO &IO, Config &Info) {
  // FIXME: Currently sections can't be serialized
  if (!IO.outputting()) {
    IO.mapOptional("sections", Info.Sections);
  }
  yaml::MappingTraits<MemoryAccesses>::mapping(IO, Info.MS.MA);
  IO.mapOptional("branches", Info.Branches);
  IO.mapOptional("burst", Info.Burst.Data);
  YAMLHistogramIO<OpcodeHistogramDecodedEntry> HistIO(Info.Histogram);
  IO.mapOptional("histogram", HistIO);
  yaml::MappingNormalization<ImmediateHistogramNormalization,
                             ImmediateHistogram>
      ImmHistNorm(IO, Info.ImmHistogram);
  IO.mapOptional("imm-hist", ImmHistNorm->Data);
  yaml::MappingTraits<CallGraphLayout>::mapping(IO, Info.CGLayout);
  Info.TargetConfig->mapConfig(IO);
  IO.mapOptional("call-graph", Info.FuncDescs);
}

namespace snippy {

static void diagnoseHistogram(LLVMContext &Ctx, const OpcodeCache &OpCC,
                              OpcodeHistogram &Histogram) {
  if (Histogram.size() == 0) {
    snippy::warn(WarningName::InstructionHistogram, Ctx,
                 "Plugin didn't fill histogram",
                 "Generating instructions with only plugin calls");
    return;
  }

  auto InvalidOpcChecker = [OpCC](auto It) {
    return OpCC.desc(It.first) == nullptr;
  };
  if (std::find_if(Histogram.begin(), Histogram.end(), InvalidOpcChecker) !=
      Histogram.end())
    report_fatal_error("Plugin filled histogram with invalid opcodes", false);

  auto InvalidWeightsChecker = [](auto It) { return It.second < 0; };
  if (std::find_if(Histogram.begin(), Histogram.end(), InvalidWeightsChecker) !=
      Histogram.end())
    report_fatal_error("Plugin filled histogram with negative opcodes weights",
                       false);
}

Config::Config(const SnippyTarget &Tgt, StringRef PluginFilename,
               StringRef PluginInfoFilename, OpcodeCache OpCC,
               bool ParseWithPlugin, LLVMContext &Ctx,
               ArrayRef<std::string> IncludedFiles)
    : Includes(IncludedFiles),
      PluginManagerImpl(std::make_unique<PluginManager>()),
      TargetConfig(Tgt.createTargetConfig()) {
  PluginManagerImpl->loadPluginLib(PluginFilename.str());
  if (ParseWithPlugin) {
    PluginManagerImpl->parseOpcodes(
        OpCC, PluginInfoFilename.str(),
        std::inserter(Histogram, Histogram.begin()));
    diagnoseHistogram(Ctx, OpCC, Histogram);
  }
}

static auto getContentsFromRelativePath(StringRef ParentDirectory,
                                        StringRef RelativePath) {
  SmallVector<char> Path{ParentDirectory.begin(), ParentDirectory.end()};
  sys::path::append(Path, RelativePath);
  auto SearchLocation = StringRef{Path.data(), Path.size()};
  LLVM_DEBUG(dbgs() << "searching include at: " << SearchLocation << "\n");
  auto Contents = MemoryBuffer::getFile(SearchLocation);
  if (Contents) {
    LLVM_DEBUG(dbgs() << "  include found, contents retrived successfully!\n");
  } else {
    LLVM_DEBUG(dbgs() << "  could not find include at the specified location ("
                      << Contents.getError().message() << ")\n");
  }
  return Contents;
}

using MemBufStrPair = std::pair<std::unique_ptr<MemoryBuffer>, std::string>;

static ErrorOr<MemBufStrPair>
makeBufPathPairOrErr(ErrorOr<std::unique_ptr<MemoryBuffer>> BufOrErr,
                     StringRef Path) {
  if (BufOrErr.getError())
    return BufOrErr.getError();
  return std::make_pair(std::move(BufOrErr.get()), Path.str());
}

static ErrorOr<MemBufStrPair>
makeRelBufPathPairOrErr(ErrorOr<std::unique_ptr<MemoryBuffer>> Contents,
                        StringRef Filename, StringRef ParentPath) {
  SmallString<32> AbsolutePath;
  sys::path::append(AbsolutePath, ParentPath, Filename);
  return makeBufPathPairOrErr(std::move(Contents), AbsolutePath.str().str());
}

static ErrorOr<MemBufStrPair>
getIncludeFileContentsAndPath(StringRef ParentPath,
                              const std::vector<std::string> &ExtraIncludeDirs,
                              StringRef IncludeFilename) {
  LLVM_DEBUG(dbgs() << "processing include: " << IncludeFilename << "\n");
  if (!sys::path::is_relative(IncludeFilename)) {
    LLVM_DEBUG(dbgs() << "include file has an absolute path\n");
    return makeBufPathPairOrErr(MemoryBuffer::getFile(IncludeFilename),
                                IncludeFilename.str());
  }

  LLVM_DEBUG(dbgs() << "include file has a relative path\n");
  auto Contents = getContentsFromRelativePath(ParentPath, IncludeFilename);
  if (Contents)
    return makeRelBufPathPairOrErr(std::move(Contents), IncludeFilename.str(),
                                   ParentPath.str());

  for (const auto &IncludeDir : ExtraIncludeDirs) {
    LLVM_DEBUG(dbgs() << "trying extra include dir: " << IncludeDir << "\n");
    auto Contents = getContentsFromRelativePath(IncludeDir, IncludeFilename);
    if (Contents)
      return makeRelBufPathPairOrErr(std::move(Contents), IncludeFilename.str(),
                                     IncludeDir);
  }

  return make_error_code(errc::no_such_file_or_directory);
}

static std::vector<std::string> getConfigIncludeFiles(StringRef Filename) {
  snippy::IncludeParsingWrapper IPW;
  auto Err = loadYAMLIgnoreUnknownKeys(IPW, Filename);
  if (Err)
    report_fatal_error(toString(std::move(Err)).c_str(), false);
  return IPW.Includes;
}

bool lineIsEmpty(StringRef Line) {
  auto Pos = Line.find_first_not_of(" \t\n");
  return Pos == StringRef::npos;
}

std::string commentIncludes(StringRef Text, unsigned IncludesN) {
  if (IncludesN == 0)
    return Text.str();
  std::string Res;
  raw_string_ostream SS(Res);
  auto StartPos = Text.find("\ninclude:") + 1;
  assert(StartPos != StringRef::npos);
  SS << Text.substr(0, StartPos);
  auto LeftToRead = Text.substr(StartPos).str();
  std::stringstream IS(LeftToRead);
  unsigned Cnt = 0;
  for (std::string Line; std::getline(IS, Line);) {
    if (Cnt < IncludesN + 1) {
      SS << "# " << Line << "\n";
      if (!lineIsEmpty(Line))
        ++Cnt;
    } else
      SS << Line << "\n";
  }
  return Res.substr(0, Res.size() - 1);
}

std::string endLineIfNeeded(StringRef Str) {
  if (Str.empty())
    return "";
  if (Str.back() == '\n')
    return "";
  return "\n";
}

static void checkSubFileContents(StringRef SubFileName, StringRef Contents) {
  if (Contents.find("\ninclude:") != std::string::npos) {
    std::string Msg;
    raw_string_ostream SS(Msg);
    SS << "In file \"" << SubFileName << "\""
       << ": included file cannot contain \"include\" section."
       << "\n";
    report_fatal_error(StringRef(Msg), false);
  }
}

void IncludePreprocessor::mergeFile(StringRef FileName, StringRef Contents) {
  checkSubFileContents(FileName, Contents);
  std::istringstream IS(Contents.str());
  unsigned LocalIdx = 1; // Line count starts from 1
  for (std::string Line; std::getline(IS, Line); ++LocalIdx) {
    // FileName is passed as a non-owning StringRef. This is intended to
    // avoid copying absolute filepaths for each line
    Lines.emplace_back(LineID{FileName, LocalIdx});
  }
  Text += Contents;
  Text += endLineIfNeeded(Contents);
}

IncludePreprocessor::IncludePreprocessor(
    StringRef Filename, const std::vector<std::string> &IncludeDirs,
    LLVMContext &Ctx) {
  auto SubFiles = getConfigIncludeFiles(Filename);
  auto ParentDirectoryPath = sys::path::parent_path(Filename);
  for (StringRef IncludeFileName : SubFiles) {
    auto IncludeFileContentsAndPath = getIncludeFileContentsAndPath(
        ParentDirectoryPath, IncludeDirs, IncludeFileName);
    if (!IncludeFileContentsAndPath)
      fatal(Ctx, "Failed to open file \"" + IncludeFileName + "\"",
            IncludeFileContentsAndPath.getError().message());
    auto &&[Contents, Path] = *IncludeFileContentsAndPath;
    auto [InsIter, _] = IncludedFiles.insert(Path);
    mergeFile(*InsIter, Contents->getBuffer());
  }
  auto MemBufOrErr = MemoryBuffer::getFile(Filename);
  if (auto EC = MemBufOrErr.getError(); !MemBufOrErr)
    fatal(Ctx, "Failed to open file \"" + Filename + "\"", EC.message());
  mergeFile(Filename,
            commentIncludes((*MemBufOrErr)->getBuffer(), SubFiles.size()));
}

void Config::dump(raw_ostream &OS, const ConfigIOContext &Ctx) const {
  outputYAMLToStream(const_cast<Config &>(*this), OS,
                     [&Ctx = const_cast<ConfigIOContext &>(Ctx)](auto &IO) {
                       IO.setContext(&Ctx);
                     });
}

} // namespace snippy
} // namespace llvm
