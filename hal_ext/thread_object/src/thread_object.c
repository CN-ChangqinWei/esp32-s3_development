#include"thread_object.h"
void* (*threadObjectMalloc)(size_t size)=malloc;
void  (*threadObjectFree)(void* p)=free;

ThreadObject* NewThreadObject(void* instance,ThreadObjectInterface interfaces){
    ASSERT_NOT_NULL(instance,"instance is not NULL",NULL);
    ThreadObject *obj = threadObjectMalloc(sizeof(ThreadObject));
    int err = InitThreadObject(obj,instance,interfaces);
    if(err){
        threadObjectFree(obj);
        obj=NULL;
    }
    return obj;
}
int InitThreadObject(ThreadObject* threadObj,void* instance,ThreadObjectInterface interfaces){
    ASSERT_NOT_NULL(instance,"instance is not NULL",-1);
    ASSERT_NOT_NULL(threadObj,"threadObj is not NULL",-2);
    threadObj->instance=instance;
    threadObj->interfaces=interfaces;
    return 0;
}
void DeleteThreadObject(ThreadObject* threadObj){
    if(threadObj==NULL) return;
    if(threadObj->interfaces.deleteInstance!=NULL){
        threadObj->interfaces.deleteInstance(threadObj->instance);   /* 传具体实例，不是抽象包装对象 */
    }
    threadObjectFree(threadObj);
}
