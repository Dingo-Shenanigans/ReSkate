#pragma once
#include "movement.hpp"
#include <cmath>

namespace bmx {
// Intrinsic durations from current bound clips and their native speed consumer.
// MasterSpeed is the selected progression OR character multiplier, not their product.
// Controller transition, hold, cancel and scoring policies remain independent.
inline void apply_streets_observed_trick_timing(TrickSettings& settings,double master_speed=1) noexcept {
    if(!std::isfinite(master_speed)||master_speed<=0)return;
    settings.enter_seconds[0]=0.520833333333/master_speed;settings.recover_seconds[0]=0.590760127315/master_speed; // Grand Slam Left
    settings.enter_seconds[1]=0.454545454545/master_speed;settings.recover_seconds[1]=0.168732809917/master_speed; // Twix Right
    settings.enter_seconds[2]=0.355555573333/master_speed;settings.recover_seconds[2]=0.296296467302/master_speed; // Helicopter Right
    settings.enter_seconds[3]=0.5/master_speed;settings.recover_seconds[3]=0.583333428571/master_speed; // Bri Flip Left
    settings.enter_seconds[4]=0.280701768421/master_speed;settings.recover_seconds[4]=0.38986355779/master_speed; // Willy Flip
    settings.enter_seconds[5]=0.555555555556/master_speed;settings.recover_seconds[5]=0.806584493827/master_speed; // Mc Griddle Left
    settings.enter_seconds[6]=0.444444466667/master_speed;settings.recover_seconds[6]=0.162037130556/master_speed; // Kick Whip Left
    settings.enter_seconds[7]=0.370370377778/master_speed;settings.recover_seconds[7]=0.596707839506/master_speed; // Superman
    settings.enter_seconds[10]=0.520833333333/master_speed;settings.recover_seconds[10]=0.590760127315/master_speed; // Grand Slam Right
    settings.enter_seconds[11]=0.370370377778/master_speed;settings.recover_seconds[11]=0.596707839506/master_speed; // Christ Air
    settings.enter_seconds[12]=0.444444466667/master_speed;settings.recover_seconds[12]=0.162037130556/master_speed; // Kick Whip Right
    settings.enter_seconds[13]=0.555555555556/master_speed;settings.recover_seconds[13]=0.806584493827/master_speed; // Mc Griddle Right
    settings.enter_seconds[14]=0.33333336/master_speed;settings.recover_seconds[14]=0.24166668/master_speed; // Bar Hug
    settings.enter_seconds[15]=0.5/master_speed;settings.recover_seconds[15]=0.583333428571/master_speed; // Bri Flip Right
    settings.enter_seconds[16]=0.355555573333/master_speed;settings.recover_seconds[16]=0.296296467302/master_speed; // Helicopter Left
    settings.enter_seconds[17]=0.33333336/master_speed;settings.recover_seconds[17]=0.276190491429/master_speed; // Nothing
    settings.enter_seconds[20]=0.33333334/master_speed;settings.recover_seconds[20]=0.241666675/master_speed; // Tuck No
    settings.enter_seconds[21]=0.264550285714/master_speed;settings.recover_seconds[21]=0.243554225246/master_speed; // Table
    settings.enter_seconds[22]=0.2777778/master_speed;settings.recover_seconds[22]=0.335648166667/master_speed; // Invert
    settings.enter_seconds[23]=0.240384615385/master_speed;settings.recover_seconds[23]=0.297912554868/master_speed; // Turndown
    settings.enter_seconds[24]=0.190476194286/master_speed;settings.recover_seconds[24]=0.157823134694/master_speed; // Suicide
    settings.enter_seconds[25]=0.2777778/master_speed;settings.recover_seconds[25]=0.268518533333/master_speed; // Euro
    settings.enter_seconds[26]=0.33333336/master_speed;settings.recover_seconds[26]=0.386666688/master_speed; // Lookback
    settings.enter_seconds[27]=0.33333336/master_speed;settings.recover_seconds[27]=0.276190491429/master_speed; // Cannon Ball
    settings.enter_seconds[31]=0.256410261538/master_speed;settings.recover_seconds[31]=0.238330038963/master_speed; // Tire Grab Left
    settings.enter_seconds[32]=0.256410261538/master_speed;settings.recover_seconds[32]=0.238330038963/master_speed; // Seat Grab Left
    settings.enter_seconds[33]=0.4166667/master_speed;settings.recover_seconds[33]=0.3250001/master_speed; // Bus Driver Left
    settings.enter_seconds[34]=0.33333336/master_speed;settings.recover_seconds[34]=0.208000064/master_speed; // Barspin Left
    settings.enter_seconds[35]=0.1388889/master_speed;settings.recover_seconds[35]=0.167824083333/master_speed; // Toboggan Left
    settings.enter_seconds[36]=0.2777778/master_speed;settings.recover_seconds[36]=0.191798952381/master_speed; // One Hander Left
    settings.enter_seconds[37]=0.266666672/master_speed;settings.recover_seconds[37]=0.38666668/master_speed; // Xup Left
    settings.enter_seconds[41]=0.266666672/master_speed;settings.recover_seconds[41]=0.38666668/master_speed; // Xup Right
    settings.enter_seconds[42]=0.2777778/master_speed;settings.recover_seconds[42]=0.191798952381/master_speed; // One Hander Right
    settings.enter_seconds[43]=0.1388889/master_speed;settings.recover_seconds[43]=0.167824083333/master_speed; // Toboggan Right
    settings.enter_seconds[44]=0.33333336/master_speed;settings.recover_seconds[44]=0.208000064/master_speed; // Barspin Right
    settings.enter_seconds[45]=0.4166667/master_speed;settings.recover_seconds[45]=0.3250001/master_speed; // Bus Driver Right
    settings.enter_seconds[46]=0.256410261538/master_speed;settings.recover_seconds[46]=0.238330038963/master_speed; // Seat Grab Right
    settings.enter_seconds[47]=0.256410261538/master_speed;settings.recover_seconds[47]=0.320512799967/master_speed; // Tire Grab Right
    settings.enter_seconds[50]=0.16666668/master_speed;settings.recover_seconds[50]=0.24166668/master_speed; // Tuck Up
    settings.enter_seconds[51]=0.432098765432/master_speed;settings.recover_seconds[51]=0.397805255805/master_speed; // No Foot Can Right
    settings.enter_seconds[52]=0.416666666667/master_speed;settings.recover_seconds[52]=0.226851888889/master_speed; // Whip Right
    settings.enter_seconds[53]=0.416666666667/master_speed;settings.recover_seconds[53]=0.226851888889/master_speed; // Whopper Right
    settings.enter_seconds[54]=0.33333335/master_speed;settings.recover_seconds[54]=0.260416816964/master_speed; // Crankflip
    settings.enter_seconds[55]=0.416666666667/master_speed;settings.recover_seconds[55]=0.226851888889/master_speed; // Whopper Left
    settings.enter_seconds[56]=0.416666666667/master_speed;settings.recover_seconds[56]=0.226851888889/master_speed; // Whip Left
    settings.enter_seconds[57]=0.411522641975/master_speed;settings.recover_seconds[57]=0.397805255805/master_speed; // No Foot Can Left
    settings.enter_seconds[61]=0.277777783333/master_speed;settings.recover_seconds[61]=0.419560182171/master_speed; // Can Left
    settings.enter_seconds[62]=0.625/master_speed;settings.recover_seconds[62]=0.6041666875/master_speed; // Moto Left
    settings.enter_seconds[63]=0.277777783333/master_speed;settings.recover_seconds[63]=0.419560182171/master_speed; // Nac Left
    settings.enter_seconds[65]=0.266666672/master_speed;settings.recover_seconds[65]=0.412444458667/master_speed; // Griz Air Left
    settings.enter_seconds[66]=0.33333336/master_speed;settings.recover_seconds[66]=0.276190491429/master_speed; // One Footer Left
    settings.enter_seconds[67]=0.303030309091/master_speed;settings.recover_seconds[67]=0.413223140496/master_speed; // Candy Bar Left
    settings.enter_seconds[71]=0.303030309091/master_speed;settings.recover_seconds[71]=0.399449049587/master_speed; // Candy Bar Right
    settings.enter_seconds[72]=0.33333336/master_speed;settings.recover_seconds[72]=0.276190491429/master_speed; // One Footer Right
    settings.enter_seconds[73]=0.266666672/master_speed;settings.recover_seconds[73]=0.412444458667/master_speed; // Griz Air Right
    settings.enter_seconds[75]=0.277777783333/master_speed;settings.recover_seconds[75]=0.419560182171/master_speed; // Nac Right
    settings.enter_seconds[76]=0.625/master_speed;settings.recover_seconds[76]=0.6041666875/master_speed; // Moto Right
    settings.enter_seconds[77]=0.277777783333/master_speed;settings.recover_seconds[77]=0.419560182171/master_speed; // Can Right
    settings.bar_seconds=(settings.enter_seconds[34]+settings.enter_seconds[44])*.5;
    settings.tail_seconds=(settings.enter_seconds[52]+settings.enter_seconds[56])*.5;
}
}
