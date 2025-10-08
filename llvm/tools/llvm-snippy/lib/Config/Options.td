include "snippy/Support/Options.td"

def ProgramOptionsCategory : OptionGroup<"Snippy Program Level Options", true> {
  let StructName = "ProgramOptions";
}

let Group = ProgramOptionsCategory in {
  // Deprecated for setting up target triple. Replaced with toolchain
  // conventional march (like rv32gc)
  def MArch : StringOption<"march">,
              Description<"Target architecture">,
              ValueDescription<"One of suppported archs: RISCV, etc..">;

  def MTargetTriple
      : StringOption<"mtriple">,
        Description<"Target triple">,
        ValueDescription<"One of suppported triple: riscv64-linux-gnu, etc..">;

  def CpuName : StringOption<"mcpu">,
                Description<"CPU name to use, leave empty to autodetect">;

  def MAttr
      : StringOption<"mattr">,
        Description<"Comma-separated list of target architecture features">,
        ValueDescription<"+feature1,-feature2,...">;

  def ABI : StringOption<"mabi">,
            Description<"Custom abi for output elf-files">;

  def Seed : StringOption<"seed">,
             Description<
                 "Seed for instruction generation. If the option is not used "
                 "or its value is not set, seed will be generated randomly.">;

  def RedefineSP : StringOption<"redefine-sp">,
                   Description<"Specify the reg to use as a stack pointer">,
                   DefaultValue<"any">;
  
  def PreserveCallerSavedRegs
      : CommaSeparatedList<"preserve-caller-saved-regs", StringType>,
        Description<
            "Preserves caller-saved registers on the stack around calls to "
             "external functions">;

  def FollowTargetABI
      : Flag<"honor-target-abi">,
        Description<
            "Automatically spill registers that are required to be preserved "
            "by snippet execution to follow ABI calling conventions">;

  def ExternalStack : Flag<"external-stack">,
                      Description<"Snippy will assume that stack pointer is "
                                  "pre-initialized externally">;

  def MangleExportedNames
      : Flag<"mangle-exported-names">,
        Description<"Enable mangling of exported symbols and section names "
                    "using snippet main function name">;

  def EntryPointName : StringOption<"entry-point">,
                       Description<"Override entry point name">,
                       ValueDescription<"label name">,
                       DefaultValue<"SnippyFunction">;

  def InitialRegisterDataFile : StringOption<"initial-regs">,
                                Description<"File for initial registers state">;
  def InitialRegisterYamlFile
      : Alias<"initial-regs-yaml", InitialRegisterDataFile>,
        Description<"Alias for -initial-regs">;

  def SpilledRegisterList
      : CommaSeparatedList<"spilled-regs-list", StringType>,
        Description<"List of registers that shall be spilled before snippet "
                    "execution and restored after">;

  def ReservedRegsList
      : CommaSeparatedList<"reserved-regs-list", StringType>,
        Description<"List of registers that shall not be used om snippet code">;

  def SkipLegacySPSpill
      : Flag<"skip-legacy-sp-spill">,
        Description<"Skips legacy stack pointer spill when 'honor-target-abi' is disabled">,
        Hidden;
}

def ModelOptionsCategory : OptionGroup<"Snippy Model Options", true> {
  let StructName = "ModelOptions";
}

let Group = ModelOptionsCategory in {
  def ModelPluginFile
      : StringOption<"model-plugin">,
        Description<"Primary model plugin to use for snippet generation. Use "
                    "'None' to disable snippet execution on the model">,
        ValueDescription<"alias|filename">,
        DefaultValue<"None">;

  def CoSimModelPluginFilesList
      : CommaSeparatedList<"cosim-model-plugins", StringType>,
        Description<"Comma separated list of hardware model plugins to use for "
                    "co-simulation">;

}


def RegInitOptionsCategory
    : OptionGroup<"Snippy Register Initialization Options", true> {
  let StructName = "RegInitOptions";
}

let Group = RegInitOptionsCategory in {
  def InitRegsInElf
      : Flag<"init-regs-in-elf">,
        Description<"include registers initialization in final elf file">;

  def DumpInitialRegisters
      : StringOption<"dump-initial-registers-yaml">,
        Description<"Request dump of an initial register file state">,
        ValueDescription<"filename">,
        DefaultValue<"none"> {
    let ValueOptional = true;
    let Callback = [{
      if(DumpInitialRegistersValue.empty()) 
        DumpInitialRegisters = DumpInitialRegistersValue;
    }];
  }

  def DumpResultingRegisters
      : StringOption<"dump-registers-yaml">,
        Description<"Request dump of the file register file state">,
        ValueDescription<"filename">,
        DefaultValue<"none"> {
    let ValueOptional = true;
    let Callback = [{
      if(DumpResultingRegistersValue.empty())
        DumpResultingRegisters = DumpResultingRegistersValue;
    }];
  }

  def ValueGramRegsDataFile
      : StringOption<"valuegram-operands-regs">,
        Description<"Set values in operands registers before each instruction "
                    "according to the file. It supported only if a number of "
                    "instructions are generated">;

  def ValuegramOperandsRegsInitOutputs
      : Flag<"valuegram-operands-regs-init-outputs">,
        Description<
            "Turning on initialization of destination registers before each "
            "non-service instruction. Requires -valuegram-operands-regs">,
        DefaultValue<"false">, Hidden;
}

def InstrGenOptionsCategory
    : OptionGroup<"Snippy Instruction Generation Options", true> {
  let StructName = "InstrGenOptions";
}

let Group = InstrGenOptionsCategory in {
  def NumInstrs : StringOption<"num-instrs">,
                  Description<"Number of instructions to generate">,
                  DefaultValue<"10">;

  def LastInstr
      : StringOption<"last-instr">,
        Description<
            "Custom choice of the last instruction. Use 'RET' to emit return">,
        DefaultValue<"EBREAK">;

  def ChainedRXSectionsFill
      : Flag<"chained-rx-sections-fill">,
        Description<"Span the generated code across all provided RX sections. "
                    "When disabled only one of all provided RX sections is "
                    "used to generate code to">;

  def ChainedRXChunkSize
      : IntegerOption<"chained-rx-chunk-size", "size_t">,
        Description<"Slice main function in blocks of specified size, "
                    "distribute over all RX sections and randomly link them">,
        DefaultValue<"0">;

  def ChainedRXSorted
      : Flag<"chained-rx-sorted">,
        Description<"Sort RX sections by their ID alphabetically when "
                    "generating chained execution routine">;

  def Backtrack
      : Flag<"backtrack">,
        Description<"Enable backtracking facilities so as not to generate "
                    "errorneous code (e.g. division by zero)">;

  def Selfcheck : StringOption<"selfcheck">,
                  Description<"Enable full or partial selfcheck with step N "
                              "(means each N instructions)">,
                  ValueDescription<"N">,
                  DefaultValue<"none"> {
    let ValueOptional = true;
  }

  def VerifyMachineInstrs
      : Flag<"verify-mi">,
        Description<"Enables verification of generated machine instructions">;

  def AddressVHOpt : Flag<"enable-address-value-hazards">,
                     Description<"Form address values based on dynamic "
                                 "register values (requires backtracking)">;

  def SelfcheckRefValueStorage
      : EnumOption<"selfcheck-ref-value-storage",
                   "SelfcheckMode">,
        Description<"Option to define how to get reference selfcheck values">,
        DefaultValue<"SelfcheckMode::Code"> {

    let AdditionalArgs = "SelfcheckModeEnumOption::getClValues()";
  }
}

def DebugOptionsCategory : OptionGroup<"debug options", true> {
  let StructName = "DebugOptions";
}
let Group = DebugOptionsCategory in {
  def DumpMF
      : Flag<"dump-mf">,
        Description<"Dump final generated machine function in Machine IR">;

  def DumpMI : Flag<"dump-mi">,
               Description<"Dump generated machine instructions in Machine IR">;

  def DumpCFG : Flag<"dump-cfg">, Description<"Dump generated CFG">;

  def Verbose : Flag<"verbose">, Description<"Show verbose output"> {
    let Callback = [{
      if (VerboseValue)
        DumpLayout = DumpMF = DumpMI = true;
    }];
  }

  def ViewCFG : Flag<"view-cfg">,
                Description<"View generated CFG"> {
    let Callback = "if(ViewCFGValue) DumpCFG = true;";
  }
  
  def DumpLayout : Flag<"dump-layout">,
                   Description<"Dump the whole snippy configuration YAML">;
  
  def DumpOptions : Flag<"dump-options">,
                    Description<"Dump snippy options' values">;
  
  def DumpPreprocessedConfig
      : Flag<"E">,
        Description<"Dump snippy config after preprocessing it">;
}
