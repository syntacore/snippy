include "snippy/Support/Options.td"

def Options : OptionGroup<"options", true> {
  let StructName = "DriverOptions";
}

def DebugOptionsCategory : OptionGroup<"debug options", true> {
  let StructName = "DebugOptions";
}

// Deprecated for setting up target triple. Replaced with toolchain conventional
// march (like rv32gc)
def MArch : StringOption<"march">,
            Description<"Target architecture">,
            ValueDescription<"one of suppported archs: RISCV, etc..">,
            Group<Options>;

def MTargetTriple : StringOption<"mtriple">,
                    Description<"Target triple">,
                    ValueDescription<"one of suppported triple: riscv64-linux-gnu, etc..">,
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
             Group<DebugOptionsCategory>;

def DumpMI : Flag<"dump-mi">,
             Description<"Dump generated machine instructions in Machine IR">,
             Group<DebugOptionsCategory>;

def DumpCFG : Flag<"dump-cfg">,
              Description<"Dump generated CFG">,
              Group<DebugOptionsCategory>;

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

def Verbose : Flag<"verbose">,
              Description<"Show verbose output">,
              Group<DebugOptionsCategory> {
  let Callback = [{
    if (Verbose)
      DumpLayout = DumpMF = DumpMI = true;
  }];
}

def ViewCFG : Flag<"view-cfg">,
              Description<"View generated CFG">,
              Group<DebugOptionsCategory> {
  let Callback = "if(ViewCFG) DumpCFG = true;";
}
