#pragma once
#include "Engine/Game/BMX/bmx_contact_patch.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// Portable query policy only. The caller owns native world/context/identity
// validation inside raycast(from,to). No pose, velocity or normal is fabricated.
namespace dingosdk::bmx::wheel_surface {
using Vec=std::array<float,3>;
inline constexpr double minimum_body_up_alignment=.17364817766693041; // cos80 degrees
inline constexpr double maximum_edge_uncertainty=.002; // world-space boundary bracket, not a contact skin
struct RaySample {bool available{},hit{};Vec point{};float fraction{};std::uint64_t entity{};std::array<std::uint64_t,2> body{};};
struct Settings {
    float radius{.34f},offset{.24f},patch_spacing{.03f},max_residual{.0025f};
    float maximum_penetration{.10f},maximum_gap{.25f};
};
enum class Issue {ready,clear,unavailable,invalid_sample,penetration_limit};
enum class Feature {none,plane,edge};
// Observations of the existing edge gates only. Coordinates/errors are usable
// behind their corresponding flags; this record does not authorize a contact.
struct EdgeDiagnostic {
    bool candidate{},bounded{},uncertainty_valid{},projection_beyond{},radial_valid{};
    bool witness_attempted{},witness_hit{},witness_near_boundary{},witness_on_plane{},witness_normal_valid{},verified{};
    unsigned bisections{};
    float uncertainty_m{},boundary_error_m{},boundary_allowance_m{},plane_error_m{};
    float radial_up_alignment{},witness_up_alignment{};
    Vec inside_point{},outside_point{},witness_point{},radial_from{},radial_to{};
    bool original_witness_hit{},retry_attempted{},retry_interior_hit{},retry_interior_on_plane{},retry_radial_attempted{};
    float retry_inset_m{},retry_uncertainty_m{};
    Vec retry_interior_point{};
    // Last valid refinement query. When rejected is true this is the exact
    // ambiguous sample that stopped bisection; no diagnostic changes its class.
    bool refinement_sampled{},refinement_hit{},refinement_projection_valid{},refinement_rejected{};
    int refinement_classification{}; // -1 clear/material drop, 0 ambiguous, +1 on the measured face
    float refinement_signed_plane_error_m{};
    Vec refinement_from{},refinement_to{},refinement_point{},refinement_projected_point{};
    Vec refinement_plane_point{},refinement_plane_normal{};
};
struct Result {
    bool available{},hit{};
    Vec point{},normal{}; // point is the actual plane-normal or finite-edge radial witness hit
    float separation{};
    unsigned rays{},candidates{},verified{};
    Issue issue{Issue::invalid_sample};
    Feature feature{Feature::none};
    float edge_uncertainty{};
    EdgeDiagnostic edge_diagnostic;
    // Measured supporting FACE, distinct from an edge's radial force normal.
    // Valid only with available&&hit; not an additional collision witness.
    Vec balance_normal{};
    bool balance_normal_available{};
    std::uint64_t entity{};std::array<std::uint64_t,2> body{}; // actual selected witness identity, opaque native reference
};
namespace detail {
inline Vec add(Vec a,Vec b) noexcept {for(unsigned i=0;i<3;++i)a[i]+=b[i];return a;}
inline Vec sub(Vec a,Vec b) noexcept {for(unsigned i=0;i<3;++i)a[i]-=b[i];return a;}
inline Vec mul(Vec a,float b) noexcept {for(auto& x:a)x*=b;return a;}
inline Vec cross(Vec a,Vec b) noexcept {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
inline double dot(Vec a,Vec b) noexcept {double value=0;for(unsigned i=0;i<3;++i)value+=double(a[i])*b[i];return value;}
inline bool finite(Vec a) noexcept {for(float x:a)if(!std::isfinite(x)||std::abs(x)>1e6)return false;return true;}
inline bool unit(Vec value,Vec& result) noexcept {
    const auto square=dot(value,value);
    if(!finite(value)||!std::isfinite(square)||square<1e-12)return false;
    result=mul(value,static_cast<float>(1/std::sqrt(square)));return true;
}
inline bool axes(Vec up,Vec forward,Vec left) noexcept {
    for(const auto& axis:{up,forward,left})if(!finite(axis)||std::abs(dot(axis,axis)-1)>.001)return false;
    return std::abs(dot(up,forward))<=.001&&std::abs(dot(up,left))<=.001&&std::abs(dot(forward,left))<=.001;
}
inline bool settings_valid(Settings s) noexcept {
    for(float v:{s.radius,s.offset,s.patch_spacing,s.max_residual,s.maximum_penetration,s.maximum_gap})
        if(!std::isfinite(v)||v<0)return false;
    return s.radius>=.05f&&s.radius<=.5f&&s.offset>=.01f&&s.offset<=.4f&&
        s.patch_spacing>=.01f&&s.patch_spacing<=.08f&&s.max_residual>=.0001f&&s.max_residual<=.01f&&
        s.maximum_penetration<=.15f&&s.maximum_gap<=.3f;
}
}
// Exactly three candidate center rays (hub and +/-forward). Reach is at least
// the wheel radius: a toe's first contact can lead the hub by R*tan(angle/2).
// The default .34m wheel is tested through75deg and bounded at80deg relative
// to body-up; vertical-wall support is outside this policy. A hit center gets
// four local patch rays and at most one normal witness: maximum18 base rays.
// A measured finite-plane boundary can add at most9 bisections and one radial
// witness. A clear near-boundary radial query alone permits one actual interior
// down-hit and one radial retry. Only if the original searches found neither
// a verified contact nor an edge bracket after a mixed patch or unmatched face
// witness AND at least one clear base ray, two fresh patches at +/-two patch
// spacings may recover a coherent face (maximum42 total). Flat support remains
//18 rays and clear air remains3; no contact is taken from an earlier frame.
// Missing/discontinuous geometry is a clear wheel, while a
// failed or malformed native query invalidates the complete result.
template<class Raycast>
Result search(Vec hub,Vec up,Vec forward,Vec left,Raycast&& raycast,Settings settings={}) noexcept {
    Result out;
    if(!detail::finite(hub)||!detail::axes(up,forward,left)||!detail::settings_valid(settings))return out;
    try {
        const auto cast=[&](Vec from,Vec to,RaySample& sample) {
            ++out.rays;sample=raycast(from,to);
            if(!sample.available) {out.hit=false;out.issue=Issue::unavailable;return false;}
            if(!sample.hit)return true;
            if(!detail::finite(sample.point)||!std::isfinite(sample.fraction)||sample.fraction<0||sample.fraction>1) {
                out.hit=false;out.issue=Issue::invalid_sample;return false;
            }
            const auto expected=detail::add(from,detail::mul(detail::sub(to,from),sample.fraction));
            const auto delta=detail::sub(sample.point,expected);
            if(detail::dot(delta,delta)>.025*.025) {out.hit=false;out.issue=Issue::invalid_sample;return false;}
            return true;
        };
        const auto down=[&](Vec center,RaySample& sample) {
            return cast(detail::add(center,detail::mul(up,.15f)),detail::sub(center,detail::mul(up,.70f)),sample);
        };
        const auto reach=std::max(settings.offset,settings.radius);
        const auto recenter=std::min(2*settings.patch_spacing,settings.radius*.5f);
        const std::array<Vec,5> centers{hub,detail::add(hub,detail::mul(forward,reach)),
            detail::sub(hub,detail::mul(forward,reach)),
            detail::add(hub,detail::mul(forward,recenter)),detail::sub(hub,detail::mul(forward,recenter))};
        std::array<RaySample,5> center_hits;
        unsigned center_count=3;
        for(unsigned i=0;i<center_count;++i)if(!down(centers[i],center_hits[i]))return out;
        const std::array<Vec,4> patch_offsets{detail::mul(forward,settings.patch_spacing),detail::mul(forward,-settings.patch_spacing),
            detail::mul(left,settings.patch_spacing),detail::mul(left,-settings.patch_spacing)};
        struct EdgeCandidate {
            bool valid{};
            Vec point{},normal{},inside_center{},outside_center{};
            RaySample inside;
            double distance_squared{1e30};
        } edge;
        bool recenter_needed=false;
        // Intersect ONLY for bracketing/conditioning. A generated point is never
        // returned as a contact. The predicted plane must lie strictly inside
        // a down ray before its clear result can prove a missing finite face.
        const auto plane_point=[&](Vec center,Vec point,Vec normal,Vec& predicted) {
            const auto alignment=detail::dot(normal,up);
            if(alignment<minimum_body_up_alignment-2e-6)return false;
            const auto height=detail::dot(detail::sub(center,point),normal)/alignment;
            if(!std::isfinite(height)||height<-.125||height>.675)return false;
            predicted=detail::sub(center,detail::mul(up,static_cast<float>(height)));
            return detail::finite(predicted);
        };
        const auto plane_class=[&](Vec center,const RaySample& sample,Vec point,Vec normal) {
            Vec predicted{};
            if(!plane_point(center,point,normal,predicted))return 0; // outside ray, or ambiguous
            if(!sample.hit)return -1;
            const auto residual=detail::dot(detail::sub(sample.point,point),normal);
            if(std::abs(residual)<=settings.max_residual)return 1;
            // An occluder above the plane does not prove that the plane ends.
            // A continuous curved/noisy patch crossing the fit tolerance is
            // ambiguous too: require a real drop/open boundary instead.
            if(residual < -std::max(.01,4.0*settings.max_residual))return -1;
            return 0;
        };
        for(unsigned pass=0;pass<2;++pass) {
            if(pass==1) {
                // A sharp top/backface can mix the original small patch while
                // the far radius probes lie outside the down-ray range. Try
                // fresh, nearby patches only after no measured face/edge won.
                // No old plane or rejected fit is reused.
                if(out.hit||edge.valid||!recenter_needed||
                   (center_hits[0].hit&&center_hits[1].hit&&center_hits[2].hit))break;
                center_count=5;
                for(unsigned i=3;i<center_count;++i)if(!down(centers[i],center_hits[i]))return out;
            }
            for(unsigned i=pass==0?0u:3u;i<center_count;++i) {
            if(!center_hits[i].hit)continue;
            ++out.candidates;
            std::array<contact_patch::Sample,5> patch{};
            std::array<RaySample,4> neighbors{};
            patch[0]={true,center_hits[i].point};
            for(unsigned j=0;j<patch_offsets.size();++j) {
                if(!down(detail::add(centers[i],patch_offsets[j]),neighbors[j]))return out;
                patch[j+1]={neighbors[j].hit,neighbors[j].point};
            }
            // Condition the plane's coordinate chart around the actual radial
            // hub-to-hit direction. This vector supplies ONLY fit axes: the
            // resulting normal still comes from three noncollinear hit points.
            // Keep the fit's .30 conditioning and .0025m residual thresholds.
            Vec fit_up=up,fit_left{},fit_forward{};
            (void)detail::unit(detail::sub(hub,center_hits[i].point),fit_up);
            if(!detail::unit(detail::sub(left,detail::mul(fit_up,static_cast<float>(detail::dot(left,fit_up)))),fit_left)||
               !detail::unit(detail::cross(fit_left,fit_up),fit_forward))continue;
            contact_patch::Tuning tuning;tuning.maximum_residual=settings.max_residual;
            auto fitted=contact_patch::fit(patch,fit_up,fit_forward,fit_left,tuning);
            if(!fitted.available) {
                if(fitted.issue==contact_patch::Issue::discontinuous)recenter_needed=true;
                continue;
            }
            if(detail::dot(fitted.normal,up)<0)fitted.normal=detail::mul(fitted.normal,-1);
            // A tiny float roundoff allowance keeps the declared80deg boundary
            // inclusive; it is not a physical extension into vertical walls.
            if(detail::dot(fitted.normal,up)<minimum_body_up_alignment-2e-6)continue;
            const auto predicted=detail::dot(detail::sub(hub,fitted.point),fitted.normal);
            RaySample witness;
            if(!cast(detail::add(hub,detail::mul(fitted.normal,.15f)),
                     detail::sub(hub,detail::mul(fitted.normal,.70f)),witness))return out;
            const auto distance=witness.hit?detail::dot(detail::sub(hub,witness.point),fitted.normal):0;
            if(!witness.hit||std::abs(distance-predicted)>settings.max_residual) {
                recenter_needed=true;
                // A disk can still touch a real finite edge after its normal
                // projection leaves the face. Locate an actual on-face/drop
                // bracket along this disk's longitudinal cross section; do not
                // extend the fitted infinite plane into the missing geometry.
                struct Along {Vec center;RaySample sample;double position;};
                std::array<Along,7> line{};
                const auto line_count=center_count+2;
                for(unsigned k=0;k<center_count;++k)
                    line[k]={centers[k],center_hits[k],detail::dot(detail::sub(centers[k],hub),forward)};
                for(unsigned k=0;k<2;++k) {
                    const auto center=detail::add(centers[i],patch_offsets[k]);
                    line[k+center_count]={center,neighbors[k],detail::dot(detail::sub(center,hub),forward)};
                }
                std::sort(line.begin(),line.begin()+line_count,[](const Along& a,const Along& b){return a.position<b.position;});
                const auto projection=detail::sub(hub,detail::mul(fitted.normal,static_cast<float>(predicted)));
                const auto projected_position=detail::dot(detail::sub(projection,hub),forward);
                for(unsigned k=1;k<line_count;++k) {
                    if(line[k].position-line[k-1].position<1e-5)continue;
                    const int a=plane_class(line[k-1].center,line[k-1].sample,fitted.point,fitted.normal);
                    const int b=plane_class(line[k].center,line[k].sample,fitted.point,fitted.normal);
                    if(a*b!=-1)continue;
                    const auto& inside=a==1?line[k-1]:line[k];
                    const auto& outside=a==1?line[k]:line[k-1];
                    const auto direction=outside.position>inside.position?1.0:-1.0;
                    // The initial bracket may still contain the projection.
                    // Require it beyond the refined outside endpoint below.
                    if(direction*(projected_position-inside.position)<=0)continue;
                    Vec boundary_limit{};
                    if(!plane_point(outside.center,fitted.point,fitted.normal,boundary_limit))continue;
                    const auto delta=detail::sub(hub,boundary_limit);
                    const auto square=detail::dot(delta,delta);
                    if(!edge.valid||square<edge.distance_squared)
                        edge={true,fitted.point,fitted.normal,inside.center,outside.center,inside.sample,square};
                }
                continue;
            }
            ++out.verified;
            const auto separation=static_cast<float>(distance-settings.radius);
            if(separation < -settings.maximum_penetration) {
                out.available=false;out.hit=false;out.issue=Issue::penetration_limit;return out;
            }
            if(separation>settings.maximum_gap)continue;
            if(!out.hit||separation<out.separation) {
                out.hit=true;out.separation=separation;out.point=witness.point;out.normal=fitted.normal;
                out.feature=Feature::plane;out.edge_uncertainty=0;
                out.entity=witness.entity;out.body=witness.body;
                out.balance_normal=fitted.normal;out.balance_normal_available=true;
            }
            }
        }
        if(edge.valid) {
            auto& diagnostic=out.edge_diagnostic;
            diagnostic.candidate=true;diagnostic.inside_point=edge.inside.point;
            bool bounded=true;
            for(unsigned k=0;k<9;++k) {
                diagnostic.bisections=k+1;
                const auto center=detail::mul(detail::add(edge.inside_center,edge.outside_center),.5f);
                // Refine the measured top face along its own normal. A tilted
                // body-up ray can cross the vertical lip backface just below
                // the top and make a real sharp edge look like noisy curvature.
                // The generated center is not a contact: measured classes and
                // the final fresh radial witness remain authoritative.
                Vec predicted_center{};
                if(!plane_point(center,edge.point,edge.normal,predicted_center)) {bounded=false;break;}
                const auto refinement_from=detail::add(predicted_center,detail::mul(edge.normal,.15f));
                const auto refinement_to=detail::sub(predicted_center,detail::mul(edge.normal,.70f));
                RaySample sample;
                if(!cast(refinement_from,refinement_to,sample))return out;
                const int classification=plane_class(center,sample,edge.point,edge.normal);
                diagnostic.refinement_sampled=true;diagnostic.refinement_hit=sample.hit;
                diagnostic.refinement_from=refinement_from;
                diagnostic.refinement_to=refinement_to;
                diagnostic.refinement_point=sample.hit?sample.point:Vec{};
                diagnostic.refinement_plane_point=edge.point;diagnostic.refinement_plane_normal=edge.normal;
                diagnostic.refinement_projected_point={};
                diagnostic.refinement_projection_valid=plane_point(center,edge.point,edge.normal,diagnostic.refinement_projected_point);
                diagnostic.refinement_signed_plane_error_m=sample.hit?
                    static_cast<float>(detail::dot(detail::sub(sample.point,edge.point),edge.normal)):0;
                diagnostic.refinement_classification=classification;diagnostic.refinement_rejected=classification==0;
                if(classification==1) {edge.inside_center=center;edge.inside=sample;}
                else if(classification==-1)edge.outside_center=center;
                else {bounded=false;break;}
            }
            diagnostic.inside_point=edge.inside.point;
            Vec limit{};
            if(bounded&&plane_point(edge.outside_center,edge.point,edge.normal,limit)) {
                diagnostic.bounded=true;diagnostic.outside_point=limit;
                const auto width=detail::sub(limit,edge.inside.point);
                const auto uncertainty=std::sqrt(detail::dot(width,width));
                const auto direction=detail::dot(detail::sub(edge.outside_center,edge.inside_center),forward)>0?1.0:-1.0;
                const auto projected=detail::sub(hub,detail::mul(edge.normal,
                    static_cast<float>(detail::dot(detail::sub(hub,edge.point),edge.normal))));
                diagnostic.uncertainty_m=static_cast<float>(uncertainty);
                diagnostic.uncertainty_valid=uncertainty<=maximum_edge_uncertainty;
                diagnostic.projection_beyond=direction*detail::dot(detail::sub(projected,limit),forward)>0;
                Vec diagnostic_radial{};
                if(detail::unit(detail::sub(hub,edge.inside.point),diagnostic_radial)) {
                    diagnostic.radial_up_alignment=static_cast<float>(detail::dot(diagnostic_radial,up));
                    diagnostic.radial_valid=detail::dot(diagnostic_radial,up)>=minimum_body_up_alignment-2e-6&&
                        detail::dot(diagnostic_radial,edge.normal)>0;
                }
                Vec radial{};
                if(uncertainty<=maximum_edge_uncertainty&&
                   direction*detail::dot(detail::sub(projected,limit),forward)>0&&
                   detail::unit(detail::sub(hub,edge.inside.point),radial)&&
                   detail::dot(radial,up)>=minimum_body_up_alignment-2e-6&&detail::dot(radial,edge.normal)>0) {
                    diagnostic.witness_attempted=true;
                    diagnostic.radial_from=detail::add(hub,detail::mul(radial,.15f));
                    diagnostic.radial_to=detail::sub(hub,detail::mul(radial,.70f));
                    RaySample witness;
                    if(!cast(detail::add(hub,detail::mul(radial,.15f)),detail::sub(hub,detail::mul(radial,.70f)),witness))return out;
                    diagnostic.original_witness_hit=witness.hit;
                    Vec verification_point=edge.inside.point;
                    double verification_uncertainty=uncertainty;
                    if(!witness.hit) {
                        // Retail oblique rays can miss a point returned by a
                        // down ray within micrometers of a triangle boundary.
                        // Retry ONLY clear queries, toward a fresh measured
                        // interior point. Keep the complete feature uncertainty
                        // within the existing2mm bound; do not widen a contact
                        // skin, accept the old hit, or infer a missing surface.
                        const auto tangent=detail::sub(forward,detail::mul(up,
                            static_cast<float>(detail::dot(forward,edge.normal)/detail::dot(up,edge.normal))));
                        Vec tangent_unit{};
                        const auto tangent_length=std::sqrt(detail::dot(tangent,tangent));
                        const auto inset=std::min(.001,maximum_edge_uncertainty-uncertainty-.0001);
                        if(inset>=.0005&&detail::unit(tangent,tangent_unit)) {
                            const auto center=detail::sub(edge.inside_center,detail::mul(forward,
                                static_cast<float>(direction*inset/tangent_length)));
                            RaySample interior;
                            diagnostic.retry_attempted=true;
                            if(!down(center,interior))return out;
                            diagnostic.retry_interior_hit=interior.hit;
                            diagnostic.retry_interior_on_plane=plane_class(center,interior,edge.point,edge.normal)==1;
                            if(diagnostic.retry_interior_on_plane) {
                                diagnostic.retry_interior_point=interior.point;
                                const auto inward=direction*detail::dot(detail::sub(edge.inside.point,interior.point),tangent_unit);
                                const auto bound=detail::sub(limit,interior.point);
                                const auto retry_uncertainty=std::sqrt(detail::dot(bound,bound));
                                diagnostic.retry_inset_m=static_cast<float>(inward);
                                diagnostic.retry_uncertainty_m=static_cast<float>(retry_uncertainty);
                                Vec retry_radial{};
                                if(inward>=.0005&&retry_uncertainty<=maximum_edge_uncertainty&&
                                   detail::unit(detail::sub(hub,interior.point),retry_radial)&&
                                   detail::dot(retry_radial,up)>=minimum_body_up_alignment-2e-6&&
                                   detail::dot(retry_radial,edge.normal)>0) {
                                    verification_point=interior.point;verification_uncertainty=retry_uncertainty;
                                    diagnostic.inside_point=interior.point;
                                    diagnostic.uncertainty_m=static_cast<float>(retry_uncertainty);
                                    diagnostic.radial_up_alignment=static_cast<float>(detail::dot(retry_radial,up));
                                    diagnostic.radial_from=detail::add(hub,detail::mul(retry_radial,.15f));
                                    diagnostic.radial_to=detail::sub(hub,detail::mul(retry_radial,.70f));
                                    diagnostic.retry_radial_attempted=true;
                                    if(!cast(diagnostic.radial_from,diagnostic.radial_to,witness))return out;
                                }
                            }
                        }
                    }
                    diagnostic.witness_hit=witness.hit;
                    if(witness.hit) {
                        diagnostic.witness_point=witness.point;
                        const auto boundary_delta=detail::sub(witness.point,verification_point);
                        const auto allowance=verification_uncertainty+settings.max_residual;
                        const auto residual=std::abs(detail::dot(detail::sub(witness.point,edge.point),edge.normal));
                        diagnostic.boundary_error_m=static_cast<float>(std::sqrt(detail::dot(boundary_delta,boundary_delta)));
                        diagnostic.boundary_allowance_m=static_cast<float>(allowance);
                        diagnostic.plane_error_m=static_cast<float>(residual);
                        diagnostic.witness_near_boundary=detail::dot(boundary_delta,boundary_delta)<=allowance*allowance;
                        diagnostic.witness_on_plane=residual<=settings.max_residual;
                        Vec diagnostic_normal{};
                        if(detail::unit(detail::sub(hub,witness.point),diagnostic_normal)) {
                            diagnostic.witness_up_alignment=static_cast<float>(detail::dot(diagnostic_normal,up));
                            diagnostic.witness_normal_valid=detail::dot(diagnostic_normal,up)>=minimum_body_up_alignment-2e-6;
                        }
                        Vec normal{};
                        if(detail::dot(boundary_delta,boundary_delta)<=allowance*allowance&&residual<=settings.max_residual&&
                           detail::unit(detail::sub(hub,witness.point),normal)&&
                           detail::dot(normal,up)>=minimum_body_up_alignment-2e-6) {
                            diagnostic.verified=true;
                            ++out.verified;
                            // Circular-wheel collision normal to an OBSERVED
                            // boundary point, not a fabricated face normal.
                            const auto delta=detail::sub(hub,witness.point);
                            const auto separation=static_cast<float>(std::sqrt(detail::dot(delta,delta))-settings.radius);
                            if(separation < -settings.maximum_penetration) {
                                out.available=false;out.hit=false;out.issue=Issue::penetration_limit;return out;
                            }
                            if(separation<=settings.maximum_gap&&(!out.hit||separation<out.separation)) {
                                out.hit=true;out.separation=separation;out.point=witness.point;out.normal=normal;
                                out.feature=Feature::edge;out.edge_uncertainty=static_cast<float>(verification_uncertainty);
                out.entity=witness.entity;out.body=witness.body;
                                out.balance_normal=edge.normal;out.balance_normal_available=true;
                            }
                        }
                    }
                }
            }
        }
        out.available=true;out.issue=out.hit?Issue::ready:Issue::clear;return out;
    } catch(...) {out.available=false;out.hit=false;out.issue=Issue::unavailable;return out;}
}
}
