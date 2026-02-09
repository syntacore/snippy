//===-- Options.cpp ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Support/Options.h"
#include "snippy/Support/DiagnosticInfo.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Regex.h"

namespace llvm {
using namespace snippy;

void yaml::MappingTraits<OptionsMappingWrapper>::mapping(
    yaml::IO &IO, OptionsMappingWrapper &) {
  IO.mapOptional("options", OptionsStorage::instance());
}

void yaml::CustomMappingTraits<OptionsStorage>::inputOne(
    yaml::IO &IO, StringRef Key, OptionsStorage &Options) {
  auto KeyStr = Key.str();
  if (!Options.count(KeyStr))
    snippy::fatal(formatv("Unknown option \"{0}\" was specified in YAML", Key));
  auto &Base = Options.get(KeyStr);
  if (Base.isSpecified())
    snippy::fatal(
        formatv("Attempt to specify option (or its alias) \"{0}\" twice", Key));
  Base.mapStoredValue(IO, KeyStr);
  Base.markAsSpecified();
}

void yaml::CustomMappingTraits<OptionsStorage>::output(
    yaml::IO &IO, OptionsStorage &Options) {
  for (auto &&Base : Options)
    Base.second->mapStoredValue(IO, Base.first);
}
void CommandOptionBase::mapStoredValue(yaml::IO &IO,
                                       std::optional<StringRef> Key) {
  if (!Key.has_value())
    doMappingWithKey(IO, Name);
  else
    doMappingWithKey(IO, Key.value());
  if (!isSpecified() && !IO.outputting())
    markAsSpecified();
}

bool cl::parser<snippy::RegexOption>::parse(cl::Option &O, StringRef ArgName,
                                            StringRef Arg,
                                            snippy::RegexOption &Opt) {
  auto MaybeValidRegex = Regex{Arg.str()};
  if (!MaybeValidRegex.isValid())
    return O.error(Twine("invalid regex: '").concat(Arg.str()).concat("': "));
  Opt = RegexOption{
      std::string(Arg),
      std::make_shared<Regex>(std::move(MaybeValidRegex)),
  };
  return false;
}

std::string
yaml::ScalarTraits<snippy::RegexOption>::input(StringRef Input, void *,
                                               snippy::RegexOption &Val) {
  auto MaybeRegex = Regex{Input};
  if (!MaybeRegex.isValid())
    return Twine("invalid regex: '").concat(Input).concat("'").str();

  Val = RegexOption{
      std::string(Input),
      std::make_shared<Regex>(std::move(MaybeRegex)),
  };

  return "";
}

void yaml::ScalarTraits<snippy::RegexOption>::output(
    const snippy::RegexOption &Val, void *, raw_ostream &OS) {
  OS << Val.Str;
}

yaml::QuotingType
yaml::ScalarTraits<snippy::RegexOption>::mustQuote(StringRef) {
  return QuotingType::Double;
}

} // namespace llvm
