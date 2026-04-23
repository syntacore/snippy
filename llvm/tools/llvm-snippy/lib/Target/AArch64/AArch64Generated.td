include "AArch64.td"

class SnippyFunctionPredicate<string name, MCStatement body>
    : FunctionPredicateBase<name, body>;

class SnippyFunctionPositivePredicate<string Name, list<Instruction> opcodes>
    : SnippyFunctionPredicate<
          Name, MCOpcodeSwitchStatement<
                    [MCOpcodeSwitchCase<opcodes, MCReturnStatement<TruePred>>],
                    MCReturnStatement<FalsePred>>>;

def IsLoadStoreFn
    : SnippyFunctionPositivePredicate<"isLoadStore",
                                      IsLoadStoreRegOffsetOp.ValidOpcodes>;

def IsCallOp : CheckOpcode<[BL, BLR]>;

def IsCallFn : SnippyFunctionPositivePredicate<"isCall", IsCallOp.ValidOpcodes>;

// Auto-generated list from *.td file
// Heuristic to generate:
// for CodeGenInstruction *I : Target.getInstructionsByEnumValue()
// if I from AArch64 namespace and not pseudo:
//    auto Uses = I->ImplicitUses;


def IsSPRelativeOp
    : CheckOpcode<[AUTIASPPCi, AUTIASPPCr, AUTIBSP, AUTIBSPPCi, AUTIBSPPCr,
                   PACIASP, PACIASPPC, PACIBSP, PACIBSPPC, PACNBIASPPC,
                   PACNBIBSPPC, RETAA, RETAASPPCi, RETAASPPCr, RETAB,
                   RETABSPPCi, RETABSPPCr, ERETAA, ERETAB]>;

def IsSPRelativeFn
    : SnippyFunctionPositivePredicate<"isSPRelative",
                                      IsSPRelativeOp.ValidOpcodes>;
