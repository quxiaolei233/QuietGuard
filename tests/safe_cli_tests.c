/* Portable tests of the actual strict parser. No Windows APIs or file writes. */
#include "safe_cli.h"
#include "qg.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static unsigned long checks;
static unsigned failures;
#define COUNT(a) (sizeof(a)/sizeof((a)[0]))
#define CHECK(x) check((x),#x,__LINE__)
static void check(bool condition,const char *expression,unsigned line){
    checks++;
    if(!condition){fprintf(stderr,"FAIL line %u: %s\n",line,expression);failures++;}
}
static QgSafeOptions defaults(void){
    QgSafeOptions o={0};o.mask=QG_ALL;o.duration_seconds=720;o.interval_seconds=60;o.timeout_seconds=15;return o;
}
static QgSafeOptions parse(int argc,const wchar_t *const *argv,QgSafeParse expected){
    QgSafeOptions out,before;memset(&out,0xa5,sizeof(out));memcpy(&before,&out,sizeof(out));
    QgSafeParse actual=qg_safe_parse(argc,argv,&out);
    if(actual!=expected){fprintf(stderr,"parse mismatch expected %d got %d:",expected,actual);for(int i=0;argv&&i<argc;i++)fprintf(stderr," [%ls]",argv[i]?argv[i]:L"(null)");fputc('\n',stderr);}
    CHECK(actual==expected);
    if(actual!=QG_SAFE_VALID)CHECK(memcmp(&out,&before,sizeof(out))==0);
    else CHECK(qg_safe_options_valid(&out));
    return out;
}
static QgSafeOptions one(const wchar_t *flag,const wchar_t *value,QgSafeParse expected){
    const wchar_t *a[]={L"QuietGuard.exe",L"--sensor-test",flag,value};return parse(4,a,expected);
}
static void baseline_and_bad_api(void){
    const wchar_t *none[]={L"QuietGuard.exe"};QgSafeOptions o;
    (void)parse(1,none,QG_SAFE_ABSENT);(void)parse(0,none,QG_SAFE_INVALID);(void)parse(-1,none,QG_SAFE_INVALID);(void)parse(1,NULL,QG_SAFE_INVALID);
    CHECK(qg_safe_parse(1,none,NULL)==QG_SAFE_INVALID);
    const wchar_t *null_program[]={NULL};(void)parse(1,null_program,QG_SAFE_INVALID);
    const wchar_t *null_arg[]={L"QuietGuard.exe",L"--sensor-test",NULL};(void)parse(3,null_arg,QG_SAFE_INVALID);
    const wchar_t *program_marker[]={L"--sensor-test"};(void)parse(1,program_marker,QG_SAFE_ABSENT);
    const wchar_t *a[]={L"QuietGuard.exe",L"--sensor-test"};o=parse(2,a,QG_SAFE_VALID);
    CHECK(o.mask==QG_ALL);CHECK(o.duration_seconds==720);CHECK(o.interval_seconds==60);CHECK(o.timeout_seconds==15);CHECK(o.output[0]==0);
    CHECK(QG_SAFE_DEFAULT_SECONDS==720u);CHECK(QG_SAFE_DEFAULT_INTERVAL==60u);CHECK(QG_SAFE_DEFAULT_TIMEOUT==15u);CHECK(QG_SAFE_MAX_SAMPLES==181u);
    const wchar_t *absent[]={L"QuietGuard.exe",L"--dry-run"};(void)parse(2,absent,QG_SAFE_ABSENT);
}
static const wchar_t *names[]={L"cpu",L"gpu",L"disk",L"network"};
static void metric_permutations(wchar_t *text,uint32_t mask,unsigned depth){
    if(depth){QgSafeOptions o=one(L"--metrics",text,QG_SAFE_VALID);CHECK(o.mask==mask);}
    for(unsigned i=0;i<4;i++)if(!(mask&(1u<<i))){
        size_t n=wcslen(text);if(n)wcscat(text,L",");wcscat(text,names[i]);metric_permutations(text,mask|(1u<<i),depth+1);text[n]=0;
    }
}
static void metric_tests(void){
    wchar_t list[64]={0};metric_permutations(list,0,0);QgSafeOptions o=one(L"--metrics",L"all",QG_SAFE_VALID);CHECK(o.mask==QG_ALL);
    const wchar_t *bad[]={L"",L"none",L"ALL",L"CPU",L"Cpu",L"net",L"networking",L"cpu,gpu,disk,net",L"cpu,cpu",L"gpu,gpu",L"disk,disk",L"network,network",L"cpu,gpu,cpu",L"all,cpu",L"cpu,all",L"all,all",L",cpu",L"cpu,",L"cpu,,gpu",L",",L",,",L"cpu gpu",L" cpu",L"cpu ",L"cpu, gpu",L"cpu\tgpu",L"cpu\ngpu",L"cpu;gpu",L"cpu|gpu",L"cpu/gpu",L"cpu+gpu",L"c\u0440u",L"\uff43\uff50\uff55",L"cpu\u200b",L"\ufeffcpu",L"--dry-run"};
    for(size_t i=0;i<COUNT(bad);i++)(void)one(L"--metrics",bad[i],QG_SAFE_INVALID);
    for(unsigned i=0;i<4;i++)for(unsigned j=0;j<4;j++){
        (void)swprintf(list,COUNT(list),L"%ls,%ls,%ls",names[i],names[j],names[i]);(void)one(L"--metrics",list,QG_SAFE_INVALID);
    }
}
static void permutations(const unsigned *order,unsigned used,unsigned depth){
    static const wchar_t *flag[]={L"--sensor-test",L"--metrics",L"--duration-seconds",L"--interval-seconds",L"--sample-timeout-seconds",L"--output"};
    static const wchar_t *value[]={NULL,L"network,cpu",L"60",L"20",L"1",L"C:\\reports\\new.json"};
    if(depth==6){
        const wchar_t *a[12];int argc=0;a[argc++]=L"QuietGuard.exe";
        for(unsigned i=0;i<6;i++){unsigned index=order[i];a[argc++]=flag[index];if(value[index])a[argc++]=value[index];}
        QgSafeOptions o=parse(argc,a,QG_SAFE_VALID);CHECK(o.mask==(QG_NET|QG_CPU));CHECK(o.duration_seconds==60);CHECK(o.interval_seconds==20);CHECK(o.timeout_seconds==1);CHECK(!wcscmp(o.output,value[5]));return;
    }
    for(unsigned i=0;i<6;i++)if(!(used&(1u<<i))){unsigned next[6]={0};memcpy(next,order,depth*sizeof(*order));next[depth]=i;permutations(next,used|(1u<<i),depth+1);}
}
static void number_tests(void){
    const wchar_t *flags[]={L"--duration-seconds",L"--interval-seconds",L"--sample-timeout-seconds"};
    const wchar_t *bad[]={L"",L" ",L"+60",L"-60",L" 60",L"60 ",L"60\t",L"\n60",L"60\r",L"6 0",L"60.0",L"60.",L".60",L"6e1",L"0x3c",L"060x",L"60s",L"NaN",L"inf",L"1/2",L"1,0",L"4294967296",L"4294967295",L"999999999999999999999999",L"0000000060",L"\uff16\uff10",L"\u0666\u0660",L"\u221260",L"60\u200b",L"--sensor-test",L"--dry-run"};
    for(size_t f=0;f<COUNT(flags);f++)for(size_t i=0;i<COUNT(bad);i++)(void)one(flags[f],bad[i],QG_SAFE_INVALID);
    (void)one(flags[0],L"180",QG_SAFE_VALID);(void)one(flags[0],L"900",QG_SAFE_VALID);(void)one(flags[0],L"901",QG_SAFE_INVALID);(void)one(flags[0],L"179",QG_SAFE_INVALID);(void)one(flags[0],L"59",QG_SAFE_INVALID);
    (void)one(flags[1],L"5",QG_SAFE_VALID);(void)one(flags[1],L"120",QG_SAFE_VALID);(void)one(flags[1],L"4",QG_SAFE_INVALID);(void)one(flags[1],L"121",QG_SAFE_INVALID);
    for(unsigned n=0;n<=31;n++){wchar_t s[12];(void)swprintf(s,COUNT(s),L"%u",n);(void)one(flags[2],s,n>=1&&n<=30?QG_SAFE_VALID:QG_SAFE_INVALID);}
    QgSafeOptions o=one(flags[0],L"000000720",QG_SAFE_VALID);CHECK(o.duration_seconds==720);
    o=one(flags[1],L"005",QG_SAFE_VALID);CHECK(o.interval_seconds==5);o=one(flags[2],L"01",QG_SAFE_VALID);CHECK(o.timeout_seconds==1);
    for(uint32_t duration=59;duration<=901;duration++)for(uint32_t interval=4;interval<=121;interval++){
        wchar_t d[16],in[16];(void)swprintf(d,COUNT(d),L"%u",duration);(void)swprintf(in,COUNT(in),L"%u",interval);
        const wchar_t *a[]={L"QuietGuard.exe",L"--sensor-test",flags[0],d,flags[1],in};
        bool valid=duration>=60&&duration<=900&&interval>=5&&interval<=120&&duration>=interval*3;
        (void)parse(6,a,valid?QG_SAFE_VALID:QG_SAFE_INVALID);
    }
}
static void flags_and_mixed_modes(void){
    const wchar_t *flags[]={L"--sensor-test",L"--metrics",L"--duration-seconds",L"--interval-seconds",L"--sample-timeout-seconds",L"--output"};
    const wchar_t *values[]={NULL,L"cpu",L"720",L"60",L"15",L"C:\\new.json"};
    for(size_t f=0;f<COUNT(flags);f++){
        const wchar_t *a[7]={L"QuietGuard.exe",L"--sensor-test"};int argc=2;
        a[argc++]=flags[f];if(values[f])a[argc++]=values[f];a[argc++]=flags[f];if(values[f])a[argc++]=values[f];(void)parse(argc,a,QG_SAFE_INVALID);
        if(f){const wchar_t *missing[]={L"QuietGuard.exe",L"--sensor-test",flags[f]};(void)parse(3,missing,QG_SAFE_INVALID);(void)one(flags[f],L"",QG_SAFE_INVALID);}
    }
    const wchar_t *dangerous[]={L"--build-info",L"--self-test",L"--dry-run",L"--background",L"--service",L"--service-dry-run",L"--install-service",L"--remove-service",L"--configure-service",L"--install-service-dry-run",L"--config",L"--shutdown",L"--shutdown-now",L"--poweroff",L"--reboot",L"--restart",L"--install",L"--uninstall",L"--startup",L"--start",L"--stop",L"--cancel",L"--force",L"/s",L"/r",L"/f",L"-s",L"-r",L"-f",L"/shutdown",L"--",L"--help",L"-h",L"",L"positional"};
    for(size_t i=0;i<COUNT(dangerous);i++){
        const wchar_t *before[]={L"QuietGuard.exe",dangerous[i],L"--sensor-test"};(void)parse(3,before,QG_SAFE_INVALID);
        const wchar_t *after[]={L"QuietGuard.exe",L"--sensor-test",dangerous[i]};(void)parse(3,after,QG_SAFE_INVALID);
        const wchar_t *config_before[]={L"QuietGuard.exe",dangerous[i],L"C:\\production.ini",L"--sensor-test"};(void)parse(4,config_before,QG_SAFE_INVALID);
        const wchar_t *config_after[]={L"QuietGuard.exe",L"--sensor-test",dangerous[i],L"C:\\production.ini"};(void)parse(4,config_after,QG_SAFE_INVALID);
        const wchar_t *with_output[]={L"QuietGuard.exe",L"--sensor-test",L"--output",L"C:\\new.json",dangerous[i]};(void)parse(5,with_output,QG_SAFE_INVALID);
    }
    const wchar_t *bad[]={L"--SENSOR-TEST",L"--Sensor-Test",L"--sensor-test=1",L"--sensor-test ",L"--sensor-test\t",L"\u2014sensor-test",L"\uff0d\uff0dsensor-test",L"--sensor\u2010test",L"--sensor-test\u200b",L"--metrics=cpu",L"--duration-seconds=60",L"--interval_seconds",L"--sample-timeout",L"--output=C:\\a.json",L"--Metrics",L"--\u043cetrics"};
    for(size_t i=0;i<COUNT(bad);i++){const wchar_t *a[]={L"QuietGuard.exe",L"--sensor-test",bad[i]};(void)parse(3,a,QG_SAFE_INVALID);}
    unsigned order[6]={0};permutations(order,0,0);
}
static void paths(void){
    const wchar_t *good[]={L"C:\\report.json",L"z:/report.json",L"C:\\new reports\\new report.json",L"C:/folder\\sub/report.json",L"C:\\报告\\测量.json",L"C:\\.hidden",L"C:\\conifer.json",L"C:\\CONSOLE.json",L"C:\\COM0.json",L"C:\\COM10.json",L"C:\\LPT0.json",L"C:\\LPT10.json",L"C:\\NULx.json",L"C:\\AUXiliary.json",L"C:\\a..b\\new.json"};
    for(size_t i=0;i<COUNT(good);i++){QgSafeOptions o=one(L"--output",good[i],QG_SAFE_VALID);CHECK(!wcscmp(o.output,good[i]));}
    const wchar_t *bad[]={L"",L"-",L"report.json",L".\\report.json",L"..\\report.json",L"C:report.json",L"C:",L"C:\\",L"C:/",L"1:\\a",L"Ç:\\a",L"\\report.json",L"/report.json",L"\\\\server\\share\\report.json",L"//server/share/report.json",L"\\\\?\\C:\\report.json",L"\\\\.\\C:\\report.json",L"\\??\\C:\\report.json",L"\\Device\\HarddiskVolume1\\report.json",L"file:///C:/report.json",L"C:\\\\report.json",L"C://report.json",L"C:\\x\\",L"C:\\x/",L"C:\\x\\.\\report.json",L"C:\\x\\..\\report.json",L"C:\\report.json.",L"C:\\report.json ",L"C:\\x.\\report.json",L"C:\\x \\report.json",L"C:\\x:ads",L"C:\\x::$DATA",L"C:\\x\\report.json:stream",L"C:\\*",L"C:\\?",L"C:\\<a>",L"C:\\a|b",L"C:\\a\"b",L"C:\\a\nb",L"C:\\a\tb",L"C:\\a\rb"};
    for(size_t i=0;i<COUNT(bad);i++)(void)one(L"--output",bad[i],QG_SAFE_INVALID);
    const wchar_t *reserved[]={L"CON",L"PRN",L"AUX",L"NUL",L"CONIN$",L"CONOUT$",L"COM1",L"COM2",L"COM3",L"COM4",L"COM5",L"COM6",L"COM7",L"COM8",L"COM9",L"LPT1",L"LPT2",L"LPT3",L"LPT4",L"LPT5",L"LPT6",L"LPT7",L"LPT8",L"LPT9",L"COM\u00b9",L"COM\u00b2",L"COM\u00b3",L"LPT\u00b9",L"LPT\u00b2",L"LPT\u00b3"};
    const wchar_t *suffix[]={L"",L".json",L".tar.json",L" .json",L"   .json",L". ",L"...",L"\\child.json"};
    for(size_t i=0;i<COUNT(reserved);i++)for(size_t j=0;j<COUNT(suffix);j++){
        wchar_t path[100],lower[32];size_t n=wcslen(reserved[i]);for(size_t k=0;k<=n;k++){wchar_t c=reserved[i][k];lower[k]=c>=L'A'&&c<=L'Z'?c+(L'a'-L'A'):c;}
        (void)swprintf(path,COUNT(path),L"C:\\%ls%ls",reserved[i],suffix[j]);(void)one(L"--output",path,QG_SAFE_INVALID);
        (void)swprintf(path,COUNT(path),L"C:\\reports\\%ls%ls",lower,suffix[j]);(void)one(L"--output",path,QG_SAFE_INVALID);
    }
    for(wchar_t control=1;control<32;control++){wchar_t path[]=L"C:\\aXb.json";path[4]=control;(void)one(L"--output",path,QG_SAFE_INVALID);}
    wchar_t long_path[QG_SAFE_OUTPUT_CAP+2];long_path[0]=L'C';long_path[1]=L':';long_path[2]=L'\\';
    for(size_t i=3;i<COUNT(long_path);i++)long_path[i]=L'x';
    /* Keep each component under 255 characters; test the total CLI buffer limit. */
    long_path[130]=L'\\';long_path[QG_SAFE_OUTPUT_CAP-1]=0;(void)one(L"--output",long_path,QG_SAFE_VALID);
    long_path[QG_SAFE_OUTPUT_CAP-1]=L'x';long_path[QG_SAFE_OUTPUT_CAP]=0;(void)one(L"--output",long_path,QG_SAFE_INVALID);
    long_path[QG_SAFE_OUTPUT_CAP]=L'x';long_path[QG_SAFE_OUTPUT_CAP+1]=0;(void)one(L"--output",long_path,QG_SAFE_INVALID);
}
static void direct_options(void){
    QgSafeOptions o=defaults();CHECK(qg_safe_options_valid(&o));CHECK(!qg_safe_options_valid(NULL));
    for(uint32_t mask=0;mask<64;mask++){o=defaults();o.mask=mask;CHECK(qg_safe_options_valid(&o)==(mask>=1&&mask<=QG_ALL));}
    const uint32_t badbits[]={UINT32_MAX,UINT32_C(0x80000000),UINT32_C(0x80000001),UINT32_C(0xffff000f)};
    for(size_t i=0;i<COUNT(badbits);i++){o=defaults();o.mask=badbits[i];CHECK(!qg_safe_options_valid(&o));}
    const uint32_t d[]={0,1,59,60,61,900,901,UINT32_MAX};
    const uint32_t in[]={0,1,4,5,20,21,120,121,UINT32_MAX};
    for(size_t i=0;i<COUNT(d);i++)for(size_t j=0;j<COUNT(in);j++){
        o=defaults();o.duration_seconds=d[i];o.interval_seconds=in[j];bool valid=d[i]>=60&&d[i]<=900&&in[j]>=5&&in[j]<=120&&d[i]>=in[j]*3;
        CHECK(qg_safe_options_valid(&o)==valid);
    }
    for(uint32_t t=0;t<=31;t++){o=defaults();o.timeout_seconds=t;CHECK(qg_safe_options_valid(&o)==(t>=1&&t<=30));}
    o=defaults();o.timeout_seconds=UINT32_MAX;CHECK(!qg_safe_options_valid(&o));
    o=defaults();for(size_t i=0;i<COUNT(o.output);i++)o.output[i]=L'x';CHECK(!qg_safe_options_valid(&o));
    o.output[0]=L'C';o.output[1]=L':';o.output[2]=L'\\';CHECK(!qg_safe_options_valid(&o));
    o=defaults();wcscpy(o.output,L"relative.json");CHECK(!qg_safe_options_valid(&o));
    o=defaults();wcscpy(o.output,L"C:\\NUL.json");CHECK(!qg_safe_options_valid(&o));
}
int qg_safe_cli_self_test(void){
    checks=0;failures=0;
    baseline_and_bad_api();metric_tests();number_tests();flags_and_mixed_modes();paths();direct_options();
    printf("safe CLI: %lu checks, %u failures (portable parser only; no native Windows execution)\n",checks,failures);
    return failures?EXIT_FAILURE:EXIT_SUCCESS;
}

#ifdef QG_SAFE_CLI_TEST_MAIN
int main(void){return qg_safe_cli_self_test();}
#endif
