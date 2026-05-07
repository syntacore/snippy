//===-- Scheduling.cpp ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/Scheduling.h"
#include "snippy/Support/Options.h"
#include "snippy/Support/YAMLUtils.h"

namespace llvm {
namespace snippy {

// NOTE: This is a indirect dependency of the options category. Arguably this
// is still better than having divergent options as well as config for
// controlling random scheduling.
extern cl::OptionCategory Options;

static snippy::opt<bool> RandomSchedulingOpt(
    "random-scheduling",
    cl::desc("Apply random scheduling pass for generated code. It doesn't "
             "change the behaviour of program"),
    cl::cat(Options), cl::init(false));

SchedulingSettings::SchedulingSettings()
    : Enabled(RandomSchedulingOpt.isSpecified()
                  ? std::optional{RandomSchedulingOpt.getValue()}
                  : std::nullopt) {}

} // namespace snippy

using snippy::SchedulingSettings;
void yaml::MappingTraits<SchedulingSettings>::mapping(yaml::IO &Io,
                                                      SchedulingSettings &Cfg) {
  using snippy::RandomSchedulingOpt;

  Io.mapOptional("enabled", Cfg.Enabled);
  if (auto E = snippy::diagnoseIfOptionAndOptionalAreBothSet(
          Cfg.Enabled, RandomSchedulingOpt, "enabled"))
    Io.setError(toString(std::move(E)));

  Io.mapOptional("max-region-size", Cfg.MaxRegionSize,
                 Cfg.kDefaultMaxRegionSize);
}

std::string
yaml::MappingTraits<SchedulingSettings>::validate(yaml::IO &IO,
                                                  SchedulingSettings &Cfg) {
  if (auto E = snippy::diagnoseIfOptionAndOptionalAreBothSet(
          Cfg.Enabled, snippy::RandomSchedulingOpt, "enabled"))
    return toString(std::move(E));
  return "";
}

} // namespace llvm
