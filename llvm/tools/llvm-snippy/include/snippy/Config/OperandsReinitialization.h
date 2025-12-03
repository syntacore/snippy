//===-- OperandsReinitialization.h ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/Config/Valuegram.h"
#include "snippy/Support/APIntSampler.h"
#include "snippy/Support/YAMLExtras.h"
#include "snippy/Support/YAMLHistogram.h"
#include "snippy/Support/YAMLUtils.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FormatVariadic.h"

#include "snippy/Support/RandUtil.h"

#include <random>
#include <string>
#include <vector>

namespace llvm {
namespace snippy {

class IOpcodeValuegramEntry {
public:
  enum class EntryKind { MapEntry, Uniform, Operands, HelperSentinel };

public:
  virtual ~IOpcodeValuegramEntry() = default;
  virtual EntryKind getKind() const = 0;
  virtual std::unique_ptr<IOpcodeValuegramEntry> clone() = 0;

  double getWeight() const { return Weight; }
  double &getWeight() { return Weight; }

protected:
  double Weight = 1.0;
};

class IOpcodeValuegramMapEntry : public IOpcodeValuegramEntry {
protected:
  virtual void mapYamlImpl(yaml::IO &IO) {}
  virtual std::string validateImpl(yaml::IO &IO) const { return ""; }

public:
  void mapYaml(yaml::IO &IO);
  std::string validate(yaml::IO &IO) const {
    if (Weight < 0)
      return "Opcode valuegram entry weight can not be less than 0";
    return validateImpl(IO);
  }
  static bool classof(const IOpcodeValuegramEntry *Entry) {
    assert(Entry);
    auto Kind = Entry->getKind();
    return (Kind > EntryKind::MapEntry && Kind < EntryKind::HelperSentinel);
  }
};

struct OpcodeValuegramUniformEntry final : public IOpcodeValuegramMapEntry {
  OpcodeValuegramUniformEntry() = default;
  EntryKind getKind() const override { return EntryKind::Uniform; }
  std::unique_ptr<IOpcodeValuegramEntry> clone() override {
    return std::make_unique<OpcodeValuegramUniformEntry>(*this);
  }
  static bool classof(const IOpcodeValuegramEntry *Entry) {
    assert(Entry);
    auto Kind = Entry->getKind();
    return Kind == EntryKind::Uniform;
  }
};

class OperandsValuesEntry final {
public:
  enum class EntryKind { Uniform, Value, Valuegram };

private:
  EntryKind Kind = EntryKind::Uniform;
  double Weight = 1.0;
  std::variant<std::monostate, FormattedAPIntWithSign, Valuegram> Underlying;

public:
  OperandsValuesEntry() = default;

  OperandsValuesEntry(const FormattedAPIntWithSign &Val)
      : Kind(EntryKind::Value), Underlying(Val) {}

  OperandsValuesEntry(const Valuegram &Val)
      : Kind(EntryKind::Valuegram), Underlying(Val) {}

  EntryKind getKind() const { return Kind; }
  double getWeight() const { return Weight; }

  auto get() const & { return Underlying; }

  FormattedAPIntWithSign &getValue() & {
    assert(std::holds_alternative<FormattedAPIntWithSign>(Underlying));
    return std::get<FormattedAPIntWithSign>(Underlying);
  }

  const FormattedAPIntWithSign &getValue() const & {
    assert(std::holds_alternative<FormattedAPIntWithSign>(Underlying));
    return std::get<FormattedAPIntWithSign>(Underlying);
  }

  Valuegram &getValuegram() & {
    assert(std::holds_alternative<Valuegram>(Underlying));
    return std::get<Valuegram>(Underlying);
  }

  const Valuegram &getValuegram() const & {
    assert(std::holds_alternative<Valuegram>(Underlying));
    return std::get<Valuegram>(Underlying);
  }
};

struct OperandsValuesEntryScalarMapper final {
  OperandsValuesEntry &Entry;

  void dump(raw_ostream &OS) const {
    switch (Entry.getKind()) {
    case OperandsValuesEntry::EntryKind::Uniform:
      OS << "uniform";
      return;
    case OperandsValuesEntry::EntryKind::Value: {
      auto Str = Entry.getValue().toString();
      if (Str.takeError())
        snippy::fatal(llvm::toString(Str.takeError()));
      OS << Str.get();
      return;
    }
    default:
      llvm_unreachable("Unrecognized OperandsValuesEntry kind");
    }
  }
};

struct OperandsValuesEntryMapMapper final {};

using OperandsValuesSettings = SmallVector<OperandsValuesEntry>;

struct OpcodeValuegramOperandsEntry final : public IOpcodeValuegramMapEntry {
  OperandsValuesSettings Values;

  OpcodeValuegramOperandsEntry() = default;
  OpcodeValuegramOperandsEntry(const OperandsValuesSettings &Vals)
      : Values(Vals) {}
  EntryKind getKind() const override { return EntryKind::Operands; }
  std::unique_ptr<IOpcodeValuegramEntry> clone() override {
    return std::make_unique<OpcodeValuegramOperandsEntry>(*this);
  }
  static bool classof(const IOpcodeValuegramEntry *Entry) {
    assert(Entry);
    auto Kind = Entry->getKind();
    return Kind == EntryKind::Operands;
  }

protected:
  void mapYamlImpl(yaml::IO &IO) override;
};

StringRef toString(IOpcodeValuegramEntry::EntryKind Kind);

class OpcodeValuegramOpcSettingsEntry final {
  std::unique_ptr<IOpcodeValuegramEntry> UnderlyingEntry;

  OpcodeValuegramOpcSettingsEntry(std::unique_ptr<IOpcodeValuegramEntry> UEntry)
      : UnderlyingEntry(std::move(UEntry)) {}

public:
  OpcodeValuegramOpcSettingsEntry() = default;
  using EntryKind = IOpcodeValuegramEntry::EntryKind;

  OpcodeValuegramOpcSettingsEntry(const OpcodeValuegramOpcSettingsEntry &Other)
      : UnderlyingEntry(Other.UnderlyingEntry ? Other.UnderlyingEntry->clone()
                                              : nullptr) {}
  OpcodeValuegramOpcSettingsEntry(OpcodeValuegramOpcSettingsEntry &&Other) =
      default;
  OpcodeValuegramOpcSettingsEntry &
  operator=(const OpcodeValuegramOpcSettingsEntry &Other) {
    return *this = OpcodeValuegramOpcSettingsEntry{Other};
  }
  OpcodeValuegramOpcSettingsEntry &
  operator=(OpcodeValuegramOpcSettingsEntry &&Other) = default;

  IOpcodeValuegramEntry *getOrNull() { return UnderlyingEntry.get(); }

  const IOpcodeValuegramEntry *getOrNull() const {
    return UnderlyingEntry.get();
  }

  IOpcodeValuegramEntry &get() {
    assert(UnderlyingEntry);
    return *UnderlyingEntry;
  }

  const IOpcodeValuegramEntry &get() const {
    assert(UnderlyingEntry);
    return *UnderlyingEntry;
  }

  double getWeight() const {
    assert(UnderlyingEntry);
    return UnderlyingEntry->getWeight();
  }
  double &getWeight() {
    assert(UnderlyingEntry);
    return UnderlyingEntry->getWeight();
  }

  IOpcodeValuegramEntry::EntryKind getKind() const {
    assert(UnderlyingEntry);
    return UnderlyingEntry->getKind();
  }

  std::unique_ptr<IOpcodeValuegramEntry> &getOwned() { return UnderlyingEntry; }

  static Expected<OpcodeValuegramOpcSettingsEntry> create(EntryKind Kind) {
    switch (Kind) {
    case EntryKind::Uniform:
      return OpcodeValuegramOpcSettingsEntry{
          std::make_unique<OpcodeValuegramUniformEntry>()};
    case EntryKind::Operands:
      return OpcodeValuegramOpcSettingsEntry{
          std::make_unique<OpcodeValuegramOperandsEntry>()};
    default:
      return createStringError(
          std::make_error_code(std::errc::invalid_argument),
          Twine("Can't create OpcodeValuegramOpcSettingsEntry of kind: ")
              .concat(toString(Kind)));
    }
  }
};

struct OpcodeValuegramSettings final
    : public SmallVector<OpcodeValuegramOpcSettingsEntry> {

  OpcodeValuegramSettings() {}

  auto weights() const {
    return map_range(*this, [&](auto &&Entry) { return Entry.getWeight(); });
  }

  auto weights() {
    return map_range(*this, [&](auto &&Entry) { return Entry.getWeight(); });
  }

  auto weightsBegin() const { return weights().begin(); }
  auto weightsEnd() const { return weights().end(); }
  auto weightsBegin() { return weights().begin(); }
  auto weightsEnd() { return weights().end(); }
};

struct IOperandAPIntSampler {
  virtual Expected<APInt> sampleByIdx(unsigned Idx, unsigned BitWidth) = 0;
  virtual std::unique_ptr<IOperandAPIntSampler> copy() const = 0;
  virtual ~IOperandAPIntSampler() = default;
};

class WeightedOperandAPIntSamplerSet final : public IOperandAPIntSampler {
  using Distribution = std::discrete_distribution<unsigned>;
  using SamplersSetT = std::vector<std::unique_ptr<IOperandAPIntSampler>>;

  Distribution Dist;
  SamplersSetT Samplers;

  WeightedOperandAPIntSamplerSet() = default;

public:
  WeightedOperandAPIntSamplerSet(const Distribution &D, SamplersSetT &&Set)
      : Dist(D), Samplers(std::move(Set)) {}

  template <typename T>
  WeightedOperandAPIntSamplerSet(T &&WeightedSamplers)
      : Dist([&]() {
          auto Weights = map_range(WeightedSamplers,
                                   [](const auto &Elt) { return Elt.Weight; });
          return Distribution{Weights.begin(), Weights.end()};
        }()) {
    llvm::copy(map_range(WeightedSamplers,
                         [](auto &&Elt) { return std::move(Elt.Sampler); }),
               std::back_inserter(Samplers));
  }

  Expected<APInt> sampleByIdx(unsigned Idx, unsigned BitWidth) override {
    auto SamplerIdx = Dist(RandEngine::engine());
    assert(Samplers.size() > SamplerIdx);
    return Samplers[SamplerIdx]->sampleByIdx(Idx, BitWidth);
  }

  std::unique_ptr<IOperandAPIntSampler> copy() const override {
    SamplersSetT SamplersCopy;
    SamplersCopy.reserve(Samplers.size());
    transform(Samplers, std::back_inserter(SamplersCopy),
              [](const auto &S) { return S->copy(); });
    return std::make_unique<WeightedOperandAPIntSamplerSet>(
        Dist, std::move(SamplersCopy));
  }
};

struct OpcodeValuegramUniformSampler final : public IOperandAPIntSampler {
  Expected<APInt> sampleByIdx(unsigned, unsigned BitWidth) override {
    return UniformAPIntSamler::generate(BitWidth);
  }
  std::unique_ptr<IOperandAPIntSampler> copy() const override {
    return std::make_unique<OpcodeValuegramUniformSampler>();
  }
};

struct OpcodeValuegramBitpatternSampler final : public IOperandAPIntSampler {
  Expected<APInt> sampleByIdx(unsigned, unsigned BitWidth) override {
    return BitPatternAPIntSamler::generate(BitWidth);
  }
  std::unique_ptr<IOperandAPIntSampler> copy() const override {
    return std::make_unique<OpcodeValuegramBitpatternSampler>();
  }
};

class OpcodeValuegramConstantSampler final : public IOperandAPIntSampler {
  APInt Val;

public:
  OpcodeValuegramConstantSampler(APInt Val) : Val(Val) {}

  Expected<APInt> sampleByIdx(unsigned, unsigned) override { return Val; }

  std::unique_ptr<IOperandAPIntSampler> copy() const override {
    return std::make_unique<OpcodeValuegramConstantSampler>(Val);
  }
};

class OpcodeValuegramRangeSampler final : public IOperandAPIntSampler {
  APIntRangeSampler OwnedSampler;

public:
  OpcodeValuegramRangeSampler(APInt Min, APInt Max, bool IsSigned = false)
      : OwnedSampler([&]() {
          auto SamplerOrErr = APIntRangeSampler::create(Min, Max, IsSigned);
          if (!SamplerOrErr)
            snippy::fatal("Internal error", SamplerOrErr.takeError());
          return *SamplerOrErr;
        }()) {}

  Expected<APInt> sampleByIdx(unsigned, unsigned) override {
    return OwnedSampler.sample();
  }

  std::unique_ptr<IOperandAPIntSampler> copy() const override {
    return std::make_unique<OpcodeValuegramRangeSampler>(*this);
  }
};

class OpcodeValuegramValuegramSampler final : public IOperandAPIntSampler {
  Valuegram Cfg;
  // Even though we already know an operand index here and it seems
  // like we could just use IAPIntSampler, we need to
  // pass a BitWidth as sampleByIdx() argument, so we can't
  std::unique_ptr<IOperandAPIntSampler> OwnedSampler;

public:
  OpcodeValuegramValuegramSampler(const Valuegram &Valuegram,
                                  std::unique_ptr<IOperandAPIntSampler> Owned)
      : Cfg(Valuegram), OwnedSampler(std::move(Owned)) {}
  OpcodeValuegramValuegramSampler(const Valuegram &Valuegram);
  Expected<APInt> sampleByIdx(unsigned Idx, unsigned BitWidth) override {
    assert(OwnedSampler);
    return OwnedSampler->sampleByIdx(Idx, BitWidth);
  }
  std::unique_ptr<IOperandAPIntSampler> copy() const override {
    assert(OwnedSampler);
    return std::make_unique<OpcodeValuegramValuegramSampler>(
        Cfg, OwnedSampler->copy());
  }
};

class OpcodeValuegramOperandsSampler final : public IOperandAPIntSampler {
  using OpSampler = std::unique_ptr<IOperandAPIntSampler>;
  std::vector<OpSampler> ValSamplers;

public:
  OpcodeValuegramOperandsSampler(std::vector<OpSampler> &&Samplers)
      : ValSamplers(std::move(Samplers)) {}

  OpcodeValuegramOperandsSampler(const OpcodeValuegramOperandsEntry &Entry);

  Expected<APInt> sampleByIdx(unsigned OpIdx, unsigned BitWidth) override {
    auto ValSamplersSize = ValSamplers.size();
    if (OpIdx >= ValSamplersSize)
      return createStringError(
          makeErrorCode(Errc::InvalidArgument),
          formatv("Incorrect operand index for opcode valuegram operands "
                  "sampler. Expected {0} operands but {1} index provided",
                  ValSamplersSize, OpIdx));
    return ValSamplers[OpIdx]->sampleByIdx(OpIdx, BitWidth);
  }

  std::unique_ptr<IOperandAPIntSampler> copy() const override {
    std::vector<OpSampler> SamplersCopy;
    SamplersCopy.reserve(ValSamplers.size());
    transform(ValSamplers, std::back_inserter(SamplersCopy),
              [](const auto &S) { return S->copy(); });
    return std::make_unique<OpcodeValuegramOperandsSampler>(
        std::move(SamplersCopy));
  }
};

template <>
std::unique_ptr<IOperandAPIntSampler>
WeightedAPIntSamplerSetBuilder<IOperandAPIntSampler>::build();

class OpcodeSettingsSampler final : public IOperandAPIntSampler {
  std::unique_ptr<IOperandAPIntSampler> ContainedSampler;
  OpcodeValuegramSettings Cfg;

public:
  OpcodeSettingsSampler(const OpcodeValuegramSettings &Settings);

  Expected<APInt> sampleByIdx(unsigned Idx, unsigned BitWidth) override {
    assert(ContainedSampler);
    return ContainedSampler->sampleByIdx(Idx, BitWidth);
  }

  std::unique_ptr<IOperandAPIntSampler> copy() const override {
    return std::make_unique<OpcodeSettingsSampler>(*this);
  }

  const OpcodeValuegramSettings getSamplerSettings() const { return Cfg; }

public:
  OpcodeSettingsSampler(const OpcodeSettingsSampler &Oth) : Cfg(Oth.Cfg) {
    assert(Oth.ContainedSampler);
    ContainedSampler = Oth.ContainedSampler->copy();
  }

  OpcodeSettingsSampler &operator=(const OpcodeSettingsSampler &Oth) {
    OpcodeSettingsSampler Tmp(Oth);
    std::swap(Tmp, *this);
    return *this;
  }

  OpcodeSettingsSampler(OpcodeSettingsSampler &&Oth) = default;
  OpcodeSettingsSampler &operator=(OpcodeSettingsSampler &&Oth) = default;
  ~OpcodeSettingsSampler() = default;
};

struct OpcodeValuegramRegEx final {
  std::string Expr;
  OpcodeValuegramSettings Data;
};

using OpcodesValuegram = std::vector<OpcodeValuegramRegEx>;

class OpcodeToSettingsMap final {
  using MapTy = std::unordered_map<unsigned, OpcodeValuegramSettings>;

  MapTy Data;

public:
  OpcodeToSettingsMap(const OpcodesValuegram &OpcVal,
                      const OpcodeHistogram &OpcHist, const OpcodeCache &OpCC);

  OpcodeToSettingsMap() = default;

  bool count(unsigned Opc) const { return Data.count(Opc); }

  const OpcodeValuegramSettings &
  getSettingsForOpcode(unsigned Opc, const OpcodeCache &OpCC) const {
    assert(Data.count(Opc) && "Opcode was not found in opcode valuegram");
    return Data.at(Opc);
  }

  auto begin() { return Data.begin(); }
  auto begin() const { return Data.begin(); }
  auto end() { return Data.end(); }
  auto end() const { return Data.end(); }
};

class OperandsReinitDataSource final {
public:
  // TODO: Support another data sources for operands reinitialization.
  enum class Kind { Valuegram };

private:
  Kind DSKind;
  double Weight = 1.0;
  std::optional<OpcodesValuegram> Valuegram;

  friend struct yaml::MappingTraits<OperandsReinitDataSource>;

public:
  OperandsReinitDataSource() = default;

  OperandsReinitDataSource(double Weight, const OpcodesValuegram &Other)
      : DSKind(Kind::Valuegram), Weight(Weight), Valuegram(Other) {}

  Kind getKind() const { return DSKind; }

  bool isValuegram() const { return DSKind == Kind::Valuegram; }

  const OpcodesValuegram &getValuegram() const {
    assert(isValuegram() && Valuegram.has_value());
    return *Valuegram;
  }

  double getWeight() const { return Weight; }
};

using OperandsReinitializationConfig = std::vector<OperandsReinitDataSource>;

class WeightedOpcToSettingsMaps final {
  using WeightedMap = std::pair<OpcodeToSettingsMap, double>;
  using WeightedSettingsMaps = SmallVector<WeightedMap>;

  WeightedSettingsMaps Maps;

public:
  WeightedOpcToSettingsMaps(const OperandsReinitializationConfig &ORConfig,
                            const OpcodeHistogram &OpcHist,
                            const OpcodeCache &OpCC);

  WeightedOpcToSettingsMaps() = default;

  auto begin() { return Maps.begin(); }
  auto begin() const { return Maps.begin(); }
  auto end() { return Maps.end(); }
  auto end() const { return Maps.end(); }
  auto empty() const { return Maps.empty(); }
};

class OpcodeValuegramSampler final {
  // OptSampler will be std::nullopt if no regex matches the opcode
  using OptSampler = std::optional<OpcodeSettingsSampler>;
  using MapTy = std::unordered_map<unsigned, OptSampler>;

  MapTy Map;

public:
  OpcodeValuegramSampler(const OpcodeToSettingsMap &SettingsMap) {
    for (auto &&OpcAndSett : SettingsMap) {
      const auto &Settings = OpcAndSett.second;
      OptSampler Sampler;
      if (!Settings.empty())
        Sampler = OpcodeSettingsSampler(Settings);
      Map.emplace(OpcAndSett.first, Sampler);
    }
  }

  Expected<std::optional<APInt>>
  sampleForOpcode(unsigned Opcode, unsigned OpIdx, unsigned BitWidth) {
    assert(Map.count(Opcode));
    auto &Sampler = Map.at(Opcode);
    if (!Sampler.has_value())
      return std::nullopt;
    return Sampler->sampleByIdx(OpIdx, BitWidth);
  }
};

class OperandsReinitializationSampler final {
  using Distribution = std::discrete_distribution<size_t>;
  using DataSourceSampler = OpcodeValuegramSampler;
  using DataSourceSamplers = SmallVector<DataSourceSampler>;

  DataSourceSamplers Samplers;
  Distribution Dist;

public:
  OperandsReinitializationSampler(
      const WeightedOpcToSettingsMaps &WeightedMaps) {
    auto Weights = make_second_range(WeightedMaps);
    Dist = Distribution(Weights.begin(), Weights.end());
    transform(make_first_range(WeightedMaps), std::back_inserter(Samplers),
              [](const auto &Map) { return DataSourceSampler{Map}; });
  }

  Expected<std::optional<APInt>>
  sampleForOpcode(unsigned Opcode, unsigned OpIdx, unsigned BitWidth) {
    auto SamplerIdx = Dist(RandEngine::engine());
    assert(SamplerIdx < Samplers.size());
    return Samplers[SamplerIdx].sampleForOpcode(Opcode, OpIdx, BitWidth);
  }
};

struct OpcodeValuegramEntryScalarMapper final {};

struct OpcodeValuegramEntryMapMapper final {
  OpcodeValuegramOpcSettingsEntry &Entry;
};

struct HistogramSubsetForOpcodeValuegram final {};

LLVM_SNIPPY_YAML_DECLARE_HISTOGRAM_TRAITS(
    std::unique_ptr<IOpcodeValuegramEntry>, HistogramSubsetForOpcodeValuegram);

} // namespace snippy

LLVM_SNIPPY_YAML_DECLARE_IS_HISTOGRAM_DENORM_ENTRY(
    std::unique_ptr<snippy::IOpcodeValuegramEntry>)

namespace yaml {

void yamlize(IO &IO, const snippy::OpcodeValuegramEntryScalarMapper &Seq, bool,
             EmptyContext &Ctx);

void yamlize(IO &IO, const snippy::OperandsValuesEntryMapMapper &Seq, bool,
             EmptyContext &Ctx);

} // namespace yaml

// Can't use LLVM_SNIPPY_..._TRAITS_NG() with const type
template <>
struct llvm::yaml::missingTraits<const snippy::OperandsValuesEntryScalarMapper,
                                 llvm::yaml::EmptyContext> : std::false_type {};
template <>
struct yaml::ScalarTraits<const snippy::OperandsValuesEntryScalarMapper, void> {
  static void output(const snippy::OperandsValuesEntryScalarMapper &Mapper,
                     void *, raw_ostream &OS) {
    Mapper.dump(OS);
  }

  static std::string input(StringRef Scalar, void *,
                           const snippy::OperandsValuesEntryScalarMapper &Val) {
    auto &Entry = Val.Entry;
    if (Scalar == "uniform") {
      Entry = snippy::OperandsValuesEntry{};
      return "";
    }
    snippy::FormattedAPIntWithSign APIntVal;
    if (auto E = APIntVal.fromString(Scalar).moveInto(APIntVal)) {
      return toString(std::move(E));
    }
    Entry = snippy::OperandsValuesEntry{APIntVal};
    return "";
  }

  static QuotingType mustQuote(StringRef) { return QuotingType::None; }
};

template <>
struct yaml::PolymorphicTraits<snippy::OpcodeValuegramOpcSettingsEntry> {
  static NodeKind
  getKind(const snippy::OpcodeValuegramOpcSettingsEntry &Entry) {
    return NodeKind::Map;
  }

  const snippy::OpcodeValuegramEntryScalarMapper static getAsScalar(
      snippy::OpcodeValuegramOpcSettingsEntry &Entry) {
    return snippy::OpcodeValuegramEntryScalarMapper{};
  }

  const snippy::OpcodeValuegramEntryMapMapper static getAsMap(
      snippy::OpcodeValuegramOpcSettingsEntry &Entry) {
    return snippy::OpcodeValuegramEntryMapMapper{Entry};
  }

  static std::unique_ptr<snippy::IOpcodeValuegramEntry> &
  getAsSequence(snippy::OpcodeValuegramOpcSettingsEntry &Entry) {
    return Entry.getOwned();
  }
};

template <> struct yaml::PolymorphicTraits<snippy::OperandsValuesEntry> {
  static NodeKind getKind(const snippy::OperandsValuesEntry &Entry) {
    return Entry.getKind() == snippy::OperandsValuesEntry::EntryKind::Valuegram
               ? NodeKind::Sequence
               : NodeKind::Scalar;
  }

  static const snippy::OperandsValuesEntryScalarMapper
  getAsScalar(snippy::OperandsValuesEntry &Entry) {
    return snippy::OperandsValuesEntryScalarMapper{Entry};
  }

  static const snippy::OperandsValuesEntryMapMapper
  getAsMap(snippy::OperandsValuesEntry &Entry) {
    return snippy::OperandsValuesEntryMapMapper{};
  }

  static snippy::Valuegram &getAsSequence(snippy::OperandsValuesEntry &Entry) {
    if (Entry.getKind() != snippy::OperandsValuesEntry::EntryKind::Valuegram)
      Entry = snippy::OperandsValuesEntry{snippy::Valuegram{}};
    return Entry.getValuegram();
  }
};

LLVM_SNIPPY_YAML_IS_SEQUENCE_ELEMENT(snippy::FormattedAPIntWithSign, false);
LLVM_SNIPPY_YAML_IS_SEQUENCE_ELEMENT(snippy::OperandsReinitDataSource, false);
LLVM_SNIPPY_YAML_IS_SEQUENCE_ELEMENT(snippy::OpcodeValuegramRegEx, false);
LLVM_SNIPPY_YAML_IS_SEQUENCE_ELEMENT(snippy::OperandsValuesEntry, false);
LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS(
    const snippy::OpcodeValuegramEntryMapMapper);
LLVM_SNIPPY_YAML_DECLARE_SCALAR_ENUMERATION_TRAITS(
    snippy::IOpcodeValuegramEntry::EntryKind);

LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS_WITH_VALIDATE(
    snippy::OperandsReinitDataSource);
LLVM_SNIPPY_YAML_DECLARE_SCALAR_ENUMERATION_TRAITS(
    snippy::OperandsReinitDataSource::Kind);
LLVM_SNIPPY_YAML_DECLARE_SEQUENCE_TRAITS(
    snippy::OpcodeValuegramSettings, snippy::OpcodeValuegramOpcSettingsEntry);

} // namespace llvm
