//===-- Utils.h -------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_TOOLS_SNIPPY_LIB_UTILS
#define LLVM_TOOLS_SNIPPY_LIB_UTILS

#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

#include <array>
#include <type_traits>

namespace llvm {

class Error;

namespace yaml {
class Output;
class Input;
} // namespace yaml

namespace snippy {

template <typename T> struct NumericRange final {
  std::optional<T> Min;
  std::optional<T> Max;
};

namespace detail {
constexpr static const char *SupportMetadataValue = "llvm.snippy.support";
bool checkMetadata(const MachineInstr &MI, StringRef MetaStr);
} // namespace detail

inline bool checkSupportMetadata(const MachineInstr &MI) {
  return detail::checkMetadata(MI, detail::SupportMetadataValue);
}

template <typename IteratorType>
size_t countPrimaryInstructions(IteratorType Begin, IteratorType End) {
  return std::count_if(
      Begin, End, [](const auto &MI) { return !checkSupportMetadata(MI); });
}

inline MDNode *getSupportMark(LLVMContext &Context) {
  return MDNode::get(Context,
                     MDString::get(Context, detail::SupportMetadataValue));
}

void setAsSupportInstr(MachineInstr &MI, LLVMContext &Ctx);

inline bool isLoadStoreInstr(unsigned Opcode, const MCInstrInfo &InstrInfo) {
  return InstrInfo.get(Opcode).mayLoad() || InstrInfo.get(Opcode).mayStore();
}

std::string addExtensionIfRequired(StringRef StrRef, std::string Ext);

void writeFile(StringRef Path, StringRef Data);

Error checkedWriteToOutput(const Twine &OutputFileName,
                           std::function<Error(raw_ostream &)> Write);

std::string floatToString(APFloat, unsigned Precision);

inline std::string floatToString(double D, unsigned Precision) {
  return floatToString(APFloat(D), Precision);
}

// This is an analog of C++20 bitCast() function
template <class To, class From>
std::enable_if_t<sizeof(To) == sizeof(From) &&
                     std::is_trivially_copyable_v<From> &&
                     std::is_trivially_copyable_v<To>,
                 To>
bitCast(const From &Src) noexcept {
  static_assert(std::is_trivially_constructible_v<To>,
                "This implementation additionally requires "
                "destination type to be trivially constructible");

  To Dst;
  std::memcpy(&Dst, &Src, sizeof(To));
  return Dst;
}

template <typename NumberT, typename It>
NumberT convertBytesToNumber(It Beg, It End) {
  using InputNonConstT = typename std::remove_const<
      typename std::iterator_traits<It>::value_type>::type;
  static_assert(sizeof(InputNonConstT) == 1,
                "Input array does not consist of bytes");
  assert((End - Beg) == sizeof(NumberT) &&
         "Input array size is not equal to number size");
  auto Buf = std::array<InputNonConstT, sizeof(NumberT)>{};
  std::copy(Beg, End, Buf.begin());
  return bitCast<NumberT>(Buf);
}

// In order to cast signed number,
//  you need cast it to the unsigned with std::bit_cast
template <
    typename NumberT, typename InsertIt,
    /*Insert iterator check*/
    typename = decltype(std::declval<InsertIt &>() = std::declval<
                            typename InsertIt::container_type::value_type>())>
void convertNumberToBytesArray(NumberT Num, InsertIt Insert) {
  using ArrayElemT = typename InsertIt::container_type::value_type;
  static_assert(sizeof(ArrayElemT) == 1,
                "Output array does not consist of bytes");
  auto Buf = bitCast<std::array<ArrayElemT, sizeof(NumberT)>>(Num);
  std::copy(Buf.rbegin(), Buf.rend(), Insert);
}

template <
    typename It, typename InsertIt,
    /*Insert iterator check*/
    typename = decltype(std::declval<InsertIt &>() = std::declval<
                            typename InsertIt::container_type::value_type>())>
void transformBytesToNumbersArray(It Beg, It End, InsertIt Insert) {
  using NumberT = typename InsertIt::container_type::value_type;
  static_assert(sizeof(typename std::iterator_traits<It>::value_type) == 1,
                "Input array does not consist of bytes");
  assert((End - Beg) % sizeof(NumberT) == 0);
  for (; Beg != End; Beg += sizeof(NumberT))
    Insert = convertBytesToNumber<NumberT>(Beg, Beg + sizeof(NumberT));
}

template <
    typename NumberT, typename InsertIt,
    /*Insert iterator check*/
    typename = decltype(std::declval<InsertIt &>() = std::declval<
                            typename InsertIt::container_type::value_type>())>
void convertNumberToBytesArrayWithEndianness(NumberT Num, size_t AddrLenInBytes,
                                             bool TargetIsLittleEndian,
                                             InsertIt Insert) {
  SmallVector<unsigned char, 8> AddrBytes;
  convertNumberToBytesArray(Num, std::back_inserter(AddrBytes));
  if (!sys::IsLittleEndianHost)
    std::reverse(AddrBytes.begin(), AddrBytes.end());
  // AddressInfo::Address is of type uint64_t to AddrBytes is of length 8.
  // In order to account for target's address size we need to chop off 8 -
  // AddrRegLen bytes. convertNumberToBytesArray puts bytes in big endian
  // order in the AddrBytes vector (for a little endian host). This rotate and
  // resize chops off the required number of leading zeros in the address.
  std::rotate(AddrBytes.begin(), AddrBytes.begin() + AddrLenInBytes,
              AddrBytes.end());

  assert(std::all_of(AddrBytes.begin() + AddrLenInBytes, AddrBytes.end(),
                     [](auto &&Value) { return Value == 0; }));
  AddrBytes.resize(AddrLenInBytes);
  if (TargetIsLittleEndian)
    std::reverse(AddrBytes.begin(), AddrBytes.end());
  std::copy(AddrBytes.begin(), AddrBytes.end(), Insert);
}

template <typename NumberT>
APInt convertNumberToCorrectEndianness(NumberT Num, size_t AddrLenInBytes,
                                       bool TargetIsLittleEndian) {
  SmallVector<unsigned char, 8> AddrBytes;
  convertNumberToBytesArrayWithEndianness(
      Num, AddrLenInBytes, TargetIsLittleEndian, std::back_inserter(AddrBytes));
  APInt Res(AddrLenInBytes * CHAR_BIT, 0);
  for (unsigned I = 0; I < AddrBytes.size(); ++I) {
    Res |= (AddrBytes[I] << (I * CHAR_BIT));
  }
  return Res;
}

struct MIRComp {
  bool operator()(const MachineFunction *A, const MachineFunction *B) const {
    assert(A && B && "MachineFunction comparator can't compare nullptrs");
    return A->getFunctionNumber() < B->getFunctionNumber();
  }

  bool operator()(const MachineBasicBlock *A,
                  const MachineBasicBlock *B) const {
    assert(A && B && "MachineBasicBlock comparator can't compare nullptrs");
    if (A->getParent() == B->getParent())
      return A->getNumber() < B->getNumber();
    return A->getParent() < B->getParent();
  }
};

template <class Target, class Source> Target narrowCast(Source Value) {
  auto Narrow = static_cast<Target>(Value);
  assert(static_cast<Source>(Narrow) == Value);
  return Narrow;
}

inline long long int alignSignedTo(long long int Value,
                                   long long unsigned Align) {
  if (Value < 0) {
    assert(Value != std::numeric_limits<long long int>::min());
    return -1ll * alignDown(std::abs(Value), Align);
  }
  return alignTo(Value, Align);
}

inline long long int alignSignedDown(long long int Value,
                                     long long unsigned Align) {
  if (Value < 0) {
    assert(Value != std::numeric_limits<long long int>::min());
    return -1ll * alignTo(std::abs(Value), Align);
  }
  return alignDown(Value, Align);
}

template <typename T,
          std::enable_if_t<std::is_integral_v<T> && std::is_signed<T>::value,
                           bool> = true>
bool IsSAddOverflow(T A, T B) {
  auto NumBits = sizeof(T) * CHAR_BIT;
  APInt Op1(NumBits, A, true);
  APInt Op2(NumBits, B, true);
  bool Overflow = false;
  (void)Op1.sadd_ov(Op2, Overflow);
  return Overflow;
}

template <typename... ArgsTy>
struct OverloadedCallable final : public ArgsTy... {
  OverloadedCallable(ArgsTy &&...Args)
      : ArgsTy(std::forward<ArgsTy>(Args))... {}
  using ArgsTy::operator()...;
};

template <typename... ArgsTy>
OverloadedCallable(ArgsTy &&...Args) -> OverloadedCallable<ArgsTy...>;

template <typename RetType, RetType AsOne, RetType AsZero>
class AsOneGenerator final {
  unsigned long long Period;
  unsigned long long State;

public:
  AsOneGenerator(unsigned long long Period = 0)
      : Period(Period), State(Period) {}

  auto operator()() {
    if (!State)
      return AsZero;

    --State;
    if (State)
      return AsZero;

    State = Period;
    return AsOne;
  }
};

template <typename ValTy> APInt toAPInt(ValTy Val, unsigned Width) {
  return APInt(Width, Val);
}

template <> inline APInt toAPInt<APInt>(APInt Val, unsigned Width) {
  assert(Width >= Val.getBitWidth() && "Value is too long");
  return Val;
}

// C++23 std::optional<T>::transform like
template <typename T, typename Func>
auto transformOpt(const std::optional<T> &O, Func &&F)
    -> std::optional<remove_cvref_t<std::invoke_result_t<Func, T>>> {
  if (O)
    return std::invoke(std::forward<Func>(F), *O);
  return std::nullopt;
}

unsigned getAutoSenseRadix(StringRef &Str);

void replaceAllSubstrs(std::string &Str, StringRef What, StringRef With);

inline unsigned requiredNumOfHexDigits(size_t Val) {
  return llvm::alignTo(Val, 4);
}
} // namespace snippy
} // namespace llvm

#endif
