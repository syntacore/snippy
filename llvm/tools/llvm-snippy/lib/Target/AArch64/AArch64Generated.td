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

// Unscaled-immediate (LDUR/STUR family, signed 9-bit offset, not pre/post).
def IsUnscaledMemOp
    : CheckOpcode<[LDURXi, LDURWi, LDURSWi,
                   LDURHHi, LDURSHXi, LDURSHWi,
                   LDURBBi, LDURSBXi, LDURSBWi,
                   LDURDi, LDURSi, LDURHi, LDURBi, LDURQi,
                   STURXi, STURWi, STURHHi, STURBBi,
                   STURDi, STURSi, STURHi, STURBi, STURQi]>;
def IsUnscaledMemFn
    : SnippyFunctionPositivePredicate<"isUnscaledMem",
                                      IsUnscaledMemOp.ValidOpcodes>;

// Pair load/store (LDP/STP, signed 7-bit scaled offset, not pre/post).
def IsPairMemOp
    : CheckOpcode<[LDPXi, LDPWi, LDPSWi, LDNPXi, LDNPWi,
                   LDPDi, LDPSi, LDPQi, LDNPDi, LDNPSi, LDNPQi,
                   STPXi, STPWi, STNPXi, STNPWi,
                   STPDi, STPSi, STPQi, STNPDi, STNPSi, STNPQi]>;
def IsPairMemFn
    : SnippyFunctionPositivePredicate<"isPairMem",
                                      IsPairMemOp.ValidOpcodes>;
