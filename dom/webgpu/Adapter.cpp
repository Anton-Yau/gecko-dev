/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/WebGPUBinding.h"
#include "Adapter.h"

#include "Device.h"
#include "Instance.h"
#include "ipc/WebGPUChild.h"
#include "mozilla/dom/Promise.h"

namespace mozilla {
namespace webgpu {

GPU_IMPL_CYCLE_COLLECTION(Adapter, mBridge, mParent)
GPU_IMPL_JS_WRAP(Adapter)

Adapter::Adapter(Instance* const aParent, RawId aId)
    : ChildOf(aParent), mBridge(aParent->GetBridge()), mId(aId) {}
Adapter::~Adapter() = default;

WebGPUChild* Adapter::GetBridge() const { return mBridge; }

already_AddRefed<dom::Promise> Adapter::RequestDevice(
    const dom::GPUDeviceDescriptor& aDesc, ErrorResult& aRv) {
  RefPtr<dom::Promise> promise = dom::Promise::Create(GetParentObject(), aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  Maybe<RawId> id = mBridge->AdapterRequestDevice(mId, aDesc);
  if (id.isSome()) {
    RefPtr<Device> device = new Device(this, id.value());
    promise->MaybeResolve(device);
  } else {
    promise->MaybeRejectWithDOMException(NS_ERROR_DOM_NOT_SUPPORTED_ERR,
                                         "Unable to instanciate a Device");
  }

  return promise.forget();
}

}  // namespace webgpu
}  // namespace mozilla
