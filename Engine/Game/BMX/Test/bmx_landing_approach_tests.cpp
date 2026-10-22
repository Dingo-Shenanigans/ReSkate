#include "Engine/Game/BMX/bmx_landing_approach.h"
#include "Engine/Game/BMX/bmx_tricks.h"
#include "Engine/Game/BMX/bmx_chassis_control.h"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
namespace approach=dingosdk::bmx::landing_approach;
using namespace dingosdk::bmx;
constexpr double h=.01666666567325592;
void check(bool value,const char* reason) {
    if(!value){std::cerr<<reason<<'\n';std::exit(1);}
}
bool near(double a,double b,double tolerance=1e-8){return std::abs(a-b)<tolerance;}
approach::Motion motion(std::uint64_t serial=100) {
    return {7,serial,10+static_cast<double>(serial-100)*h,h,{0,1,0},{0,-1,2},{},0,true};
}
approach::Motion next(approach::Motion value) {
    ++value.serial;value.simulation_time+=h;return value;
}
std::array<approach::Support,2> floor_support() {
    return {approach::Support{true,{0,0,0},{0,1,0},{}},approach::Support{}};
}
struct CaughtBar {
    TrickController tricks;
    TrickInput input;
    explicit CaughtBar(unsigned wait_steps=30,double heading=0,bool native_orientation=false,std::array<double,4> quaternion={0,0,0,1}) {
        input.body_yaw=input.travel_yaw=heading;input.orientation_available=native_orientation;input.body_quaternion=quaternion;
        input.enabled=input.grounded=input.bar_neutral=input.rear_neutral=true;
        input.landing_quality_available=true;input.owner=7;input.device=2;tick();
        input.grounded=false;check(tick().took_off,"real trick controller enters an air attempt before preparing a caught bar");
        input.bar_neutral=false;input.barspin=TrickDirection::left;tick();
        input.barspin=TrickDirection::none;input.bar_neutral=true;
        for(unsigned i=0;i<wait_steps;++i)tick();
        if(wait_steps==30)check(tricks.state().bar_caught&&tricks.state().pending_barspins==1,
            "landing regression uses an actually completed and neutrally caught barspin");
    }
    TrickEvents tick(){return tricks.update(h,input);}
    TrickEvents land(const approach::Evidence& evidence) {
        input.grounded=true;input.closing_normal_speed=evidence.scoring_closing_speed;
        if(evidence.approach_available&&evidence.approach_travel_available)input.travel_yaw=evidence.approach_travel_yaw;
        else if(evidence.post_travel_available)input.travel_yaw=evidence.post_travel_yaw;
        return tick();
    }
};
void stopped_hard_impact_cannot_become_a_clean_landing() {
    approach::Tracker tracker;
    auto before=motion();before.linear_velocity={0,-8,2};
    const auto clear=tracker.observe(before,{}, {0,-9.8,0});
    check(clear.valid&&!clear.post_available&&!clear.approach_available,"clear observation is history, not invented contact");
    auto stopped=next(before);stopped.linear_velocity={0,0,2};
    const auto evidence=tracker.observe(stopped,floor_support(),{0,-9.8,0});
    check(evidence.valid&&evidence.first_contact&&evidence.approach_available&&evidence.post_available,
        "adjacent same-owner first contact retains both observed sides of the impact");
    check(near(evidence.post_closing_speed,0)&&near(evidence.observed_previous_closing_speed,8)&&
        near(evidence.gravity_estimated_closing_speed,8+9.8*h)&&near(evidence.approach_age_seconds,h),
        "native-stopped impact preserves observed8m/s and separately labels the gravity-only point estimate");
    check(evidence.approach.linear_velocity==before.linear_velocity&&evidence.approach.com==before.com&&
        evidence.approach.angular_velocity==before.angular_velocity&&evidence.approach.body_yaw==before.body_yaw,
        "history exposes its own observed COM, linear/angular velocity and heading without pose reconstruction");
    CaughtBar actual;
    const auto rejected=actual.land(evidence);
    check(rejected.failed&&rejected.failure==TrickFailure::unstable_landing&&!actual.tricks.state().landing_pending,
        "current velocity zero cannot disguise the prior hard landing as clean or start settling");
    for(unsigned i=0;i<10;++i)actual.tick();
    check(actual.tricks.state().tally.landed_combos==0,"later low impact speed cannot revive a rejected first contact");
    CaughtBar post_only;auto legacy=evidence;legacy.scoring_closing_speed=legacy.post_closing_speed;
    post_only.land(legacy);for(unsigned i=0;i<4;++i)post_only.tick();
    check(post_only.tricks.state().tally.landed_combos==1,
        "control demonstrates why current native post-collision velocity alone misses this regression");
}
void sideways_native_redirection_cannot_rewrite_approach_alignment() {
    approach::Tracker tracker;auto before=motion();before.linear_velocity={4,-1,0};
    tracker.observe(before,{}, {0,-9.8,0});
    auto redirected=next(before);redirected.linear_velocity={0,0,4};
    const auto evidence=tracker.observe(redirected,floor_support(),{0,-9.8,0});
    check(evidence.approach_travel_available&&evidence.post_travel_available&&
        near(evidence.approach_travel_yaw,std::numbers::pi/2)&&near(evidence.post_travel_yaw,0),
        "sideways approach and newly redirected post-contact velocity are kept distinct");
    CaughtBar actual;const auto result=actual.land(evidence);
    check(result.failed&&result.failure==TrickFailure::unaligned_landing,
        "body heading0 landing with approach heading90deg fails even when native post-state points forward");
    CaughtBar post_only;auto old=evidence;old.approach_available=false;
    post_only.land(old);for(unsigned i=0;i<4;++i)post_only.tick();
    check(post_only.tricks.state().tally.landed_combos==1,
        "post-only control would incorrectly credit the redirected sideways impact");
}
void clean_contact_keeps_settling_and_frozen_approach_evidence() {
    approach::Tracker tracker;auto before=motion();before.linear_velocity={0,-2,3};
    tracker.observe(before,{}, {0,-9.8,0});auto supported=next(before);supported.linear_velocity={0,0,3};
    auto evidence=tracker.observe(supported,floor_support(),{0,-9.8,0});
    CaughtBar actual;const auto touch=actual.land(evidence);
    check(!touch.combo_landed&&actual.tricks.state().landing_pending&&actual.tricks.state().landing_support_seconds==0,
        "clean first contact starts at zero support time and does not spend the prior airborne interval");
    for(unsigned i=0;i<4;++i) {
        supported=next(supported);evidence=tracker.observe(supported,floor_support(),{0,-9.8,0});
        check(!evidence.first_contact&&evidence.approach_available&&evidence.approach.serial==before.serial&&
            near(evidence.approach_age_seconds,h)&&near(evidence.scoring_closing_speed,2+9.8*h),
            "fresh consecutive support preserves the first-touch approach and its original observation age");
        const auto landed=actual.land(evidence);
        check((i==3)==landed.combo_landed,"only following sixty milliseconds of support can commit the caught trick");
    }
    actual.tick();check(actual.tricks.state().tally.landed_combos==1,"continued contact cannot double-credit");
}
void point_lever_and_gravity_are_explicit_estimates() {
    approach::Tracker tracker;auto before=motion();before.angular_velocity={2,0,0};
    tracker.observe(before,{}, {0,-9.8,0});auto contact=next(before);contact.linear_velocity=contact.angular_velocity={};
    auto supports=floor_support();supports[0].point={0,0,1};
    const auto evidence=tracker.observe(contact,supports,{0,-9.8,0});
    check(near(evidence.observed_previous_closing_speed,3)&&near(evidence.gravity_estimated_closing_speed,3+9.8*h),
        "previous angular velocity is evaluated at the newly observed contact point, with gravity separate");
    check(evidence.gravity_estimate_available&&evidence.estimate_gravity==approach::Vec{0,-9.8,0},
        "reported acceleration makes the unobserved gravity-only extrapolation explicit");
    approach::Tracker threshold;before=motion();before.linear_velocity={0,-6.9,2};
    threshold.observe(before,{}, {0,-9.8,0});contact=next(before);contact.linear_velocity={0,0,2};
    const auto conservative=threshold.observe(contact,floor_support(),{0,-9.8,0});
    check(conservative.observed_previous_closing_speed<7&&conservative.scoring_closing_speed>7,
        "gravity estimate is conservative across the short unobserved contact interval rather than a claim of measured speed");
    CaughtBar actual;check(actual.land(conservative).failed,"conservative threshold cannot award a marginal hard impact");
}
void stale_pause_identity_and_bad_samples_clear_history() {
    for(unsigned change=0;change<5;++change) {
        approach::Tracker tracker;auto before=motion();before.linear_velocity={0,-8,2};
        tracker.observe(before,{}, {0,-9.8,0});auto current=next(before);current.linear_velocity={0,0,2};
        if(change==0){++current.serial;current.simulation_time+=h;}
        if(change==1)current.simulation_time+=h;
        if(change==2)++current.token;
        if(change==3)tracker.reset(); // pause/focus/menu path
        if(change==4)current.simulation_time=before.simulation_time-h;
        const auto result=tracker.observe(current,floor_support(),{0,-9.8,0});
        check(result.valid&&!result.approach_available&&!result.gravity_estimate_available&&near(result.scoring_closing_speed,0),
            "nonadjacent/stale/changed-token/reset/reversed clock cannot reuse an old unsupported impact estimate");
    }
    approach::Tracker tracker;auto previous=motion();tracker.observe(previous,{}, {0,-9.8,0});
    auto invalid=next(previous);invalid.linear_velocity[0]=std::numeric_limits<double>::quiet_NaN();
    check(!tracker.observe(invalid,floor_support(),{0,-9.8,0}).valid,"invalid measured velocity cancels scoring evidence");
    auto valid=next(previous);valid.linear_velocity={};
    check(!tracker.observe(valid,floor_support(),{0,-9.8,0}).approach_available,"invalid sample clears retained history");
    auto supports=floor_support();supports[0].normal={0,2,0};valid=next(valid);
    check(!tracker.observe(valid,supports,{0,-9.8,0}).valid,"malformed actual normal cannot become landing evidence");
}
void first_actual_touch_remains_authoritative_during_a_new_hop() {
    namespace control=chassis_control;
    auto body=control::proposed_body();body.center_of_mass={0,.9,-.05};
    std::array<control::ContactSample,2> contacts;
    for(unsigned i=0;i<2;++i) {
        contacts[i].contact.status=tire::ContactStatus::hit;
        contacts[i].contact.point={0,0,i==0?.525:-.525};
    }
    bool actual_supported=false;
    for(const auto& contact:contacts)actual_supported=actual_supported||control::eligible_support(body,contact);
    control::Controller controller;control::Controls hop;hop.hop_strength=1;hop.hop_event_id=1;
    const auto predicted=controller.step(h,body,control::proposed_wheels(),contacts,hop);
    check(actual_supported&&predicted.valid&&predicted.hop_applied&&predicted.grounded,
        "actual native force controller preserves measured support even while applying a new hop impulse");
    CaughtBar incomplete(20);
    check(incomplete.tricks.state().bar_rotating,"first-touch regression begins with a real incomplete air rotation");
    incomplete.input.grounded=actual_supported;
    const auto rejected=incomplete.tick();
    check(rejected.failed&&rejected.failure==TrickFailure::under_rotated&&!incomplete.tricks.state().landing_pending,
        "measured first touch fails an incomplete trick before a simultaneous hop can add air time");
    incomplete.input.grounded=false;
    const auto new_air=incomplete.tick();
    check(new_air.took_off&&!incomplete.tricks.state().bar_rotating&&incomplete.tricks.state().pending_barspins==0,
        "the following hop starts a new air attempt without carrying the prior under-rotation");
    incomplete.input.bar_neutral=false;incomplete.input.barspin=TrickDirection::right;
    incomplete.tick();incomplete.input.barspin=TrickDirection::none;incomplete.input.bar_neutral=true;
    for(unsigned i=0;i<30;++i)incomplete.tick();
    check(incomplete.tricks.state().bar_caught&&incomplete.tricks.state().pending_barspins==1,
        "a distinct later air gesture can complete in the new hop without reviving the failed combo");

    CaughtBar late_catch(20);late_catch.input.bar_neutral=false;
    for(unsigned i=0;i<10;++i)late_catch.tick();
    check(late_catch.tricks.state().bar_waiting_catch,"late-catch regression reaches full rotation while still held");
    late_catch.input.bar_neutral=true;late_catch.input.grounded=actual_supported;
    check(late_catch.tick().failure==TrickFailure::uncaught,
        "first-touch neutral cannot catch after contact even when a new physical hop is applied");

    CaughtBar ignored_touch(20);ignored_touch.input.grounded=false;
    ignored_touch.tick();for(unsigned i=0;i<10;++i)ignored_touch.tick();
    check(ignored_touch.tricks.state().bar_caught,
        "negative control shows that ignoring actual first touch would incorrectly extend an incomplete air rotation");
    approach::Evidence clean;clean.post_available=true;clean.valid=true;
    ignored_touch.land(clean);for(unsigned i=0;i<4;++i)ignored_touch.tick();
    check(ignored_touch.tricks.state().tally.landed_combos==1,
        "negative control would bank extra post-touch hop time; actual-support contract forbids that path");
}

struct SettleRow162 {
    std::uint64_t serial;approach::Vec com;std::array<double,4> quaternion;approach::Vec velocity,angular_velocity;
    std::array<approach::Vec,2> points,normals;std::array<double,2> gaps,loads;std::array<bool,2> strict;double closing;
};
const std::array<SettleRow162,18> settle162{{
        {437ULL,{295.60482788085938,275.22702026367188,-487.189208984375},{0.024237975478172302,0.93565928936004639,-0.02989562600851059,-0.35079970955848694},{-1.3966192007064819,-4.3165502548217773,-1.6597727537155151},{-0.12088111042976379,-0.0021428137551993132,-0.033086512237787247},{{{295.1630859375,274.32159423828125,-487.63946533203125},{0,0,0}}},{{{-0.0026776189915835857,0.99999368190765381,0.0023315376602113247},{0,0,0}}},{-0.018373213708400726,0.031004521995782852},{6382.458984375,0},{true,false},4.3586797714233398},
        {438ULL,{295.59237670898438,275.18960571289062,-487.20645141601562},{0.031650949269533157,0.93583124876022339,-0.010728860273957253,-0.35086038708686829},{-0.74670565128326416,-2.2443616390228271,-1.0339072942733765},{1.8395682573318481,-0.082234904170036316,-1.640528678894043},{{{295.13565063476562,274.32171630859375,-487.6748046875},{295.92633056640625,274.31918334960938,-486.76791381835938}}},{{{-0.0026756497099995613,0.99999368190765381,0.0023350287228822708},{-0.00023932845215313137,0.99999356269836426,0.0035834214650094509}}},{-0.029542727395892143,-0.029573244974017143},{4852.822265625,12129.9267578125},{true,true},4.3586797714233398},
        {439ULL,{295.58026123046875,275.19400024414062,-487.22509765625},{0.033530518412590027,0.9357527494430542,-0.010795931331813335,-0.3508930504322052},{-0.72723650932312012,0.2627885639667511,-1.1194542646408081},{-0.086663253605365753,0.004936728160828352,-0.20858366787433624},{{{295.12161254882812,274.32174682617188,-487.69265747070312},{295.91256713867188,274.31924438476562,-486.78582763671875}}},{{{-0.0026757840532809496,0.99999368190765381,0.0023331055417656898},{-0.00095513102132827044,0.99999332427978516,0.0035317146684974432}}},{-0.024263069033622742,-0.025758320465683937},{230.733154296875,521.88836669921875},{false,false},0},
        {440ULL,{295.56817626953125,275.1976318359375,-487.24383544921875},{0.035216864198446274,0.93569040298461914,-0.01153674628585577,-0.35087049007415771},{-0.72481966018676758,0.21780931949615479,-1.1234844923019409},{-0.15436482429504395,0.00086445757187902927,-0.15838883817195892},{{{295.1080322265625,274.32177734375,-487.70993041992188},{295.89874267578125,274.31930541992188,-486.8031005859375}}},{{{-0.0026728333905339241,0.99999368190765381,0.0023325872607529163},{-0.00023778132162988186,0.99999356269836426,0.0035817991010844707}}},{-0.020631503313779831,-0.021913275122642517},{375.48977661132812,445.01858520507812},{false,false},0},
        {441ULL,{295.55615234375,275.20068359375,-487.26260375976562},{0.036944381892681122,0.93562406301498413,-0.012292853556573391,-0.35084420442581177},{-0.72064727544784546,0.18350331485271454,-1.126897931098938},{-0.1578361988067627,0.0006264873081818223,-0.16238042712211609},{{{295.094482421875,274.32177734375,-487.72726440429688},{295.88543701171875,274.3193359375,-486.82040405273438}}},{{{-0.0026759658940136433,0.99999368190765381,0.0023328943643718958},{-0.00095665076514706016,0.99999326467514038,0.0035354241263121367}}},{-0.017549244686961174,-0.018647786229848862},{390.95236206054688,457.78927612304688},{false,false},0},
        {442ULL,{295.54421997070312,275.2032470703125,-487.28143310546875},{0.038722559809684753,0.93555247783660889,-0.013050859794020653,-0.35081583261489868},{-0.7161443829536438,0.15363956987857819,-1.1305950880050659},{-0.16019913554191589,0.00054243946215137839,-0.16800764203071594},{{{295.08099365234375,274.32177734375,-487.74456787109375},{295.8719482421875,274.31939697265625,-486.8377685546875}}},{{{-0.0026748955715447664,0.99999368190765381,0.0023320626933127642},{-0.00095394940581172705,0.99999332427978516,0.0035330678801983595}}},{-0.014924750663340092,-0.015870582312345505},{403.2645263671875,474.37008666992188},{false,false},0},
        {443ULL,{295.53237915039062,275.20538330078125,-487.30035400390625},{0.040553756058216095,0.93547523021697998,-0.013818223960697651,-0.35078564286231995},{-0.71128678321838379,0.12832029163837433,-1.1346107721328735},{-0.16349925100803375,0.00054388865828514099,-0.17359019815921783},{{{295.06756591796875,274.32177734375,-487.761962890625},{295.85824584960938,274.3194580078125,-486.85516357421875}}},{{{-0.0026719421148300171,0.99999368190765381,0.0023336051963269711},{-0.00023764405341353267,0.99999356269836426,0.00358003587462008}}},{-0.012727417983114719,-0.013520888052880764},{414.71188354492188,489.08575439453125},{false,false},0},
        {444ULL,{295.52059936523438,275.2071533203125,-487.3193359375},{0.042441241443157196,0.93539154529571533,-0.014600883238017559,-0.35075369477272034},{-0.70603221654891968,0.1071142703294754,-1.1389285326004028},{-0.16760493814945221,0.0005982479196973145,-0.17929315567016602},{{{295.05419921875,274.32177734375,-487.77926635742188},{295.84503173828125,274.31948852539062,-486.87258911132812}}},{{{-0.0030047183390706778,0.99999356269836426,0.001946247648447752},{-0.00095134414732456207,0.99999332427978516,0.0035284128971397877}}},{-0.010835343040525913,-0.01147608645260334},{420.0289306640625,494.36737060546875},{false,false},0},
        {445ULL,{295.5089111328125,275.2086181640625,-487.33840942382812},{0.044390790164470673,0.93530124425888062,-0.015396827831864357,-0.35071930289268494},{-0.70053929090499878,0.087577961385250092,-1.1435365676879883},{-0.17173242568969727,0.00056164187844842672,-0.18572945892810822},{{{295.04074096679688,274.32177734375,-487.796875},{295.8316650390625,274.31954956054688,-486.89007568359375}}},{{{-0.0026712510734796524,0.99999374151229858,0.0023285243660211563},{-0.00095170660642907023,0.99999332427978516,0.0035287293139845133}}},{-0.0092179039493203163,-0.0097976299002766609},{431.09561157226562,507.27700805664062},{false,false},0},
        {446ULL,{295.49734497070312,275.20980834960938,-487.3575439453125},{0.046406995505094528,0.9352034330368042,-0.016208413988351822,-0.35068246722221375},{-0.69463413953781128,0.071813218295574188,-1.1484537124633789},{-0.1763204038143158,0.00056888168910518289,-0.19258828461170197},{{{295.02734375,274.32177734375,-487.81439208984375},{295.81826782226562,274.319580078125,-486.90762329101562}}},{{{-0.0026692128740251064,0.99999374151229858,0.0023267476353794336},{-0.00095134240109473467,0.99999332427978516,0.003528412664309144}}},{-0.0078751416876912117,-0.0083632059395313263},{436.22891235351562,514.32989501953125},{false,false},0},
        {447ULL,{295.48587036132812,275.21078491210938,-487.37677001953125},{0.048493262380361557,0.93509757518768311,-0.017039863392710686,-0.35064300894737244},{-0.68839329481124878,0.05796600878238678,-1.1536704301834106},{-0.1815284937620163,0.00058365316363051534,-0.19965477287769318},{{{295.0140380859375,274.32180786132812,-487.8319091796875},{295.80496215820312,274.31964111328125,-486.92514038085938}}},{{{-0.0026662717573344707,0.99999374151229858,0.002326238201931119},{-0.00095179531490430236,0.99999332427978516,0.0035264270845800638}}},{-0.0067460015416145325,-0.0071425135247409344},{441.5650634765625,520.495361328125},{false,false},0},
        {448ULL,{295.47451782226562,275.2115478515625,-487.39608764648438},{0.050654109567403793,0.9349830150604248,-0.017892859876155853,-0.35060063004493713},{-0.68180090188980103,0.045928746461868286,-1.1591912508010864},{-0.18711933493614197,0.00059969315771013498,-0.20716147124767303},{{{295.000732421875,274.32180786132812,-487.8494873046875},{295.79165649414062,274.3197021484375,-486.94271850585938}}},{{{-0.0026665425393730402,0.99999374151229858,0.0023267364595085382},{-0.00094829185400158167,0.99999332427978516,0.0035257521085441113}}},{-0.0057694446295499802,-0.0061354418285191059},{445.46011352539062,526.39068603515625},{false,false},0},
        {449ULL,{295.4632568359375,275.21212768554688,-487.41549682617188},{0.052892737090587616,0.93485891819000244,-0.018772615119814873,-0.35055482387542725},{-0.67485350370407104,0.035432416945695877,-1.165030837059021},{-0.19343510270118713,0.00060921040130779147,-0.21481300890445709},{{{294.98751831054688,274.32180786132812,-487.86715698242188},{295.7784423828125,274.31973266601562,-486.9603271484375}}},{{{-0.0026644538156688213,0.99999374151229858,0.00232503074221313},{-0.00094891246408224106,0.99999332427978516,0.0035262943711131811}}},{-0.0049148900434374809,-0.0052199200727045536},{448.29653930664062,528.96905517578125},{false,false},0},
        {450ULL,{295.45211791992188,275.21255493164062,-487.43502807617188},{0.055214047431945801,0.93472456932067871,-0.019678909331560135,-0.35050547122955322},{-0.66756534576416016,0.025788910686969757,-1.1711604595184326},{-0.19993641972541809,0.00062490970594808459,-0.22302916646003723},{{{294.97430419921875,274.32180786132812,-487.88482666015625},{295.76522827148438,274.31979370117188,-486.97799682617188}}},{{{-0.0026637611445039511,0.99999374151229858,0.0023220288567245007},{-0.000945508130826056,0.99999332427978516,0.0035233264788985252}}},{-0.0042129931971430779,-0.0044875075109302998},{492.47457885742188,581.76141357421875},{true,true},0.11364845931529999},
        {451ULL,{295.44064331054688,275.21307373046875,-487.45425415039062},{0.057279318571090698,0.93458986282348633,-0.020493606105446815,-0.35048660635948181},{-0.68759304285049438,0.031273778527975082,-1.1534655094146729},{-0.17870138585567474,0.0040630572475492954,-0.1982283890247345},{{{294.96096801757812,274.32180786132812,-487.90237426757812},{295.751953125,274.31985473632812,-486.99560546875}}},{{{-0.0026627506595104933,0.99999374151229858,0.0023213729728013277},{-0.00094452640041708946,0.99999332427978516,0.0035224705934524536}}},{-0.0034500597976148129,-0.0037245743442326784},{461.30712890625,547.13134765625},{true,true},0.11364845931529999},
        {452ULL,{295.4288330078125,275.2135009765625,-487.47317504882812},{0.059099704027175903,0.93446540832519531,-0.021224569529294968,-0.35047271847724915},{-0.70822197198867798,0.02641097828745842,-1.135266900062561},{-0.15895086526870728,0.0042133820243179798,-0.17422790825366974},{{{294.947509765625,274.32180786132812,-487.91976928710938},{295.73855590820312,274.31988525390625,-487.0130615234375}}},{{{-0.0026599646080285311,0.99999374151229858,0.0023202556185424328},{-0.00094381015514954925,0.99999332427978516,0.0035218449775129557}}},{-0.0028091981075704098,-0.0030226758681237698},{464.57080078125,547.70428466796875},{true,true},0.11364845931529999},
        {453ULL,{295.41668701171875,275.2138671875,-487.49179077148438},{0.060682017356157303,0.93435269594192505,-0.021863142028450966,-0.35046356916427612},{-0.72869223356246948,0.022152060642838478,-1.1171706914901733},{-0.13851350545883179,0.0042093251831829548,-0.15134736895561218},{{{294.93392944335938,274.32183837890625,-487.93707275390625},{295.72500610351562,274.3199462890625,-487.03042602539062}}},{{{-0.0026607250329107046,0.99999374151229858,0.0023196069523692131},{-0.00094282958889380097,0.99999338388442993,0.0035209914203733206}}},{-0.0022904023062437773,-0.0024123094044625759},{467.9345703125,550.8790283203125},{true,true},0.11364845931529999},
        {454ULL,{295.40420532226562,275.21417236328125,-487.51010131835938},{0.062030289322137833,0.93425273895263672,-0.022408220916986465,-0.35045963525772095},{-0.74893301725387573,0.018922187387943268,-1.0992633104324341},{-0.11811545491218567,0.0041909590363502502,-0.1289590448141098},{{{294.91976928710938,274.32183837890625,-487.95452880859375},{295.71142578125,274.32000732421875,-487.04763793945312}}},{{{-0.0012774295173585415,0.99999427795410156,0.00313759152777493},{-0.00094113295199349523,0.99999338388442993,0.0035195122472941875}}},{-0.0018022408476099372,-0.0019240049878135324},{466.85296630859375,561.35406494140625},{true,true},0.11364845931529999},
}};
const std::array<SettleRow162,4> bounce161{{
        {4145ULL,{299.48440551757812,275.3221435546875,-481.814208984375},{-0.048964597284793854,0.90155595541000366,-0.11222352087497711,-0.4149760901927948},{-2.0383052825927734,-2.8594205379486084,-1.7549399137496948},{1.1582567691802979,-0.0065605868585407734,-1.3781753778457642},{{{299.105712890625,274.30816650390625,-482.13555908203125},{0,0,0}}},{{{-0.0012362211709842086,0.99999487400054932,0.0029574010986834764},{0,0,0}}},{-0.02771206758916378,0.25},{7466.41845703125,0},{true,false},4.6313629150390625},
        {4146ULL,{299.45068359375,275.29132080078125,-481.84323120117188},{-0.034300584346055984,0.90486782789230347,-0.08079306036233902,-0.41654545068740845},{-2.0232236385345459,-1.8488044738769531,-1.7412652969360352},{2.7054047584533691,-0.0073839155957102776,-3.1934027671813965},{{{299.03567504882812,274.30825805664062,-482.19549560546875},{0,0,0}}},{{{-0.0012220157077535987,0.99999487400054932,0.0029667331837117672},{0,0,0}}},{-0.022340869531035423,0.18945248425006866},{0,0},{false,false},0},
        {4147ULL,{299.41696166992188,275.25778198242188,-481.87225341796875},{-0.019588705152273178,0.90707689523696899,-0.049267169088125229,-0.41761291027069092},{-2.0232236385345459,-2.0121376514434814,-1.7412652969360352},{2.7047533988952637,-0.0067461738362908363,-3.193958044052124},{{{298.96759033203125,274.308349609375,-482.25372314453125},{0,0,0}}},{{{-0.0012141673360019922,0.99999481439590454,0.0029742487240582705},{0,0,0}}},{-0.016451023519039154,0.11224230378866196},{0,0},{false,false},0},
        {4148ULL,{299.38323974609375,275.22152709960938,-481.90127563476562},{-0.0048468019813299179,0.90818023681640625,-0.017684243619441986,-0.41817757487297058},{-2.0232236385345459,-2.1754710674285889,-1.7412652969360352},{2.704113245010376,-0.0060235960409045219,-3.1945037841796875},{{{298.90176391601562,274.3084716796875,-482.31008911132812},{0,0,0}}},{{{-0.0012111413525417447,0.99999481439590454,0.0029780126642435789},{0,0,0}}},{-0.010225376114249229,0.034726984798908234},{0,0},{false,false},0},
}};
approach::SettleSample settle_sample(const SettleRow162& row) {
    approach::SettleSample sample;sample.pending=sample.available=sample.caught_frozen=sample.neutral_controls=true;
    sample.identity={1,2,3,4,5,6,7};sample.quaternion=row.quaternion;
    const auto q=row.quaternion;const auto yaw=std::atan2(2*(q[0]*q[2]+q[1]*q[3]),1-2*(q[0]*q[0]+q[1]*q[1]));
    sample.motion={7,row.serial,100+(row.serial-437)*h,h,row.com,row.velocity,row.angular_velocity,yaw,true};
    for(unsigned i=0;i<2;++i)sample.wheels[i]={true,row.strict[i],row.points[i],row.normals[i],{},row.gaps[i],row.loads[i]};
    return sample;
}
void caught_pair(CaughtBar& ride) {
    ride.input.tailwhip=TrickDirection::right;ride.input.rear_neutral=false;ride.tick();
    ride.input.tailwhip=TrickDirection::none;ride.input.rear_neutral=true;for(unsigned i=0;i<40;++i)ride.tick();
    check(ride.tricks.state().bar_caught&&ride.tricks.state().tail_caught&&ride.tricks.state().pending_units==2,
        "HOLD fixture has actually completed/neutrally caught bar and tail before any contact");
}
void qualified_settle_run162() {
    const auto first=settle_sample(settle162.front());
    CaughtBar actual(30,first.motion.body_yaw,true,first.quaternion),baseline(30,first.motion.body_yaw,true,first.quaternion);caught_pair(actual);caught_pair(baseline);
    approach::SettleHoldTracker tracker;unsigned held=0;
    const auto bar_angle=actual.tricks.state().bar_angle,tail_angle=actual.tricks.state().tail_angle;
    for(const auto& row:settle162) {
        auto sample=settle_sample(row);sample.pending=actual.tricks.state().landing_pending;
        const auto hold=tracker.observe(sample);
        auto& input=actual.input;input.body_yaw=sample.motion.body_yaw;
        input.body_quaternion=row.quaternion;
        input.travel_yaw=std::atan2(row.velocity[0],row.velocity[2]);input.grounded=row.strict[0]||row.strict[1];
        input.closing_normal_speed=row.closing;input.landing_quality_available=true;
        const auto q=row.quaternion;const approach::Vec up{2*(q[0]*q[1]-q[2]*q[3]),1-2*(q[0]*q[0]+q[2]*q[2]),2*(q[1]*q[2]+q[0]*q[3])};
        input.body_up_alignment=1;input.support_up_alignment=1;
        for(unsigned i=0;i<2;++i)if(row.strict[i]) {
            input.body_up_alignment=std::min(input.body_up_alignment,approach::detail::dot(up,row.normals[i]));
            input.support_up_alignment=std::min(input.support_up_alignment,row.normals[i][1]);
        }
        baseline.input=input;baseline.input.landing_settle_hold=baseline.input.landing_settle_hold_invalid=false;
        baseline.tick();input.landing_settle_hold=hold.hold;input.landing_settle_hold_invalid=hold.cancel;
        const auto event=actual.tick();
        if(row.serial>=439&&row.serial<=449) {
            check(hold.hold&&!hold.cancel&&actual.tricks.state().landing_pending&&!event.combo_landed&&
                actual.tricks.state().landing_support_seconds==0,"eleven loaded negative-gap162 samples HOLD only; none earns support time or credit");
            check(actual.tricks.state().bar_angle==bar_angle&&actual.tricks.state().tail_angle==tail_angle&&
                actual.tricks.state().pending_units==2,"pending HOLD cannot rotate, late catch, or create more trick units");++held;
        }
        if(row.serial==441)check(!baseline.tricks.state().landing_pending&&baseline.tricks.state().tally.canceled_combos==1&&
            baseline.tricks.state().tally.failed_combos==0,"unchanged generic40ms policy reproduces162 cancellation, distinct from failed tally");
        if(row.serial==450)check(actual.tricks.state().landing_support_seconds==0,
            "first strict recontact ends an unsupported interval and begins at ZERO settled seconds");
        if(row.serial>=450&&row.serial<=453)check(!event.combo_landed,"strict recontact must earn a complete NEW60ms interval after HOLD");
        if(row.serial==454)check(event.combo_landed&&event.barspins==1&&event.tailwhips==1,
            "only four fresh strict60Hz intervals commit the previously caught pair");
    }
    check(held==11&&actual.tricks.state().tally.landed_combos==1,"bounded qualified162 candidate earns exactly one combo after actual strict settling");
    for(unsigned fault=0;fault<16;++fault) {
        approach::SettleHoldTracker guarded;const auto seed=settle_sample(settle162[1]);
        check(!guarded.observe(seed).hold,"actual strict438 planes seed without credit");
        check(guarded.observe(settle_sample(settle162[2])).hold,"actual439 loaded seam begins one monotonic hold lease");
        auto invalid=settle_sample(settle162[3]);
        if(fault==0)invalid.available=false;
        if(fault==1)invalid.caught_frozen=false;
        if(fault==2)invalid.neutral_controls=false; // hop/manual/new trick/late catch input must all map false
        if(fault==3)invalid.identity[5]++;
        if(fault==4)invalid.motion.token++;
        if(fault==5)invalid.motion.serial++;
        if(fault==6)invalid.motion.simulation_time+=h;
        if(fault==7)invalid.quaternion={0,0,0,0};
        if(fault==8)invalid.wheels[0].verified_plane=false;
        if(fault==9)invalid.wheels[0].normal_load=0;
        if(fault==10)invalid.wheels[0].separation=.001;
        if(fault==11)invalid.wheels[0].separation=-.03;
        if(fault==12)invalid.wheels[0].point[1]+=.003;
        if(fault==13)invalid.motion.linear_velocity[1]+=.5;
        if(fault==14)invalid.quaternion={1,0,0,0};
        if(fault==15)invalid.motion.linear_velocity[0]*=-1;
        const auto refused=guarded.observe(invalid);
        check(!refused.hold&&refused.cancel,"used HOLD revokes on input, identity, clock, quaternion, plane, load, gap, rebound or heading failure");
    }
    approach::SettleHoldTracker duplicate;auto seed=settle_sample(settle162[1]);duplicate.observe(seed);
    auto first_gap=settle_sample(settle162[2]);check(duplicate.observe(first_gap).hold,"duplicate fixture begins a loaded hold");
    const auto paused=duplicate.observe(first_gap);check(!paused.hold&&!paused.cancel&&paused.issue==approach::SettleIssue::duplicate,
        "a duplicate epoch cannot provide fresh hold proof or spend native time");
    check(duplicate.observe(settle_sample(settle162[3])).hold,"same candidate resumes only on the next coherent native epoch");
    duplicate.reset();check(!duplicate.observe(settle_sample(settle162[3])).hold,
        "reset/new mount cannot seed a lease from an unsupported sample or reuse the preceding contact");
    const auto first_bounce=settle_sample(bounce161.front());CaughtBar freeflight(30,first_bounce.motion.body_yaw);
    approach::SettleHoldTracker no_lease;
    for(const auto& row:bounce161) {
        auto sample=settle_sample(row);sample.pending=freeflight.tricks.state().landing_pending;
        const auto proof=no_lease.observe(sample);check(!proof.hold,"actual161 initial zero-load/rear-clear bounce never obtains a recovery lease");
        freeflight.input.grounded=row.strict[0]||row.strict[1];freeflight.input.body_yaw=sample.motion.body_yaw;
        freeflight.input.travel_yaw=std::atan2(row.velocity[0],row.velocity[2]);freeflight.input.closing_normal_speed=row.closing;
        freeflight.input.landing_settle_hold=proof.hold;freeflight.input.landing_settle_hold_invalid=proof.cancel;freeflight.tick();
    }
    check(freeflight.tricks.state().tally.canceled_combos==1&&!freeflight.tricks.state().landing_pending,
        "real161 zero-load bounce still cancels at the original third unsupported tick/40ms limit");
    CaughtBar timed;timed.input.landing_quality_available=true;timed.land(approach::Evidence{.valid=true,.post_available=true});
    bool expired=false;for(unsigned i=0;i<20&&!expired;++i) {
        timed.input.grounded=i%3==2;timed.input.landing_settle_hold=!timed.input.grounded;
        expired=timed.tick().canceled;
    }
    check(expired&&timed.tricks.state().tally.landed_combos==0,"brief strict returns cannot reset the200ms unsupported HOLD deadline or bank nonconsecutive support");
    CaughtBar brief;brief.land(approach::Evidence{.valid=true,.post_available=true});
    brief.input.grounded=false;brief.tick();brief.input.grounded=true;
    check(!brief.tick().combo_landed&&brief.tricks.state().landing_support_seconds==0,
        "generic short-gap recontact also begins at zero; no exception depends on qualified HOLD");
    for(unsigned i=0;i<4;++i)check(brief.tick().combo_landed==(i==3),
        "four intervals wholly following generic strict recontact are needed for60ms credit");
    InputMapper mapper;RawInput raw;raw.available=true;raw.device=2;mapper.update(raw,{false},h);
    const auto threshold=mapper.settings().trigger_neutral;
    raw.pedal=threshold+.001;auto drive=mapper.update(raw,{false},h);
    check(drive.active&&drive.pedal==0&&!approach::settle_drive_neutral(raw.pedal,raw.brake,threshold),
        "explicit held pedal revokes HOLD even though airborne mapper output suppresses pedal");
    raw.pedal=0;raw.brake=threshold+.001;drive=mapper.update(raw,{false},h);
    check(drive.active&&drive.brake>0&&!approach::settle_drive_neutral(raw.pedal,raw.brake,threshold),
        "explicit held brake revokes HOLD at the same mapper neutral threshold");
    raw.pedal=raw.brake=threshold;
    check(approach::settle_drive_neutral(raw.pedal,raw.brake,threshold),"raw neutral boundary remains inclusive like InputMapper");
    check(!approach::settle_drive_neutral(std::numeric_limits<double>::quiet_NaN(),0,threshold)&&
        !approach::settle_drive_neutral(0,std::numeric_limits<double>::infinity(),threshold),"invalid raw drive commands fail closed");
    for(unsigned command=0;command<2;++command) {
        approach::SettleHoldTracker guarded;guarded.observe(settle_sample(settle162[1]));guarded.observe(settle_sample(settle162[2]));
        auto next=settle_sample(settle162[3]);next.neutral_controls=approach::settle_drive_neutral(
            command==0?threshold+.001:0,command==1?threshold+.001:0,threshold);
        const auto refused=guarded.observe(next);check(!refused.hold&&refused.cancel,
            "production raw-drive neutral proof revokes an already used HOLD for either explicit drive command");
    }
}

}
int main() {
    stopped_hard_impact_cannot_become_a_clean_landing();
    sideways_native_redirection_cannot_rewrite_approach_alignment();
    clean_contact_keeps_settling_and_frozen_approach_evidence();
    point_lever_and_gravity_are_explicit_estimates();
    stale_pause_identity_and_bad_samples_clear_history();
    first_actual_touch_remains_authoritative_during_a_new_hop();
    qualified_settle_run162();
    std::cout<<"BMX prior unsupported landing observations, conservative impact scoring and lifecycle tests passed\n";
}
