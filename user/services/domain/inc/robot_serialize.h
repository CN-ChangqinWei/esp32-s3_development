#ifndef _ROBOT_SERIALIZE_H
#define _ROBOT_SERIALIZE_H

#include "robot_domain.h"
#include "cJSON.h"

void* RobotDomainDeserialize(char* jsonStr);
char* RobotDomainSerialize(void* domain);

#endif
