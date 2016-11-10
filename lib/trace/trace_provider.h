// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_TRACING_LIB_TRACE_TRACE_PROVIDER_H_
#define APPS_TRACING_LIB_TRACE_TRACE_PROVIDER_H_

#include "apps/modular/lib/app/application_context.h"
#include "apps/tracing/services/trace_manager.fidl.h"

namespace tracing {

// Initializes the global |Tracer| and registers its underlying
// |TraceProvider| with the system default trace provider registry service.
// Does not take ownership of |app_connector|.
void InitializeTracer(modular::ApplicationContext* app_context);

// Initializes the global |Tracer| and registers its underlying
// |TraceProvider| with the specified |registry|.
void InitializeTracer(TraceRegistryPtr registry);

// Destroys the global Tracer if one has been initialized with
// |InitializeTracer|.
void DestroyTracer();

}  // namespace tracing

#endif  // APPS_TRACING_LIB_TRACE_TRACE_PROVIDER_H_
