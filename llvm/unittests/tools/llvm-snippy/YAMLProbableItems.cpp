
#include "snippy/Support/YAMLProbableItems.h"

#include "gtest/gtest.h"

using llvm::snippy::ProbableItems;
using llvm::yaml::Input;
using llvm::yaml::Output;

// Operators to use with yaml::Input and yaml::Output.
// Basically a copy of SequenceTraits operators `<<` and `>>` from YAMLTraits.h
#define YAML_PROBABLE_ITEMS_DOCUMENT_OPS(_elem_type)                           \
  namespace yaml {                                                             \
  static Input &operator>>(Input &Yin,                                         \
                           snippy::ProbableItems<_elem_type> &Val) {           \
    EmptyContext Ctx;                                                          \
    if (Yin.setCurrentDocument())                                              \
      yamlize(Yin, Val, true, Ctx);                                            \
    return Yin;                                                                \
  }                                                                            \
  static Output &operator<<(Output &Yout,                                      \
                            snippy::ProbableItems<_elem_type> &Val) {          \
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

namespace llvm {
LLVM_SNIPPY_YAML_IS_PROBABLE_ITEMS(std::string)
YAML_PROBABLE_ITEMS_DOCUMENT_OPS(std::string)
} // namespace llvm

TEST(YAMLProbableItems, TestSimpleRead) {
  ProbableItems<std::string> Doc;
  {
    Input Yin("---\n- [a, 1.5]\n- [b, 2.5]\n...\n");
    Yin >> Doc;

    EXPECT_FALSE(Yin.error());
    ASSERT_EQ(Doc.size(), 2u);
    EXPECT_EQ(Doc[0].Element, "a");
    EXPECT_DOUBLE_EQ(Doc[0].Prob, 1.5);
    EXPECT_EQ(Doc[1].Element, "b");
    EXPECT_DOUBLE_EQ(Doc[1].Prob, 2.5);
  }

  {
    Input Yin("---\n[[a, 1.5], [b, 2.5]]\n...\n");
    Yin >> Doc;

    EXPECT_FALSE(Yin.error());
    ASSERT_EQ(Doc.size(), 2u);
    EXPECT_EQ(Doc[0].Element, "a");
    EXPECT_DOUBLE_EQ(Doc[0].Prob, 1.5);
    EXPECT_EQ(Doc[1].Element, "b");
    EXPECT_DOUBLE_EQ(Doc[1].Prob, 2.5);
  }
}

TEST(YAMLProbableItems, TestSimpleWrite) {
  ProbableItems<std::string> Doc{{"a", 1.5}, {"b", 2.5}};
  std::string Str;
  llvm::raw_string_ostream OS(Str);
  Output Yout(OS);
  Yout << Doc;
  EXPECT_EQ(Str, "---\n- [ a, 1.5 ]\n- [ b, 2.5 ]\n...\n");
}

TEST(YAMLProbableItems, TestNegativeWeight) {
  auto TestDiagnostic = [](const llvm::SMDiagnostic &Error, void *) {
    EXPECT_EQ(Error.getMessage(), "weights must be non-negative!");
  };
  ProbableItems<std::string> Doc;
  Input Yin("---\n- [a, 1.5]\n- [b, -2.5]\n...\n", nullptr, TestDiagnostic);
  Yin >> Doc;

  EXPECT_TRUE(Yin.error());
}

TEST(YAMLProbableItems, TestAllZeroWeights) {
  auto TestDiagnostic = [](const llvm::SMDiagnostic &Error, void *) {
    EXPECT_EQ(Error.getMessage(), "at least one weight must be positive!");
  };
  ProbableItems<std::string> Doc;
  Input Yin("---\n- [a, 0.0]\n- [b, 0.0]\n...\n", nullptr, TestDiagnostic);
  Yin >> Doc;

  EXPECT_TRUE(Yin.error());
}

TEST(YAMLProbableItems, TestWrongEntrySize) {
  auto TestDiagnostic = [](const llvm::SMDiagnostic &Error, void *) {
    EXPECT_EQ(Error.getMessage(),
              "expected 2 element(s) in the sequence, got 3");
  };
  ProbableItems<std::string> Doc;
  Input Yin("---\n- [a, 1.5, 2.5]\n...\n", nullptr, TestDiagnostic);
  Yin >> Doc;

  EXPECT_TRUE(Yin.error());
}

// Element with Label and validate()
struct NamedString final : std::string {
  static constexpr const char *Label = "NS";

  std::string validate() const {
    if (*this == "forbidden")
      return "the element is forbidden!";
    return {};
  }
};

template <> struct llvm::yaml::ScalarTraits<NamedString> {
  static void output(const NamedString &Val, void *Ctx, raw_ostream &OS) {
    ScalarTraits<std::string>::output(Val, Ctx, OS);
  }
  static StringRef input(StringRef Scalar, void *Ctx, NamedString &Val) {
    return ScalarTraits<std::string>::input(Scalar, Ctx, Val);
  }
  static QuotingType mustQuote(StringRef S) {
    return ScalarTraits<std::string>::mustQuote(S);
  }
};

namespace llvm {
LLVM_SNIPPY_YAML_IS_PROBABLE_ITEMS(NamedString)
YAML_PROBABLE_ITEMS_DOCUMENT_OPS(NamedString)
} // namespace llvm

TEST(YAMLProbableItems, TestLabeledRead) {
  ProbableItems<NamedString> Doc;
  Input Yin("---\n- [a, 1.5]\n...\n");
  Yin >> Doc;

  EXPECT_FALSE(Yin.error());
  ASSERT_EQ(Doc.size(), 1u);
  EXPECT_EQ(Doc[0].Element, "a");
  EXPECT_DOUBLE_EQ(Doc[0].Prob, 1.5);
}

TEST(YAMLProbableItems, TestLabeledDiagnostics) {
  ProbableItems<NamedString> Doc;
  {
    auto TestDiagnostic = [](const llvm::SMDiagnostic &Error, void *) {
      EXPECT_EQ(Error.getMessage(), "NS: weights must be non-negative!");
    };
    Input Yin("---\n- [a, -1.5]\n...\n", nullptr, TestDiagnostic);
    Yin >> Doc;

    EXPECT_TRUE(Yin.error());
  }

  {
    auto TestDiagnostic = [](const llvm::SMDiagnostic &Error, void *) {
      EXPECT_EQ(Error.getMessage(),
                "NS: at least one weight must be positive!");
    };
    Input Yin("---\n- [a, 0.0]\n...\n", nullptr, TestDiagnostic);
    Yin >> Doc;

    EXPECT_TRUE(Yin.error());
  }

  {
    auto TestDiagnostic = [](const llvm::SMDiagnostic &Error, void *) {
      EXPECT_EQ(Error.getMessage(),
                "NS: expected 2 element(s) in the sequence, got 3");
    };
    Input Yin("---\n- [a, 1.5, 2.5]\n...\n", nullptr, TestDiagnostic);
    Yin >> Doc;

    EXPECT_TRUE(Yin.error());
  }
}

TEST(YAMLProbableItems, TestLabeledWrite) {
  ProbableItems<NamedString> Doc{{NamedString{"a"}, 1.5}};
  std::string Str;
  llvm::raw_string_ostream OS(Str);
  Output Yout(OS);
  Yout << Doc;
  EXPECT_EQ(Str, "---\n- [ a, 1.5 ]\n...\n");
}

TEST(YAMLProbableItems, TestMemberValidate) {
  auto TestDiagnostic = [](const llvm::SMDiagnostic &Error, void *) {
    EXPECT_EQ(Error.getMessage(), "the element is forbidden!");
  };
  ProbableItems<NamedString> Doc;
  Input Yin("---\n- [forbidden, 1.5]\n...\n", nullptr, TestDiagnostic);
  Yin >> Doc;

  EXPECT_TRUE(Yin.error());
}

struct MapWithItems {
  ProbableItems<std::string> StrItems;
};

template <> struct llvm::yaml::MappingTraits<MapWithItems> {
  static void mapping(IO &Io, MapWithItems &Val) {
    Io.mapRequired("MyItems", Val.StrItems);
  }
};

TEST(YAMLProbableItems, TestMapWithProbableItems) {
  MapWithItems Doc;
  {
    Input Yin("---\nMyItems:\n  - [str1, 1.5]\n  - [str2, 2.5]\n...\n");
    Yin >> Doc;

    EXPECT_FALSE(Yin.error());
    ASSERT_EQ(Doc.StrItems.size(), 2u);
    EXPECT_EQ(Doc.StrItems[0].Element, "str1");
    EXPECT_DOUBLE_EQ(Doc.StrItems[0].Prob, 1.5);
    EXPECT_EQ(Doc.StrItems[1].Element, "str2");
    EXPECT_DOUBLE_EQ(Doc.StrItems[1].Prob, 2.5);
  }

  {
    std::string Str;
    llvm::raw_string_ostream OS(Str);
    Output Yout(OS);
    Yout << Doc;
    EXPECT_EQ(Str,
              "---\nMyItems:\n  - [ str1, 1.5 ]\n  - [ str2, 2.5 ]\n...\n");
  }
}
