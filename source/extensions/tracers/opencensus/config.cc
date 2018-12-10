#include "extensions/tracers/opencensus/config.h"

#include <iostream>

#include "envoy/registry/registry.h"

#include "common/tracing/http_tracer_impl.h"

#include "extensions/tracers/opencensus/opencensus_tracer_impl.h"
#include "extensions/tracers/well_known_names.h"

namespace Envoy {
namespace Extensions {
namespace Tracers {
namespace OpenCensus {

OpenCensusTracerFactory::OpenCensusTracerFactory() : FactoryBase(TracerNames::get().OpenCensus) {
  std::cerr << "OpenCensusTracerFactory()\n";
}

Tracing::HttpTracerPtr OpenCensusTracerFactory::createHttpTracerTyped(
    const envoy::config::trace::v2::OpenCensusConfig& proto_config, Server::Instance& server) {
  std::cerr << "OpenCensusTracerFactory::createHttpTracerTyped()\n";
  Tracing::DriverPtr driver = std::make_unique<Driver>(proto_config);
  return std::make_unique<Tracing::HttpTracerImpl>(std::move(driver), server.localInfo());
}

/**
 * Static registration for the OpenCensus tracer. @see RegisterFactory.
 */
static Registry::RegisterFactory<OpenCensusTracerFactory, Server::Configuration::TracerFactory>
    register_;

} // namespace OpenCensus
} // namespace Tracers
} // namespace Extensions
} // namespace Envoy
