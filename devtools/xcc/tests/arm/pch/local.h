/* A local header: a change to it must be seen (its sum) */
#ifndef LOCAL_H
#define LOCAL_H
#define LOCAL_VALUE 1
struct point { int x, y; };
static inline int manhattan(struct point p) { return (p.x < 0 ? -p.x : p.x) + (p.y < 0 ? -p.y : p.y); }
#endif
