#ifndef _PROTOCOL_H
#define _PROTOCOL_H

typedef enum{
    Health=0,
    PROTO_ECHO,
    PROTO_MOTOR,
    PROTO_ROBOT_POSITION,
    NUM_OF_PROTO
} Protocol;

#endif
