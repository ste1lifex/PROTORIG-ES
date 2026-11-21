/* Hardware-independent gesture classification; device pixels, not CSS points. */
#ifndef UI_GESTURE_H
#define UI_GESTURE_H
#include <stdint.h>
#include <stdlib.h>
typedef struct { int x, y, travel; uint32_t started; } ui_gesture_t;
enum { UI_NONE, UI_TAP, UI_LEFT, UI_RIGHT, UI_UP, UI_DOWN };
static inline void ui_gesture_begin(ui_gesture_t *g, int x, int y, uint32_t t)
{ g->x=x; g->y=y; g->travel=0; g->started=t; }
static inline int ui_gesture_update(ui_gesture_t *g, int x, int y)
{
    int dx=x-g->x, dy=y-g->y, ax=abs(dx), ay=abs(dy);
    if (ax+ay > g->travel) g->travel=ax+ay;
    if (ax >= 42 && ax*2 > ay*3) return dx < 0 ? UI_LEFT : UI_RIGHT;
    if (ay >= 52 && ay*2 > ax*3) return dy < 0 ? UI_UP : UI_DOWN;
    return UI_NONE;
}
static inline int ui_gesture_end(ui_gesture_t *g, int x, int y, uint32_t t)
{
    int swipe=ui_gesture_update(g,x,y);
    if (swipe) return swipe;
    return g->travel <= 24 && (uint32_t)(t-g->started) <= 650 ? UI_TAP : UI_NONE;
}
#endif
