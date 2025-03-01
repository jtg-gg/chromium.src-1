// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CC_SURFACES_SURFACE_AGGREGATOR_H_
#define CC_SURFACES_SURFACE_AGGREGATOR_H_

#include <set>
#include <unordered_map>
#include <unordered_set>

#include "base/macros.h"
#include "base/memory/scoped_ptr.h"
#include "base/memory/weak_ptr.h"
#include "cc/quads/draw_quad.h"
#include "cc/quads/render_pass.h"
#include "cc/resources/transferable_resource.h"
#include "cc/surfaces/surface_id.h"
#include "cc/surfaces/surfaces_export.h"

namespace cc {

class CompositorFrame;
class DelegatedFrameData;
class ResourceProvider;
class Surface;
class SurfaceDrawQuad;
class SurfaceManager;

class CC_SURFACES_EXPORT SurfaceAggregatorClient {
 public:
  virtual ~SurfaceAggregatorClient() {}

  virtual void AddSurface(Surface* surface) = 0;
  virtual void RemoveSurface(Surface* surface) = 0;
};

class CC_SURFACES_EXPORT SurfaceAggregator {
 public:
  using SurfaceIndexMap = std::unordered_map<SurfaceId, int, SurfaceIdHash>;

  SurfaceAggregator(SurfaceAggregatorClient* client,
                    SurfaceManager* manager,
                    ResourceProvider* provider,
                    bool aggregate_only_damaged);
  ~SurfaceAggregator();

  scoped_ptr<CompositorFrame> Aggregate(SurfaceId surface_id);
  void ReleaseResources(SurfaceId surface_id);
  SurfaceIndexMap& previous_contained_surfaces() {
    return previous_contained_surfaces_;
  }
  void SetFullDamageForSurface(SurfaceId surface_id);

 private:
  struct ClipData {
    ClipData() : is_clipped(false) {}
    ClipData(bool is_clipped, const gfx::Rect& rect)
        : is_clipped(is_clipped), rect(rect) {}

    bool is_clipped;
    gfx::Rect rect;
  };

  struct PrewalkResult {
    PrewalkResult();
    ~PrewalkResult();
    bool has_copy_requests = false;
    // This is the set of Surfaces that were referenced by another Surface, but
    // not included in a SurfaceDrawQuad.
    std::set<SurfaceId> undrawn_surfaces;
  };

  ClipData CalculateClipRect(const ClipData& surface_clip,
                             const ClipData& quad_clip,
                             const gfx::Transform& target_transform);

  RenderPassId RemapPassId(RenderPassId surface_local_pass_id,
                           SurfaceId surface_id);

  void HandleSurfaceQuad(const SurfaceDrawQuad* surface_quad,
                         const gfx::Transform& target_transform,
                         const ClipData& clip_rect,
                         RenderPass* dest_pass);
  SharedQuadState* CopySharedQuadState(const SharedQuadState* source_sqs,
                                       const gfx::Transform& target_transform,
                                       const ClipData& clip_rect,
                                       RenderPass* dest_render_pass);
  void CopyQuadsToPass(
      const QuadList& source_quad_list,
      const SharedQuadStateList& source_shared_quad_state_list,
      const std::unordered_map<ResourceId, ResourceId>& resource_to_child_map,
      const gfx::Transform& target_transform,
      const ClipData& clip_rect,
      RenderPass* dest_pass,
      SurfaceId surface_id);
  gfx::Rect PrewalkTree(SurfaceId surface_id, PrewalkResult* result);
  void CopyUndrawnSurfaces(PrewalkResult* prewalk);
  void CopyPasses(const DelegatedFrameData* frame_data, Surface* surface);

  // Remove Surfaces that were referenced before but aren't currently
  // referenced from the ResourceProvider.
  // Also notifies SurfaceAggregatorClient of newly added and removed
  // child surfaces.
  void ProcessAddedAndRemovedSurfaces();

  int ChildIdForSurface(Surface* surface);
  gfx::Rect DamageRectForSurface(const Surface* surface,
                                 const RenderPass& source,
                                 const gfx::Rect& full_rect) const;

  SurfaceAggregatorClient* client_;  // Outlives this class.
  SurfaceManager* manager_;
  ResourceProvider* provider_;

  class RenderPassIdAllocator;
  using RenderPassIdAllocatorMap =
      std::unordered_map<SurfaceId,
                         scoped_ptr<RenderPassIdAllocator>,
                         SurfaceIdHash>;
  RenderPassIdAllocatorMap render_pass_allocator_map_;
  int next_render_pass_id_;
  const bool aggregate_only_damaged_;

  using SurfaceToResourceChildIdMap =
      std::unordered_map<SurfaceId, int, SurfaceIdHash>;
  SurfaceToResourceChildIdMap surface_id_to_resource_child_id_;

  // The following state is only valid for the duration of one Aggregate call
  // and is only stored on the class to avoid having to pass through every
  // function call.

  // This is the set of surfaces referenced in the aggregation so far, used to
  // detect cycles.
  using SurfaceSet = std::set<SurfaceId>;
  SurfaceSet referenced_surfaces_;

  // For each Surface used in the last aggregation, gives the frame_index at
  // that time.
  SurfaceIndexMap previous_contained_surfaces_;
  SurfaceIndexMap contained_surfaces_;

  // After surface validation, every Surface in this set is valid.
  std::unordered_set<SurfaceId, SurfaceIdHash> valid_surfaces_;

  // This is the pass list for the aggregated frame.
  RenderPassList* dest_pass_list_;

  // The root damage rect of the currently-aggregating frame.
  gfx::Rect root_damage_rect_;

  // True if the frame that's currently being aggregated has copy requests.
  // This is valid during Aggregate after PrewalkTree is called.
  bool has_copy_requests_;

  // Resource list for the aggregated frame.
  TransferableResourceArray* dest_resource_list_;

  base::WeakPtrFactory<SurfaceAggregator> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(SurfaceAggregator);
};

}  // namespace cc

#endif  // CC_SURFACES_SURFACE_AGGREGATOR_H_
