#include"bitmap.h"
#define _DEF_NOF_BITS (sizeof(Bit)*8)
void* (*bitmapMalloc)(size_t size)=malloc;
void  (*bitmapFree)(void*p)=free;
static Bit     at(Bitmap* bitmap,int row,int col);
static int     set(Bitmap* bitmap,int row,int col,Bit val);
static int     reverse(Bitmap* bitmap,int row,int col);//反转对应行列的bit位
static int     copyTo(Bitmap*dst,Bitmap* src);
static int     byteReverse(Bitmap* bitmap);
static int     getBytes(Bitmap*bitmap);
static int     clear(Bitmap*bitmap);
static int     compare(Bitmap* bitmap,Bitmap* cmp);
static char*   toModRect(Bitmap* bitmap,int* pWidth,int* pHeight);
Bitmap* NewBitmap(int width,int height){
    if(width<=0||height<=0) return NULL;
    int nBits = width*height;
    int nBytes=0;
    if(nBits%_DEF_NOF_BITS) nBytes = nBits/_DEF_NOF_BITS+1;
    else nBytes=nBits/_DEF_NOF_BITS;
    
    Bitmap* bitmap=bitmapMalloc(sizeof(Bitmap));
    if(NULL==bitmap) return NULL;
    memset(bitmap,0,sizeof(Bitmap));

    Bit*data=bitmapMalloc(sizeof(Bit)*nBytes);
    if(NULL==data){
        bitmapFree(bitmap);
        return NULL;
    } 
    memset(data,0,sizeof(Bit)*nBytes);
    
    Bitmap example = {
        .at=at,
        .byteReverse=byteReverse,
        .clear=clear,
        .compare=compare,
        .copyTo=copyTo,
        .height=height,
        .width=width,
        .reverse=reverse,
        .set=set,
        .data=data,
        .getBytes=getBytes,
        .byteReverse=byteReverse,
        .toModRect=toModRect
    };
    *bitmap=example;
    return bitmap;
}//根据位图的长宽创建bitmap,注意是位图

Bitmap* NewBitmapFromMod(char* dataRect,int wRect,int hRect,int type){
    if(NULL==dataRect) return NULL;
    int wBitmap = wRect;
    int hBitmap = hRect*_DEF_NOF_BITS;
    Bitmap* bitmap = NewBitmap(wBitmap,hBitmap);
    int rectCol;
    int rectRow;
    int rectCur;
    int bitCur;
    for(int col = 0;col<wBitmap;col++){
        rectCol=col;
        for(int row = 0;row<hBitmap;row++){
            rectRow = row/_DEF_NOF_BITS;
            rectCur = rectRow*wRect+rectCol;
            if(BottomToTop==type){
                bitCur=row%_DEF_NOF_BITS;
            }else{
                bitCur=(_DEF_NOF_BITS-1)-row%_DEF_NOF_BITS;
            }
            Bit val = ((1<<bitCur)&dataRect[rectCur])!=0?(Bit)1:(Bit)0;
            bitmap->set(bitmap,row,col,val);
        }
    }
    return bitmap;
}

void DeleteBitmap(Bitmap* instance){
    if(NULL==instance) return;
    bitmapFree(instance->data);
    bitmapFree(instance);
}

static Bit at(Bitmap* bitmap,int row,int col){
    if(NULL==bitmap||row<0||row>=bitmap->height||col<0||col>=bitmap->width) return -1;
    //获取对应的字节和对应的bit位
    int byteCur=(row*bitmap->width+col)/_DEF_NOF_BITS;
    int bitCur =(row*bitmap->width+col)%_DEF_NOF_BITS;
    Bit data=bitmap->data[byteCur];
    Bit check = data&(1<<bitCur);
    return check==0?(Bit)0:(Bit)1;
}
static int set(Bitmap* bitmap,int row,int col,Bit val){
    if(NULL==bitmap||row<0||row>=bitmap->height||col<0||col>=bitmap->width) return -1;
    int byteCur=(row*bitmap->width+col)/_DEF_NOF_BITS;
    int bitCur =(row*bitmap->width+col)%_DEF_NOF_BITS;
    if(val){
        bitmap->data[byteCur]|=(1<<bitCur);
    }else{
        bitmap->data[byteCur]&=(~(1<<bitCur))&bitmap->data[byteCur];
    }
    return 0;
}
static int reverse(Bitmap* bitmap,int row,int col){
    return set(bitmap,row,col,!at(bitmap,row,col));
}//反转对应行列的bit位
static int copyTo(Bitmap*dst,Bitmap* src){
    if(NULL==src||NULL==dst) return -1;
    

    int width=src->width;
    int height=src->height;
    dst->width=width;
    dst->height=height;
    int nBits = width*height;
    int nBytes=0;
    if(nBits%_DEF_NOF_BITS) nBytes = nBits/_DEF_NOF_BITS+1;
    else nBytes=nBits/_DEF_NOF_BITS;

    char*data = bitmapMalloc(nBytes);
    if(dst->data==NULL) return -1;
    bitmapFree(dst->data);
    dst->data=data;
    memcpy(data,src->data,nBytes);
    return 0;
}

static int byteReverse(Bitmap* bitmap){
    if(NULL==bitmap||NULL==bitmap->data) return -1;
    int nBytes = getBytes(bitmap);
    Bit* data = bitmap->data;
    for(int i=0;i<nBytes;i++){
        
    }
    return 0;
}

static int getBytes(Bitmap* bitmap){
    if(NULL==bitmap) return -1;
    int width=bitmap->width;
    int height=bitmap->height;
    int nBits = width*height;
    int nBytes=0;
    if(nBits%_DEF_NOF_BITS) nBytes = nBits/_DEF_NOF_BITS+1;
    else nBytes=nBits/_DEF_NOF_BITS;
    return nBytes;
}

static int clear(Bitmap* bitmap){
    if(NULL==bitmap) return -1;
    int nBytes = getBytes(bitmap);
    memset(bitmap->data,0,nBytes);
    return 0;
}
static int compare(Bitmap* bitmap,Bitmap* cmp){
    if(NULL==bitmap||NULL==cmp) return -1;
    if(bitmap==cmp) return 0;
    if(bitmap->height!=cmp->height||bitmap->width!=cmp->width) return 1;
    
    int nBytesThis = getBytes(bitmap);
    int nBytesCmp = getBytes(cmp);
    if(nBytesCmp!=nBytesThis) return 1;
    
    char* byteThis = (char*)bitmap->data;
    char* byteCmp  = (char*)cmp->data;

    for(int i =0;i<nBytesThis;i++){
        if(byteThis[i]!=byteCmp[i]) return 1;
    }

    return 0;
}

static char* toModRect(Bitmap* bitmap,int* pWidth,int* pHeight){
    int wBitmap = bitmap->width;
    int hBitmap = bitmap->height;
    int hRect = hBitmap/_DEF_NOF_BITS+(hBitmap%_DEF_NOF_BITS==0?0:1);
    int wRect = wBitmap;
    int size = hRect*wRect*sizeof(Bit);
    char* pRect = bitmapMalloc(size);
    memset(pRect,0,size);
    int rectCol;
    int rectRow;
    int rectCur;
    int bitCur;
    for(int col = 0;col<wBitmap;col++){
        rectCol=col;
        for(int row = 0;row<hBitmap;row++){
            rectRow = row/_DEF_NOF_BITS;
            rectCur = rectRow*wRect+rectCol;
            if(BottomToTop){
                bitCur=row%_DEF_NOF_BITS;
            }else{
                bitCur=(_DEF_NOF_BITS-1)-row%_DEF_NOF_BITS;
            }
            Bit val = bitmap->at(bitmap,row,col);
            if(-1==val){
                bitmapFree(pRect);
                return NULL;
            }
            if(1==val)
            pRect[rectCur]|=1<<bitCur;
        }
    }
    return pRect;
}
