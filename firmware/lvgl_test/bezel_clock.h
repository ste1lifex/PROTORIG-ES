#ifndef BEZEL_CLOCK_H
#define BEZEL_CLOCK_H
/* Clockwise tenths of degrees, zero at 12 o'clock. Independent of LVGL. */
static inline int bezel_angle(int hand, int hh, int mm, int ss)
{
    if(hand==0) return (hh%12)*300+mm*5+ss/12;
    if(hand==1) return mm*60+ss;
    return ss*60;
}
#endif
