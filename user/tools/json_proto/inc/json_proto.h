#ifndef _JSON_PROTO_H
#define _JSON_PROTO_H
#include "hashmap.h"
#include "communication.h"
typedef struct hashmap Hashmap;
typedef struct JsonProto{
    Communication* comm;
    Hashmap*       protoSerializeMap;
}JsonProto;
JsonProto* NewJsonProto(Communication* comm,Hashmap* protoSerializeMap);
void* JsonProtoProtoRecvPackage(JsonProto* proto, int* len);
void DeleteJsonProto(JsonProto* proto);
// 发送包（先发4字节长度，再发数据）
void JsonProtoSendPackage(JsonProto* proto, char* data, int len);
#endif
