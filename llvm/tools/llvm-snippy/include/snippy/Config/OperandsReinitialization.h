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
#include "snippy/Support/YAMLExtras.h"
#include "snippy/Support/YAMLHistogram.h"
#include "snippy/Support/YAMLUtils.h"

#include "llvm/ADT/SmallVector.h"

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
  enum class EntryKind { Uniform, Value };

private:
  EntryKind Kind = EntryKind::Uniform;
  double Weight = 1.0;
  std::optional<FormattedAPIntWithSign> Value;

public:
  OperandsValuesEntry() = default;

  OperandsValuesEntry(const FormattedAPIntWithSign &Val)
      : Kind(EntryKind::Value), Value(Val) {}

  OperandsValuesEntry(FormattedAPIntWithSign &&Val)
      : Kind(EntryKind::Value), Value(Val) {}

  EntryKind getKind() const { return Kind; }
  double getWeight() const { return Weight; }
  FormattedAPIntWithSign &getValue() & {
    assert(Value.has_value());
    return Value.value();
  }

  const FormattedAPIntWithSign &getValue() const & {
    assert(Value.has_value());
    return Value.value();
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
    }
    llvm_unreachable("Unrecognized OperandsValuesEntry kind");
  }
};

struct OperandsValuesEntryMapMapper final {};

struct OperandsValuesEntrySeqMapper final {};

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

struct OpcodeValuegramOpcodeSettings final
    : public SmallVector<OpcodeValuegramOpcSettingsEntry> {

  OpcodeValuegramOpcodeSettings() {}

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

struct OpcodeValuegramRegEx final {
  std::string Expr;
  OpcodeValuegramOpcodeSettings Data;
};

using OpcodesValuegram = std::vector<OpcodeValuegramRegEx>;

class OpcodeToValuegramMap final {
  using MapTy = std::unordered_map<unsigned, OpcodeValuegramOpcodeSettings>;

  MapTy Data;

public:
  OpcodeToValuegramMap(const OpcodesValuegram &OpcVal,
                       const OpcodeHistogram &OpcHist, const OpcodeCache &OpCC);

  OpcodeToValuegramMap() = default;

  bool count(unsigned Opc) const { return Data.count(Opc); }

  const OpcodeValuegramOpcodeSettings &
  getConfigForOpcode(unsigned Opc, const OpcodeCache &OpCC) const {
    assert(Data.count(Opc) && "Opcode was not found in opcode valuegram");
    return Data.at(Opc);
  }
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

class CommonOpcodeToValuegramMap final {
  using Distribution = std::discrete_distribution<size_t>;
  using WeightedValuegramMaps =
      SmallVector<std::pair<OpcodeToValuegramMap, double>>;

  WeightedValuegramMaps MapsAndWeights;

private:
  SmallVector<double> createContainingMapsWeightsRange(unsigned Opcode) const;

public:
  CommonOpcodeToValuegramMap(const OperandsReinitializationConfig &ORConfig,
                             const OpcodeHistogram &OpcHist,
                             const OpcodeCache &OpCC);

  CommonOpcodeToValuegramMap() = default;

  const OpcodeValuegramOpcodeSettings &
  getConfigForOpcode(unsigned Opc, const OpcodeCache &OpCC) const;

  auto begin() { return MapsAndWeights.begin(); }
  auto begin() const { return MapsAndWeights.begin(); }
  auto end() { return MapsAndWeights.end(); }
  auto end() const { return MapsAndWeights.end(); }
  auto empty() const { return MapsAndWeights.empty(); }
};

struct OpcodeValuegramEntryScalarMapper final {};

struct OpcodeValuegramEntryMapMapper final {
  OpcodeValuegramOpcSettingsEntry &Entry;
};

struct HistogramSubsetForOpcodeValuegram final {};

LLVM_SNIPPY_YAML_DECLARE_HISTOGRAM_TRAITS(
    std::unique_ptr<IOpcodeValuegramEntry>, HistogramSubsetForOpcodeValuegram);

std::optional<APInt> sampleOpcodeValuegramForOneReg(
    const OpcodeValuegramOpcodeSettings &OpcValuegram, unsigned OperandIndex,
    unsigned NumBits, std::discrete_distribution<size_t> &Dist, unsigned Opcode,
    [[maybe_unused]] const MCInstrInfo &II);
} // namespace snippy

LLVM_SNIPPY_YAML_DECLARE_IS_HISTOGRAM_DENORM_ENTRY(
    std::unique_ptr<snippy::IOpcodeValuegramEntry>)

namespace yaml {

void yamlize(IO &IO, const snippy::OpcodeValuegramEntryScalarMapper &Seq, bool,
             EmptyContext &Ctx);

void yamlize(IO &IO, const snippy::OperandsValuesEntryMapMapper &Seq, bool,
             EmptyContext &Ctx);

void yamlize(IO &IO, const snippy::OperandsValuesEntrySeqMapper &Seq, bool,
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
    return NodeKind::Scalar;
  }

  static const snippy::OperandsValuesEntryScalarMapper
  getAsScalar(snippy::OperandsValuesEntry &Entry) {
    return snippy::OperandsValuesEntryScalarMapper{Entry};
  }

  static const snippy::OperandsValuesEntryMapMapper
  getAsMap(snippy::OperandsValuesEntry &Entry) {
    return snippy::OperandsValuesEntryMapMapper{};
  }

  static const snippy::OperandsValuesEntrySeqMapper
  getAsSequence(snippy::OperandsValuesEntry &Entry) {
    return snippy::OperandsValuesEntrySeqMapper{};
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
    snippy::OpcodeValuegramOpcodeSettings,
    snippy::OpcodeValuegramOpcSettingsEntry);

} // namespace llvm