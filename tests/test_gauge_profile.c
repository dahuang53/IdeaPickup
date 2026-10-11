#include "memo_gauge_profile.h"
#include "power_state.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t ram[4], staged[6];
    uint16_t full, design, device;
    unsigned sec, key, access_key, calls, fail_at, commits, controls, delays;
    bool cfg, selected, stale_standard, bad_echo, bad_commit, fail_after_commit;
} fake_t;
static uint16_t be(const uint8_t *p) { return (uint16_t)p[0] * 256U + p[1]; }
static bool fail(fake_t *f) { return ++f->calls == f->fail_at; }
static void publish(fake_t *f)
{
    f->full = be(f->ram); f->design = be(f->ram + 2);
}
static int read_reg(void *ctx, uint8_t reg, uint8_t *data, size_t length)
{
    fake_t *f = ctx;
    if (fail(f)) return -1;
    if (reg == 0x3e) {
        assert(length == 6 && f->selected);
        data[0] = f->bad_echo ? 0 : 0x9d; data[1] = 0x92;
        memcpy(data + 2, f->ram, 4);
        return 0;
    }
    assert(length == 2);
    uint16_t value = 0;
    switch (reg) {
    case 0x12: value=f->full; break;
    case 0x3c: value=f->design; break;
    case 0x3a: value=(uint16_t)((f->sec << 1) | (f->cfg ? 0x400 : 0)); break;
    case 0x40: value=f->device; break;
    default: assert(!"unexpected register read");
    }
    data[0]=(uint8_t)value;data[1]=(uint8_t)(value >> 8);
    return 0;
}
static int write_reg(void *ctx, uint8_t reg, const uint8_t *data, size_t length)
{
    fake_t *f = ctx;
    if (fail(f)) return -1;
    if (reg == 0) {
        assert(length == 2);f->controls++;
        uint16_t command = (uint16_t)data[0] | (uint16_t)data[1] << 8;
        switch (command) {
        case 0x0001: break;
        case 0x0414: f->key=1;break;
        case 0x3672: if (f->key) f->sec=2;break;
        case 0xffff: if (++f->access_key >= 2 && f->sec != 3) f->sec=1;break;
        case 0x0090: if (f->sec != 1) return -1;f->cfg=true;break;
        case 0x0091: f->cfg=false;if (!f->stale_standard) publish(f);f->sec=3;f->key=f->access_key=0;break;
        case 0x0092: f->cfg=false;if (!f->stale_standard) publish(f);f->sec=3;f->key=f->access_key=0;break;
        case 0x0030: f->sec=3;break;
        default: assert(!"OTP, reset, ROM or unsupported control command");
        }
        return 0;
    }
    if (reg == 0x3e) {
        assert(data[0]==0x9d && data[1]==0x92);
        assert(length==2 || length==6);
        f->selected=true;
        if (length==6) { assert(f->cfg && f->sec==1);memcpy(f->staged,data,6); }
        return 0;
    }
    assert(reg==0x60 && length==2 && data[1]==8 && f->cfg && f->sec==1);
    uint8_t sum=0;
    for (unsigned i=0;i<6;++i)sum+=f->staged[i];
    assert(data[0]==(uint8_t)(255U-sum));
    f->commits++;
    if (!f->bad_commit)memcpy(f->ram,f->staged+2,4);
    if (f->fail_after_commit) {f->fail_after_commit=false;return -1;}
    return 0;
}
static void delay(void *ctx, unsigned ms) { ((fake_t *)ctx)->delays+=ms; }
static fake_t initial(void)
{
    fake_t f={.ram={0x0b,0x38,0x0b,0xb8},.sec=3,.device=0x220};
    publish(&f);return f;
}
static int run(fake_t *f, memo_gauge_profile_result_t *r)
{
    memo_gauge_bus_t b={f,read_reg,write_reg,delay};
    return memo_gauge_configure(&b,r);
}
int main(void)
{
    memo_gauge_profile_result_t r;
    fake_t f=initial();
    assert(run(&f,&r)==0 && r.changed && r.reinitialized);
    assert(r.previous_design_mah==3000 && r.previous_full_mah==2872);
    assert(f.design==700 && f.full==700 && f.sec==3 && !f.cfg && f.commits==1);
    unsigned normal_calls=f.calls;
    f.ram[0]=2;f.ram[1]=100;publish(&f); /* learned 612 mAh */
    unsigned writes=f.commits, controls=f.controls;
    assert(run(&f,&r)==0 && !r.changed && f.full==612);
    assert(f.commits==writes && f.controls==controls);
    /* An MCU reset while the gauge remains in CFGUPDATE must finish the
     * interrupted transaction, even if standard capacity words already match. */
    f.cfg=true;f.sec=1;
    assert(run(&f,&r)==0 && r.changed && !f.cfg && f.sec==3);
    for(unsigned n=1;n<=normal_calls;++n) {
        f=initial();f.fail_at=n;
        assert(run(&f,&r)!=0 && !r.changed);
        assert(!f.cfg);
        if(r.rollback_verified)assert(be(f.ram)==2872 && be(f.ram+2)==3000);
    }
    /* Matching RAM alone is insufficient: reject a stale ordinary FCC. */
    f=initial();f.stale_standard=true;
    assert(run(&f,&r)!=0 && r.reinitialized && !r.changed && r.rollback_verified);
    assert(f.full==2872 && !f.cfg && f.sec==3);
    f=initial();f.fail_after_commit=true;
    assert(run(&f,&r)!=0 && r.rollback_attempted && r.rollback_verified && !f.cfg);
    assert(be(f.ram)==2872 && be(f.ram+2)==3000);
    f=initial();f.device=0x1234;
    assert(run(&f,&r)!=0 && f.commits==0);
    f=initial();f.bad_echo=true;
    assert(run(&f,&r)!=0 && f.commits==0 && !f.cfg);
    f=initial();f.bad_commit=true;
    assert(run(&f,&r)!=0 && !r.changed && !f.cfg);
    assert(!memo_gauge_capacity_valid(3000,2872));
    assert(!memo_gauge_capacity_valid(700,2872));
    assert(!memo_gauge_capacity_valid(700,0));
    assert(memo_gauge_capacity_valid(700,650));
    assert(memo_gauge_protection_soc(80,true,3900)==80);
    assert(memo_gauge_protection_soc(80,true,3450)==5);
    assert(memo_gauge_protection_soc(80,true,3300)==2);
    assert(memo_gauge_protection_soc(1,true,3800)==1);
    assert(memo_gauge_protection_soc(1,false,3800)==100);
    assert(memo_gauge_protection_soc(90,false,3400)==5);
    assert(memo_gauge_protection_soc(90,false,3200)==2);
    memo_battery_guard_t guard={0};
    uint16_t low=memo_gauge_protection_soc(80,true,3400);
    assert(!memo_battery_shutdown_due(&guard,false,false,true,low,1));
    assert(memo_battery_shutdown_due(&guard,false,false,true,low,10001));
    assert(!memo_battery_shutdown_due(&guard,true,false,true,low,10002));
    assert(!memo_battery_recovery_ready(true,false,true,low));
    assert(memo_battery_recovery_ready(true,true,true,low));
    puts("PASS: gauge RAM protocol, capacity preservation, all transaction failures, rollback, no OTP, voltage reserve");
}
