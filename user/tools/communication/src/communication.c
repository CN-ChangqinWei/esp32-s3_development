#include "communication.h"
#include <string.h>

Communication* NewCommunication(void* instance, CommInterface interface) {
    Communication* comm = (Communication*)pvPortMalloc(sizeof(Communication));
    if (comm == NULL) return NULL;
    comm->instance = instance;
    comm->interface = interface;
    return comm;
}

void DeleteCommunication(Communication* comm) {
    if (comm != NULL) {
        vPortFree(comm);
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
