typedef struct { int a, b, c; } S3;
typedef struct { char c; } S1;
typedef struct { double d; int i; } SD;
typedef struct { short x, y; } S4;
typedef struct { long long q; char c; } SQ;

double g_mix(int a, long long b, int c, double d, float e, float f, double g, int h, float i, long long j);
double g_vsum(int n, ...);   /* pairs: type tag, value */
long long g_ll(long long a, long long b, int c);
float g_f(float a, double b, float c, float d, float e, float f, float g, float h, float i, float j, float k, float l, float m, float n, float o, float p, float q, float r);
int g_s3(S3 s, int x, S1 t, S4 u);
S3 g_rs3(int a, int b, int c);
S4 g_rs4(short x, short y);
double g_sd(int a, SD s, int b);
long long g_sq(int a, SQ s, int b, SQ t);
int g_fp(int (*f)(int, double), int x);
int g_callx(void);

double x_mix(int a, long long b, int c, double d, float e, float f, double g, int h, float i, long long j);
double x_vsum(int n, ...);
int x_s3(S3 s, int x, S1 t, S4 u);
S3 x_rs3(int a, int b, int c);
double x_sd(int a, SD s, int b);
long long x_sq(int a, SQ s, int b, SQ t);
int x_cb(int a, double d);
