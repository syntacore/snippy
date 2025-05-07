include "snippy/Support/Options.td"

def Options : OptionGroup<"options", true>;

// FIXME: Unlike clang, march here is not something like rv64gc (provided by
// attrs and CPU). Instead it is something like target (try riscv64-linux-gnu).
def MArch : StringOption<"march">,
            Description<"Target architecture">,
            ValueDescription<"triple">,
            Group<Options>;

def CpuName : StringOption<"mcpu">,
              Description<"CPU name to use, leave empty to autodetect">,
              Group<Options>;

def MAttr : StringOption<"mattr">,
            Description<"Comma-separated list of target architecture features">,
            ValueDescription<"+feature1,-feature2,...">,
            Group<Options>;

def ABI : StringOption<"mabi">,
          Description<"Custom abi for output elf-files">,
          Group<Options>;

def LayoutIncludeDirectories
    : CommaSeparatedList<"layout-include-dir", StringType>,
      Description<"Extra directory where to look for include files">,
      Group<Options>;

def ListOpcodeNames : Flag<"list-opcode-names">,
                      Description<"List available opcode names">,
                      Group<Options>;

def DumpMF : Flag<"dump-mf">,
             Description<"Dump final generated machine function in Machine IR">,
             Group<Options>;

def DumpMI : Flag<"dump-mi">,
             Description<"Dump generated machine instructions in Machine IR">,
             Group<Options>;

def DumpCFG : Flag<"dump-cfg">,
              Description<"Dump generated CFG">,
              Group<Options>;

def Seed
    : StringOption<"seed">,
      Description<"Seed for instruction generation. If the option is not used "
                  "or its value is not set, seed will be generated randomly.">,
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

def DumpLayout : Flag<"dump-layout">,
                 Description<"Dump the whole snippy configuration YAML">,
                 Group<Options>;

def DumpOptions : Flag<"dump-options">,
                  Description<"Dump snippy options' values">,
                  Group<Options>;

def DumpPreprocessedConfig
    : Flag<"E">,
      Description<"Dump snippy config after preprocessing it">,
      Group<Options>;
