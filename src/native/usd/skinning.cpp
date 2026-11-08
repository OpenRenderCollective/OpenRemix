// Static capture composition adapted from the Godot toolkit. CPU-only OpenUSD 25.11.
#include "skinning.h"

#include <cmath>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdSkel/bindingAPI.h>
#include <pxr/usd/usdSkel/root.h>
#include <pxr/usd/usdSkel/skeletonQuery.h>
#include <pxr/usd/usdSkel/utils.h>
#include <stdexcept>

PXR_NAMESPACE_USING_DIRECTIVE
namespace remix::usd {
  namespace {
    bool finite(const GfVec3f &value) {
      return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
    }
  } // namespace

  std::string SceneSkinning::deform(const UsdGeomMesh &mesh, VtVec3fArray &points, VtVec3fArray &normals,
                                    TfToken &normal_interpolation, const VtIntArray &indices,
                                    const VtIntArray &counts) {
    const auto prim = mesh.GetPrim();
    const auto binding = UsdSkelBindingAPI(prim);
    const auto skeleton = binding.GetInheritedSkeleton();
    if (!skeleton) {
      if (UsdGeomPrimvarsAPI(prim).FindPrimvarWithInheritance(TfToken("skel:jointIndices")).HasValue())
        throw std::runtime_error("Skeleton binding is missing or unresolved");
      return {};
    }
    const auto root = UsdSkelRoot::Find(prim);
    if (!root)
      throw std::runtime_error("Skinned mesh has no SkelRoot ancestor");
    if (populated_.insert(root.GetPath()).second && !cache_.Populate(root, UsdTraverseInstanceProxies()))
      throw std::runtime_error("Could not load skeleton bindings");
    const auto skin = cache_.GetSkinningQuery(prim);
    if (!skin || !skin.HasJointInfluences())
      throw std::runtime_error("Joint influences are missing or invalid");
    const auto time = transforms_.GetTime();
    auto pose = poses_.find(skeleton.GetPath());
    if (pose == poses_.end()) {
      auto query = cache_.GetSkelQuery(skeleton);
      VtMatrix4dArray matrices;
      if (!query || !query.ComputeSkinningTransforms(&matrices, time))
        throw std::runtime_error("Could not evaluate the skeleton pose");
      pose = poses_.emplace(skeleton.GetPath(), std::move(matrices)).first;
    }
    // A binding can reorder or use only a subset of the skeleton's joints.
    auto joint_xforms = pose->second;
    if (skin.GetJointMapper() && !skin.GetJointMapper()->RemapTransforms(pose->second, &joint_xforms))
      throw std::runtime_error("Could not map mesh joints to the skeleton");
    VtIntArray joints;
    VtFloatArray weights;
    if (!skin.ComputeVaryingJointInfluences(points.size(), &joints, &weights, time))
      throw std::runtime_error("Joint indices and weights do not match the mesh");
    for (size_t i = 0; i < joints.size(); ++i)
      if (joints[i] < 0 || size_t(joints[i]) >= joint_xforms.size() || !std::isfinite(weights[i]))
        throw std::runtime_error("Joint index or weight is invalid");
    const auto geom_bind = skin.GetGeomBindTransform(time);
    const auto method = skin.GetSkinningMethod();
    auto posed_points = points;
    if (!UsdSkelSkinPoints(method, geom_bind, joint_xforms, joints, weights, skin.GetNumInfluencesPerComponent(),
                           posed_points))
      throw std::runtime_error("Could not skin mesh vertices");

    // USD skinning produces skeleton-space positions. Convert to mesh-local
    // coordinates, retaining the same mesh transform and selection/gizmo path.
    const auto mesh_world = transforms_.GetLocalToWorldTransform(prim);
    if (!std::isfinite(mesh_world.GetDeterminant()) || std::abs(mesh_world.GetDeterminant()) < 1e-12)
      throw std::runtime_error("Skinned mesh transform is singular");
    const auto to_mesh = transforms_.GetLocalToWorldTransform(skeleton.GetPrim()) * mesh_world.GetInverse();
    for (auto &point : posed_points) {
      point = GfVec3f(to_mesh.Transform(point));
      if (!finite(point))
        throw std::runtime_error("Skeleton pose produced a non-finite vertex");
    }
    VtVec3fArray posed_normals;
    if (!normals.empty()) {
      // Expand all normal interpolations to corners, so hard edges and constant
      // normals can follow each corner's vertex influences independently.
      posed_normals.reserve(indices.size());
      size_t corner = 0;
      for (size_t face = 0; face < counts.size(); ++face) {
        for (int i = 0; i < counts[face]; ++i, ++corner) {
          const size_t n = normal_interpolation == UsdGeomTokens->constant      ? 0
                           : normal_interpolation == UsdGeomTokens->uniform     ? face
                           : normal_interpolation == UsdGeomTokens->faceVarying ? corner
                                                                                : size_t(indices[corner]);
          if (n >= normals.size())
            throw std::runtime_error("Normals do not match the mesh interpolation");
          posed_normals.push_back(normals[n]);
        }
      }
      VtMatrix3dArray normal_xforms(joint_xforms.size());
      for (size_t i = 0; i < joint_xforms.size(); ++i)
        normal_xforms[i] = joint_xforms[i].ExtractRotationMatrix().GetInverse().GetTranspose();
      if (!UsdSkelSkinFaceVaryingNormals(method, geom_bind.ExtractRotationMatrix().GetInverse().GetTranspose(),
                                         normal_xforms, joints, weights, skin.GetNumInfluencesPerComponent(), indices,
                                         posed_normals))
        throw std::runtime_error("Could not skin mesh normals");
      const auto to_mesh_normals = to_mesh.ExtractRotationMatrix().GetInverse().GetTranspose();
      for (auto &normal : posed_normals) {
        normal = GfVec3f(normal * to_mesh_normals).GetNormalized();
        if (!finite(normal))
          throw std::runtime_error("Skeleton pose produced a non-finite normal");
      }
    }
    const auto id = skeleton.GetPath().GetString();
    if (!skeletons_.contains(id)) {
      const auto query = cache_.GetSkelQuery(skeleton);
      VtMatrix4dArray joint_pose;
      if (!query.ComputeJointSkelTransforms(&joint_pose, time))
        throw std::runtime_error("Could not evaluate skeleton joint positions");
      // Queries can share a prototype definition. Use this binding's skeleton
      // prim for the world transform, including native USD instance proxies.
      const auto skeleton_world = transforms_.GetLocalToWorldTransform(skeleton.GetPrim());
      const auto names = query.GetJointOrder();
      const auto &parents = query.GetTopology().GetParentIndices();
      if (names.size() != joint_pose.size() || parents.size() != joint_pose.size())
        throw std::runtime_error("Skeleton joint hierarchy is inconsistent");
      Json positions = Json::array(), joints = Json::array(), hierarchy = Json::array();
      for (size_t i = 0; i < joint_pose.size(); ++i) {
        const auto position = (joint_pose[i] * skeleton_world).ExtractTranslation();
        if (!std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(position[2]))
          throw std::runtime_error("Skeleton pose produced a non-finite joint");
        positions.push_back({position[0], position[1], position[2]});
        joints.push_back(names[i].GetString());
        hierarchy.push_back(parents[i]);
      }
      skeletons_[id] = {{"positions", positions}, {"joints", joints}, {"parents", hierarchy}};
    }
    points = std::move(posed_points);
    normals = std::move(posed_normals);
    normal_interpolation = UsdGeomTokens->faceVarying;
    return id;
  }
} // namespace remix::usd
