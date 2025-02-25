/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=2 ts=8 et tw=80 : */

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "DocumentChannelChild.h"
#include "SerializedLoadContext.h"
#include "mozIThirdPartyUtil.h"
#include "mozilla/LoadInfo.h"
#include "mozilla/dom/BrowserChild.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/ipc/IPCStreamUtils.h"
#include "mozilla/ipc/URIUtils.h"
#include "mozilla/net/HttpChannelChild.h"
#include "mozilla/net/NeckoChild.h"
#include "mozilla/net/UrlClassifierCommon.h"
#include "nsContentSecurityManager.h"
#include "nsDocShellLoadState.h"
#include "nsHttpHandler.h"
#include "nsQueryObject.h"
#include "nsSerializationHelper.h"
#include "nsStringStream.h"
#include "mozilla/dom/nsCSPContext.h"
#include "nsStreamListenerWrapper.h"
#include "mozilla/extensions/StreamFilterParent.h"

using namespace mozilla::dom;
using namespace mozilla::ipc;

namespace mozilla {
namespace net {

NS_INTERFACE_MAP_BEGIN(DocumentChannelChild)
  if (mWasOpened && aIID == NS_GET_IID(nsIHttpChannel)) {
    // DocumentChannelChild generally is doing an http connection
    // internally, but doesn't implement the interface. Everything
    // before AsyncOpen should be duplicated in the parent process
    // on the real http channel, but anything trying to QI to nsIHttpChannel
    // after that will be failing and get confused.
    NS_WARNING(
        "Trying to request nsIHttpChannel from DocumentChannelChild, this is "
        "likely broken");
  }
  NS_INTERFACE_MAP_ENTRY(nsITraceableChannel)
  NS_INTERFACE_MAP_ENTRY_CONCRETE(DocumentChannelChild)
NS_INTERFACE_MAP_END_INHERITING(nsBaseChannel)

NS_IMPL_ADDREF_INHERITED(DocumentChannelChild, nsBaseChannel)
NS_IMPL_RELEASE_INHERITED(DocumentChannelChild, nsBaseChannel)

DocumentChannelChild::DocumentChannelChild(
    nsDocShellLoadState* aLoadState, net::LoadInfo* aLoadInfo,
    const nsString* aInitiatorType, nsLoadFlags aLoadFlags, uint32_t aLoadType,
    uint32_t aCacheKey, bool aIsActive, bool aIsTopLevelDoc,
    bool aHasNonEmptySandboxingFlags)
    : mLoadState(aLoadState),
      mInitiatorType(aInitiatorType ? Some(*aInitiatorType) : Nothing()),
      mLoadType(aLoadType),
      mCacheKey(aCacheKey),
      mIsActive(aIsActive),
      mIsTopLevelDoc(aIsTopLevelDoc),
      mHasNonEmptySandboxingFlags(aHasNonEmptySandboxingFlags) {
  mEventQueue = new ChannelEventQueue(static_cast<nsIChannel*>(this));
  SetURI(aLoadState->URI());
  SetLoadInfo(aLoadInfo);
  SetLoadFlags(aLoadFlags);
  RefPtr<nsHttpHandler> handler = nsHttpHandler::GetInstance();
  uint64_t channelId;
  Unused << handler->NewChannelId(channelId);
  mChannelId.emplace(channelId);
  mAsyncOpenTime = TimeStamp::Now();
}

NS_IMETHODIMP
DocumentChannelChild::AsyncOpen(nsIStreamListener* aListener) {
  nsresult rv = NS_OK;

  nsCOMPtr<nsIStreamListener> listener = aListener;
  rv = nsContentSecurityManager::doContentSecurityCheck(this, listener);
  NS_ENSURE_SUCCESS(rv, rv);

  NS_ENSURE_TRUE(gNeckoChild, NS_ERROR_FAILURE);
  NS_ENSURE_ARG_POINTER(listener);
  NS_ENSURE_TRUE(!mIsPending, NS_ERROR_IN_PROGRESS);
  NS_ENSURE_TRUE(!mWasOpened, NS_ERROR_ALREADY_OPENED);

  // Port checked in parent, but duplicate here so we can return with error
  // immediately, as we've done since before e10s.
  rv = NS_CheckPortSafety(nsBaseChannel::URI());  // Need to disambiguate,
                                                  // because in the child ipdl,
                                                  // a typedef URI is defined...
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIURI> topWindowURI;
  nsCOMPtr<nsIURI> uriBeingLoaded =
      AntiTrackingCommon::MaybeGetDocumentURIBeingLoaded(this);
  nsCOMPtr<nsIPrincipal> contentBlockingAllowListPrincipal;

  nsCOMPtr<mozIThirdPartyUtil> util = services::GetThirdPartyUtil();
  if (util) {
    nsCOMPtr<mozIDOMWindowProxy> win;
    rv =
        util->GetTopWindowForChannel(this, uriBeingLoaded, getter_AddRefs(win));
    if (NS_SUCCEEDED(rv)) {
      util->GetURIFromWindow(win, getter_AddRefs(topWindowURI));

      Unused << util->GetContentBlockingAllowListPrincipalFromWindow(
          win, uriBeingLoaded,
          getter_AddRefs(contentBlockingAllowListPrincipal));
    }
  }

  // add ourselves to the load group.
  if (mLoadGroup) {
    mLoadGroup->AddRequest(this, nullptr);
  }

  if (mCanceled) {
    // We may have been canceled already, either by on-modify-request
    // listeners or by load group observers; in that case, don't create IPDL
    // connection. See nsHttpChannel::AsyncOpen().
    return mStatus;
  }

  gHttpHandler->OnOpeningDocumentRequest(this);

  DocumentChannelCreationArgs args;

  SerializeURI(topWindowURI, args.topWindowURI());
  args.loadState() = mLoadState->Serialize();
  Maybe<LoadInfoArgs> maybeArgs;
  rv = LoadInfoToLoadInfoArgs(mLoadInfo, &maybeArgs);
  NS_ENSURE_SUCCESS(rv, rv);
  MOZ_DIAGNOSTIC_ASSERT(maybeArgs);

  if (contentBlockingAllowListPrincipal) {
    PrincipalInfo principalInfo;
    rv = PrincipalToPrincipalInfo(contentBlockingAllowListPrincipal,
                                  &principalInfo);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
    args.contentBlockingAllowListPrincipal() = Some(principalInfo);
  }

  args.loadInfo() = *maybeArgs;
  GetLoadFlags(&args.loadFlags());
  args.initiatorType() = mInitiatorType;
  args.loadType() = mLoadType;
  args.cacheKey() = mCacheKey;
  args.isActive() = mIsActive;
  args.isTopLevelDoc() = mIsTopLevelDoc;
  args.hasNonEmptySandboxingFlags() = mHasNonEmptySandboxingFlags;
  args.channelId() = *mChannelId;
  args.asyncOpenTime() = mAsyncOpenTime;

  nsCOMPtr<nsILoadContext> loadContext;
  NS_QueryNotificationCallbacks(this, loadContext);
  if (loadContext) {
    nsCOMPtr<mozIDOMWindowProxy> domWindow;
    loadContext->GetAssociatedWindow(getter_AddRefs(domWindow));
    if (domWindow) {
      auto* pDomWindow = nsPIDOMWindowOuter::From(domWindow);
      nsIDocShell* docshell = pDomWindow->GetDocShell();
      if (docshell) {
        docshell->GetCustomUserAgent(args.customUserAgent());
      }
    }
  }

  nsCOMPtr<nsIBrowserChild> iBrowserChild;
  GetCallback(iBrowserChild);
  BrowserChild* browserChild = static_cast<BrowserChild*>(iBrowserChild.get());
  if (MissingRequiredBrowserChild(browserChild, "ftp")) {
    return NS_ERROR_ILLEGAL_VALUE;
  }

  // TODO: What happens if the caller has called other methods on the
  // nsIChannel after the ctor, but before this?

  gNeckoChild->SendPDocumentChannelConstructor(
      this, browserChild, IPC::SerializedLoadContext(this), args);

  mIsPending = true;
  mWasOpened = true;
  mListener = listener;

  return NS_OK;
}

IPCResult DocumentChannelChild::RecvFailedAsyncOpen(
    const nsresult& aStatusCode) {
  mEventQueue->RunOrEnqueue(new NeckoTargetChannelFunctionEvent(
      this, [self = UnsafePtr<DocumentChannelChild>(this), aStatusCode]() {
        self->ShutdownListeners(aStatusCode);
      }));
  return IPC_OK();
}

void DocumentChannelChild::ShutdownListeners(nsresult aStatusCode) {
  mStatus = aStatusCode;

  nsCOMPtr<nsIStreamListener> l = mListener;
  if (l) {
    l->OnStartRequest(this);
  }

  mIsPending = false;

  l = mListener;  // it might have changed!
  if (l) {
    l->OnStopRequest(this, aStatusCode);
  }
  mListener = nullptr;
  mCallbacks = nullptr;

  if (mLoadGroup) {
    mLoadGroup->RemoveRequest(this, nullptr, aStatusCode);
    mLoadGroup = nullptr;
  }

  if (CanSend()) {
    Send__delete__(this);
  }
}

IPCResult DocumentChannelChild::RecvDisconnectChildListeners(
    const nsresult& aStatus) {
  MOZ_ASSERT(NS_FAILED(aStatus));
  // Make sure we remove from the load group before
  // setting mStatus, as existing tests expect the
  // status to be successful when we disconnect.
  if (mLoadGroup) {
    mLoadGroup->RemoveRequest(this, nullptr, aStatus);
    mLoadGroup = nullptr;
  }

  ShutdownListeners(aStatus);
  return IPC_OK();
}

IPCResult DocumentChannelChild::RecvDeleteSelf() {
  // This calls NeckoChild::DeallocPGenericChannel(), which deletes |this| if
  // IPDL holds the last reference.  Don't rely on |this| existing after here!
  Send__delete__(this);
  return IPC_OK();
}

IPCResult DocumentChannelChild::RecvRedirectToRealChannel(
    const uint32_t& aRegistrarId, nsIURI* aURI, const uint32_t& aNewLoadFlags,
    const Maybe<ReplacementChannelConfigInit>& aInit,
    const Maybe<LoadInfoArgs>& aLoadInfo,
    nsTArray<DocumentChannelRedirect>&& aRedirects, const uint64_t& aChannelId,
    nsIURI* aOriginalURI, const uint32_t& aRedirectMode,
    const uint32_t& aRedirectFlags, const Maybe<uint32_t>& aContentDisposition,
    const Maybe<nsString>& aContentDispositionFilename,
    RedirectToRealChannelResolver&& aResolve) {
  RefPtr<dom::Document> loadingDocument;
  mLoadInfo->GetLoadingDocument(getter_AddRefs(loadingDocument));

  RefPtr<dom::Document> cspToInheritLoadingDocument;
  nsCOMPtr<nsIContentSecurityPolicy> policy = mLoadInfo->GetCspToInherit();
  if (policy) {
    nsWeakPtr ctx =
        static_cast<nsCSPContext*>(policy.get())->GetLoadingContext();
    cspToInheritLoadingDocument = do_QueryReferent(ctx);
  }
  nsCOMPtr<nsILoadInfo> loadInfo;
  MOZ_ALWAYS_SUCCEEDS(LoadInfoArgsToLoadInfo(aLoadInfo, loadingDocument,
                                             cspToInheritLoadingDocument,
                                             getter_AddRefs(loadInfo)));

  mRedirects = std::move(aRedirects);
  mRedirectResolver = std::move(aResolve);

  nsCOMPtr<nsIChannel> newChannel;
  nsresult rv =
      NS_NewChannelInternal(getter_AddRefs(newChannel), aURI, loadInfo,
                            nullptr,     // PerformanceStorage
                            mLoadGroup,  // aLoadGroup
                            nullptr,     // aCallbacks
                            aNewLoadFlags);

  RefPtr<HttpChannelChild> httpChild = do_QueryObject(newChannel);
  RefPtr<nsIChildChannel> childChannel = do_QueryObject(newChannel);
  if (NS_FAILED(rv)) {
    MOZ_DIAGNOSTIC_ASSERT(false, "NS_NewChannelInternal failed");
    return IPC_OK();
  }

  // This is used to report any errors back to the parent by calling
  // CrossProcessRedirectFinished.
  auto scopeExit = MakeScopeExit([&]() {
    mRedirectResolver(rv);
    mRedirectResolver = nullptr;
  });

  if (httpChild) {
    rv = httpChild->SetChannelId(aChannelId);
  }
  if (NS_FAILED(rv)) {
    return IPC_OK();
  }

  rv = newChannel->SetOriginalURI(aOriginalURI);
  if (NS_FAILED(rv)) {
    return IPC_OK();
  }

  if (httpChild) {
    rv = httpChild->SetRedirectMode(aRedirectMode);
  }
  if (NS_FAILED(rv)) {
    return IPC_OK();
  }

  newChannel->SetNotificationCallbacks(mCallbacks);

  if (aInit) {
    HttpBaseChannel::ReplacementChannelConfig config(*aInit);
    HttpBaseChannel::ConfigureReplacementChannel(
        newChannel, config,
        HttpBaseChannel::ConfigureReason::DocumentChannelReplacement);
  }

  if (aContentDisposition) {
    newChannel->SetContentDisposition(*aContentDisposition);
  }

  if (aContentDispositionFilename) {
    newChannel->SetContentDispositionFilename(*aContentDispositionFilename);
  }

  // transfer any properties. This appears to be entirely a content-side
  // interface and isn't copied across to the parent. Copying the values
  // for this from this into the new actor will work, since the parent
  // won't have the right details anyway.
  // TODO: What about the process switch equivalent
  // (ContentChild::RecvCrossProcessRedirect)? In that case there is no local
  // existing actor in the destination process... We really need all information
  // to go up to the parent, and then come down to the new child actor.
  nsCOMPtr<nsIWritablePropertyBag> bag(do_QueryInterface(newChannel));
  if (bag) {
    for (auto iter = mPropertyHash.Iter(); !iter.Done(); iter.Next()) {
      bag->SetProperty(iter.Key(), iter.UserData());
    }
  }

  // connect parent.
  if (childChannel) {
    rv = childChannel->ConnectParent(aRegistrarId);  // creates parent channel
    if (NS_FAILED(rv)) {
      return IPC_OK();
    }
  }
  mRedirectChannel = newChannel;

  nsCOMPtr<nsIEventTarget> target = GetNeckoTarget();
  MOZ_ASSERT(target);
  rv = gHttpHandler->AsyncOnChannelRedirect(this, newChannel, aRedirectFlags,
                                            target);

  if (NS_SUCCEEDED(rv)) {
    scopeExit.release();
  }

  // scopeExit will call CrossProcessRedirectFinished(rv) here
  return IPC_OK();
}

NS_IMETHODIMP
DocumentChannelChild::OnRedirectVerifyCallback(nsresult aStatusCode) {
  nsCOMPtr<nsIChannel> redirectChannel = std::move(mRedirectChannel);
  RedirectToRealChannelResolver redirectResolver = std::move(mRedirectResolver);

  // If we've already shut down, then just notify the parent that
  // we're done.
  if (NS_FAILED(mStatus)) {
    redirectChannel->SetNotificationCallbacks(nullptr);
    redirectResolver(aStatusCode);
    return NS_OK;
  }

  nsresult rv = aStatusCode;
  if (NS_SUCCEEDED(rv)) {
    if (nsCOMPtr<nsIChildChannel> childChannel =
            do_QueryInterface(redirectChannel)) {
      rv = childChannel->CompleteRedirectSetup(mListener, nullptr);
    } else {
      rv = redirectChannel->AsyncOpen(mListener);
    }
  } else {
    redirectChannel->SetNotificationCallbacks(nullptr);
  }

  redirectResolver(rv);

  if (NS_FAILED(rv)) {
    ShutdownListeners(rv);
    return NS_OK;
  }

  if (mLoadGroup) {
    mLoadGroup->RemoveRequest(this, nullptr, NS_BINDING_REDIRECTED);
  }
  mCallbacks = nullptr;
  mListener = nullptr;

  // This calls NeckoChild::DeallocPDocumentChannel(), which deletes |this| if
  // IPDL holds the last reference.  Don't rely on |this| existing after here!
  if (CanSend()) {
    Send__delete__(this);
  }

  return NS_OK;
}

IPCResult DocumentChannelChild::RecvConfirmRedirect(
    const LoadInfoArgs& aLoadInfo, nsIURI* aNewUri,
    ConfirmRedirectResolver&& aResolve) {
  // This is effectively the same as AsyncOnChannelRedirect, except since we're
  // not propagating the redirect into this process, we don't have an nsIChannel
  // for the redirection and we have to do the checks manually.
  // This just checks CSP thus far, hopefully there's not much else needed.
  RefPtr<dom::Document> loadingDocument;
  mLoadInfo->GetLoadingDocument(getter_AddRefs(loadingDocument));
  RefPtr<dom::Document> cspToInheritLoadingDocument;
  nsCOMPtr<nsIContentSecurityPolicy> policy = mLoadInfo->GetCspToInherit();
  if (policy) {
    nsWeakPtr ctx =
        static_cast<nsCSPContext*>(policy.get())->GetLoadingContext();
    cspToInheritLoadingDocument = do_QueryReferent(ctx);
  }
  nsCOMPtr<nsILoadInfo> loadInfo;
  MOZ_ALWAYS_SUCCEEDS(LoadInfoArgsToLoadInfo(Some(aLoadInfo), loadingDocument,
                                             cspToInheritLoadingDocument,
                                             getter_AddRefs(loadInfo)));

  nsCOMPtr<nsIURI> originalUri;
  nsresult rv = GetOriginalURI(getter_AddRefs(originalUri));
  if (NS_FAILED(rv)) {
    aResolve(Tuple<const nsresult&, const Maybe<nsresult>&>(NS_BINDING_FAILED,
                                                            Some(rv)));
    return IPC_OK();
  }

  Maybe<nsresult> cancelCode;
  rv = CSPService::ConsultCSPForRedirect(originalUri, aNewUri, mLoadInfo,
                                         cancelCode);
  aResolve(Tuple<const nsresult&, const Maybe<nsresult>&>(rv, cancelCode));
  return IPC_OK();
}

mozilla::ipc::IPCResult DocumentChannelChild::RecvAttachStreamFilter(
    Endpoint<extensions::PStreamFilterParent>&& aEndpoint) {
  extensions::StreamFilterParent::Attach(this, std::move(aEndpoint));
  return IPC_OK();
}

//-----------------------------------------------------------------------------
// DocumentChannelChild::nsITraceableChannel
//-----------------------------------------------------------------------------

NS_IMETHODIMP
DocumentChannelChild::SetNewListener(nsIStreamListener* aListener,
                                     nsIStreamListener** _retval) {
  NS_ENSURE_ARG_POINTER(aListener);

  nsCOMPtr<nsIStreamListener> wrapper = new nsStreamListenerWrapper(mListener);

  wrapper.forget(_retval);
  mListener = aListener;
  return NS_OK;
}

NS_IMETHODIMP
DocumentChannelChild::Cancel(nsresult aStatusCode) {
  if (mCanceled) {
    return NS_OK;
  }

  mCanceled = true;
  if (CanSend()) {
    SendCancel(aStatusCode);
  }

  ShutdownListeners(aStatusCode);

  return NS_OK;
}

NS_IMETHODIMP
DocumentChannelChild::Suspend() {
  NS_ENSURE_TRUE(CanSend(), NS_ERROR_NOT_AVAILABLE);

  if (!mSuspendCount++) {
    SendSuspend();
  }

  mEventQueue->Suspend();
  return NS_OK;
}

NS_IMETHODIMP
DocumentChannelChild::Resume() {
  NS_ENSURE_TRUE(CanSend(), NS_ERROR_NOT_AVAILABLE);

  MOZ_ASSERT(mSuspendCount);
  if (!--mSuspendCount) {
    SendResume();
  }

  mEventQueue->Resume();
  return NS_OK;
}

}  // namespace net
}  // namespace mozilla
