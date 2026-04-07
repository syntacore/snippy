#include "snippy/Config/OpcodeHistogramVisitor.h"

#include "llvm/ADT/STLExtras.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include <unordered_map>
#include <utility>

namespace {

using namespace llvm::snippy;
using OpcCategory = OpcodeNode::OpcodeCategory;

enum Opcodes { ADD, SUB, MUL, DIV, MOV, AND };

using OpcodeWeightsTy = std::vector<std::pair<unsigned, double>>;

// We generate a given number of patterns and verify that the final distribution
// of opcodes matches the theoretically calculated one (stored in OpcProb).
void checkBySamplingTree(const ChoiceNode &OpcodesTree,
                         const std::unordered_map<unsigned, double> &OpcProb,
                         size_t SampleNum) {
  // Must be initialized before calling evaluate()
  RandEngine::init(::testing::GTEST_FLAG(random_seed));
  std::unordered_map<unsigned, size_t> OpcCount;
  // Generate SampleNum patterns
  assert(SampleNum);
  while (SampleNum--) {
    auto Pattern = OpcodesTree.evaluate();
    for (auto &&Opc : Pattern)
      OpcCount[Opc]++;
  }
  auto CountRange = llvm::make_second_range(OpcCount);
  auto TotalSize =
      std::accumulate(CountRange.begin(), CountRange.end(), /* init */ 0u);
  for (auto &&[Opc, Count] : OpcCount) {
    auto Found = OpcProb.find(Opc);
    assert(Found != OpcProb.end());
    auto Prob = Found->second;
    EXPECT_THAT(Prob, ::testing::DoubleNear(
                          static_cast<double>(Count) / TotalSize, 0.05));
  }
}

void checkOpcodeProbabilities(const ChoiceNode &OpcodesTree,
                              const OpcodeWeightsTy &OpcodeWeights) {
  OpcodeProbVisitor Vis(OpcodesTree);
  std::unordered_map<unsigned, double> OpcProb;
  auto Weights = llvm::make_second_range(OpcodeWeights);
  auto TotalWeight =
      std::accumulate(Weights.begin(), Weights.end(), /* init */ 0.0);
  EXPECT_DOUBLE_EQ(Vis.getTotalWeight(), TotalWeight);
  llvm::transform(OpcodeWeights, std::inserter(OpcProb, OpcProb.end()),
                  [TotalWeight](auto &&OpcWeight) {
                    return std::make_pair(OpcWeight.first,
                                          OpcWeight.second / TotalWeight);
                  });
  llvm::for_each(OpcProb, [&Vis](auto &&OpcToProb) {
    EXPECT_DOUBLE_EQ(Vis.getProbability(OpcToProb.first), OpcToProb.second);
  });

  checkBySamplingTree(OpcodesTree, OpcProb, /* SampleNum */ 10000);
}

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

  OpcodeWeightsTy OpcodeWeights{{ADD, 2.0}, {SUB, 1.0}, {MUL, 3.0},
                                {DIV, 1.0}, {MOV, 1.0}, {AND, 2.0}};
  checkOpcodeProbabilities(OpcodesTree, OpcodeWeights);
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

  OpcodeWeightsTy OpcodeWeights{{ADD, 4.0}, {MUL, 5.0}, {MOV, 1.0},
                                {AND, 1.0}, {DIV, 0.0}, {SUB, 0.0}};
  checkOpcodeProbabilities(OpcodesTree, OpcodeWeights);
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

  OpcodeWeightsTy OpcodeWeights{{ADD, 7.0 / 3},  {AND, 17.0 / 3},
                                {MUL, 16.0 / 3}, {DIV, 4.0 / 3},
                                {SUB, 4.0 / 3},  {MOV, 12.0}};
  checkOpcodeProbabilities(OpcodesTree, OpcodeWeights);
}

TEST(OpcodeHistogramVisitor, CheckWeightedOpcodes) {
  //            OpcodesTree
  //  --------------------------------------------------------------
  //        |                                         |            |
  //    [ChoiceNode1][2.0]                        [AND][2.0]   [ADD][1.0]
  //    ------------------------------------------
  //    |               |            |           |
  // [AND][3.5]   [ChoiceNode2]  [ADD][7.0]    [POWNODE]
  //            ----------------              ---------
  //            |        |                    |       |
  //         [MUL][2.0] [SUB][0.5]     [ChoiceNode3] [5]
  //                                   -------------
  //                                      |        |
  //                                 [MUL][2.0]  [AND]
  // Weights:
  // ADD -> 1.0 + 7.0 * (2 / 12.5) = 265 / 125
  // AND -> 2.0 + 3.5 * (2 / 12.5) + (2 / 12.5) * (1 * 1 / 3) * 5 = 1060 / 375
  // SUB -> 0.5 * (2 / 12.5) * (1 / 2.5) = 32 / 1000
  // MUL -> 2.0 * (2 / 12.5) * (1 / 2.5) + (2 / 12.5) * (2 * 1 / 3) * 5 = 1240 /
  // 1875
  ChoiceNode OpcodesTree;
  OpcodesTree.emplace<OpcodeNode>(AND, 2.0);
  OpcodesTree.emplace<OpcodeNode>(ADD, 1.0);
  ChoiceNode ChoiceNode1;
  ChoiceNode1.emplace<OpcodeNode>(AND, 3.5);
  ChoiceNode1.emplace<OpcodeNode>(ADD, 7.0);
  ChoiceNode ChoiceNode2;
  ChoiceNode2.emplace<OpcodeNode>(MUL, 2.0);
  ChoiceNode2.emplace<OpcodeNode>(SUB, 0.5);
  ChoiceNode1.insert(ChoiceNode2.clone());
  ChoiceNode ChoiceNode3;
  ChoiceNode3.emplace<OpcodeNode>(MUL, 2.0);
  ChoiceNode3.emplace<OpcodeNode>(AND);
  ChoiceNode1.emplace<RepeatNode>(ChoiceNode3.clone(), 5);
  OpcodesTree.emplace<HistogramNode>("ChoiceNode1", ChoiceNode1.clone(), 2.0);

  OpcodeWeightsTy OpcodeWeights{{ADD, 265.0 / 125},
                                {AND, 1060.0 / 375},
                                {SUB, 32.0 / 1000},
                                {MUL, 1240.0 / 1875}};
  checkOpcodeProbabilities(OpcodesTree, OpcodeWeights);
}

TEST(OpcodeHistogramVisitor, CheckWeightedOpcodesPatterns) {
  //            OpcodesTree
  //  --------------------------------------------------------------
  //        |                                         |            |
  //    [ChoiceNode1][2.0]                        [AND][2.0]   [ADD][1.0]
  //    ------------------------------------------
  //    |               |            |           |
  // [AND][5.0] [Pattern1][2.0]   [ADD][7.0] [POWNODE]
  //            -----------------            ----------
  //            |        |                   |        |
  //         [MUL][2.0] [SUB][4.0]       [Pattern2]  [5]
  //                                   -------------
  //                                      |        |
  //                                 [MUL][2.0] CartesianNode1
  //                                          -----------------
  //                                          |   *   |  *    |
  //                                       SUB[3.0] MUL[2.0] AND[4.0]
  // Weights:
  // ADD -> 1.0 + 7.0 * (2.0 / 15) = 29.0 / 15
  // AND -> 2.0 + 5.0 * (2.0 / 15) + (2.0 / 15) * (1.0 / 26) * 24 * 5 = 1920.0 /
  // 585 SUB -> 4.0 * (2.0 / 6) * (2.0 / 15) + (2.0 / 15) * (1.0 / 26) * 24 * 5
  // = 1392.0 / 1755 MUL -> 2.0 * (2.0 / 15) * (2.0 / 6) + 5 * (2.0 / 15) = 68.0
  // / 90
  ChoiceNode OpcodesTree;
  OpcodesTree.emplace<OpcodeNode>(AND, 2.0);
  OpcodesTree.emplace<OpcodeNode>(ADD, 1.0);
  ChoiceNode ChoiceNode1;
  ChoiceNode1.emplace<OpcodeNode>(AND, 5.0);
  ChoiceNode1.emplace<OpcodeNode>(ADD, 7.0);
  ChoiceNode Pattern1;
  Pattern1.emplace<OpcodeNode>(MUL, 2.0);
  Pattern1.emplace<OpcodeNode>(SUB, 4.0);
  ChoiceNode1.emplace<HistogramNode>("Pattern1", Pattern1.clone(), 2.0);
  CartesianNode CartesianNode1;
  CartesianNode1.emplace<OpcodeNode>(SUB, 3.0);
  CartesianNode1.emplace<OpcodeNode>(MUL, 2.0);
  CartesianNode1.emplace<OpcodeNode>(AND, 4.0);
  ChoiceNode Pattern2;
  Pattern2.emplace<OpcodeNode>(MUL, 2.0);
  Pattern2.insert(CartesianNode1.clone());
  ChoiceNode1.emplace<RepeatNode>(Pattern2.clone(), 5);
  OpcodesTree.emplace<HistogramNode>("ChoiceNode1", ChoiceNode1.clone(), 2.0);

  OpcodeWeightsTy OpcodeWeights{{ADD, 29.0 / 15},
                                {AND, 384.0 / 117},
                                {SUB, 1392.0 / 1755},
                                {MUL, 68.0 / 90}};
  checkOpcodeProbabilities(OpcodesTree, OpcodeWeights);
}

} // namespace
