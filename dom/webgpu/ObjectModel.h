/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GPU_OBJECT_MODEL_H_
#define GPU_OBJECT_MODEL_H_

#include "nsWrapperCache.h"
#include "nsString.h"

class nsIGlobalObject;

namespace mozilla {
namespace webgpu {

template <typename T>
class ChildOf {
 public:
  const RefPtr<T> mParent;

  explicit ChildOf(T* const parent);

 protected:
  virtual ~ChildOf();

 public:
  nsIGlobalObject* GetParentObject() const;
};

class ObjectBase : public nsWrapperCache {
 private:
  nsString mLabel;

 protected:
  virtual ~ObjectBase() = default;

 public:
  void GetLabel(nsAString& aValue) const;
  void SetLabel(const nsAString& aLabel);
};

}  // namespace webgpu
}  // namespace mozilla

#define GPU_DECL_JS_WRAP(T)                                             \
  JSObject* WrapObject(JSContext* cx, JS::Handle<JSObject*> givenProto) \
      override;

#define GPU_DECL_CYCLE_COLLECTION(T)                     \
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_NATIVE_CLASS(T) \
  NS_INLINE_DECL_CYCLE_COLLECTING_NATIVE_REFCOUNTING(T)

#define GPU_IMPL_JS_WRAP(T)                                                  \
  JSObject* T::WrapObject(JSContext* cx, JS::Handle<JSObject*> givenProto) { \
    return dom::GPU##T##_Binding::Wrap(cx, this, givenProto);                \
  }

#define GPU_IMPL_CYCLE_COLLECTION(T, ...)            \
  NS_IMPL_CYCLE_COLLECTION_ROOT_NATIVE(T, AddRef)    \
  NS_IMPL_CYCLE_COLLECTION_UNROOT_NATIVE(T, Release) \
  NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(T, __VA_ARGS__)

template <typename T>
void ImplCycleCollectionTraverse(nsCycleCollectionTraversalCallback& callback,
                                 const RefPtr<T>& field, const char* name,
                                 uint32_t flags) {
  CycleCollectionNoteChild(callback, field.get(), name, flags);
}

template <typename T>
void ImplCycleCollectionUnlink(const RefPtr<T>& field) {
  const auto mutPtr = const_cast<RefPtr<T>*>(&field);
  ImplCycleCollectionUnlink(*mutPtr);
}

#endif  // GPU_OBJECT_MODEL_H_
