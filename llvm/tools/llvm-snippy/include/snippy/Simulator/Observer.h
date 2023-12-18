//===-- Observer.h ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Types.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>

namespace llvm {
namespace snippy {

class Observer {
public:
  virtual void memUpdateNotification(MemoryAddressType Addr, const char *Data,
                                     size_t Size) {}
  virtual void xregUpdateNotification(unsigned RegID, RegisterType Value) {}
  virtual void fregUpdateNotification(unsigned RegID, RegisterType Value) {}
  virtual void vregUpdateNotification(unsigned RegID, ArrayRef<char> Data) {}
  virtual void PCUpdateNotification(ProgramCounterType PC) {}

  virtual ~Observer() {}
};

} // namespace snippy
} // namespace llvm

class RVMCallbackHandler {
private:
  using HandleCounterType = uint64_t;
  llvm::DenseMap<HandleCounterType, std::unique_ptr<llvm::snippy::Observer>>
      Observers;
  HandleCounterType UniqueHandleCounter{};

public:
  template <typename ObserverType> class ObserverHandle final {
    HandleCounterType Handle;
    ObserverHandle(HandleCounterType HandleVal) : Handle(HandleVal) {}

  public:
    using Type = ObserverType;

    operator HandleCounterType() const { return Handle; }

    friend RVMCallbackHandler;
  };

  template <typename ObserverType, typename... CtorArgs>
  std::unique_ptr<ObserverHandle<ObserverType>>
  createAndSetObserver(CtorArgs &&...Args) {
    [[maybe_unused]] auto EmplaceResult = Observers.try_emplace(
        UniqueHandleCounter,
        std::make_unique<ObserverType>(std::forward<CtorArgs>(Args)...));
    assert(EmplaceResult.second && "Fail to create and set an observer");
    return std::unique_ptr<ObserverHandle<ObserverType>>(
        new ObserverHandle<ObserverType>(UniqueHandleCounter++));
  }

  auto getObservers() { return llvm::make_second_range(Observers); }

  template <typename ObserverHandleType>
  auto &getObserverByHandle(const ObserverHandleType &Handle) {
    auto ObserverIt = Observers.find(Handle);
    assert(ObserverIt != Observers.end() &&
           "Can not find the observer with this id");
    return static_cast<typename ObserverHandleType::Type &>(
        *ObserverIt->second);
  }

  template <typename ObserverHandleType>
  void eraseByHandle(const ObserverHandleType &Handle) {
    [[maybe_unused]] auto Res = Observers.erase(Handle);
    assert(Res && "Observer handle to erase must be in the map");
  }
};
