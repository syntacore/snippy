#include "snippy/Support/ProbabilityUtils.h"

#include "gtest/gtest.h"

using namespace llvm::snippy;

// Non contiguous enum
enum class TestEnum { A = 10, B = 20, C, D };

struct TestEnumList {
  static constexpr std::array<TestEnum, 4> Arr = {TestEnum::A, TestEnum::B,
                                                  TestEnum::C, TestEnum::D};
};

struct TestCustomClass {
  char Data = 'a';

  TestCustomClass() = default;
  TestCustomClass(char Data) : Data(Data) {}
};

TEST(MappedArrayTest, DefaultConstructor) {
  MappedArray<TestEnumList, double> Arr;
  EXPECT_EQ(Arr.size(), 4ul);
  EXPECT_DOUBLE_EQ(Arr[TestEnum::A], 0.0);
  EXPECT_DOUBLE_EQ(Arr[TestEnum::B], 0.0);
  EXPECT_DOUBLE_EQ(Arr[TestEnum::C], 0.0);
  EXPECT_DOUBLE_EQ(Arr[TestEnum::D], 0.0);
}

TEST(MappedArrayTest, DefaultConstructorCustomClass) {
  MappedArray<TestEnumList, TestCustomClass> Arr;
  EXPECT_EQ(Arr.size(), 4ul);
  EXPECT_EQ(Arr[TestEnum::A].Data, 'a');
  EXPECT_EQ(Arr[TestEnum::B].Data, 'a');
  EXPECT_EQ(Arr[TestEnum::C].Data, 'a');
  EXPECT_EQ(Arr[TestEnum::D].Data, 'a');
}

TEST(MappedArrayTest, ValueConstructor) {
  MappedArray<TestEnumList, double> Arr(1.1, 2.2, 3.3, 4.4);
  EXPECT_DOUBLE_EQ(Arr[TestEnum::A], 1.1);
  EXPECT_DOUBLE_EQ(Arr[TestEnum::B], 2.2);
  EXPECT_DOUBLE_EQ(Arr[TestEnum::C], 3.3);
  EXPECT_DOUBLE_EQ(Arr[TestEnum::D], 4.4);
}

TEST(MappedArrayTest, ValueConstructorCustomClass) {
  MappedArray<TestEnumList, TestCustomClass> Arr('x', 'y', 'z', 'w');
  EXPECT_EQ(Arr[TestEnum::A].Data, 'x');
  EXPECT_EQ(Arr[TestEnum::B].Data, 'y');
  EXPECT_EQ(Arr[TestEnum::C].Data, 'z');
  EXPECT_EQ(Arr[TestEnum::D].Data, 'w');
}

TEST(MappedArrayTest, CopyConstructorAndAssignment) {
  MappedArray<TestEnumList, double> Arr1(1.0, 2.0, 3.0, 4.0);
  MappedArray<TestEnumList, double> Arr2;
  Arr2 = Arr1;
  EXPECT_EQ(Arr2[TestEnum::A], 1.0);
  EXPECT_EQ(Arr2[TestEnum::B], 2.0);
  EXPECT_EQ(Arr2[TestEnum::C], 3.0);
  EXPECT_EQ(Arr2[TestEnum::D], 4.0);
}

TEST(MappedArrayTest, Iterators) {
  MappedArray<TestEnumList, double> Arr(1.0, 2.0, 3.0, 4.0);
  double Sum = 0.0;
  for (const auto &[key, val] : Arr)
    Sum += val;
  EXPECT_DOUBLE_EQ(Sum, 10.0);

  const auto &ConstArr = Arr;
  double ConstSum = 0.0;
  for (const auto &[key, val] : ConstArr)
    ConstSum += val;
  EXPECT_DOUBLE_EQ(ConstSum, 10.0);
}

TEST(MappedArrayTest, IndexOperator) {
  MappedArray<TestEnumList, double> Arr;
  Arr[TestEnum::A] = 1.75;
  Arr[TestEnum::B] = 21.5;
  Arr[TestEnum::C] = Arr[TestEnum::A];
  Arr[TestEnum::D] += 1.0;

  EXPECT_DOUBLE_EQ(Arr[TestEnum::A], 1.75);
  EXPECT_DOUBLE_EQ(Arr[TestEnum::B], 21.5);
  EXPECT_DOUBLE_EQ(Arr[TestEnum::C], 1.75);
  EXPECT_DOUBLE_EQ(Arr[TestEnum::D], 1.0);
}

TEST(MappedArrayTest, NormalizeWeights) {
  MappedArray<TestEnumList, double> Arr(1.0, 2.0, 3.0, 4.0);
  auto Norm = normalizeWeights(Arr);

  double Sum = 0.0;
  for (const auto &[key, val] : Norm)
    Sum += val;
  EXPECT_DOUBLE_EQ(Sum, 1.0);

  EXPECT_DOUBLE_EQ(Norm[TestEnum::A], 0.1);
  EXPECT_DOUBLE_EQ(Norm[TestEnum::B], 0.2);
  EXPECT_DOUBLE_EQ(Norm[TestEnum::C], 0.3);
  EXPECT_DOUBLE_EQ(Norm[TestEnum::D], 0.4);
}

TEST(JointProbabilityDistributionTest, SingleMap) {
  std::map<int, double> Map = {{1, 0.2}, {2, 0.5}, {3, 0.3}};
  auto Res = jointProbabilityDistribution(Map);
  ASSERT_EQ(Res.size(), Map.size());

  for (const auto &[key, val] : Res) {
    int K = std::get<0>(key);
    auto It = Map.find(K);
    ASSERT_NE(It, Map.end());
    EXPECT_DOUBLE_EQ(val, It->second);
  }
}

TEST(JointProbabilityDistributionTest, TwoMaps) {
  std::map<std::string, double> Map1 = {{"x", 0.5}, {"y", 0.5}};
  std::map<int, double> Map2 = {{1, 0.2}, {2, 0.8}};
  auto Res = jointProbabilityDistribution(Map1, Map2);
  EXPECT_EQ(Res.size(), Map1.size() * Map2.size());

  std::map<std::tuple<std::string, int>, double> Expected;
  for (const auto &[k1, v1] : Map1) {
    for (const auto &[k2, v2] : Map2)
      Expected[{k1, k2}] = v1 * v2;
  }

  for (const auto &[key, val] : Res) {
    auto It = Expected.find(key);
    ASSERT_NE(It, Expected.end());
    EXPECT_DOUBLE_EQ(val, It->second);
  }
}

TEST(JointProbabilityDistributionTest, ThreeMaps) {
  std::map<char, double> Map1 = {{'a', 0.1}, {'b', 0.9}};
  std::map<int, double> Map2 = {{1, 0.3}, {2, 0.7}};
  std::map<std::string, double> Map3 = {{"foo", 0.4}, {"bar", 0.6}};

  auto Res = jointProbabilityDistribution(Map1, Map2, Map3);
  EXPECT_EQ(Res.size(), Map1.size() * Map2.size() * Map3.size());

  std::map<std::tuple<char, int, std::string>, double> Expected;
  for (const auto &[k1, v1] : Map1) {
    for (const auto &[k2, v2] : Map2) {
      for (const auto &[k3, v3] : Map3)
        Expected[{k1, k2, k3}] = v1 * v2 * v3;
    }
  }

  for (const auto &[key, val] : Res) {
    auto It = Expected.find(key);
    ASSERT_NE(It, Expected.end());
    EXPECT_DOUBLE_EQ(val, It->second);
  }
}

TEST(JointProbabilityDistributionTest, MapAndMappedArray) {
  MappedArray<TestEnumList, double> Map1(0.2, 0.3, 0.4, 0.1);
  std::map<int, double> Map2 = {{1, 0.5}, {2, 0.5}};

  auto Res = jointProbabilityDistribution(Map1, Map2);
  EXPECT_EQ(Res.size(), Map1.size() * Map2.size());

  std::map<std::tuple<TestEnum, int>, double> Expected;
  for (const auto &[k1, v1] : Map1) {
    for (const auto &[k2, v2] : Map2)
      Expected[{k1, k2}] = v1 * v2;
  }

  for (const auto &[key, val] : Res) {
    auto It = Expected.find(key);
    ASSERT_NE(It, Expected.end());
    EXPECT_DOUBLE_EQ(val, It->second);
  }
}
