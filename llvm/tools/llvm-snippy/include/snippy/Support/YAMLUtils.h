//===-- YAMLUtils.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Support/Utils.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"

#define LLVM_SNIPPY_YAML_IS_SEQUENCE_ELEMENT(_type, _flow)                     \
  template <> struct llvm::yaml::SequenceElementTraits<_type, void> {          \
    static constexpr bool flow = _flow;                                        \
  }

#define LLVM_SNIPPY_YAML_DECLARE_SCALAR_ENUMERATION_TRAITS(_type)              \
  template <> struct llvm::yaml::ScalarEnumerationTraits<_type, void> {        \
    static void enumeration(llvm::yaml::IO &IO, _type &);                      \
  }

#define LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS(_type)                         \
  template <> struct llvm::yaml::MappingTraits<_type> {                        \
    static void mapping(yaml::IO &, _type &);                                  \
  }

#define LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS_WITH_VALIDATE(_type)           \
  template <> struct llvm::yaml::MappingTraits<_type> {                        \
    static void mapping(yaml::IO &, _type &);                                  \
    static std::string validate(yaml::IO &, _type &);                          \
  }

#define LLVM_SNIPPY_YAML_DECLARE_SCALAR_TRAITS(_type)                          \
  template <> struct llvm::yaml::ScalarTraits<_type, void> {                   \
    static void output(const _type &, void *, raw_ostream &);                  \
    static StringRef input(StringRef scalar, void *, _type &value);            \
    static QuotingType mustQuote(StringRef);                                   \
  }

#define LLVM_SNIPPY_YAML_DECLARE_CUSTOM_MAPPING_TRAITS(_type)                  \
  template <> struct llvm::yaml::CustomMappingTraits<_type> {                  \
    static void inputOne(yaml::IO &, StringRef, _type &);                      \
    static void output(yaml::IO &, _type &);                                   \
  }

#define LLVM_SNIPPY_YAML_STRONG_TYPEDEF(_mapped, _type)                        \
  struct _type {                                                               \
    _type() : value() {}                                                       \
    _type(const _mapped &w) : value(w) {}                                      \
    operator const _mapped &() const { return value; }                         \
    _mapped value;                                                             \
  }

#define LLVM_SNIPPY_YAML_STRONG_TYPEDEF_SCALAR(_mapped, _type, _quoting)       \
  void llvm::yaml::ScalarTraits<_type>::output(const _type &Value, void *Ctx,  \
                                               raw_ostream &OS) {              \
    ScalarTraits<_mapped>::output(Value, Ctx, OS);                             \
  }                                                                            \
  llvm::StringRef llvm::yaml::ScalarTraits<_type>::input(                      \
      StringRef Value, void *Ctx, _type &Out) {                                \
    return ScalarTraits<_mapped>::input(Value, Ctx, Out.value);                \
  }                                                                            \
  llvm::yaml::QuotingType llvm::yaml::ScalarTraits<_type>::mustQuote(          \
      StringRef S) {                                                           \
    return _quoting;                                                           \
  }

#define LLVM_SNIPPY_YAML_INSTANTIATE_HISTOGRAM_IO(_type)                       \
  template void llvm::yaml::yamlize(                                           \
      yaml::IO &Io, snippy::YAMLHistogramIO<_type> &, bool, EmptyContext &Ctx)

#define LLVM_SNIPPY_YAML_DECLARE_IS_HISTOGRAM_DENORM_ENTRY(_type)              \
  template <>                                                                  \
  void llvm::yaml::yamlize(yaml::IO &Io, _type &E, bool, EmptyContext &Ctx)

#define LLVM_SNIPPY_YAML_DECLARE_HISTOGRAM_TRAITS(_type, _map_type)            \
  template <> struct YAMLHistogramTraits<_type> {                              \
    using MapType = _map_type;                                                 \
    static _type denormalizeEntry(yaml::IO &Io, StringRef Key, double Weight); \
    static void normalizeEntry(yaml::IO &Io, const _type &,                    \
                               SmallVectorImpl<SValue> &);                     \
    static MapType denormalizeMap(yaml::IO &Io, ArrayRef<_type>);              \
    static void normalizeMap(yaml::IO &Io, const MapType &,                    \
                             std::vector<_type> &);                            \
    static std::string validate(ArrayRef<_type>);                              \
  }

#define LLVM_SNIPPY_YAML_DECLARE_SEQUENCE_TRAITS(_type, _elem)                 \
  template <> struct llvm::yaml::SequenceTraits<_type, void> {                 \
    static size_t size(IO &io, _type &list);                                   \
    static _elem &element(IO &io, _type &list, size_t index);                  \
    static const bool flow = false;                                            \
  }

namespace llvm {

class Error;

namespace yaml {

class Output;
class Input;
class IO;
struct EmptyContext;
enum class QuotingType;

template <typename> struct MappingTraits;
template <typename, typename> struct MappingContextTraits;
template <typename, typename> struct ScalarEnumerationTraits;
template <typename, typename> struct ScalarBitSetTraits;
template <typename, typename> struct ScalarTraits;
template <typename, typename> struct SequenceTraits;
template <typename, typename> struct SequenceElementTraits;
template <typename> struct DocumentListTraits;
template <typename> struct CustomMappingTraits;
template <typename> struct PolymorphicTraits;

} // namespace yaml

namespace snippy {
template <typename T, typename> struct YAMLHistogramIO;
} // namespace snippy

namespace yaml {
template <typename T>
void yamlize(yaml::IO &Io, snippy::YAMLHistogramIO<T, void> &Hist, bool,
             yaml::EmptyContext &Ctx);
} // namespace yaml

namespace snippy {

template <typename T, typename InT = yaml::Input, typename CallableT,
          typename DiagHandlerTy, typename DiagCtxTy>
Error loadYAMLFromBuffer(T &ToLoad, StringRef Buf, CallableT &&Tap,
                         DiagHandlerTy &&Handler, DiagCtxTy &Ctx) {
  InT Yin(Buf, nullptr, Handler, &Ctx);
  Tap(Yin);
  Yin >> ToLoad;
  if (auto EC = Yin.error())
    return createStringError(EC, EC.message());
  return Error::success();
}

namespace detail {
inline constexpr StringRef YAMLUnknownKeyStartString = "unknown key";
struct AllowDiagnosticsForAllUnknownKeys {
  bool operator()(const SMDiagnostic &Diag) const {
    return Diag.getMessage().starts_with(YAMLUnknownKeyStartString);
  }
};
} // namespace detail

template <typename T, typename InT = yaml::Input>
Error loadYAMLFromBufferIgnoreUnknown(T &ToLoad, StringRef Buf) {
  InT Yin(Buf, nullptr, [](const auto &Diag, void *) {
    if (!detail::AllowDiagnosticsForAllUnknownKeys{}(Diag))
      Diag.print(nullptr, errs());
  });
  Yin.setAllowUnknownKeys(true);
  Yin >> ToLoad;
  if (auto EC = Yin.error())
    return createStringError(EC, EC.message());
  return Error::success();
}

// This monstrocity is necessary to set the correct filename in the diagnostic
inline SMDiagnostic getSMDiagWithFilename(StringRef Filename,
                                          const SMDiagnostic &Diag) {
  return SMDiagnostic(*Diag.getSourceMgr(), Diag.getLoc(), Filename,
                      Diag.getLineNo(), Diag.getColumnNo(), Diag.getKind(),
                      Diag.getMessage(), Diag.getLineContents(),
                      Diag.getRanges());
}

// Note that this template trick is necessary to avoid including the YAMLTraits
// header. The client should make sure that it's available where necessary.
template <typename T, typename InT = yaml::Input, typename CallableT>
Error loadYAMLFromFile(T &ToLoad, const Twine &Filename, CallableT &&Tap) {
  auto MemBufOrErr = MemoryBuffer::getFile(Filename);
  if (auto EC = MemBufOrErr.getError(); !MemBufOrErr)
    return createStringError(EC, "Failed to open file \"" + Filename +
                                     "\": " + EC.message());

  std::unique_ptr<MemoryBuffer> MemBuf = std::move(*MemBufOrErr);
  auto NameCtx = Filename.str();
  auto Err = loadYAMLFromBuffer(
      ToLoad, MemBuf->getBuffer(), std::forward<CallableT>(Tap),
      [](const auto &Diag, void *Ctx) {
        assert(Ctx);
        const auto &Filename = *static_cast<std::string *>(Ctx);
        getSMDiagWithFilename(Filename, Diag).print(nullptr, errs());
      },
      NameCtx);

  if (Err) {
    auto EC = errorToErrorCode(std::move(Err));
    return createStringError(EC, "Failed to parse file \"" + Filename +
                                     "\": " + EC.message());
  }

  return Error::success();
}

// Note that this template trick is necessary to avoid including the YAMLTraits
// header. The client should make sure that it's available where necessary.
template <typename T, typename InT = yaml::Input,
          typename CallableT = detail::AllowDiagnosticsForAllUnknownKeys>
Error loadYAMLIgnoreUnknownKeys(T &ToLoad, const Twine &Filename,
                                CallableT IsDiagnosticAllowed = {}) {
  auto MemBufOrErr = MemoryBuffer::getFile(Filename);
  if (auto EC = MemBufOrErr.getError(); !MemBufOrErr)
    return createStringError(EC, "Failed to open file " + Filename + ": " +
                                     EC.message());

  struct DiagContext {
    decltype(IsDiagnosticAllowed) DiagAllowed;
    std::string Filename;
  };

  DiagContext Ctx{IsDiagnosticAllowed, Filename.str()};

  std::unique_ptr<MemoryBuffer> MemBuf = std::move(*MemBufOrErr);
  InT Yin(
      MemBuf->getBuffer(), nullptr,
      [](const auto &Diag, void *CtxPtr) {
        const DiagContext &Ctx = *static_cast<DiagContext *>(CtxPtr);
        if (!Ctx.DiagAllowed(Diag))
          getSMDiagWithFilename(Ctx.Filename, Diag).print(nullptr, errs());
      },
      &Ctx);

  Yin.setAllowUnknownKeys(true);

  Yin >> ToLoad;
  if (auto EC = Yin.error())
    return createStringError(EC, "Failed to parse file \"" + Filename +
                                     "\": " + EC.message());

  return Error::success();
}

template <typename T, typename InT = yaml::Input>
Error loadYAMLFromFile(T &ToLoad, const Twine &Filename) {
  return loadYAMLFromFile(ToLoad, Filename, [](auto &&) {});
}

template <typename ObjT, typename CallableT, typename OutT = yaml::Output,
          typename = std::enable_if_t<std::is_copy_constructible_v<ObjT>>>
void outputYAMLToStream(const ObjT &Obj, raw_ostream &OS, CallableT Tap) {
  OutT Yout(OS);
  Tap(Yout);
  auto Copy = Obj;
  Yout << Copy;
}

template <typename ObjT, typename CallableT, typename OutT = yaml::Output>
void outputYAMLToStream(ObjT &Obj, raw_ostream &OS, CallableT Tap) {
  OutT Yout(OS);
  Tap(Yout);
  Yout << Obj;
}

template <typename ObjT, typename OutT = yaml::Output>
void outputYAMLToStream(ObjT &&Obj, raw_ostream &OS) {
  outputYAMLToStream(std::forward<ObjT>(Obj), OS, [](auto &&) {});
}

template <typename ObjT, typename OutT = yaml::Output>
void outputYAMLToFileOrFatal(ObjT &Obj, const Twine &Path,
                             const Twine &Description = "") {
  if (Error Err = checkedWriteToOutput(Path, [&Obj](raw_ostream &OS) {
        outputYAMLToStream(Obj, OS, [](auto &&) {});
        return Error::success();
      }))
    report_fatal_error(
        (Description.isTriviallyEmpty() ? "" : Description + ": ") +
            toString(std::move(Err)),
        false);
}

template <typename T> Expected<T> loadYAMLFromFile(const Twine &Filename) {
  T ToLoad;
  if (auto Err = loadYAMLFromFile(ToLoad, Filename))
    return std::move(Err);
  return ToLoad;
}

template <typename T>
T loadYAMLFromFileOrFatal(const Twine &Filename,
                          const Twine &Description = "") {
  auto Loaded = loadYAMLFromFile<T>(Filename);
  if (!Loaded)
    report_fatal_error(
        (Description.isTriviallyEmpty() ? "" : Description + ": ") +
            toString(Loaded.takeError()),
        false);
  return *Loaded;
}

template <typename T>
void loadYAMLFromFileOrFatal(T &Obj, const Twine &Filename,
                             const Twine &Description = "") {
  auto Err = loadYAMLFromFile(Obj, Filename);
  if (Err)
    report_fatal_error(
        (Description.isTriviallyEmpty() ? "" : Description + ": ") +
            toString(std::move(Err)),
        false);
}

// Normalization for Hex64, Hex32, Hex16 and Hex8
template <typename T> struct NormalizedYAMLStrongTypedef {
  using ValueType = decltype(T{}.value);
  NormalizedYAMLStrongTypedef(yaml::IO &Io) {}
  NormalizedYAMLStrongTypedef(yaml::IO &Io, ValueType &Val) : Value{Val} {}
  auto denormalize(yaml::IO &Io) { return Value.value; }
  T Value;
};

LLVM_SNIPPY_YAML_STRONG_TYPEDEF(std::string, SValue);
using SList = std::vector<SValue>;

} // namespace snippy

LLVM_SNIPPY_YAML_DECLARE_SCALAR_TRAITS(snippy::SValue);
LLVM_SNIPPY_YAML_IS_SEQUENCE_ELEMENT(snippy::SList, false /* not a flow */);
LLVM_SNIPPY_YAML_IS_SEQUENCE_ELEMENT(snippy::SValue, true /* flow sequence */);

} // namespace llvm
