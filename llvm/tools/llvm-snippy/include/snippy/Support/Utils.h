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
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

#include <array>

namespace llvm {

class Error;

namespace yaml {
class Output;
class Input;
} // namespace yaml

namespace snippy {

namespace detail {
constexpr static const char *SupportMetadataValue = "llvm.snippy.support";
bool checkMetadata(const MachineInstr &MI, StringRef MetaStr);
} // namespace detail

inline bool checkSupportMetadata(const MachineInstr &MI) {
  return detail::checkMetadata(MI, detail::SupportMetadataValue);
}

inline MDNode *getSupportMark(LLVMContext &Context) {
  return MDNode::get(Context,
                     MDString::get(Context, detail::SupportMetadataValue));
}

void setAsSupportInstr(MachineInstr &MI, LLVMContext &Ctx);

template <typename... DstArgs>
MachineInstrBuilder
getSupportInstBuilder(MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
                      LLVMContext &Context, const MCInstrDesc &Desc,
                      DstArgs... DstReg) {
  static_assert(sizeof...(DstReg) <= 1, "Only 0 or 1 dst regs supported");
  return BuildMI(MBB, Ins, MIMetadata({}, getSupportMark(Context)), Desc,
                 DstReg...);
}

template <typename... DstArgs>
MachineInstrBuilder getInstBuilder(bool IsSupport, MachineBasicBlock &MBB,
                                   MachineBasicBlock::iterator Ins,
                                   LLVMContext &Context,
                                   const MCInstrDesc &Desc, DstArgs... DstReg) {
  if (IsSupport)
    return getSupportInstBuilder(MBB, Ins, Context, Desc, DstReg...);
  return BuildMI(MBB, Ins, MIMetadata(), Desc, DstReg...);
}

std::string addExtensionIfRequired(StringRef StrRef, std::string Ext);

void writeFile(StringRef Path, StringRef Data);

Error checkedWriteToOutput(const Twine &OutputFileName,
                           std::function<Error(raw_ostream &)> Write);

std::string floatToString(APFloat, unsigned Precision);

inline std::string floatToString(double D, unsigned Precision) {
  return floatToString(APFloat(D), Precision);
}

// This is an anolog of C++20 bitCast() function
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

} // namespace snippy
} // namespace llvm

#endif
