#include "Engine/Game/BMX/bmx_native_coupled_calibration.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace c=dingosdk::bmx::native_coupled::calibration;
namespace p=dingosdk::bmx::native_coupled::policy;
unsigned checks{};void check(bool okay,const char* name){++checks;if(!okay){std::fprintf(stderr,"FAIL %s\n",name);std::exit(1);}}
c::Sample initial(){c::Sample s;s.epoch.owner_world={1,2,3,4,5,6,7,8};s.epoch.bodies={p::BodyKey{6,1,1},p::BodyKey{6,2,1}};
 s.epoch.token=1;s.epoch.serial=1;s.epoch.time=p::native_seconds;s.epoch.seconds=static_cast<float>(p::native_seconds);s.clearance={.08f,.08f};return s;}
void next(c::Sample& s){++s.epoch.serial;s.epoch.time+=s.epoch.seconds;}
// Actual native269 accepted rows; source JSON SHA256 22ABBB8414B2D500B950EBB38B5761A5ACE231938C489ECCEB92A16DC1C830F9
c::Sample native269(unsigned index){
    auto s=initial();
    switch(index){
    case 0:
        s.epoch.serial=1;s.epoch.time=0.01666666567325592;
        s.position[0]={307.127594f,274.665253f,-476.926788f};s.linear[0]={0.0f,0.0f,0.0f};s.angular[0]={0.0f,0.0f,0.0f};s.clearance[0]=0.0799858198f;
        s.position[1]={305.713196f,274.379791f,-475.374786f};s.linear[1]={0.0f,0.0f,0.0f};s.angular[1]={0.0f,0.0f,0.0f};s.clearance[1]=0.0799989402f;
        return s;
    case 1:
        s.epoch.serial=2;s.epoch.time=0.03333333134651184;
        s.position[0]={307.127594f,274.662537f,-476.926788f};s.linear[0]={0.0f,-0.163333327f,0.0f};s.angular[0]={0.0f,0.0f,0.0f};s.clearance[0]=0.0772699714f;
        s.position[1]={305.713196f,274.377075f,-475.374786f};s.linear[1]={0.0f,-0.163333327f,0.0f};s.angular[1]={0.0f,0.0f,0.0f};s.clearance[1]=0.0772830993f;
        return s;
    case 2:
        s.epoch.serial=3;s.epoch.time=0.04999999701976776;
        s.position[0]={307.127594f,274.657104f,-476.926788f};s.linear[0]={0.0f,-0.326666653f,0.0f};s.angular[0]={0.0f,0.0f,0.0f};s.clearance[0]=0.0718382746f;
        s.position[1]={305.713196f,274.371643f,-475.374786f};s.linear[1]={0.0f,-0.326666653f,0.0f};s.angular[1]={0.0f,0.0f,0.0f};s.clearance[1]=0.0718514025f;
        return s;
    case 3:
        s.epoch.serial=4;s.epoch.time=0.06666666269302368;
        s.position[0]={307.127594f,274.648926f,-476.926788f};s.linear[0]={0.0f,-0.48999995f,0.0f};s.angular[0]={0.0f,0.0f,0.0f};s.clearance[0]=0.0636602044f;
        s.position[1]={305.713196f,274.363464f,-475.374786f};s.linear[1]={0.0f,-0.48999995f,0.0f};s.angular[1]={0.0f,0.0f,0.0f};s.clearance[1]=0.0636733547f;
        return s;
    case 4:
        s.epoch.serial=5;s.epoch.time=0.0833333283662796;
        s.position[0]={307.127594f,274.638031f,-476.926788f};s.linear[0]={0.0f,-0.653333306f,0.0f};s.angular[0]={0.0f,0.0f,0.0f};s.clearance[0]=0.0527662896f;
        s.position[1]={305.713196f,274.35257f,-475.374786f};s.linear[1]={0.0f,-0.653333306f,0.0f};s.angular[1]={0.0f,0.0f,0.0f};s.clearance[1]=0.0527794547f;
        return s;
    case 5:
        s.epoch.serial=6;s.epoch.time=0.09999999403953552;
        s.position[0]={307.127594f,274.62442f,-476.926788f};s.linear[0]={0.0f,-0.816666663f,0.0f};s.angular[0]={0.0f,0.0f,0.0f};s.clearance[0]=0.0391565226f;
        s.position[1]={305.713196f,274.338959f,-475.374786f};s.linear[1]={0.0f,-0.816666663f,0.0f};s.angular[1]={0.0f,0.0f,0.0f};s.clearance[1]=0.0391697139f;
        return s;
    case 6:
        s.epoch.serial=7;s.epoch.time=0.11666665971279144;
        s.position[0]={307.127594f,274.608093f,-476.926788f};s.linear[0]={0.0f,-0.980000019f,0.0f};s.angular[0]={0.0f,0.0f,0.0f};s.clearance[0]=0.0228309054f;
        s.position[1]={305.713196f,274.322632f,-475.374786f};s.linear[1]={0.0f,-0.980000019f,0.0f};s.angular[1]={0.0f,0.0f,0.0f};s.clearance[1]=0.0228441246f;
        return s;
    case 7:
        s.epoch.serial=8;s.epoch.time=0.13333332538604736;
        s.position[0]={307.127594f,274.58905f,-476.926788f};s.linear[0]={0.0f,-1.14333332f,0.0f};s.angular[0]={0.0f,0.0f,0.0f};s.clearance[0]=0.00378943887f;
        s.position[1]={305.713196f,274.303589f,-475.374786f};s.linear[1]={0.0f,-1.14333332f,0.0f};s.angular[1]={0.0f,0.0f,0.0f};s.clearance[1]=0.00380269019f;
        return s;
    case 8:
        s.epoch.serial=9;s.epoch.time=0.14999999105930328;
        s.position[0]={307.127686f,274.585205f,-476.92688f};s.linear[0]={0.00505189039f,-0.231538653f,-0.00520852115f};s.angular[0]={-0.0174684487f,-0.00173423323f,-0.0171886254f};s.clearance[0]=-5.46018382e-05f;
        s.position[1]={305.713898f,274.288086f,-475.374023f};s.linear[1]={0.0429267734f,-0.929262757f,0.0463248938f};s.angular[1]={-1.16494393f,0.0140547948f,1.32186007f};s.clearance[1]=-0.00136221177f;
        return s;
    case 9:
        s.epoch.serial=10;s.epoch.time=0.1666666567325592;
        s.position[0]={307.127777f,274.585205f,-476.927002f};s.linear[0]={0.00629170565f,3.32517593e-05f,-0.00743154157f};s.angular[0]={-0.0257890429f,-0.00183263398f,-0.0217859f};s.clearance[0]=-5.34783321e-05f;
        s.position[1]={305.715302f,274.27298f,-475.37262f};s.linear[1]={0.0839302838f,-0.907534122f,0.0843178183f};s.angular[1]={-1.69921291f,0.0206944011f,1.95729661f};s.clearance[1]=-0.00152759929f;
        return s;
    case 10:
        s.epoch.serial=11;s.epoch.time=0.18333332240581512;
        s.position[0]={307.127899f,274.585175f,-476.927124f};s.linear[0]={0.00715321023f,-0.00112592103f,-0.00744045526f};s.angular[0]={-0.0277512036f,-0.000410781649f,-0.0267189108f};s.clearance[0]=-0.000109998546f;
        s.position[1]={305.717346f,274.256531f,-475.370605f};s.linear[1]={0.122241445f,-0.991825163f,0.120820716f};s.angular[1]={-1.90805936f,0.0161294062f,2.19546032f};s.clearance[1]=-0.00165191747f;
        return s;
    case 11:
        s.epoch.serial=12;s.epoch.time=0.19999998807907104;
        s.position[0]={307.128052f,274.585175f,-476.927246f};s.linear[0]={0.00913958624f,-0.000885456568f,-0.00752856117f};s.angular[0]={-0.0331075229f,-0.00192952971f,-0.0254780278f};s.clearance[0]=-8.23396258e-05f;
        s.position[1]={305.720093f,274.238831f,-475.367889f};s.linear[1]={0.164923117f,-1.0670774f,0.162804574f};s.angular[1]={-2.12217999f,0.0065882476f,2.44154096f};s.clearance[1]=-0.00182587293f;
        return s;
    case 12:
        s.epoch.serial=13;s.epoch.time=0.21666665375232697;
        s.position[0]={307.128174f,274.585175f,-476.927399f};s.linear[0]={0.00783722568f,-0.000269716635f,-0.00830446091f};s.angular[0]={-0.0360717997f,-0.00501891738f,-0.0313638709f};s.clearance[0]=-8.09636331e-05f;
        s.position[1]={305.723633f,274.220154f,-475.36438f};s.linear[1]={0.213283554f,-1.13061023f,0.210374311f};s.angular[1]={-2.34334278f,-0.00352169294f,2.6946826f};s.clearance[1]=-0.00198551547f;
        return s;
    case 13:
        s.epoch.serial=14;s.epoch.time=0.23333331942558289;
        s.position[0]={307.128296f,274.585175f,-476.927551f};s.linear[0]={0.00790009648f,-0.00029573639f,-0.00902630668f};s.angular[0]={-0.0396514013f,-0.005024957f,-0.0344896019f};s.clearance[0]=-7.95876404e-05f;
        s.position[1]={305.728119f,274.200714f,-475.359955f};s.linear[1]={0.26927796f,-1.1783886f,0.265453398f};s.angular[1]={-2.57183766f,-0.0139707867f,2.95395756f};s.clearance[1]=-0.00225142855f;
        return s;
    case 14:
        s.epoch.serial=15;s.epoch.time=0.2499999850988388;
        s.position[0]={307.128479f,274.585144f,-476.927704f};s.linear[0]={0.0104136337f,-0.000998157077f,-0.00954136532f};s.angular[0]={-0.0404814705f,-0.00206141802f,-0.0371970609f};s.clearance[0]=-0.000106829735f;
        s.position[1]={305.733704f,274.180939f,-475.354462f};s.linear[1]={0.335558742f,-1.20493913f,0.330650598f};s.angular[1]={-2.8045435f,-0.0253381263f,3.2173748f};s.clearance[1]=-0.00247155759f;
        return s;
    case 15:
        s.epoch.serial=16;s.epoch.time=0.2666666507720947;
        s.position[0]={307.128662f,274.585144f,-476.927917f};s.linear[0]={0.010368824f,-0.000326567795f,-0.0122821545f};s.angular[0]={-0.0431551076f,-0.00522067584f,-0.0374748595f};s.clearance[0]=-0.00010743898f;
        s.position[1]={305.740631f,274.161255f,-475.347656f};s.linear[1]={0.415493965f,-1.20332718f,0.409278423f};s.angular[1]={-3.03277969f,-0.0387890078f,3.47873545f};s.clearance[1]=-0.00282971468f;
        return s;
    case 16:
        s.epoch.serial=17;s.epoch.time=0.28333331644535065;
        s.position[0]={307.128845f,274.585144f,-476.928131f};s.linear[0]={0.01114171f,-0.00013122776f,-0.0130009092f};s.angular[0]={-0.046602957f,-0.00211714255f,-0.039901033f};s.clearance[0]=-7.49292885e-05f;
        s.position[1]={305.749298f,274.142548f,-475.339142f};s.linear[1]={0.519805491f,-1.15491784f,0.511776268f};s.angular[1]={-3.2560215f,-0.0491224304f,3.72115898f};s.clearance[1]=-0.00298302411f;
        return s;
    case 17:
        s.epoch.serial=18;s.epoch.time=0.29999998211860657;
        s.position[0]={307.129028f,274.585144f,-476.928375f};s.linear[0]={0.0118541764f,-0.000154348134f,-0.0138221681f};s.angular[0]={-0.0496542007f,-0.00217157113f,-0.0425468609f};s.clearance[0]=-7.29347666e-05f;
        s.position[1]={305.76004f,274.125458f,-475.328583f};s.linear[1]={0.64418298f,-1.06557214f,0.633997023f};s.angular[1]={-3.42873192f,-0.0591237247f,3.90858912f};s.clearance[1]=-0.00326536875f;
        return s;
    case 18:
        s.epoch.serial=19;s.epoch.time=0.3166666477918625;
        s.position[0]={307.129242f,274.585144f,-476.928619f};s.linear[0]={0.0125687411f,-0.000178742572f,-0.0146423792f};s.angular[0]={-0.0527165458f,-0.00221499149f,-0.0452134274f};s.clearance[0]=-7.1167342e-05f;
        s.position[1]={305.773163f,274.110657f,-475.316376f};s.linear[1]={0.788316786f,-0.936603606f,0.73341924f};s.angular[1]={-3.56807852f,-0.0121012088f,4.05620337f};s.clearance[1]=-0.00362605136f;
        return s;
    default:std::abort();
    }
}
int main(){
    check(static_cast<unsigned>(c::Kind::pair_filter)==0&&static_cast<unsigned>(c::Kind::world_contact)==1&&static_cast<unsigned>(c::Kind::pair_positive)==2,"existing mode values unchanged");
    c::Monitor replay;check(replay.begin(c::Kind::world_contact_only,native269(0)),"recorded269 starts bounded contact-only replay");
    unsigned completion{};
    for(unsigned i=1;i<19;++i){check(replay.observe(native269(i)),"recorded accepted pre-cap rows support honest bounded contact-only observation");
      if(replay.report().phase==c::Phase::complete){completion=i;break;}}
    check(completion>0&&completion<18,"contact-only stops before recorded native269 horizontal speed failure");
    const auto& contact=replay.report();check(contact.contact_candidate[0]&&contact.contact_candidate[1]&&contact.falling[0]&&contact.falling[1],"both actual recorded roles fall and show bounded contact deceleration");
    check(contact.settled[1]<3&&std::strcmp(contact.issue,"both_actual_roles_contact_candidate_no_settling_claim")==0,"contact-only never requires or claims unsupported Spring settling");
    check(!replay.observe(native269(18))&&replay.report().count==contact.count,"terminal contact proof cannot accept later observations");
    c::Monitor old;check(old.begin(c::Kind::world_contact,native269(0)),"unchanged world45-step mode starts");
    for(unsigned i=1;i<18;++i)check(old.observe(native269(i)),"oldworld retains every accepted native269 row");
    check(!old.observe(native269(18))&&old.report().phase==c::Phase::failed,"oldworld native269 remains failed");
    check(old.report().rejected_role==1&&std::strcmp(old.report().rejected_cap,"horizontal_speed")==0&&old.report().rejected_limit==1,"original speed cap and exact failure retained");
    for(unsigned variant=0;variant<5;++variant){c::Monitor m;auto s=initial();check(m.begin(c::Kind::world_contact_only,s),"negative contact fixture starts");
      next(s);for(unsigned role=0;role<2;++role){s.position[role][1]=-.004f;s.linear[role][1]=-.3f;s.clearance[role]=.05f;}
      check(m.observe(s),"negative prelude falls");next(s);s.linear={};s.clearance={.01f,.01f};
      if(variant==0)s.clearance[1]=.1f;
      if(variant==1)s.linear[1][1]=-.4f;
      if(variant==2)s.epoch.bodies[1].generation++;
      if(variant==3)s.angular[1][0]=8.1f;
      if(variant==4)s.epoch.serial++;
      const auto accepted=m.observe(s);
      check(m.report().phase!=c::Phase::complete&&!m.report().neutral_response_candidate,"one role missing contact or any pair/cadence/cap failure cannot complete");
      check(variant<2?accepted:!accepted,"missing contact stays running while invalid evidence rejects");
    }
    c::Monitor timeout;auto s=initial();check(timeout.begin(c::Kind::world_contact_only,s),"finite contact timeout begins");
    for(unsigned i=1;i<=45;++i){next(s);(void)timeout.observe(s);}
    check(timeout.report().phase==c::Phase::failed&&!timeout.report().neutral_response_candidate,"no-contact stationary data cannot pass contact-only mode");
    std::printf("contact-only actual269 offline replay: %u checks PASS, stopped row%u; no settling or new native proof\n",checks,completion);
}
