#include "ui_layout.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned checks;
static void expect(int condition){checks++;assert(condition);}
static void inside(QgUiRect child,QgUiRect parent){
    expect(child.w>=0&&child.h>=0);
    expect(child.x>=parent.x&&child.y>=parent.y);
    expect(child.x+child.w<=parent.x+parent.w);
    expect(child.y+child.h<=parent.y+parent.h);
}
static void validate(int width,int height,QgUiPage page){
    QgUiLayout u,again;qg_ui_layout(width,height,page,0,&u);qg_ui_layout(width,height,page,0,&again);
    expect(memcmp(&u,&again,sizeof(u))==0);
    QgUiRect window={0,0,width,height};inside(u.sidebar,window);inside(u.header,window);inside(u.footer,window);inside(u.viewport,window);
    expect(u.viewport.x==u.sidebar.w);expect(u.viewport.y==u.header.h);expect(u.viewport.y+u.viewport.h==u.footer.y);
    for(int i=0;i<4;i++){inside(u.actions[i],u.footer);expect(u.actions[i].w>=90);if(i)expect(u.actions[i].x>=u.actions[i-1].x+u.actions[i-1].w+8);}
    inside(u.notice,u.footer);expect(u.notice.y+u.notice.h<=u.actions[0].y);
    QgUiRect content={0,0,u.viewport.w,u.content_height};
    for(int i=0;i<u.card_count;i++)inside(u.cards[i],content);
    if(page==QG_PAGE_OVERVIEW){
        inside(u.status_title,u.cards[0]);inside(u.status_detail,u.cards[0]);expect(u.status_title.y>=u.cards[0].y+52);expect(u.status_title.y+u.status_title.h<=u.status_detail.y);
        for(int i=0;i<4;i++){inside(u.live[i],u.cards[i+1]);expect(u.live[i].y>=u.cards[i+1].y+52);}
    }else if(page==QG_PAGE_RULES){
        for(int i=0;i<4;i++){
            inside(u.check[i],u.cards[0]);inside(u.label[i],u.cards[0]);inside(u.edit[i],u.cards[0]);inside(u.live[i],u.cards[0]);
            expect(u.check[i].x+u.check[i].w<=u.label[i].x);expect(u.label[i].x+u.label[i].w<=u.edit[i].x);expect(u.edit[i].w>=82);expect(u.label[i].w>=120);
            expect(u.live[i].y>=u.edit[i].y+u.edit[i].h);
        }
        for(int i=0;i<3;i++){inside(u.field_label[i],u.cards[1]);inside(u.field[i],u.cards[1]);expect(u.field_label[i].w>=110);expect(u.field_label[i].x+u.field_label[i].w<=u.field[i].x);}
    }else{
        QgUiRect closed_combo=u.mode;closed_combo.h=30;inside(closed_combo,u.cards[0]);inside(u.veto,u.cards[1]);expect(u.mode.w>=360);
    }
    expect((u.scrollbar.w>0)==(u.scroll_max>0));if(u.scroll_max>0)inside(u.scrollbar,window);
    qg_ui_layout(width,height,page,-42,&again);expect(again.scroll==0);
    qg_ui_layout(width,height,page,100000,&again);expect(again.scroll==again.scroll_max);
    expect(again.scroll_max+again.viewport.h>=again.content_height);
}
int main(void){
    /* Effective DIP sizes include 1366x768 at 100/150/200%, frame/taskbar already removed. */
    const int sizes[][2]={{1060,676},{1040,650},{892,442},{650,342},{624,300},{800,480},{1280,720},{1920,1080}};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++)for(int page=0;page<3;page++)validate(sizes[i][0],sizes[i][1],(QgUiPage)page);
    /* Sweep around all column/sidebar transitions and very short usable windows. */
    for(int width=624;width<=1600;width+=7)for(int height=300;height<=900;height+=31)for(int page=0;page<3;page++)validate(width,height,(QgUiPage)page);
    for(int page=0;page<3;page++){
        QgUiLayout normal;qg_ui_layout(1060,676,(QgUiPage)page,0,&normal);expect(normal.scroll_max==0);
        QgUiLayout small;qg_ui_layout(650,342,(QgUiPage)page,0,&small);expect(small.scroll_max>0);expect(small.actions[2].y+small.actions[2].h<=342);
    }
    printf("UI layout: %u assertions passed (portable geometry only)\n",checks);return 0;
}
