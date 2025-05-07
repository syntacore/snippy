//===--- Options.td -------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef SNIPPY_SUPPORT_OPTIONS_TD
#define SNIPPY_SUPPORT_OPTIONS_TD

// Define the kinds of options.
class OptionKind<string name> { string Name = name; }

// An option group.
def KindGroup : OptionKind<"Group">;
// An option followed by its values. Represented by a sequence value in YAML
// format.
def KindList : OptionKind<"List">;
// An option which is which takes multiple (separate) arguments. Scalar value
// in YAML format.
def KindPlain : OptionKind<"Plain">;
// Alias option.
def KindAlias : OptionKind<"Alias">;

// Define the option group class.
class OptionGroup<string name, bit external> {
  string Name = name;
  bit External = external;
  string Description = ?;
  OptionGroup Group = ?;
}

// Define option types.

class OptionType<code type> { code Type = type; }

def FlagType : OptionType<"bool">;
def StringType : OptionType<"std::string">;

// Define the option class.

class Option<string name, OptionKind kind, OptionType type> {
  string Name = name;
  OptionKind Kind = kind;
  OptionType Type = type;
  OptionGroup Group = ?;
  Option Alias = ?;
  bit Hidden = false;
  string Description = ?;
  string ValueDescription = ?;
  code DefaultValue = ?;
  bit CommaSeparated = ?;
  code Callback = ?;
}

// Helpers for defining options.

class Flag<string name> : Option<name, KindPlain, FlagType>;

class StringOption<string name> : Option<name, KindPlain, StringType>;

class List<string name, OptionType type> : Option<name, KindList, type>;

class Alias<string name, Option aliasto> : Option<name, KindAlias, ?> {
  let Alias = aliasto;
}

// Mix-ins for adding optional attributes.

class Group<OptionGroup group> { OptionGroup Group = group; }

class Description<string description> { string Description = description; }

class ValueDescription<string description> {
  string ValueDescription = description;
}

class Hidden { bit Hidden = true; }

class DefaultValue<code defaultvalue> { string DefaultValue = defaultvalue; }

class CommaSeparated { bit CommaSeparated = true; }

// Helper classes for conciseness.

class CommaSeparatedList<string name, OptionType type>
    : Option<name, KindList, type>, CommaSeparated;

#endif // SNIPPY_SUPPORT_OPTIONS_TD
