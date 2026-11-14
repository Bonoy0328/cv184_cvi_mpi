#include "ir_exposure.h"
#include "pvaTypeDef.h"
#include <assert.h>
#include <stdio.h>
static int target=56,gets,sets,get_fail,set_fail,stable=1;
static uint64_t sequence;
static IrExposureSample sample;
static int get(IrExposureReading *r)
{
    ++gets;if(get_fail)return -1;
    *r=(IrExposureReading){.target=target,.time_us=1200,.again=1024,.dgain=1024,.isp_gain=1024,
        .luma=80,.stable=stable,.sequence=sequence};return 0;
}
static int set(int v){++sets;if(set_fail)return -1;assert(v>=1&&v<=255);target=v;return 0;}
static const IrExposureBackend backend={get,set};
static int frame(IrExposure *s,int status,uint64_t ms)
{
    sample=(IrExposureSample){.sequence=++sequence,.captured_ms=ms,.target=target,.stable=stable,.valid=1};
    return ir_exposure_feedback(s,1,status,ms,&sample);
}
int main(void)
{
    IrExposure s;ir_exposure_init(&s,&backend,-1);
    for(int status=0;status<=22;status++)assert(!ir_exposure_feedback(&s,0,status,1000+status,NULL));
    assert(!gets&&!sets);
    assert(!frame(&s,STATUS_TOO_BRIGHT,1100));
    IrExposureSample old=sample;
    assert(frame(&s,STATUS_TOO_BRIGHT,1140)&&target==52&&sets==1);
    /* Old images cannot drive another exposure change. */
    assert(ir_exposure_feedback(&s,1,STATUS_TOO_BRIGHT,2000,&old)&&sets==1);
    /* First new, SDK-accepted sample is usable, even with global AE unstable. */
    stable=0;assert(!frame(&s,STATUS_OK,1180));
    IrExposureProof proof=ir_exposure_proof(&s,&sample);
    assert(ir_exposure_success(&s,&proof,1400)==52); /* later async completion */
    assert(ir_exposure_feedback(&s,1,STATUS_TOO_BRIGHT,1500,&sample)&&sets==1); /* duplicate */
    stable=1;
    for(int status=0;status<=22;status++)if(status!=STATUS_TOO_BRIGHT&&status!=STATUS_TOO_DARK)
        assert(!frame(&s,status,1700+status));
    assert(sets==1);
    assert(!frame(&s,STATUS_TOO_DARK,1800));
    assert(frame(&s,STATUS_TOO_DARK,1840)&&target==54&&sets==2); /* reverse: half step */
    assert(ir_exposure_success(&s,&proof,1850)<0); /* old exposure generation */
    int n=gets;assert(!ir_exposure_feedback(&s,0,STATUS_TOO_DARK,1900,NULL));
    assert(!ir_exposure_feedback(&s,-1,STATUS_TOO_BRIGHT,2900,NULL));assert(gets==n&&sets==2);
    ir_exposure_reset(&s);
    assert(ir_exposure_success(&s,&proof,2950)<0); /* departed session */
    assert(frame(&s,STATUS_TOO_BRIGHT,3000)&&target==52&&sets==3);
    proof=ir_exposure_proof(&s,&sample);assert(ir_exposure_success(&s,&proof,3001)<0); /* image was target 54 */
    stable=0;assert(!frame(&s,STATUS_OK,3040));
    proof=ir_exposure_proof(&s,&sample);assert(ir_exposure_success(&s,&proof,3100)==52);
    target=70;assert(ir_exposure_success(&s,&proof,3140)<0); /* external target change */
    ir_exposure_reset(&s);n=sets;
    /* Good quality never gets disrupted just to restore a historical target. */
    assert(!frame(&s,STATUS_OK,4000)&&target==70&&sets==n);
    proof=ir_exposure_proof(&s,&sample);assert(ir_exposure_success(&s,&proof,4200)==70);
    ir_exposure_reset(&s);target=56;
    assert(frame(&s,STATUS_TOO_DARK,4300)&&target==70);
    sequence+=7;assert(!frame(&s,STATUS_OK,4620)); /* latest-frame drops are harmless */
    IrExposure restarted;target=56;ir_exposure_init(&restarted,&backend,52);
    assert(frame(&restarted,STATUS_TOO_BRIGHT,5100)&&target==52);
    ir_exposure_init(&s,&backend,-1);target=1;
    frame(&s,STATUS_TOO_BRIGHT,6000);n=sets;
    frame(&s,STATUS_TOO_BRIGHT,6040);assert(target==1&&sets==n);
    ir_exposure_init(&s,&backend,-1);target=255;
    frame(&s,STATUS_TOO_DARK,7000);n=sets;
    frame(&s,STATUS_TOO_DARK,7040);assert(target==255&&sets==n);
    ir_exposure_init(&s,&backend,-1);target=56;set_fail=1;
    frame(&s,STATUS_TOO_BRIGHT,8000);
    assert(frame(&s,STATUS_TOO_BRIGHT,8040)&&target==56);n=sets;
    assert(frame(&s,STATUS_TOO_BRIGHT,8500)&&sets==n);set_fail=0;
    ir_exposure_init(&s,&backend,40);get_fail=1;n=sets;
    assert(frame(&s,STATUS_TOO_BRIGHT,10000)&&sets==n);
    get_fail=0;assert(frame(&s,STATUS_TOO_BRIGHT,11000)&&target==40);
    puts("PASS IR AE: no fixed wait, unstable-ISP good samples, old/duplicate feedback, async save proof, generation/reset rejection, quality-aware recall, bounds and failures");
}
