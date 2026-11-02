#include "forward/probes/automatic_capture.h"
#include "forward/probes/capture_dump.h"
#include "forward/probes/placement.h"
#include "platform/workers.h"
#include "scene/types.h"
#include <cstring>

namespace remix::forward::probes {
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
      WorkerPool workers;
      filtered = probes::filter(captures, 128, cancelled, &workers);
    }
    prepareImage(physical, device, cancelled);
  }
  void AutomaticCapture::prepareImage(VkPhysicalDevice physical, VkDevice device, std::stop_token cancelled) {
    requirePreparing();
    image = std::make_shared<ImageAsset>(physical, device, filteredView(), cancelled);
  }
  void AutomaticCapture::prepareContent(std::stop_token cancelled) {
    requirePreparing();
    content = captureContent(captureView(), filteredView(), inputHash, cancelled);
  }
  void AutomaticCapture::sealImage() {
    if (!image || m_imageReady.load(std::memory_order_relaxed))
      throw std::logic_error("Reflection image completion requires one prepared image");
    m_imageReady.store(true, std::memory_order_release);
  }
  std::shared_ptr<ImageAsset> AutomaticCapture::recordImageUpload(VkCommandBuffer command) {
    if (m_imageUploaded || !m_imageReady.load(std::memory_order_acquire)) return {};
    if (!image) throw std::logic_error("Ready reflection publication has no image");
    image->recordUpload(command);
    m_imageUploaded = true;
    return image;
  }

}
