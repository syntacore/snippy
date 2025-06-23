include "snippy/Support/Options.td"

def Options : OptionGroup<"options", true> { let StructName = "DriverOptions"; }

def LayoutIncludeDirectories
    : CommaSeparatedList<"layout-include-dir", StringType>,
      Description<"Extra directory where to look for include files">,
      Group<Options>;

def ListOpcodeNames : Flag<"list-opcode-names">,
                      Description<"List available opcode names">,
                      Group<Options>;

def OutputFileBasename : StringOption<"o">,
                         Description<"Override output file base name">,
                         ValueDescription<"filename">,
                         Group<Options>;

def GeneratorPluginFile
    : StringOption<"generator-plugin">,
      Description<
          "Plugin for custom instruction generation. Use =None to generate "
          "instructions with build-in histogram. (=None - default value)">,
      ValueDescription<"filename">,
      DefaultValue<"None">,
      Group<Options>;

def GeneratorPluginParserFile
    : StringOption<"plugin-info-file">,
      Description<"File with info to parse with plugin generator. Use =None to "
                  "parse histogram with build-in Snippy parser.">,
      ValueDescription<"filename">,
      DefaultValue<"None">,
      Group<Options>;
