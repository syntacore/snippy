include "snippy/Support/Options.td"

def DiagnosticOptions : OptionGroup<"llvm-diagnostic", true>;

def WError : CommaSeparatedList<"Werror", StringType>,
             Description<"Comma separated list of warning types that should be "
                         "treated as errors">,
             Group<DiagnosticOptions> {
  let ValueOptional = true;
}

def WNoError : CommaSeparatedList<"Wno-error", StringType>,
               Description<"Comma separated list of warning types that should "
                           "be excluded from WError option">,
               Group<DiagnosticOptions> {
  let ValueOptional = true;
}

def WDisable : CommaSeparatedList<"Wdisable", StringType>,
               Description<"Comma-separated list of warning types to suppress">,
               Group<DiagnosticOptions>;
