#ifndef _LEGENDRE_
#define _LEGENDRE_

float P(int l, int m, float x) {
  // associated legendre polynomials
  if (l == 0 && m == 0) {
    return 1.0;
  } else if (l == 1) {
    if (m == -1) {
      return 0.5 * sqrt(max(1.0 - x * x, 0.0));
    } else if (m == 0) {
      return x;
    } else if (m == 1) {
      return -sqrt(max(1.0 - x * x, 0.0));
    }
  } else if (l == 2) {
    if (m == -2) {
      return 0.125 * (1.0 - x * x);
    } else if (m == -1) {
      return 0.5 * x * sqrt(max(1.0 - x * x, 0.0));
    } else if (m == 0) {
      return 0.5 * (3.0 * x * x - 1.0);
    } else if (m == 1) {
      return - 3.0 * x * sqrt(max(1.0 - x * x, 0.0));
    } else if (m == 2) {
      return 3.0 * (1.0 - x * x);
    }
  } else if (l == 3) {
    if ( m == -3) {
      float y = max(1.0 - x * x, 0.0);
      y *= y * y;
      return sqrt(y) / 48.0;
    } else if (m == -2) {
      return 0.125 * x * (1.0 - x * x);
    } else if (m == -1) {
      return -0.125 * (1.0 - 5.0 * x * x) * sqrt(max(1.0 - x * x, 0.0));
    } else if (m == 0) {
      return 0.5 * (5.0 * x * x * x - 3.0 * x);
    } else if (m == 1) {
      return 1.5 * (1.0 - 5.0 * x * x) * sqrt(max(1.0 - x * x, 0.0));
    } else if (m == 2) {
      return 15.0 * x * (1.0 - x * x);
    } else if (m == 3) {
      float y = max(1.0 - x * x, 0.0);
      y *= y * y;
      return -15.0 * sqrt(y);
    }
  }

  return 0.0;
}

float P(float x, uint u) {
  int i = int(u);

  int l = 0;
  int m = 0;
  if (i == 0) {
    l = 0;
    m = 0; 
  } else if (i < 4) {
    l = 1;
    m = i-2;
  } else if (i < 9) {
    l = 2;
    m = i-6;
  } else if (i < 16) {
    l = 3;
    m = i-12;
  } else {
    return 0.0;
  }

  float p = P(l, m, x);

  // Apply weight function
  p /= 2.0 / (2.0 * l + 1.0);
  for (int j = l-m+1; j<=l+m; ++j) {
    p /= j;
  }
  return p;
}
#endif // _LEGENDRE_