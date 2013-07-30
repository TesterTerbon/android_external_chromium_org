// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/aura/browser_compositor_output_surface_proxy.h"

#include "base/bind.h"
#include "content/browser/aura/browser_compositor_output_surface.h"
#include "content/browser/gpu/browser_gpu_channel_host_factory.h"
#include "content/common/gpu/gpu_messages.h"

namespace content {

BrowserCompositorOutputSurfaceProxy::BrowserCompositorOutputSurfaceProxy(
    RefCountedIDMap<BrowserCompositorOutputSurface>* surface_map)
    : surface_map_(surface_map),
      connected_to_gpu_process_host_(false) {}

BrowserCompositorOutputSurfaceProxy::~BrowserCompositorOutputSurfaceProxy() {}

void BrowserCompositorOutputSurfaceProxy::ConnectToGpuProcessHost(
    base::SingleThreadTaskRunner* compositor_thread_task_runner) {
  // TODO(danakj): When the GpuProcessHost changes (e.g. after GPU proces
  // crash), we must connect to the new one to continue receiving these
  // updates.
  if (connected_to_gpu_process_host_)
    return;

  const uint32 kMessagesToFilter[] = { GpuHostMsg_UpdateVSyncParameters::ID };
  BrowserGpuChannelHostFactory::instance()->SetHandlerForControlMessages(
      kMessagesToFilter,
      arraysize(kMessagesToFilter),
      base::Bind(&BrowserCompositorOutputSurfaceProxy::
                     OnMessageReceivedOnCompositorThread,
                 this),
      compositor_thread_task_runner);
  connected_to_gpu_process_host_ = true;
}

void BrowserCompositorOutputSurfaceProxy::OnMessageReceivedOnCompositorThread(
    const IPC::Message& message) {
  IPC_BEGIN_MESSAGE_MAP(BrowserCompositorOutputSurfaceProxy, message)
      IPC_MESSAGE_HANDLER(GpuHostMsg_UpdateVSyncParameters,
                          OnUpdateVSyncParametersOnCompositorThread);
  IPC_END_MESSAGE_MAP()
}

void
BrowserCompositorOutputSurfaceProxy::OnUpdateVSyncParametersOnCompositorThread(
    int surface_id,
    base::TimeTicks timebase,
    base::TimeDelta interval) {
  BrowserCompositorOutputSurface* surface = surface_map_->Lookup(surface_id);
  if (surface)
    surface->OnUpdateVSyncParameters(timebase, interval);
}
}  // namespace content
