#include "snippy/Config/OpcodeHistogramVisitor.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include <unordered_map>

namespace {

using namespace llvm::snippy;
using OpcCategory = OpcodeNode::OpcodeCategory;

enum Opcodes { ADD, SUB, MUL, DIV, MOV, AND };

// Logic for calculating weights and final probability:
// - The weight of a ChoiceNode (marked with |) is distributed equally among the
// expressions separated by |.
// - For a CartesianNode (marked with *), the weight of its children is equal to
// the weight of the CartesianNode itself.
// - The weight for a RepeatNode argument
// (denoted by ^) is calculated as the weight of the RepeatNode itself
// multiplied by the repetition count of that argument.
// - The total weight is calculated as the sum of the weights of all opcodes.
// The probability of an individual opcode is calculated as the opcode's weight
// divided by the total weight.
TEST(OpcodeHistogramVisitor, CheckWeightMethodSimple) {
  //                         OpcodesTree
  //     -------------------------------------------------------
  //     |          |         |          |          |          |
  // ADD[2.0]   SUB[1.0]   MUL[3.0]   DIV[1.0]   MOV[1.0]   AND[2.0]

  ChoiceNode OpcodesTree;
  // Building OpcodesTree above
  // TotalWeight = 10.0
  OpcodesTree.emplace<OpcodeNode>(ADD, 2.0);
  OpcodesTree.emplace<OpcodeNode>(SUB, 1.0);
  OpcodesTree.emplace<OpcodeNode>(MUL, 3.0);
  OpcodesTree.emplace<OpcodeNode>(DIV, 1.0);
  OpcodesTree.emplace<OpcodeNode>(MOV, 1.0);
  OpcodesTree.emplace<OpcodeNode>(AND, 2.0);
  OpcodeProbVisitor Vis(OpcodesTree);

  EXPECT_DOUBLE_EQ(Vis.getProbability(ADD), 0.2);
  EXPECT_DOUBLE_EQ(Vis.getProbability(SUB), 0.1);
  EXPECT_DOUBLE_EQ(Vis.getProbability(MUL), 0.3);
  EXPECT_DOUBLE_EQ(Vis.getProbability(DIV), 0.1);
  EXPECT_DOUBLE_EQ(Vis.getProbability(MOV), 0.1);
  EXPECT_DOUBLE_EQ(Vis.getProbability(AND), 0.2);
}

TEST(OpcodeHistogramVisitor, CheckWeightMethodMedium) {
  //            OpcodesTree
  //  --------------------------------
  //        |                   |
  //    [MULNODE][2.0]        MUL[5.0]
  //  -------------
  //  |  *  |  *  |
  // ADD   ADD [ORNODE]
  //           --------
  //           |      |
  //          MOV    AND
  //
  // Weigths:
  // MUL -> 5.0
  // ADD -> 2.0 + 2.0 = 4.0 (two ADD opcodes)
  // MOV -> 1.0
  // AND -> 1.0
  // TotalWeight = 5.0 + 4.0 + 1.0 + 1.0 = 11.0
  ChoiceNode OpcodesTree;
  // Building OpcodesTree above
  ChoiceNode MovOrAnd;
  MovOrAnd.emplace<OpcodeNode>(MOV, 1.0);
  MovOrAnd.emplace<OpcodeNode>(AND, 1.0);
  CartesianNode AddAddMovOrAnd;
  AddAddMovOrAnd.emplace<OpcodeNode>(ADD, 1.0);
  AddAddMovOrAnd.emplace<OpcodeNode>(ADD, 1.0);
  AddAddMovOrAnd.insert(MovOrAnd.clone());
  OpcodesTree.emplace<HistogramNode>("MULNODE", AddAddMovOrAnd.clone(), 2.0);
  OpcodesTree.emplace<OpcodeNode>(MUL, 5.0);
  OpcodeProbVisitor Vis(OpcodesTree);
  double TotalWeight = 11.0;
  double ADDProbability = 4.0 / TotalWeight;
  double MULProbability = 5.0 / TotalWeight;
  double MOVProbability = 1.0 / TotalWeight;
  double ANDProbability = 1.0 / TotalWeight;
  double DIVProbability = 0.0;
  double SUBProbability = 0.0;
  std::unordered_map<unsigned, double> OpcProb;
  OpcProb[ADD] = ADDProbability;
  OpcProb[MUL] = MULProbability;
  OpcProb[MOV] = MOVProbability;
  OpcProb[AND] = ANDProbability;
  OpcProb[DIV] = DIVProbability;
  OpcProb[SUB] = SUBProbability;
  EXPECT_DOUBLE_EQ(Vis.getProbability(ADD), OpcProb[ADD]);
  EXPECT_DOUBLE_EQ(Vis.getProbability(MUL), OpcProb[MUL]);
  EXPECT_DOUBLE_EQ(Vis.getProbability(MOV), OpcProb[MOV]);
  EXPECT_DOUBLE_EQ(Vis.getProbability(AND), OpcProb[AND]);
  EXPECT_DOUBLE_EQ(Vis.getProbability(DIV), OpcProb[DIV]);
  EXPECT_DOUBLE_EQ(Vis.getProbability(SUB), OpcProb[SUB]);

  // Must be initialized before calling evaluate()
  RandEngine::init(1);
  std::unordered_map<unsigned, size_t> OpcCount;
  // Generate 1000 patterns
  size_t TotalSize = 0;
  for (size_t Count = 0; Count < 1000; ++Count) {
    auto Pattern = OpcodesTree.evaluate();
    for (auto &&Opc : Pattern) {
      if (!OpcCount.count(Opc))
        OpcCount[Opc] = 0;
      OpcCount[Opc]++;
      TotalSize++;
    }
  }
  for (auto &&[Opc, Count] : OpcCount)
    EXPECT_THAT(
        OpcProb[Opc],
        ::testing::DoubleNear(static_cast<double>(Count) / TotalSize, 0.05));
}

TEST(OpcodeHistogramVisitor, CheckWeightMethodHard) {
  //            OpcodesTree
  //  --------------------------------------------------------------
  //        |                                         |            |
  //    [MULNODE][4.0]                               [AND][3.0]   [ADD][1.0]
  //    ------------------------------------------
  //           |                           |
  //       [MULNODE]                    [ORNODE]
  // ----------------------          --------------
  //     |   *   |   *    |          |      |     |
  // [POWNODE] [MUL]   [ORNODE]     [SUB] [AND] [MUL]
  // --------         -------------
  //  |     |         |      |    |
  // [MOV] [3]       [ADD] [AND] [DIV]
  //

  // Weights:
  // ADD -> 1.0 + 4.0 / 3 = 7.0 / 3
  // AND -> 3.0 + 4.0 / 3 + 4.0 / 3 = 17.0 / 3
  // MUL -> 4.0 + 4.0 / 3 = 16.0 / 3
  // MOV -> 4.0 * 3 = 12.0
  // DIV -> 4.0 / 3
  // SUB -> 4.0 / 3
  // TotalWeight = 12.0 + 16.0 / 3 + 17.0 / 3 + 7.0 / 3  + 8.0 / 3= 84.0 / 3
  ChoiceNode OpcodesTree;
  // Building OpcodesTree above
  CartesianNode LowMul;
  LowMul.emplace<RepeatNode>(BaseNode::create<OpcodeNode>(MOV), 3);
  LowMul.emplace<OpcodeNode>(MUL);
  ChoiceNode LowOr;
  LowOr.emplace<OpcodeNode>(ADD);
  LowOr.emplace<OpcodeNode>(AND);
  LowOr.emplace<OpcodeNode>(DIV);
  LowMul.insert(LowOr.clone());
  ChoiceNode UpOr;
  UpOr.emplace<OpcodeNode>(SUB);
  UpOr.emplace<OpcodeNode>(AND);
  UpOr.emplace<OpcodeNode>(MUL);
  CartesianNode UpMul;
  UpMul.insert(LowMul.clone());
  UpMul.insert(UpOr.clone());
  OpcodesTree.emplace<HistogramNode>("MULNODE", UpMul.clone(), 4.0);
  OpcodesTree.emplace<OpcodeNode>(AND, 3.0);
  OpcodesTree.emplace<OpcodeNode>(ADD, 1.0);

  OpcodeProbVisitor Vis(OpcodesTree);
  double TotalWeight = 84.0 / 3;
  double ADDProbability = (7.0 / 3) / TotalWeight;
  double ANDProbability = (17.0 / 3) / TotalWeight;
  double MULProbability = (16.0 / 3) / TotalWeight;
  double DIVProbability = (4.0 / 3) / TotalWeight;
  double SUBProbability = (4.0 / 3) / TotalWeight;
  double MOVProbability = 12.0 / TotalWeight;
  std::unordered_map<unsigned, double> OpcProb;
  OpcProb[ADD] = ADDProbability;
  OpcProb[AND] = ANDProbability;
  OpcProb[MUL] = MULProbability;
  OpcProb[DIV] = DIVProbability;
  OpcProb[SUB] = SUBProbability;
  OpcProb[MOV] = MOVProbability;
  EXPECT_DOUBLE_EQ(Vis.getProbability(ADD), OpcProb[ADD]);
  EXPECT_DOUBLE_EQ(Vis.getProbability(AND), OpcProb[AND]);
  EXPECT_DOUBLE_EQ(Vis.getProbability(MUL), OpcProb[MUL]);
  EXPECT_DOUBLE_EQ(Vis.getProbability(DIV), OpcProb[DIV]);
  EXPECT_DOUBLE_EQ(Vis.getProbability(SUB), OpcProb[SUB]);
  EXPECT_DOUBLE_EQ(Vis.getProbability(MOV), OpcProb[MOV]);

  // Must be initialized before calling evaluate()
  RandEngine::init(1);
  std::unordered_map<unsigned, size_t> OpcCount;
  // Generate 1000 patterns
  size_t TotalSize = 0;
  for (size_t Count = 0; Count < 1000; ++Count) {
    auto Pattern = OpcodesTree.evaluate();
    for (auto &&Opc : Pattern) {
      if (!OpcCount.count(Opc))
        OpcCount[Opc] = 0;
      OpcCount[Opc]++;
      TotalSize++;
    }
  }
  for (auto &&[Opc, Count] : OpcCount)
    EXPECT_THAT(
        OpcProb[Opc],
        ::testing::DoubleNear(static_cast<double>(Count) / TotalSize, 0.05));
}

} // namespace
