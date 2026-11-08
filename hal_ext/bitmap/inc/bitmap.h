
#ifndef _BITMAP_H
#define _BITMAP_H
#ifdef __cplusplus
extern "C" {
#endif
#include<stdlib.h>
#include<string.h>
typedef char Bit;

typedef enum{
    BottomToTop=0,
    TopToBottom
}ModeRectTpye;

typedef struct Bitmap{
    Bit*    data;
    int     width;
    int     height;
    Bit     (*at)(struct Bitmap* bitmap,int row,int col);//获取某一行列的bit值
    int     (*set)(struct Bitmap* bitmap,int row,int col,Bit val);//设置某一行列bit的值
    int     (*reverse)(struct Bitmap* bitmap,int row,int col);//反转对应行列的bit位
    int     (*copyTo)(struct Bitmap*dst,struct Bitmap* src);//从src深拷贝到dst
    int     (*byteReverse)(struct Bitmap* bitmap);
    int     (*getBytes)(struct Bitmap* bitmap);//获取bitmap data的字节数
    int     (*clear)(struct Bitmap* bitmap);//整个bitmap全部重置为0
    int     (*compare)(struct Bitmap* bitmap,struct Bitmap* cmp);//对比内容是否相等，长宽，数据内容，空指针问题返回-1，不同返回1，相同返回0
    char*   (*toModRect)(struct Bitmap* bitmap,int* pWidth,int* pHeight);
}Bitmap;

Bitmap* NewBitmap(int width,int height);//根据位图的长宽创建bitmap,注意是位图
Bitmap* NewBitmapFromMod(char* dataRect,int wRect,int hRect,int type);
void DeleteBitmap(Bitmap* instance);

extern void* (*bitmapMalloc)(size_t size);
extern void  (*bitmapFree)(void*p);

#ifdef __cplusplus
}
#endif

#endif