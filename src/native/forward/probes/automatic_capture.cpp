#include "forward/probes/automatic_capture.h"
#include "forward/probes/capture_dump.h"
#include "forward/probes/placement.h"
#include "platform/workers.h"
#include "scene/types.h"
#include <cstring>

namespace remix::forward::probes {
  void AutomaticCapture::prepare(pipeline::Service &service, const std::string &key, std::function<void()> work) {
    if (!preparationReady()) throw std::logic_error("Reflection preparation replaced before completion");
    m_preparation = service.prepare(key, std::move(work));
  }

  void AutomaticCapture::restoreProjection(const scene::BakeScene &source, float detailTolerance,
                                            std::stop_token cancelled) {
    requirePreparing();
    // Derived overlap may change without invalidating captured radiance.
    configureInfluence(source, captures, detailTolerance, cancelled);
    for (size_t i = 0; i < captures.size(); ++i)
      filtered.header.probes[i] = projectionRecord(captures[i]);
  }
  void AutomaticCapture::prepareRays(std::stop_token cancelled) {
    requirePreparing();
    std::vector<FittedRegion>().swap(candidateRegions);
    seeds = {}; // release spatial inputs before allocating capture rays
    work = CaptureWork(captures, cancelled);
    if (!captures.empty())
      inputHash = scene::fastHash(work.rays().data(), work.rays().size() * sizeof(bake::Point)) | 1;
  }
  void AutomaticCapture::restoreCapture(std::vector<Capture> cached, Data data, std::stop_token cancelled) {
    requirePreparing();
    if (cached.size() != captures.size() || data.header.parameters[0] != captures.size()) return;
    for (size_t i = 0; i < captures.size(); ++i) {
      const auto expected = projectionInputRecord(captures[i]);
      auto actual = data.header.probes[i];
      actual.position[3] = 0; // captured confidence is output, not projection input
      if (!sameCaptureRays(captures[i], cached[i]) || std::memcmp(&expected, &actual, sizeof(expected))) return;
    }
    // Legacy raw captures omit explicit feather distance; retain the newly fitted
    // metadata and reuse only transport. The filtered header was checked above.
    for (size_t i = 0; i < captures.size(); ++i)
      captures[i].radiance = std::move(cached[i].radiance);
    filtered = std::move(data);
    inputHash = CaptureWork::inputHash(captures, cancelled);
    cachedRadiance = true;
    std::vector<FittedRegion>().swap(candidateRegions);
    seeds = {};
    work = {}; // completed cached radiance needs no GPU capture submissions
  }
  void AutomaticCapture::complete(std::stop_token cancelled) {
    requirePreparing();
    if (cancelled.stop_requested()) throw std::runtime_error("Reflection capture completion superseded");
    if (!cachedRadiance) work.complete(captures, cancelled);
    work = {};
  }
  void AutomaticCapture::filter(VkPhysicalDevice physical, VkDevice device, std::stop_token cancelled) {
    requirePreparing();
    if (!cachedRadiance) {
      if (captures.empty()) {
        filtered = probes::filter(captures, 128, cancelled);
      } else {
        WorkerPool workers;
        filtered = probes::filter(captures, 128, cancelled, &workers);
      }
    }
    prepareImage(physical, device, cancelled);
  }
  void AutomaticCapture::prepareImage(VkPhysicalDevice physical, VkDevice device, std::stop_token cancelled) {
    requirePreparing();
    if (cancelled.stop_requested()) throw std::runtime_error("Reflection image preparation superseded");
    if (emptyRadiance()) {
      image.reset();
      return;
    }
    image = std::make_shared<ImageAsset>(physical, device, filteredView(), cancelled, reference);
  }
  void AutomaticCapture::prepareContent(std::stop_token cancelled) {
    requirePreparing();
    content = captureContent(captureView(), filteredView(), inputHash, cancelled);
  }
  void AutomaticCapture::finishRetainedPublication(VkPhysicalDevice physical, VkDevice device,
                                                    std::stop_token cancelled) {
    filter(physical, device, cancelled);
    prepareContent(cancelled);
    retainPayload();
    sealPublication();
  }
  void AutomaticCapture::finishImageOnlyPublication(VkPhysicalDevice physical, VkDevice device,
                                                     std::stop_token cancelled) {
    requirePreparing();
    if (captures.empty()) throw std::logic_error("Image-only reflection publication requires captures");
    filter(physical, device, cancelled);
    captures.clear();
    filtered = {};
    sealPublication();
  }
  std::unique_ptr<const CapturePayload> AutomaticCapture::finishTransferredPublication(
      VkPhysicalDevice physical, VkDevice device, std::stop_token cancelled) {
    requirePreparing();
    if (captures.empty()) throw std::logic_error("Transferred reflection publication requires captures");
    filter(physical, device, cancelled);
    auto payload = std::make_unique<CapturePayload>();
    payload->captures = std::move(captures);
    payload->data = std::move(filtered);
    sealPublication();
    return payload;
  }
  void AutomaticCapture::sealPublication() {
    if ((!image && (!m_payload || !emptyRadiance())) || m_publicationReady.load(std::memory_order_relaxed))
      throw std::logic_error("Reflection completion requires a prepared image or retained empty payload");
    m_publicationReady.store(true, std::memory_order_release);
  }
  std::shared_ptr<ImageAsset> AutomaticCapture::recordImageUpload(VkCommandBuffer command) {
    if (m_publicationConsumed || !m_publicationReady.load(std::memory_order_acquire)) return {};
    if (!image) {
      if (!m_payload || !emptyRadiance()) throw std::logic_error("Ready reflection publication has no image");
      m_publicationConsumed = true;
      return {};
    }
    image->recordUpload(command);
    m_publicationConsumed = true;
    return image;
  }

}
