/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_BroadcastChannelService_h
#define mozilla_dom_BroadcastChannelService_h

#include "nsISupportsImpl.h"
#include "nsHashKeys.h"
#include "nsTHashtable.h"

#ifdef XP_WIN
#undef PostMessage
#endif

namespace mozilla {
namespace dom {

class BroadcastChannelParent;
class ClonedMessageData;

class BroadcastChannelService MOZ_FINAL
{
public:
  NS_INLINE_DECL_REFCOUNTING(BroadcastChannelService)

  static already_AddRefed<BroadcastChannelService> GetOrCreate();

  void RegisterActor(BroadcastChannelParent* aParent);
  void UnregisterActor(BroadcastChannelParent* aParent);

  void PostMessage(BroadcastChannelParent* aParent,
                   const ClonedMessageData& aData,
                   const nsAString& aOrigin,
                   const nsAString& aChannel);

private:
  BroadcastChannelService();
  ~BroadcastChannelService();

  nsTHashtable<nsPtrHashKey<BroadcastChannelParent>> mAgents;
};

} // dom namespace
} // mozilla namespace

#endif // mozilla_dom_BroadcastChannelService_h
