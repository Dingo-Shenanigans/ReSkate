#pragma once
#include "effect_surface.h"
#include "Extension/Objects/ParkEditor/park_editor_surface.h"
#include "Engine/Game/Build/20260929/effect_surface.h"
#include <unordered_map>

namespace dingosdk::effects {
namespace receiver_build = addr::effect_surface;
// Called only on the native effect/decal game thread. Physics identities include
// a generation, so deleting a prop cannot attach its marks to a recycled slot.
class Receivers {
    editor::NativeSurfaceApi api_;
    std::uintptr_t base_{},world_{};
    std::unordered_map<std::uint64_t,std::optional<Matrix>> poses_;
    template<class F> F function(std::uintptr_t rva) const {return reinterpret_cast<F>(base_+rva);}
public:
    bool initialize(std::uintptr_t base) {
        base_=base; api_=editor::surface_api(base);
        for (const auto& c:receiver_build::contracts) {
            std::array<unsigned char,32> bytes{};
            if (!memory::read_bytes(base+c.rva,bytes.data(),bytes.size()) || bytes!=c.bytes) return false;
        }
        return api_.ready;
    }
    void begin(std::uintptr_t context) {poses_.clear(); world_=context ? api_.world(context) : 0;}
    bool pose(const ReceiverId& id,Matrix& out) {
        if (!world_ || id[0]!=world_) return false;
        auto [at,inserted]=poses_.try_emplace(id[1]);
        if (inserted) {
            namespace build=receiver_build;
            alignas(16) const auto body=id;
            if (function<bool(*)(const void*)>(build::contracts[1].rva)(body.data())) {
                alignas(16) std::array<float,4> position{},rotation{};
                function<float*(*)(const void*,float*)>(build::body_position)(body.data(),position.data());
                function<float*(*)(const void*,float*)>(build::body_rotation)(body.data(),rotation.data());
                Matrix matrix;
                if (surface::pose(rotation,position,matrix)) at->second=matrix;
            }
        }
        if (!at->second) return false;
        out=*at->second; return true;
    }
    bool bind(const Matrix& transform, float lift, Receiver& receiver) {
        if (!world_) return false;
        const auto& t=transform;
        const float depth=std::sqrt(t[4]*t[4]+t[5]*t[5]+t[6]*t[6]);
        if (!std::isfinite(depth) || depth<.001f) return false;
        editor::NativeSurfaceRay ray;
        for (unsigned i=0;i<3;++i) {
            const float normal=t[4+i]/depth,point=t[12+i]-normal*lift;
            ray.start[i]=point+normal*.03f;
            ray.end[i]=point-normal*.06f;
        }
        editor::NativeSurfaceResult result;
        api_.ray(world_,&result,&ray,"ReSkate_Receiver");
        struct Cleanup {
            const editor::NativeSurfaceApi& api; editor::NativeSurfaceResult& result;
            ~Cleanup() {
                if (result.scope[2]) {
                    const auto allocator=api.scope_allocator();
                    std::uintptr_t vtable{},pop{};
                    if (memory::peek(allocator,vtable) && memory::peek(vtable+receiver_build::scope_pop_slot,pop) && pop)
                        reinterpret_cast<void(*)(std::uintptr_t,void*)>(pop)(allocator,result.scope.data());
                }
                if (result.allocator) api.release(&result.allocator_vtable,result.data);
            }
        } cleanup{api_,result};
        if (result.world!=world_ || result.count>4096 || result.first>UINT32_MAX-result.count) return false;
        if (!result.count) return false;
        if (!result.data) return false;
        std::uintptr_t fractions{};
        if (!memory::peek(result.data+receiver_build::result_fractions,fractions) || !fractions) return false;
        float closest=2; std::uint32_t index{};
        for (std::uint32_t n=0;n<result.count;++n) {
            float fraction{};
            if (!memory::peek(fractions+std::uintptr_t(result.first+n)*sizeof(float),fraction)) return false;
            if (std::isfinite(fraction) && fraction>=0 && fraction<=1 && fraction<closest) {
                closest=fraction; index=result.first+n;
            }
        }
        if (closest>1) return false;
        alignas(16) const std::array<std::uint64_t,3> hit{world_,index,result.data};
        alignas(16) ReceiverId body{};
        function<void*(*)(const void*,void*)>(receiver_build::contracts[0].rva)(hit.data(),body.data());
        if (!pose(body,receiver.pose)) return false;
        receiver.id=body; return true;
    }
};
}
