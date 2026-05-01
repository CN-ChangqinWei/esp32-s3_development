#include "three_axis_irb460.h"

// ========== 具体函数实现（静态化，不暴露）==========

// 析构函数
static void ThreeAxisIrb460Delete(void* p) {
    if (NULL == p) return;
    robotFree(p);
}

// 逆运动学求解
static void ThreeAxisIrb460Inverse(void* instance, AxisFloat* in, AxisFloat* out) {
    if (NULL == instance || NULL == in || NULL == out) return;
    
    ThreeAxisIrb460Instance* self = (ThreeAxisIrb460Instance*)instance;
    
    // 输入：笛卡尔坐标 (x, y, z)
    AxisFloat x = in[0];
    AxisFloat y = in[1];
    AxisFloat z = in[2];
    
    // 输出：关节角 (alpha, beta, gama)
    AxisFloat alpha, beta, gamma;
    AxisFloat theta3;
    // 一、基础坐标变换：转换为极坐标
    alpha = atan2(x, y);  // 底座旋转角
    AxisFloat L = sqrt(x * x + y * y);  // 水平面投影距离
    
    // 提取参数
    AxisFloat a = self->a;
    AxisFloat b = self->b;
    AxisFloat H = self->H;
    

    // 三、情况一：目标高度等于底座高度（z = H）
    if (fabs(z - H) < 1e-6) {
        // 水平平面，构成三角形，两边为 a、b，底边为 L
        // 顶点角 gama
        AxisFloat cos_theta3 = (a * a + b * b - L * L) / (2 * a * b);
        // 限制范围防止数值误差
        if (cos_theta3 > 1.0f) cos_theta3 = 1.0f;
        if (cos_theta3 < -1.0f) cos_theta3 = -1.0f;
        theta3 = acos(cos_theta3);
        
        // 底边角 theta_1
        AxisFloat cos_theta1 = (a * a + L * L - b * b) / (2 * a * L);
        if (cos_theta1 > 1.0f) cos_theta1 = 1.0f;
        if (cos_theta1 < -1.0f) cos_theta1 = -1.0f;
        AxisFloat theta_1 = acos(cos_theta1);
        
        // 关节角 beta
        beta = M_PI / 2.0f + theta_1;
    }
    // 四、情况二：目标高度低于底座高度（z < H）
    else if (z < H) {
        AxisFloat r_1 = H - z;  // 垂直下降距离
        AxisFloat r_2 = sqrt(r_1 * r_1 + L * L);  // 斜边长度
        
        // 顶点角 theta3
        AxisFloat cos_theta3 = (a * a + b * b - r_2 * r_2) / (2 * a * b);
        if (cos_theta3 > 1.0f) cos_theta3 = 1.0f;
        if (cos_theta3 < -1.0f) cos_theta3 = -1.0f;
        theta3 = acos(cos_theta3);
        
        // 角度 theta_1（上臂与垂直方向夹角）
        AxisFloat cos_theta1 = (a * a + r_2 * r_2 - b * b) / (2 * a * r_2);
        if (cos_theta1 > 1.0f) cos_theta1 = 1.0f;
        if (cos_theta1 < -1.0f) cos_theta1 = -1.0f;
        AxisFloat theta_1 = acos(cos_theta1);
        
        // 角度 theta_2（下臂与斜边 r_2 的夹角）
        AxisFloat cos_theta2 = (r_1 * r_1 + r_2 * r_2 - L * L) / (2 * r_1 * r_2);
        if (cos_theta2 > 1.0f) cos_theta2 = 1.0f;
        if (cos_theta2 < -1.0f) cos_theta2 = -1.0f;
        AxisFloat theta_2 = acos(cos_theta2);
        
        // 关节角 beta
        beta = theta_1 + theta_2;
    }
    // 五、情况三：目标高度高于底座高度（z > H）
    else {
        AxisFloat r_1 = z - H;  // 垂直上升距离
        AxisFloat r_2 = sqrt(r_1 * r_1 + L * L);  // 斜边长度
        
        // 顶点角 theta3
        AxisFloat cos_theta3 = (a * a + b * b - r_2 * r_2) / (2 * a * b);
        if (cos_theta3 > 1.0f) cos_theta3 = 1.0f;
        if (cos_theta3 < -1.0f) cos_theta3 = -1.0f;
        theta3 = acos(cos_theta3);
        
        // 角度 theta_1（上臂与垂直方向的偏角）
        AxisFloat cos_theta1 = (a * a + r_2 * r_2 - b * b) / (2 * a * r_2);
        if (cos_theta1 > 1.0f) cos_theta1 = 1.0f;
        if (cos_theta1 < -1.0f) cos_theta1 = -1.0f;
        AxisFloat theta_1 = acos(cos_theta1);
        
        // 角度 theta_2（斜边 r_2 与垂直方向的夹角）
        AxisFloat cos_theta2 = r_1 / r_2;  // 简化：cos(theta_2) = r_1 / r_2
        if (cos_theta2 > 1.0f) cos_theta2 = 1.0f;
        if (cos_theta2 < -1.0f) cos_theta2 = -1.0f;
        AxisFloat theta_2 = acos(cos_theta2);
        
        // 关节角 beta
        beta = theta_1 + theta_2;
    }
    gamma= theta3-M_PI+beta;

    beta =  M_PI - beta;
    beta = _FUNC_RANGE(beta,0,M_PI_2);
    gamma = _FUNC_RANGE(gamma,0,M_PI_2);

    // 输出结果
    out[0] = alpha;   // 底座旋转角
    out[1] = beta;    // 第二个关节角
    out[2] = gamma;   // 肘关节角度（顶点角）
}

// ========== 接口表初始化 ==========
static const RobotPositionInterface threeAxisIrb460Interface = {
    .inverse = ThreeAxisIrb460Inverse,
    .deleteInstance = ThreeAxisIrb460Delete
};

// ========== 创建函数 ==========
RobotPositionResolve* NewThreeAxisIrb460(AxisFloat a, AxisFloat b,  AxisFloat H) {
    // 分配具体实例内存
    ThreeAxisIrb460Instance* instance = (ThreeAxisIrb460Instance*)robotMalloc(sizeof(ThreeAxisIrb460Instance));
    if (NULL == instance) return NULL;
    
    // 初始化具体实例参数
    instance->a = a;
    instance->b = b;
    instance->H = H;
    
    // 创建抽象类实例
    RobotPositionResolve* resolve = NewRobotPositionResolve(instance, threeAxisIrb460Interface);
    if (NULL == resolve) {
        robotFree(instance);
        return NULL;
    }
    
    return resolve;
}
