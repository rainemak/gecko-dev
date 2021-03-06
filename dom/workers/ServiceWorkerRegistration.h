/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_ServiceWorkerRegistration_h
#define mozilla_dom_ServiceWorkerRegistration_h

#include "mozilla/DOMEventTargetHelper.h"
#include "mozilla/dom/ServiceWorkerCommon.h"

class nsPIDOMWindow;

namespace mozilla {
namespace dom {

class Promise;

namespace workers {
class ServiceWorker;
}

class ServiceWorkerRegistration MOZ_FINAL : public DOMEventTargetHelper
                                          , public nsIObserver
{
public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_NSIOBSERVER
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(ServiceWorkerRegistration,
                                           DOMEventTargetHelper)

  IMPL_EVENT_HANDLER(updatefound)

  ServiceWorkerRegistration(nsPIDOMWindow* aWindow,
                            const nsAString& aScope);

  nsPIDOMWindow*
  GetParentObject() const
  {
    return mWindow;
  }

  JSObject*
  WrapObject(JSContext* aCx);

  already_AddRefed<workers::ServiceWorker>
  GetInstalling();

  already_AddRefed<workers::ServiceWorker>
  GetWaiting();

  already_AddRefed<workers::ServiceWorker>
  GetActive();

  void
  GetScope(nsAString& aScope) const
  {
    aScope = mScope;
  }

  already_AddRefed<Promise>
  Unregister(ErrorResult& aRv);

  // Useful methods for ServiceWorkerManager:

  nsIURI*
  GetDocumentURI() const;

  void
  InvalidateWorkerReference(WhichServiceWorker aWhichOnes);

private:
  ~ServiceWorkerRegistration();

  already_AddRefed<workers::ServiceWorker>
  GetWorkerReference(WhichServiceWorker aWhichOne);

  void
  StartListeningForEvents();

  void
  StopListeningForEvents();

  nsCOMPtr<nsPIDOMWindow> mWindow;

  // The following properties are cached here to ensure JS equality is satisfied
  // instead of acquiring a new worker instance from the ServiceWorkerManager
  // for every access. A null value is considered a cache miss.
  // These three may change to a new worker at any time.
  nsRefPtr<workers::ServiceWorker> mInstallingWorker;
  nsRefPtr<workers::ServiceWorker> mWaitingWorker;
  nsRefPtr<workers::ServiceWorker> mActiveWorker;

  const nsString mScope;

  uint64_t mInnerID;
  bool mIsListeningForEvents;
};

} // namespace dom
} // namespace mozilla

#endif /* mozilla_dom_ServiceWorkerRegistration_h */
