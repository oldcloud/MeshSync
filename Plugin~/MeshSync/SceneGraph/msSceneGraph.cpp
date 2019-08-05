#include "pch.h"
#include "msSceneGraph.h"
#include "msMesh.h"
#include "msConstraints.h"
#include "msAnimation.h"
#include "msMaterial.h"
#include "msAudio.h"
#include "msEntityConverter.h"


namespace ms {


// Scene
#pragma region Scene

#define EachMember(F)\
    F(name) F(handedness) F(scale_factor)

void SceneSettings::serialize(std::ostream& os) const
{
    EachMember(msWrite);
}
void SceneSettings::deserialize(std::istream& is)
{
    EachMember(msRead);
}
#undef EachMember


#define EachMember(F)\
    F(settings) F(assets) F(entities) F(constraints)

Scene::Scene()
{
}

Scene::~Scene()
{
}

ScenePtr Scene::create(std::istream& is)
{
    auto ret = create();
    ret->deserialize(is);
    return ret;
}

ScenePtr Scene::clone(bool detach)
{
    auto ret = create();
    *ret = *this;
    parallel_for(0, (int)entities.size(), 10, [this, detach, &ret](int ei) {
        ret->entities[ei] = std::static_pointer_cast<Transform>(entities[ei]->clone(detach));
    });
    return ret;
}

void Scene::serialize(std::ostream& os) const
{
    uint64_t validation_hash = hash();
    write(os, validation_hash);
    EachMember(msWrite);
}
void Scene::deserialize(std::istream& is)
{
    uint64_t validation_hash;
    read(is, validation_hash);
    EachMember(msRead);
    if (validation_hash != hash()) {
        throw std::runtime_error("scene hash doesn't match");
    }
}

void Scene::strip(Scene& base)
{
    size_t entity_count = entities.size();
    if (entity_count == base.entities.size()) {
        parallel_for(0, (int)entity_count, 10, [this, &base](int ei) {
            auto& ecur = entities[ei];
            auto& ebase = base.entities[ei];
            if (ecur->id == ebase->id)
                ecur->strip(*ebase);
        });
    }
}

void Scene::merge(Scene& base)
{
    size_t entity_count = entities.size();
    if (entity_count == base.entities.size()) {
        parallel_for(0, (int)entity_count, 10, [this, &base](int ei) {
            auto& ecur = entities[ei];
            auto& ebase = base.entities[ei];
            if (ecur->id == ebase->id)
                ecur->merge(*ebase);
        });
    }
}

void Scene::diff(const Scene& s1, const Scene& s2)
{
    size_t entity_count = s1.entities.size();
    if (entity_count == s2.entities.size()) {
        settings = s1.settings;
        entities.resize(entity_count);
        parallel_for(0, (int)entity_count, 10, [this, &s1, &s2](int i) {
            auto& e1 = s1.entities[i];
            auto& e2 = s2.entities[i];
            if (e1->id == e2->id) {
                auto e3 = e1->clone();
                e3->diff(*e1, *e2);
                entities[i] = std::static_pointer_cast<Transform>(e3);
            }
            else {
                msLogError("Scene::diff(): should not be here!\n");
            }
        });
    }
}

void Scene::lerp(const Scene& s1, const Scene& s2, float t)
{
    size_t entity_count = s1.entities.size();
    if (entity_count == s2.entities.size()) {
        settings = s1.settings;
        entities.resize(entity_count);
        parallel_for(0, (int)entity_count, 10, [this, &s1, &s2, t](int i) {
            auto& e1 = s1.entities[i];
            auto& e2 = s2.entities[i];
            if (e1->id == e2->id) {
                if (e1->isGeometry() && !e1->cache_flags.constant_topology) {
                    entities[i] = e1;
                }
                else {
                    auto e3 = e1->clone();
                    e3->lerp(*e1, *e2, t);
                    entities[i] = std::static_pointer_cast<Transform>(e3);
                }
            }
        });
    }
}

void Scene::clear()
{
    settings = {};
    assets.clear();
    entities.clear();
    constraints.clear();
    scene_buffers.clear();
}

uint64_t Scene::hash() const
{
    uint64_t ret = 0;
    for (auto& a : assets)
        ret += a->hash();
    for (auto& e : entities)
        ret += e->hash();
    return ret;
}

void Scene::sanitizeHierarchyPath(std::string& /*path*/)
{
    // nothing to do for now
}

void Scene::import(const SceneImportSettings& cv)
{
    // receive and convert assets
    bool flip_x = settings.handedness == Handedness::Right || settings.handedness == Handedness::RightZUp;
    bool swap_yz = settings.handedness == Handedness::LeftZUp || settings.handedness == Handedness::RightZUp;

    std::vector<EntityConverterPtr> converters;
    if (settings.scale_factor != 1.0f) {
        float scale = 1.0f / settings.scale_factor;
        converters.push_back(ScaleConverter::create(scale));
    }
    if (flip_x) {
        converters.push_back(FlipX_HandednessCorrector::create());
    }
    if (swap_yz) {
        if (cv.zup_correction_mode == ZUpCorrectionMode::FlipYZ)
            converters.push_back(FlipYZ_ZUpCorrector::create());
        else if (cv.zup_correction_mode == ZUpCorrectionMode::RotateX)
            converters.push_back(RotateX_ZUpCorrector::create());
    }

    auto convert = [&converters](auto& obj) {
        for (auto& cv : converters)
            cv->convert(obj);
    };

    parallel_for_each(entities.begin(), entities.end(), [&](TransformPtr& obj) {
        sanitizeHierarchyPath(obj->path);
        sanitizeHierarchyPath(obj->reference);

        bool is_mesh = obj->getType() == EntityType::Mesh;
        if (is_mesh) {
            auto& mesh = static_cast<Mesh&>(*obj);
            for (auto& bone : mesh.bones)
                sanitizeHierarchyPath(bone->path);
            mesh.refine_settings.flags.split = 1;
            mesh.refine_settings.split_unit = cv.mesh_split_unit;
            mesh.refine_settings.max_bone_influence = cv.mesh_max_bone_influence;
            mesh.refine();
        }

        if (!converters.empty())
            convert(*obj);
        if (is_mesh)
            static_cast<Mesh&>(*obj).updateBounds();
    });

    for (auto& asset : assets) {
        if (asset->getAssetType() == AssetType::Animation) {
            auto& clip = static_cast<AnimationClip&>(*asset);
            parallel_for_each(clip.animations.begin(), clip.animations.end(), [&](AnimationPtr& anim) {
                sanitizeHierarchyPath(anim->path);
                convert(*anim);
            });
        }
    }

    settings.handedness = Handedness::Left;
    settings.scale_factor = 1.0f;
}

TransformPtr Scene::findEntity(const std::string& path) const
{
    TransformPtr ret;
    for (auto& e : entities) {
        if (e->path == path) {
            ret = e;
            break;
        }
    }
    return ret;
}

static float4x4 CalcGlobalMatrix(Transform& t)
{
    if (!t.parent)
        return t.local_matrix;
    else
        return t.local_matrix * CalcGlobalMatrix(*t.parent);
}

void Scene::buildHierarchy()
{
    auto sorted = entities;
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a->path < b->path; });

    auto find = [&sorted](const std::string& path) {
        auto it = std::lower_bound(sorted.begin(), sorted.end(), path, [](auto& a, auto& path) { return a->path < path; });
        return it != sorted.end() && (*it)->path == path ? *it : nullptr;
    };

    int n = (int)entities.size();
    parallel_for_blocked(0, n, 32, [&](int begin, int end) {
        std::string path;
        for (int i = begin; i < end; ++i) {
            auto& e = entities[i];
            e->getParentPath(path);
            e->parent = find(path).get();
            e->local_matrix = e->toMatrix();
        }
    });
    parallel_for_blocked(0, n, 32, [&](int begin, int end) {
        std::string path;
        for (int i = begin; i < end; ++i) {
            auto& e = entities[i];
            e->global_matrix = CalcGlobalMatrix(*e);
        }
    });
}

void Scene::flatternHierarchy()
{
    std::map<std::string, TransformPtr> result;
    std::string name, tmp_name;
    for (auto& e : entities) {
        if (e->getType() != EntityType::Transform) {
            e->getName(name);
            {
                auto& dst = result[name];
                if (!dst) {
                    dst = e;
                    continue;
                }
            }
            for (int i = 0; ; ++i) {
                char buf[32];
                sprintf(buf, "%x", i);
                tmp_name = name;
                tmp_name += buf;

                auto& dst = result[tmp_name];
                if (!dst) {
                    dst = e;
                    break;
                }
            }
        }
    }

    entities.clear();
    for (auto& kvp : result) {
        auto& e = kvp.second;
        e->path = "/";
        e->path += kvp.first;
        entities.push_back(e);
    }
}

template<class AssetType>
std::vector<std::shared_ptr<AssetType>> Scene::getAssets() const
{
    std::vector<std::shared_ptr<AssetType>> ret;
    for (auto& asset : assets) {
        if (auto p = std::dynamic_pointer_cast<AssetType>(asset))
            ret.push_back(p);
    }
    return ret;
}
template std::vector<std::shared_ptr<Texture>> Scene::getAssets<Texture>() const;
template std::vector<std::shared_ptr<Material>> Scene::getAssets<Material>() const;
template std::vector<std::shared_ptr<AnimationClip>> Scene::getAssets<AnimationClip>() const;
template std::vector<std::shared_ptr<Audio>> Scene::getAssets<Audio>() const;
template std::vector<std::shared_ptr<FileAsset>> Scene::getAssets<FileAsset>() const;


template<class EntityType>
std::vector<std::shared_ptr<EntityType>> Scene::getEntities() const
{
    std::vector<std::shared_ptr<EntityType>> ret;
    for (auto& asset : entities) {
        if (auto p = std::dynamic_pointer_cast<EntityType>(asset))
            ret.push_back(p);
    }
    return ret;
}
template std::vector<std::shared_ptr<Camera>> Scene::getAssets<Camera>() const;
template std::vector<std::shared_ptr<Light>> Scene::getAssets<Light>() const;
template std::vector<std::shared_ptr<Mesh>> Scene::getAssets<Mesh>() const;
template std::vector<std::shared_ptr<Points>> Scene::getAssets<Points>() const;

#undef EachMember
#pragma endregion

} // namespace ms
