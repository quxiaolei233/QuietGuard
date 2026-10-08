#ifndef QG_UI_LAYOUT_H
#define QG_UI_LAYOUT_H
/* Pure integer-DIP geometry. No Win32, allocation, timers, or monitoring state. */
typedef struct { int x,y,w,h; } QgUiRect;
typedef enum { QG_PAGE_OVERVIEW, QG_PAGE_RULES, QG_PAGE_STARTUP } QgUiPage;
typedef struct {
    QgUiRect sidebar,header,footer,viewport,scrollbar,nav[3],actions[4],notice;
    QgUiRect cards[5],check[4],label[4],edit[4],live[4];
    QgUiRect field_label[3],field[3],mode,veto,status_title,status_detail;
    int card_count,columns,content_height,scroll_max,scroll;
} QgUiLayout;
void qg_ui_layout(int width,int height,QgUiPage page,int scroll,QgUiLayout *out);
#endif
