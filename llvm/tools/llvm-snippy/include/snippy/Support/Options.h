//===-- Options.h -----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Support/Utils.h"
#include "snippy/Support/YAMLUtils.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/YAMLTraits.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace llvm {
namespace snippy {

/// \class CommandOptionBase
/// \brief Non-template class for command options of different types.
/// Manages option's name and tracks whether option was specified in either
/// command line or YAML file. Provides interface for yaml mapping.
class CommandOptionBase {
  std::string Name;
  bool Specified = false;

public:
  CommandOptionBase(StringRef Key) : Name(Key.str()) {}

  virtual ~CommandOptionBase() {}

  void markAsSpecified() { Specified = true; }

  bool isSpecified() const { return Specified; }

  const std::string &getName() const { return Name; }

  void mapStoredValue(yaml::IO &IO) {
    doMapping(IO);
    if (!isSpecified())
      markAsSpecified();
  }

  std::unique_ptr<CommandOptionBase> clone() const { return doClone(); }

private:
  virtual void doMapping(yaml::IO &IO) = 0;
  virtual std::unique_ptr<CommandOptionBase> doClone() const = 0;
};

/// \class CommandOption
/// \brief Stores value of concrete DataType.
/// Knows how to map its value to yaml and how to clone itself.
template <typename DataType> struct CommandOption : public CommandOptionBase {
  DataType Val = DataType();

  CommandOption(StringRef Name) : CommandOptionBase(Name) {}

  void doMapping(yaml::IO &IO) override {
    IO.mapRequired(getName().c_str(), Val);
  }

  std::unique_ptr<CommandOptionBase> doClone() const override {
    return std::make_unique<CommandOption<DataType>>(*this);
  }
};

/// \class OptionsStorage
/// \brief Singleton class that stores CommandOptions by pointer to base class
class OptionsStorage final {
  using StorageType =
      std::unordered_map<std::string, std::unique_ptr<CommandOptionBase>>;
  StorageType Data;
  OptionsStorage() = default;

public:
  OptionsStorage(const OptionsStorage &) = delete;
  OptionsStorage(OptionsStorage &&) = delete;
  OptionsStorage &operator=(const OptionsStorage &) = delete;
  OptionsStorage &operator=(OptionsStorage &&) = delete;

  static OptionsStorage &instance() {
    static OptionsStorage Instance;
    return Instance;
  }

  auto begin() { return Data.begin(); }
  auto end() { return Data.end(); }
  auto begin() const { return Data.begin(); }
  auto end() const { return Data.end(); }
  auto cbegin() const { return Data.cbegin(); }
  auto cend() const { return Data.cend(); }
  auto count(const std::string &Key) const { return Data.count(Key); }
  bool empty() const { return Data.empty(); }
  auto size() const { return Data.size(); }
  bool insert(StringRef Key, std::unique_ptr<CommandOptionBase> &&Val) {
    return Data.emplace(Key.data(), std::move(Val)).second;
  }

  CommandOptionBase &get(const std::string &Key) {
    assert(Data.count(Key));
    return *Data.at(Key);
  }
};

/// \class opt
/// \brief cl::opt wrapper
/// Modifies OptionsStorage and therefore creation of snippy::opt class entity
/// makes yaml parser outomatically able to parse that option
template <typename DataType> class opt : public cl::opt<DataType, true> {
  using BaseTy = cl::opt<DataType, true>;

  static DataType &allocateLocation(const char *Name) {
    auto &Options = OptionsStorage::instance();
    auto Tmp = std::make_unique<CommandOption<DataType>>(Name);
    auto &Res = Tmp->Val;
    bool WasInserted = Options.insert(Name, std::move(Tmp));
    assert(WasInserted && "Duplicated option name");
    return Res;
  }

public:
  using BaseTy::ArgStr;
  using BaseTy::getValue;
  using BaseTy::setValue;

  template <typename... Modes>
  opt(const char *Name, Modes &&...Ms)
      : BaseTy(StringRef(Name), cl::location(allocateLocation(Name)),
               std::forward<Modes>(Ms)...) {
    this->setCallback(
        [Name, CB = std::move(this->Callback)](const DataType &SetVal) {
          CB(SetVal);
          auto &OptionBase = OptionsStorage::instance().get(Name);
          OptionBase.markAsSpecified();
        });
  }

  operator DataType() const { return getValue(); }
  operator DataType &() { return getValue(); }

  DataType &operator=(const DataType &Val) {
    setValue(Val);
    return getValue();
  }

  bool isSpecified() const {
    return OptionsStorage::instance().get(ArgStr.data()).isSpecified();
  }

  bool operator==(const DataType &Val) const { return getValue() == Val; }

  bool operator!=(const DataType &Val) const { return !(*this == Val); }
};

/// \class list
/// \brief cl::opt wrapper
/// Modifies OptionsState and therefore creation of snippy::opt class entity
/// makes yaml parser outomatically able to parse that option
template <typename DataType>
class opt_list : public cl::list<DataType, std::vector<DataType>> {
  using ValueTy = std::vector<DataType>;
  using BaseTy = cl::list<DataType, ValueTy>;

  static ValueTy &allocateLocation(const char *Name) {
    auto &Options = OptionsStorage::instance();
    auto Tmp = std::make_unique<CommandOption<ValueTy>>(Name);
    auto &Res = Tmp->Val;
    bool WasInserted = Options.insert(Name, std::move(Tmp));
    assert(WasInserted && "Duplicated option name");
    return Res;
  }

  static ValueTy &getLocation(const char *Name) {
    auto &Options = OptionsStorage::instance();
    assert(Options.count(Name) && "Unknown option");
    auto &Opt = static_cast<CommandOption<ValueTy> &>(
        OptionsStorage::instance().get(Name));
    return Opt.Val;
  }

public:
  using BaseTy::ArgStr;

  template <typename... Modes>
  opt_list(const char *Name, Modes &&...Ms)
      : BaseTy(StringRef(Name), cl::location(allocateLocation(Name)),
               std::forward<Modes>(Ms)...) {
    this->setCallback(
        [Name, CB = std::move(this->Callback)](const DataType &SetVal) {
          CB(SetVal);
          auto &OptionBase = OptionsStorage::instance().get(Name);
          OptionBase.markAsSpecified();
        });
  }

  auto begin() { return getLocation(ArgStr.data()).begin(); }
  auto end() { return getLocation(ArgStr.data()).end(); }
  auto cbegin() const { return getLocation(ArgStr.data()).cbegin(); }
  auto cend() const { return getLocation(ArgStr.data()).cend(); }
  auto begin() const { return cbegin(); }
  auto end() const { return cend(); }
  auto size() const { return getLocation(ArgStr.data()).size(); }
  bool empty() const { return getLocation(ArgStr.data()).size(); }
  auto &front() { return getLocation(ArgStr.data()).front(); }
  const auto &front() const { return getLocation(ArgStr.data()).front(); }
  auto &back() { return getLocation(ArgStr.data()).back(); }
  const auto &back() const { return getLocation(ArgStr.data()).back(); }
  auto &operator[](unsigned Idx) { return getLocation(ArgStr.data())[Idx]; }
  const auto &operator[](unsigned Idx) const {
    return getLocation(ArgStr.data())[Idx];
  }

  bool isSpecified() const {
    return OptionsStorage::instance().get(ArgStr.data()).isSpecified();
  }
};

/// Empty class to allow parsing 'options' section only in yaml.
struct OptionsMappingWrapper final {};

struct EnumMapper {
  template <typename T, typename = std::enable_if_t<!std::is_same_v<int, T>>>
  void enumCase(T Value, const char *Name, const char *Description) {
    enumCase(narrowCast<int>(Value), Name, Description);
  }

  virtual void enumCase(int Value, const char *Name,
                        const char *Description) const = 0;
};

class ClEnumValues : private SmallVector<cl::OptionEnumValue> {
  using BaseType = SmallVector<cl::OptionEnumValue>;

public:
  template <class Opt> void apply(Opt &O) const {
    for (const auto &Value : *this)
      O.getParser().addLiteralOption(Value.Name, Value.Value,
                                     Value.Description);
  }

  using BaseType::push_back;
};

namespace detail {
struct EnumClMapper : public EnumMapper {
  EnumClMapper(ClEnumValues &Vals) : ClValues(Vals) {}

  void enumCase(int Value, const char *Name,
                const char *Description) const override {
    ClValues.push_back(clEnumValN(Value, Name, Description));
  }

  ClEnumValues &ClValues;
};

template <typename T, typename YIO = yaml::IO>
struct EnumYAMLMapper : public EnumMapper {
  EnumYAMLMapper(YIO &Io, T &EnumVal) : TheIo(Io), TheEnumVal(EnumVal) {}

  void enumCase(int Value, const char *Name,
                const char *Description) const override {
    TheIo.enumCase(TheEnumVal, Name, static_cast<T>(Value));
  }

  YIO &TheIo;
  T &TheEnumVal;
};
} // namespace detail

template <typename DerivedT> struct EnumOptionMixin {
  static ClEnumValues getClValues() {
    ClEnumValues Vals;
    detail::EnumClMapper M(Vals);
    DerivedT::doMapping(M);
    return Vals;
  }

  template <typename YIO, typename T> static void mapYAML(YIO &Io, T &Val) {
    detail::EnumYAMLMapper<T> M(Io, Val);
    DerivedT::doMapping(M);
  }
};

#define LLVM_SNIPPY_OPTION_DEFINE_ENUM_OPTION_YAML(Enum, OptionClass)          \
  LLVM_SNIPPY_YAML_DECLARE_SCALAR_ENUMERATION_TRAITS(Enum);                    \
  void yaml::ScalarEnumerationTraits<Enum>::enumeration(yaml::IO &IO,          \
                                                        Enum &Val) {           \
    OptionClass::mapYAML(IO, Val);                                             \
  }

} // namespace snippy

LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS(snippy::OptionsMappingWrapper);
LLVM_SNIPPY_YAML_DECLARE_CUSTOM_MAPPING_TRAITS(snippy::OptionsStorage);

} // namespace llvm
