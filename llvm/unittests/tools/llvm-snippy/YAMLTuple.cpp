
#include "snippy/Support/YAMLTuple.h"

#include "gtest/gtest.h"

using llvm::yaml::Input;
using llvm::yaml::Output;

// Operators to use with yaml::Input and yaml::Output.
// Basically a copy of SequenceTraits operators `<<` and `>>` from YAMLTraits.h
#define YAML_TUPLE_DOCUMENT_OPS(_type)                                         \
  namespace yaml {                                                             \
  static Input &operator>>(Input &Yin, _type &Val) {                           \
    EmptyContext Ctx;                                                          \
    if (Yin.setCurrentDocument())                                              \
      yamlize(Yin, Val, true, Ctx);                                            \
    return Yin;                                                                \
  }                                                                            \
  static Output &operator<<(Output &Yout, _type &Val) {                        \
    EmptyContext Ctx;                                                          \
    Yout.beginDocuments();                                                     \
    if (Yout.preflightDocument(0)) {                                           \
      yamlize(Yout, Val, true, Ctx);                                           \
      Yout.postflightDocument();                                               \
    }                                                                          \
    Yout.endDocuments();                                                       \
    return Yout;                                                               \
  }                                                                            \
  } // namespace yaml

struct FooBar {
  int Foo;
  int Bar;
};

template <> struct llvm::snippy::YAMLTupleTraits<FooBar> {
  static auto members(FooBar &Val) { return std::tie(Val.Foo, Val.Bar); }
};
namespace llvm {
LLVM_SNIPPY_YAML_IS_TUPLE(FooBar)
YAML_TUPLE_DOCUMENT_OPS(FooBar)
} // namespace llvm

TEST(YAMLTuple, TestSimpleRead) {
  FooBar Doc;
  {
    Input Yin("---\n[3, 5]\n...\n");
    Yin >> Doc;

    EXPECT_FALSE(Yin.error());
    EXPECT_EQ(Doc.Foo, 3);
    EXPECT_EQ(Doc.Bar, 5);
  }

  {
    Input Yin("---\n- 3\n- 5\n...\n");
    Yin >> Doc;

    EXPECT_FALSE(Yin.error());
    EXPECT_EQ(Doc.Foo, 3);
    EXPECT_EQ(Doc.Bar, 5);
  }
}

TEST(YAMLTuple, TestMalformedSimpleRead) {
  FooBar Doc;
  Input Yin("---\n[3; 5]\n...\n");
  Yin >> Doc;

  EXPECT_TRUE(Yin.error());
}

// Output is always done in a flow style (single line)
TEST(YAMLTuple, TestSimpleWrite) {
  FooBar Doc{3, 5};
  std::string Str;
  llvm::raw_string_ostream OS(Str);
  Output Yout(OS);
  Yout << Doc;
  EXPECT_EQ(Str, "---\n[ 3, 5 ]\n...\n");
}

struct WeightedString {
  std::string S;
  double W;
};

template <> struct llvm::snippy::YAMLTupleTraits<WeightedString> {
  static auto members(WeightedString &Val) { return std::tie(Val.S, Val.W); }
};
namespace llvm {
LLVM_SNIPPY_YAML_IS_TUPLE(WeightedString)
YAML_TUPLE_DOCUMENT_OPS(WeightedString)
} // namespace llvm

TEST(YAMLTuple, TestHeterogeneousRead) {
  WeightedString Doc;
  {
    Input Yin("---\n[my_string, 5.0]\n...\n");
    Yin >> Doc;

    EXPECT_FALSE(Yin.error());
    EXPECT_EQ(Doc.S, "my_string");
    EXPECT_DOUBLE_EQ(Doc.W, 5.0);
  }

  {
    Input Yin("---\n- my_string\n- 5.0\n...\n");
    Yin >> Doc;

    EXPECT_FALSE(Yin.error());
    EXPECT_STREQ(Doc.S.c_str(), "my_string");
    EXPECT_DOUBLE_EQ(Doc.W, 5.0);
  }

  {
    Input Yin("---\n- \"my_string\"\n- 5.0\n...\n");
    Yin >> Doc;

    EXPECT_FALSE(Yin.error());
    EXPECT_STREQ(Doc.S.c_str(), "my_string");
    EXPECT_DOUBLE_EQ(Doc.W, 5.0);
  }
}

TEST(YAMLTuple, TestHeterogeneousWrite) {
  WeightedString Doc{"my_string", 5.1};
  std::string Str;
  llvm::raw_string_ostream OS(Str);
  Output Yout(OS);
  Yout << Doc;

  EXPECT_EQ(Str, "---\n[ my_string, 5.1 ]\n...\n");
}

struct StructWithSubTuple {
  std::string A;
  FooBar B;
  double C;
};

template <> struct llvm::snippy::YAMLTupleTraits<StructWithSubTuple> {
  static auto members(StructWithSubTuple &Val) {
    return std::tie(Val.A, Val.B, Val.C);
  }
};
namespace llvm {
LLVM_SNIPPY_YAML_IS_TUPLE(StructWithSubTuple)
YAML_TUPLE_DOCUMENT_OPS(StructWithSubTuple)
} // namespace llvm

TEST(YAMLTuple, TestSubTupleRead) {
  StructWithSubTuple Doc;
  {
    Input Yin("---\n[my_string, [3, 5], 5.0]\n...\n");
    Yin >> Doc;

    EXPECT_FALSE(Yin.error());
    EXPECT_EQ(Doc.A, "my_string");
    EXPECT_EQ(Doc.B.Foo, 3);
    EXPECT_EQ(Doc.B.Bar, 5);
    EXPECT_DOUBLE_EQ(Doc.C, 5.0);
  }

  {
    Input Yin("---\n- my_string\n- [3, 5]\n- 5.0\n...\n");
    Yin >> Doc;

    EXPECT_FALSE(Yin.error());
    EXPECT_STREQ(Doc.A.c_str(), "my_string");
    EXPECT_EQ(Doc.B.Foo, 3);
    EXPECT_EQ(Doc.B.Bar, 5);
    EXPECT_DOUBLE_EQ(Doc.C, 5.0);
  }
}

TEST(YAMLTuple, TestSubTupleWrite) {
  StructWithSubTuple Doc{"my_string", {3, 5}, 5.1};
  std::string Str;
  llvm::raw_string_ostream OS(Str);
  Output Yout(OS);
  Yout << Doc;
  EXPECT_EQ(Str, "---\n[ my_string, [ 3, 5 ], 5.1 ]\n...\n");
}

struct MapWithTuple {
  FooBar A;
  FooBar B;
};

template <> struct llvm::yaml::MappingTraits<MapWithTuple> {
  static void mapping(IO &Io, MapWithTuple &Val) {
    Io.mapRequired("A", Val.A);
    Io.mapRequired("B", Val.B);
  }
};

TEST(YAMLTuple, TestMapWithTuple) {
  MapWithTuple Doc{{3, 5}, {4, 6}};
  std::string Str;
  llvm::raw_string_ostream OS(Str);
  Output Yout(OS);
  Yout << Doc;

  EXPECT_EQ(Str,
            "---\nA:               [ 3, 5 ]\nB:               [ 4, 6 ]\n...\n");
}
