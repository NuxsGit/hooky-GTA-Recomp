#pragma once

#include "contact_rays.h"

#include <plume_render_interface.h>

namespace gtav::native {

// Alpha is the guest's light visibility. Keep RGB and all depth/stencil state intact.
inline plume::RenderGraphicsPipelineDesc ContactRayPipelineDesc(const plume::RenderPipelineLayout* layout,
                                                                const plume::RenderShader* vs,
                                                                const plume::RenderShader* ps) {
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = layout;
  desc.vertexShader = vs;
  desc.pixelShader = ps;
  desc.depthClipEnabled = true;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = plume::RenderFormat::R8G8B8A8_UNORM;
  auto& blend = desc.renderTargetBlend[0];
  blend = plume::RenderBlendDesc::Copy();
  blend.blendEnabled = true;
  blend.srcBlendAlpha = plume::RenderBlend::ZERO;
  blend.dstBlendAlpha = plume::RenderBlend::SRC_ALPHA;
  blend.renderTargetWriteMask = uint8_t(plume::RenderColorWriteEnable::ALPHA);
  return desc;
}

// The caller has already bound the verified mask, native layout and texture descriptors.
inline void RecordContactRayMask(plume::RenderCommandList* list, const plume::RenderPipeline* pipeline,
                                 Extent mask, const ContactRayPush& push) {
  list->setPipeline(pipeline);
  list->setViewports(plume::RenderViewport(0, 0, float(mask.width), float(mask.height)));
  list->setScissors(plume::RenderRect(0, 0, int32_t(mask.width), int32_t(mask.height)));
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
}

}  // namespace gtav::native
