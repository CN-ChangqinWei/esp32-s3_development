#include "communication.h"
#include <string.h>

void* (*CommMalloc)(size_t size) = pvPortMalloc;
void  (*CommFree)(void* p) = vPortFree;


Communication* NewCommunication(void* instance, CommInterface interface) {
    Communication* comm = (Communication*)CommMalloc(sizeof(Communication));
    if (comm == NULL) return NULL;
    comm->instance = instance;
    comm->interface = interface;
    return comm;
}

void DeleteCommunication(Communication* comm) {
    if (comm != NULL) {
        comm->interface.deleteInstance(comm->instance);
        CommFree(comm);
    }
}

uint32_t CommSend(Communication* comm, char* buf, uint32_t len) {
    if (comm == NULL || buf == NULL || len == 0) return 0;
    return comm->interface.send(comm->instance, buf, len);
}

uint32_t CommRecv(Communication* comm, char* buf, uint32_t len) {
    if (comm == NULL || buf == NULL || len == 0) return 0;
    return comm->interface.recv(comm->instance, buf, len);
}
