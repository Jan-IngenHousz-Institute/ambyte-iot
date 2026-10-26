/* Bit-exact and decimal-equivalent widening of the actual wire formatter. */
#include "../../components/sensor_inventory/sensor_json.c"
#include <assert.h>

typedef struct { char text[64]; size_t size; } output_t;
static bool collect(void *context, const void *bytes, size_t count)
{
    output_t *out=context;
    assert(out->size+count<sizeof out->text);
    memcpy(out->text+out->size,bytes,count);out->size+=count;
    return true;
}
static unsigned checked;
static void check(uint32_t bits)
{
    unsigned char raw_bits[4];
    for(unsigned i=0;i<4;++i) raw_bits[i]=(unsigned char)(bits>>(8*i));
    output_t out={0};writer_t writer={.sink=collect,.context=&out,.ok=true};
    floating(&writer,raw_bits);assert(writer.ok);
    if((bits&0x7f800000u)==0x7f800000u) assert(!strcmp(out.text,"null"));
    else {
        float native;memcpy(&native,&bits,sizeof native);
        double expected=(double)native,actual=binary32_to_binary64(bits);
        uint64_t expected_bits,actual_bits;
        memcpy(&expected_bits,&expected,sizeof expected_bits);memcpy(&actual_bits,&actual,sizeof actual_bits);
        assert(expected_bits==actual_bits);
        char text[64];snprintf(text,sizeof text,"%.9g",expected);
        assert(!strcmp(text,out.text));
    }
    ++checked;
}
int main(void)
{
    const uint32_t edges[]={0,1,0x007fffff,0x00800000,0x3f000000,0x3f800000,0x7f7fffff,0x7f800000,0x7fffffff};
    for(unsigned i=0;i<sizeof edges/sizeof edges[0];++i) {check(edges[i]);check(edges[i]|0x80000000u);}
    for(unsigned exponent=0;exponent<255;++exponent) {
        const uint32_t fractions[]={0,1,0x003fffff,0x007fffff};
        for(unsigned i=0;i<4;++i) {
            check((exponent<<23)|fractions[i]);check(0x80000000u|(exponent<<23)|fractions[i]);
        }
    }
    for(unsigned bit=0;bit<23;++bit) {check(1u<<bit);check((1u<<bit)-1);check(0x80000000u|(1u<<bit));}
    uint32_t state=0x164a4u;
    for(unsigned i=0;i<16384;++i) {state^=state<<13;state^=state>>17;state^=state<<5;check(state);}
    printf("FLOAT_WIDEN_OK patterns=%u exact_bits=1 identical_json=1\n",checked);
}
