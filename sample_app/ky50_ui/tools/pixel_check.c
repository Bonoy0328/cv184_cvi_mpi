/* Compare the board NEON implementation against independent scalar equations. */
#include "pixels.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
static unsigned char clamp(int x) { return x<0?0:x>255?255:x; }
int main(void)
{
    unsigned char y[480],vu[480],rgb[1440],out[1440];uint32_t ui[480];
    srand(42);
    for(unsigned run=0;run<256;run++) {
        for(unsigned x=0;x<480;x++) {
            y[x]=rand();vu[x]=rand();ui[x]=(run<<24)|(rand()&0xffffff);
        }
        pixels_nv21(rgb,y,vu,480);
        for(unsigned x=0;x<480;x++) {
            int l=298*(y[x]-16),v=vu[x&~1U]-128,u=vu[(x&~1U)+1]-128;
            assert(rgb[3*x]==clamp((l+409*v+128)>>8));
            assert(rgb[3*x+1]==clamp((l-100*u-208*v+128)>>8));
            assert(rgb[3*x+2]==clamp((l+516*u+128)>>8));
        }
        pixels_blend(out,rgb,ui,480);
        for(unsigned x=0;x<480;x++) for(unsigned c=0;c<3;c++)
            assert(out[3*x+c]==(((ui[x]>>(16-8*c))&255)*run+rgb[3*x+c]*(255-run)+127)/255);
        pixels_gray(rgb,y,480);
        for(unsigned x=0;x<480;x++)for(unsigned c=0;c<3;c++)
            assert(rgb[3*x+c]==clamp((298*(y[x]-16)+128)>>8));
    }
    puts("PASS: NEON NV21, Y-only equal RGB grayscale and straight alpha match scalar, 122880 pixels, all 256 alpha values");
    return 0;
}
