#ifndef _LEGENDRE_
#define _LEGENDRE_

#define DECL_LEGENDRE_HELPERS(X)  \
  float x2 = x * x;               \
  float x4 = x2 * x2;             \
  float _1_x2 = 1.0 - x2;         \
  float _sqrt_1_x2 = sqrt(_1_x2); \
  float _sqrt_1_x2_pow_3 = _1_x2 * _sqrt_1_x2;

#define P0_0 1.0
#define P0_1 x
#define P1_1 -_sqrt_1_x2
#define P0_2 (1.5 * x2 - 0.5)
#define P1_2 -3.0 * x * _sqrt_1_x2
#define P2_2 3.0 * _1_x2
#define P0_3 0.5 * x * (5.0 * x2 - 3.0)
#define P1_3 1.5 * (1.0 - 5.0 * x2) * _sqrt_1_x2
#define P2_3 15.0 * x * _1_x2
#define P3_3 -15.0 * _sqrt_1_x2_pow_3
#define P0_4 0.125 * (35.0 * x4 - 30.0 * x2 + 3.0)
#define P1_4 2.5 * x * (3.0 - 7 * x2) * _sqrt_1_x2
#define P2_4 7.5 * (7.0 * x2 - 1.0) * _1_x2
#define P3_4 -105.0 * x * _sqrt_1_x2_pow_3
#define P4_4 105.0 * _1_x2 * _1_x2
#define P0_5 0.125 * x * (63.0 * x4 - 70.0 * x2 + 15.0)

float _P(float x, uint i) {
  DECL_LEGENDRE_HELPERS(x)

  if (i == 0)
  {
    return P0_0;
  } else if (i == 1)
  {
    return P0_1;
  } else if (i == 2)
  {
    return P1_1;    
  } else if (i == 3)
  {
    return P0_2;
  } else if (i == 4)
  {
    return P1_2;
  } else if (i == 5)
  {
    return P2_2;
  } else if (i == 6)
  {
    return P0_3;
  } else if (i == 7)
  {
    return P1_3;
  } else if (i == 8)
  {
    return P2_3;
  } else if (i == 9)
  {
    return P3_3;
  } else if (i == 10)
  {
    return P0_4;
  } else if (i == 11)
  {
    return P1_4;
  } else if (i == 12)
  {
    return P2_4;
  } else if (i == 13)
  {
    return P3_4;
  } else if (i == 14)
  {
    return P4_4;
  } else if (i == 15)
  {
    return P0_5;
  }

  return 0.0;  
}

float P(float x, uint i) {
  // legendre function
  float p = _P(x, i);

  // apply phase
  // if (i % 2 == 1) {
  //   p = -p;
  // }

  // apply weight
  // float w = (1.01 - x * x);
  // p = p / (w * w);

  return p; 
}
#endif // _LEGENDRE_