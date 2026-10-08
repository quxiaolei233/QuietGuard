#include "ui_layout.h"
#include <string.h>
static int maximum(int a,int b){return a>b?a:b;}
static QgUiRect rect(int x,int y,int w,int h){QgUiRect r={x,y,maximum(0,w),maximum(0,h)};return r;}
static void content(int width,QgUiPage page,QgUiLayout *o){
    int gap=12, pad=12, w=maximum(0,width-pad*2);
    o->columns=w>=640?2:1;
    if(page==QG_PAGE_OVERVIEW){
        o->card_count=5;o->cards[0]=rect(pad,12,w,168);
        o->status_title=rect(pad+18,74,w-36,54);o->status_detail=rect(pad+18,130,w-36,42);
        int cw=o->columns==2?(w-gap)/2:w;
        for(int i=0;i<4;i++){
            int x=pad+(i%o->columns)*(cw+gap),y=192+(i/o->columns)*116;
            o->cards[i+1]=rect(x,y,cw,104);o->live[i]=rect(x+18,y+58,cw-36,32);
        }
        o->content_height=192+((4+o->columns-1)/o->columns)*116+52;
    }else if(page==QG_PAGE_RULES){
        o->card_count=2;
        int cw=o->columns==2?(w-gap)*57/100:w;
        o->cards[0]=rect(pad,12,cw,370);
        for(int i=0;i<4;i++){
            int y=74+i*72;
            o->check[i]=rect(pad+16,y,82,30);
            o->label[i]=rect(pad+104,y+2,cw-216,28);
            o->edit[i]=rect(pad+cw-100,y,82,30);
            o->live[i]=rect(pad+18,y+34,cw-36,24);
        }
        int x=o->columns==2?pad+cw+gap:pad,y=o->columns==2?12:394,tw=o->columns==2?w-cw-gap:w;
        o->cards[1]=rect(x,y,tw,o->columns==2?370:288);
        for(int i=0;i<3;i++){
            o->field_label[i]=rect(x+18,y+62+i*66,tw-146,28);
            o->field[i]=rect(x+tw-110,y+58+i*66,92,34);
        }
        o->content_height=y+o->cards[1].h+74;
    }else{
        o->card_count=2;
        /* Long Chinese safety explanations stay in generously sized full-width cards. */
        o->columns=1;o->cards[0]=rect(pad,12,w,208);o->mode=rect(pad+18,78,w-36,168);
        o->cards[1]=rect(pad,232,w,216);o->veto=rect(pad+18,292,w-36,46);
        o->content_height=462;
    }
}
void qg_ui_layout(int width,int height,QgUiPage page,int scroll,QgUiLayout *o){
    memset(o,0,sizeof(*o));width=maximum(width,0);height=maximum(height,0);
    int sidebar=width>=840?188:144;if(sidebar>width/3)sidebar=width/3;
    int header=64,footer=112;if(height<240){header=48;footer=96;}
    if(header+footer>height)header=maximum(0,height-footer);
    footer=height<header+footer?height-header:footer;
    o->sidebar=rect(0,0,sidebar,height);o->header=rect(sidebar,0,width-sidebar,header);
    o->footer=rect(sidebar,height-footer,width-sidebar,footer);
    int vw=width-sidebar,vh=maximum(0,height-header-footer);
    o->viewport=rect(sidebar,header,vw,vh);
    for(int i=0;i<3;i++)o->nav[i]=rect(10,92+i*52,sidebar-20,44);
    int aw=vw-32,gap=8,bw=maximum(0,(aw-gap*3)/4);if(bw>132)bw=132;
    for(int i=0;i<4;i++)o->actions[i]=rect(sidebar+16+i*(bw+gap),height-52,bw,38);
    o->notice=rect(sidebar+16,height-footer+10,vw-32,44);
    content(vw,page,o);
    if(o->content_height>vh){
        o->viewport.w=maximum(0,vw-16);o->scrollbar=rect(width-16,header,16,vh);
        content(o->viewport.w,page,o);
    }
    o->scroll_max=maximum(0,o->content_height-vh);
    o->scroll=scroll<0?0:(scroll>o->scroll_max?o->scroll_max:scroll);
}
