#include "forward/bake/job.h"
#include "forward/bake/storage_options.h"
#include <cstdio>
#include <fstream>

namespace remix::forward::bake {
  namespace {
    void dumpTransfer(std::span<const uint32_t> words) noexcept {
      const auto path = std::getenv("REMIX_DEBUG_COARSE_TRANSFER_OUTPUT");
      if (!path || !*path) return;
      try {
        const auto outputPath = std::filesystem::u8path(path);
        if (std::filesystem::exists(outputPath)) throw std::runtime_error("report already exists");
        std::ofstream output(outputPath,std::ios::binary);
        output.exceptions(std::ios::failbit|std::ios::badbit);
        output.write(reinterpret_cast<const char *>(words.data()),std::streamsize(words.size_bytes()));
      } catch (const std::exception &error) {
        std::fprintf(stderr,"Coarse transfer report unavailable: %s\n",error.what());
      }
    }
  }
  bool Controller::recordCoarseTransfer(VkCommandBuffer command, const Config &config) {
    if (!coarseTransferPreparationRequested()) return false;
    auto &job = *m_job;
    if (job.coarseTransferFinished) {
      // Completion required every transfer slot's fence; no remaining GPU user.
      for (auto &slot : job.coarseTransferSlots) { slot.output.reset(); slot.readback.reset(); }
      return false;
    }
    static std::atomic<uint64_t> generation{0};
    if (!job.coarseTransferPlanned) {
      job.coarseTransferPlanned = true;
      job.coarseTransferBegan = std::chrono::steady_clock::now();
      job.finishStage.store(Job::FinishStage::CoarseTransfer, std::memory_order_relaxed);
      const auto owner = m_job;
      job.ticket = m_pipelines->prepare("forward-coarse-transfer-plan-"+std::to_string(++generation), [owner] {
        CoarseTransfer::Options options;
        options.epsilon = .0001f / owner->source->sceneScale;
        owner->coarseTransfer = CoarseTransfer::plan(owner->grid, options, owner->cancellation.get_token());
        owner->coarseTransferPlanningMs = std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-owner->coarseTransferBegan).count();
        owner->finishStage.store(Job::FinishStage::None,std::memory_order_relaxed);
      });
      status(REMIXAPI_BAKE_PREPARING_EXT,"Preparing coarse probe connection visibility");
      return true;
    }
    if (!job.coarseTransfer) {
      job.coarseTransferFinished = true;
      std::fprintf(stderr,"Forward coarse transfer preparation declined: no adaptive field or budget exceeded\n");
      return false;
    }
    auto &plan = *job.coarseTransfer;
    if (plan.complete()) {
      job.coarseTransferFinished = true;
      job.finishStage.store(Job::FinishStage::CoarseTransfer,std::memory_order_relaxed);
      const auto owner = m_job;
      job.ticket = m_pipelines->prepare("forward-coarse-transfer-finish-"+std::to_string(++generation), [owner] {
        const auto words = owner->coarseTransfer->words();
        owner->coarseTransferWords = std::make_shared<const std::vector<uint32_t>>(words.begin(),words.end());
        dumpTransfer(words);
        const auto elapsed = std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-owner->coarseTransferBegan).count();
        std::fprintf(stderr,"Forward coarse transfer prepared: %u rays, %zu payload bytes, %llu owned scratch bytes; "
                            "planning_ms=%.3f gpu_ms=%.3f readback_ms=%.3f wall_ms=%.3f; pending volume publication\n",
                     owner->coarseTransfer->rays(),words.size_bytes(),
                     static_cast<unsigned long long>(owner->coarseTransfer->scratchBytes()),
                     owner->coarseTransferPlanningMs,owner->coarseTransferGpuMs,owner->coarseTransferReadbackMs,elapsed);
        owner->coarseTransfer.reset();
        owner->finishStage.store(Job::FinishStage::None,std::memory_order_relaxed);
      });
      return true;
    }
    if (job.coarseTransferNext < plan.rays()) {
      auto &submission = m_submissions[m_slot];
      if (submission.job) throw std::logic_error("Coarse transfer slot reused before its fence");
      constexpr uint32_t maximumBatch = 65536;
      const uint32_t count = std::min(maximumBatch,job.count(Work::CoarseTransfer,config,
          plan.rays()-job.coarseTransferNext,1));
      auto &slot = job.coarseTransferSlots[m_slot];
      if (!slot.output) {
        slot.output = std::make_unique<Buffer>(); slot.readback = std::make_unique<Buffer>();
        slot.output->create(m_physical,m_device,maximumBatch*sizeof(float),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,Buffer::Memory::Device);
        slot.readback->create(m_physical,m_device,maximumBatch*sizeof(float),
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,Buffer::Memory::Readback);
      }
      const auto rays = plan.batch(job.coarseTransferNext,count);
      job.transport->recordDepth(command,m_slot,rays,slot.output->descriptor(),0,plan.maximumDistance());
      const VkBufferCopy copy{0,0,count*sizeof(float)};
      vkCmdCopyBuffer(command,slot.output->handle,slot.readback->handle,1,&copy);
      const VkMemoryBarrier host{.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
          .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
      vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
          0,1,&host,0,nullptr,0,nullptr);
      submission.job = m_job; submission.coarseTransfer = true;
      submission.first = job.coarseTransferNext; submission.count = count;
      job.coarseTransferNext += count;
    }
    char progress[128];
    std::snprintf(progress,sizeof(progress),"Baking coarse probe connections (%u/%u rays)",
                  job.coarseTransferReceived,plan.rays());
    status(REMIXAPI_BAKE_TRACING_EXT,progress);
    return true;
  }
}
