#ifndef _ROBOT_MOTION_RESOLVE_H
#define _ROBOT_MOTION_RESOLVE_H

typedef struct{
    void (*deleteInstance)(void *p);

}RobotMotionInterface;

typedef struct{
    void* instance;
    RobotMotionInterface interface;
}RobotMotionResolve;

#endif