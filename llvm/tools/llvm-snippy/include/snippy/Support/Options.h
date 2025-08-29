//===-- Options.h -----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Support/DiagnosticInfo.h"
#include "snippy/Support/YAMLUtils.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/YAMLTraits.h"

#include <memory>
#include <string_view>
#include <unordered_map>

namespace llvm {
namespace snippy {

/// \class CommandOptionBase
/// \brief Non-template class for command options of different types.
/// Manages option's name and tracks whether option was specified in either
/// command line or YAML file. Provides interface for yaml mapping.
class CommandOptionBase {
  // Name is always a static lifetime string literal because snippy::opt only
  // accepts `const char *`. This makes lookup work without dynamic
  // allocations.
  StringRef Name;
  bool Specified = false;

public:
  CommandOptionBase(StringRef Key) : Name(Key) {}

  virtual ~CommandOptionBase() {}

  void markAsSpecified() { Specified = true; }

  bool isSpecified() const { return Specified; }

  StringRef getName() const { return Name; }

  void mapStoredValue(yaml::IO &IO,
                      std::optional<StringRef> Key = std::nullopt);

  std::unique_ptr<CommandOptionBase> clone() const { return doClone(); }

private:
  virtual void doMappingWithKey(yaml::IO &IO, StringRef Key) = 0;
  virtual std::unique_ptr<CommandOptionBase> doClone() const = 0;
};

/// \class CommandOption
/// \brief Stores value of concrete DataType.
/// Knows how to map its value to yaml and how to clone itself.
template <typename DataType> struct CommandOption : public CommandOptionBase {
  DataType Val = DataType();

  CommandOption(StringRef Name) : CommandOptionBase(Name) {}

  void doMappingWithKey(yaml::IO &IO, StringRef Key) override {
    IO.mapRequired(Key.data(), Val);
  }

  std::unique_ptr<CommandOptionBase> doClone() const override {
    return std::make_unique<CommandOption<DataType>>(*this);
  }
};

/// \class OptionsStorage
/// \brief Singleton class that stores CommandOptions by pointer to base class
class OptionsStorage final {
  struct StringRefHasher {
    std::hash<std::string_view> Hasher;
    std::size_t operator()(StringRef Key) const {
      return Hasher(std::string_view(Key.data(), Key.size()));
    }
  };

  static_assert(std::is_default_constructible_v<StringRefHasher>);

  using StorageType =
      std::unordered_map<StringRef, std::shared_ptr<CommandOptionBase>,
                         StringRefHasher>;
  StorageType Data;
  OptionsStorage(){};

  auto find(StringRef Key) { return Data.find(Key); }

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
  auto count(StringRef Key) const { return Data.count(Key); }
  bool empty() const { return Data.empty(); }
  auto size() const { return Data.size(); }

  CommandOptionBase &get(StringRef Key) {
    auto Found = find(Key);
    assert(Found != Data.end() && "Unknown key");
    return *Found->second;
  }

private:
  // These classes has to be able to use 'allocateLocation' and 'getLocation'
  // member functions of OptionsStorage. But we don't want these functions to be
  // accessible by everyone. So we made them private and added opt and opt_list
  // as friends.
  template <typename T> friend class opt;
  template <typename T> friend class opt_list;
  friend class aliasopt;

  auto insertNewOption(StringRef Key, std::shared_ptr<CommandOptionBase> Val) {
    auto Found = find(Key);
    if (Found != end())
      return std::make_pair(Found, false);
    return Data.emplace(std::pair{Key, std::move(Val)});
  }

  template <typename T> T &allocateLocation(StringRef Name) {
    auto Tmp = std::make_unique<CommandOption<T>>(Name);
    auto [It, WasInserted] = insertNewOption(Name, std::move(Tmp));
    if (!WasInserted) {
      LLVMContext Ctx;
      snippy::fatal(Ctx, "Inconsistent options",
                    "Duplicated option name \"" + Twine(Name) + "\"");
    }
    return static_cast<CommandOption<T> &>(*It->second).Val;
  }

  template <typename T> T &getLocation(StringRef Name) {
    auto Found = find(Name);
    if (Found == Data.end()) {
      LLVMContext Ctx;
      snippy::fatal(Ctx, "Inconsistent options",
                    "Attempt to get the location of option \"" + Twine(Name) +
                        "\" that does not exist.");
    }
    return static_cast<CommandOption<T> &>(*Found->second).Val;
  }
};

/// \class opt
/// \brief cl::opt wrapper
/// Modifies OptionsStorage and therefore creation of snippy::opt class entity
/// makes yaml parser outomatically able to parse that option
template <typename DataType> class opt : public cl::opt<DataType, true> {
  using BaseTy = cl::opt<DataType, true>;

public:
  using BaseTy::ArgStr;
  using BaseTy::getValue;
  using BaseTy::setValue;

  template <typename... Modes>
  opt(const char *Name, Modes &&...Ms)
      : BaseTy(StringRef(Name),
               cl::location(
                   OptionsStorage::instance().allocateLocation<DataType>(Name)),
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

  static ValueTy &getLocation(StringRef Key) {
    return OptionsStorage::instance().getLocation<ValueTy>(Key.data());
  }

public:
  using BaseTy::ArgStr;

  template <typename... Modes>
  opt_list(const char *Name, Modes &&...Ms)
      : BaseTy(StringRef(Name),
               cl::location(
                   OptionsStorage::instance().allocateLocation<ValueTy>(Name)),
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
  auto &operator[](unsigned Idx) {
    assert(Idx < size());
    return getLocation(ArgStr.data())[Idx];
  }
  const auto &operator[](unsigned Idx) const {
    assert(Idx < size());
    return OptionsStorage::instance().getLocation(ArgStr.data())[Idx];
  }

  bool isSpecified() const {
    return OptionsStorage::instance().get(ArgStr.data()).isSpecified();
  }
};

using cl::alias;
// struct to be used instead of cl::aliasopt to make your option visible in YAML
class aliasopt final : private cl::aliasopt {
public:
  explicit aliasopt(cl::Option &O) : cl::aliasopt(O) {}

  void apply(cl::alias &A) const {
    auto &Key = Opt.ArgStr;
    auto &Options = OptionsStorage::instance();
    auto Found = Options.find(Key);
    if (Found == Options.end()) {
      LLVMContext Ctx;
      snippy::fatal(Ctx, "Inconsistent options",
                    "Alias to unknown option \"" + Twine(Key) + "\"");
    }
    Options.insertNewOption(A.ArgStr, Found->second);
    cl::aliasopt::apply(A);
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
                        const char *Description) = 0;

  virtual ~EnumMapper() {}
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

  void enumCase(int Value, const char *Name, const char *Description) override {
    ClValues.push_back(clEnumValN(Value, Name, Description));
  }

  ClEnumValues &ClValues;
};

template <typename T, typename YIO = yaml::IO>
struct EnumYAMLMapper : public EnumMapper {
  EnumYAMLMapper(YIO &Io, T &EnumVal) : TheIo(Io), TheEnumVal(EnumVal) {}

  void enumCase(int Value, const char *Name, const char *Description) override {
    TheIo.enumCase(TheEnumVal, Name, static_cast<T>(Value));
  }

  YIO &TheIo;
  T &TheEnumVal;
};

template <typename T> struct ToStringMapper : public EnumMapper {
  ToStringMapper(const T &Val) : Val(Val) {}

  void enumCase(int Value, const char *Name, const char *) override {
    if (Val == static_cast<T>(Value))
      Result = StringRef(Name);
  }

  std::optional<StringRef> Result;
  const T &Val;
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
  template <typename T> static std::optional<StringRef> toString(T &Val) {
    detail::ToStringMapper<T> M(Val);
    DerivedT::doMapping(M);
    return M.Result;
  }
};

#define LLVM_SNIPPY_OPTION_DEFINE_ENUM_OPTION_YAML_NO_DECL(Enum, OptionClass)  \
  void yaml::ScalarEnumerationTraits<Enum>::enumeration(yaml::IO &IO,          \
                                                        Enum &Val) {           \
    OptionClass::mapYAML(IO, Val);                                             \
  }

#define LLVM_SNIPPY_OPTION_DEFINE_ENUM_OPTION_YAML(Enum, OptionClass)          \
  LLVM_SNIPPY_YAML_DECLARE_SCALAR_ENUMERATION_TRAITS(Enum);                    \
  LLVM_SNIPPY_OPTION_DEFINE_ENUM_OPTION_YAML_NO_DECL(Enum, OptionClass)

/// @brief Helper function to check whether an option has been set and a
/// std::optional of the same type holds a value. Use for unified handling of
/// YAML options that depend both on the snippy::opt and the Config.
template <typename T>
Error diagnoseIfOptionAndOptionalAreBothSet(const std::optional<T> &Val,
                                            const snippy::opt<T> &Opt,
                                            StringRef Name,
                                            StringRef QuoteSep = "'") {
  if (Val.has_value() && Opt.isSpecified())
    return createStringError(inconvertibleErrorCode(),
                             Twine(QuoteSep)
                                 .concat(Opt.ArgStr)
                                 .concat(QuoteSep)
                                 .concat(" has been specified both as an "
                                         "option and as a configuration field ")
                                 .concat(QuoteSep)
                                 .concat(Name)
                                 .concat(QuoteSep));
  return Error::success();
}
} // namespace snippy

LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS(snippy::OptionsMappingWrapper);
LLVM_SNIPPY_YAML_DECLARE_CUSTOM_MAPPING_TRAITS(snippy::OptionsStorage);

} // namespace llvm
