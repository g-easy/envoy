#include "extensions/tracers/opencensus/opencensus_tracer_impl.h"

#include "absl/strings/str_cat.h"
#include "opencensus/exporters/trace/stackdriver/stackdriver_exporter.h"
#include "opencensus/exporters/trace/stdout/stdout_exporter.h"
#include "opencensus/exporters/trace/zipkin/zipkin_exporter.h"
#include "opencensus/trace/propagation/cloud_trace_context.h"
#include "opencensus/trace/sampler.h"
#include "opencensus/trace/span.h"
#include "opencensus/trace/trace_config.h"
#include "opencensus/trace/trace_params.h"

namespace Envoy {
namespace Extensions {
namespace Tracers {
namespace OpenCensus {

namespace {

/**
 * OpenCensus tracing implementation of the Envoy Span object.
 */
class Span : public Tracing::Span {
public:
  Span(const Tracing::Config& config, const envoy::config::trace::v2::OpenCensusConfig* oc_config,
       Http::HeaderMap& request_headers, const std::string& operation_name, SystemTime start_time,
       const Tracing::Decision tracing_decision);

  // Used by spawnChild().
  Span(const envoy::config::trace::v2::OpenCensusConfig* oc_config,
       ::opencensus::trace::Span&& span);

  void setOperation(const std::string& operation) override;
  void setTag(const std::string& name, const std::string& value) override;
  void finishSpan() override;
  void injectContext(Http::HeaderMap& request_headers) override;
  Tracing::SpanPtr spawnChild(const Tracing::Config& config, const std::string& name,
                              SystemTime start_time) override;
  void setSampled(bool sampled) override;

private:
  // TODO: Do we take the sampling decision from StartSpan, or use the sampler
  // configured in TraceConfig?
  ::opencensus::trace::Sampler* GetSampler(bool is_sampled) {
    static ::opencensus::trace::AlwaysSampler always_sampler;
    static ::opencensus::trace::NeverSampler never_sampler;
    if (is_sampled)
      return &always_sampler;
    return &never_sampler;
  }

  ::opencensus::trace::Span span_;
  const envoy::config::trace::v2::OpenCensusConfig* oc_config_;
};

Span::Span(const Tracing::Config& config,
           const envoy::config::trace::v2::OpenCensusConfig* oc_config,
           Http::HeaderMap& /*request_headers*/, const std::string& operation_name,
           SystemTime /*start_time*/, const Tracing::Decision tracing_decision)
    : span_(::opencensus::trace::Span::StartSpan(
          operation_name, /*parent=*/nullptr,
          ::opencensus::trace::StartSpanOptions(GetSampler(tracing_decision.traced)))),
      oc_config_(oc_config) {
  span_.AddAttribute("OperationName", config.operationName() == Tracing::OperationName::Ingress
                                          ? "Ingress"
                                          : "Egress");
}

Span::Span(const envoy::config::trace::v2::OpenCensusConfig* oc_config,
           ::opencensus::trace::Span&& span)
    : span_(std::move(span)), oc_config_(oc_config) {}

void Span::setOperation(const std::string& operation) {
  span_.AddAnnotation("setOperation", {{"operation", operation}});
}

void Span::setTag(const std::string& name, const std::string& value) {
  span_.AddAttribute(name, value);
}

void Span::finishSpan() { span_.End(); }

void Span::injectContext(Http::HeaderMap& request_headers) {
  if (oc_config_->propagate_cloud_trace_context()) {
    request_headers.addCopy(
        Http::LowerCaseString{"x-cloud-trace-context"},
        ::opencensus::trace::propagation::ToCloudTraceContextHeader(span_.context()));
  }
  if (oc_config_->propagate_trace_context()) {
    // TODO: Replace this with propagation/trace_context.h after
    // https://github.com/census-instrumentation/opencensus-cpp/pull/260 is merged.
    request_headers.addCopy(Http::LowerCaseString{"traceparent"},
                            absl::StrCat("00-", span_.context().ToString()));
  }
  if (oc_config_->propagate_grpc_trace_bin()) {
    // TODO: Unimplemented.
  }
}

Tracing::SpanPtr Span::spawnChild(const Tracing::Config& /*config*/, const std::string& name,
                                  SystemTime /*start_time*/) {
  span_.AddAnnotation("spawnChild");
  return std::make_unique<Span>(oc_config_,
                                ::opencensus::trace::Span::StartSpan(name, /*parent=*/&span_));
}

void Span::setSampled(bool sampled) { span_.AddAnnotation("setSampled", {{"sampled", sampled}}); }

} // namespace

Driver::Driver(const envoy::config::trace::v2::OpenCensusConfig& oc_config)
    : oc_config_(oc_config) {
  ApplyTraceConfig(oc_config.trace_config());
  if (oc_config.stdout_exporter_enabled()) {
    ::opencensus::exporters::trace::StdoutExporter::Register();
  }
  if (oc_config.stackdriver_exporter_enabled()) {
    ::opencensus::exporters::trace::StackdriverOptions opts;
    opts.project_id = oc_config.stackdriver_project_id();
    ::opencensus::exporters::trace::StackdriverExporter::Register(opts);
  }
  if (oc_config.zipkin_exporter_enabled()) {
    ::opencensus::exporters::trace::ZipkinExporterOptions opts(oc_config.zipkin_url());
    opts.service_name = oc_config.zipkin_service_name();
    ::opencensus::exporters::trace::ZipkinExporter::Register(opts);
  }
  if (oc_config.propagate_grpc_trace_bin()) {
    ENVOY_LOG(warn, "propagate_grpc_trace_bin is unimplemented.");
  }
}

void Driver::ApplyTraceConfig(const opencensus::proto::trace::v1::TraceConfig& config) {
  using SamplerCase = opencensus::proto::trace::v1::TraceConfig::SamplerCase;
  constexpr double kDefaultSamplingProbability = 1e-4;
  double probability = kDefaultSamplingProbability;

  switch (config.sampler_case()) {
  case SamplerCase::kProbabilitySampler:
    probability = config.probability_sampler().samplingprobability();
    break;
  case SamplerCase::kConstantSampler:
    probability = config.constant_sampler().decision() ? 1. : 0.;
    break;
  case SamplerCase::kRateLimitingSampler:
    ENVOY_LOG(error, "RateLimitingSampler is not supported.");
  case SamplerCase::SAMPLER_NOT_SET:
    break; // Keep default.
  default:
    ENVOY_LOG(error, "Unknown sampler type in TraceConfig.");
  }

  ::opencensus::trace::TraceConfig::SetCurrentTraceParams(::opencensus::trace::TraceParams{
      uint32_t(config.max_number_of_attributes()), uint32_t(config.max_number_of_annotations()),
      uint32_t(config.max_number_of_message_events()), uint32_t(config.max_number_of_links()),
      ::opencensus::trace::ProbabilitySampler(probability)});
}

Tracing::SpanPtr Driver::startSpan(const Tracing::Config& config, Http::HeaderMap& request_headers,
                                   const std::string& operation_name, SystemTime start_time,
                                   const Tracing::Decision tracing_decision) {
  return std::make_unique<Span>(config, &oc_config_, request_headers, operation_name, start_time,
                                tracing_decision);
}

} // namespace OpenCensus
} // namespace Tracers
} // namespace Extensions
} // namespace Envoy
