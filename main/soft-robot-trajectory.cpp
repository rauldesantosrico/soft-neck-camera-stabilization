#include "Cia402device.h"
#include "SocketCanPort.h"
#include "imu3dmgx510.h"
#include "fcontrol.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <cstdio>
#include <vector>

#include <yarp/os/Network.h>
#include <yarp/os/Property.h>
#include <yarp/os/Time.h>

#include <yarp/dev/PolyDriver.h>
#include <yarp/dev/IControlMode.h>
#include <yarp/dev/IEncodersTimed.h>
#include <yarp/dev/IPositionControl.h>
#include <yarp/dev/IVelocityControl.h>


#define LG0 0.003 //meters
#define TRADIO 0.0075 // radio del tambor
#define PRADIO 0.05 // radio de la plataforma

std::vector<double> trunkPoss {0,0};

//-------------------------------------------------------------------------

// pitch, roll to velocity in meters/sec
void pr2tendons(double pitch, double roll, std::vector<double> & vel)
{
    double T  = PRADIO / TRADIO;
    vel[0] =  pitch * T;
    vel[1] =  roll * T * sin(2*M_PI/3) + pitch * T * cos(2*M_PI/3);
    vel[2] =  roll * T * sin(4*M_PI/3) + pitch * T * cos(4*M_PI/3);

    //printf("Final angular velocities: %f %f %f\n", vel[0], vel[1], vel[2]);
}

// tense tendons:
// return true if it's stopped and tensed
bool tenseTendons(CiA402Device motor){
    motor.Setup_Torque_Mode();
    motor.SetTorque(0.07);
    while(motor.GetVelocity() > 0.2){
        printf("Tensing thread... vel: %f\n", motor.GetVelocity());
    }
    motor.Setup_Velocity_Mode(0);
    return true;
}

bool moveTrunk(){
    // -------- Connection with teo-trunk through YARP --------

    yarp::os::Network yarp;

    if (!yarp::os::Network::checkNetwork())
    {
        std::printf("Please start a yarp name server first\n");
        return 1;
    }

    yarp::os::Property options;
    options.put("device", "remote_controlboard");
    options.put("remote", "/teo/trunk");
    options.put("local", "/local");

    yarp::dev::PolyDriver dd(options);

    if (!dd.isValid())
    {
        std::printf("Device not available.\n");
        return 1;
    }

    yarp::dev::IPositionControl *pos;
    yarp::dev::IVelocityControl *vel;
    yarp::dev::IEncodersTimed *enc;
    yarp::dev::IControlMode *mode;

    bool ok = true;
    ok &= dd.view(pos);
    ok &= dd.view(vel);
    ok &= dd.view(enc);
    ok &= dd.view(mode);

    if (!ok)
    {
        std::printf("ERROR: Problems acquiring trunk robot interface\n");
        return 1;
    }

    std::printf("SUCCESS: Acquired trunk robot interface\n");

    int axes;
    pos->getAxes(&axes);

    std::vector<int> posModes(axes, VOCAB_CM_POSITION);
    mode->setControlModes(posModes.data());

    std::vector<double> pose_h {0,0};
    std::vector<double> pose_1 {20,-12};
    std::vector<double> pose_2 {20, 12};
    std::vector<double> pose_3 {-20,-12};
    std::vector<double> pose_4 {-20,12};

    //yarp::os::Time::delay(10.0);

    std::printf("test positionMove(20, -12)\n");
    trunkPoss = pose_1;
    pos->positionMove(trunkPoss.data());
    yarp::os::Time::delay(8.0);

    std::printf("test positionMove(20, 12)\n");
    trunkPoss = pose_2;
    pos->positionMove(trunkPoss.data());
    yarp::os::Time::delay(8.0);

    std::printf("test positionMove(-20,-12)\n");
    trunkPoss = pose_3;
    pos->positionMove(trunkPoss.data());
    yarp::os::Time::delay(8.0);

    std::printf("test positionMove(-20,12)\n");
    trunkPoss = pose_4;
    pos->positionMove(trunkPoss.data());
    yarp::os::Time::delay(8.0);

    std::printf("test positionMove(0,0)\n");
    trunkPoss = pose_h;
    pos->positionMove(trunkPoss.data());
    yarp::os::Time::delay(8.0);

    return 0;
}

int main ()
{

    // home
    vector<double> targetPose{0, 0}; 

    std::vector<double> targetVel(3);
    int tc = 0; //tensedCounter
    bool tensed = false;


    // control loop sampling time
    double freq=50; //sensor use values: 50,100,500...
    double dts=1/freq;
    SamplingTime Ts;
    Ts.SetSamplingTime(dts); // 0.020

    // configuring IMU
    IMU3DMGX510 imu("/dev/ttyUSB0",freq);
    double pitch,roll, yaw;

    cout<<"Calibrating IMU..."<<endl;

    for (double t=0;t<10;t+=dts)
    {
       imu.GetPitchRollYaw(pitch,roll,yaw);
    }

    cout<<"Calibrated!"<<endl;

    // file results
    FILE *file;
    file = fopen("../trajectory-data.csv","w+");
    fprintf(file,"Time, TargetAxialTrunk, SensorPitch, TargetFrontalTrunk, SensorRoll, VelM1, VelM2, VelM3\n");

    //m1 setup
    SocketCanPort pm1("can0");
    CiA402SetupData sd1(2048,24,0.001, 0.144, 10);
    CiA402Device m1 (1, &pm1, &sd1);
    m1.StartNode();
    m1.SwitchOn();
    m1.Setup_Velocity_Mode(10);

    //m2 setup
    SocketCanPort pm2("can0");
    CiA402SetupData sd2(2048,24,0.001, 0.144, 10);
    CiA402Device m2 (2, &pm2, &sd2);
    m2.StartNode();
    m2.SwitchOn();
    m2.Setup_Velocity_Mode(10);

    //m3 setup
    SocketCanPort pm3("can0");
    CiA402SetupData sd3(2048,24,0.001, 0.144, 10);
    CiA402Device m3 (3, &pm3, &sd3);
    m3.StartNode();
    m3.SwitchOn();
    m3.Setup_Velocity_Mode(10);

    // controller for motor
    PIDBlock cntrl1(0.015,36,0,dts);
    PIDBlock cntrl2(0.015,36,0,dts);
    PIDBlock cntrl3(0.015,36,0,dts);

    // -- Final tests
    //test 9: FPD w=3, pm=50
    //        Pitch: alpha =-0.8500, kp = 2.5773, ka = 3.2325
    //        Roll:  alpha = -0.8600, kp = 2.6299, ka = 3.2395
    FPDBlock fcPitchVelocity(2.5773, 3.2325, -0.8500, dts);
    FPDBlock fcRollVelocity(2.6299, 3.2395, -0.8600, dts);

    auto start = std::chrono::system_clock::now();
    std::chrono::duration<float,std::milli> duration;

    //------- thread of trunk ---------------------------
    std::thread thread(moveTrunk);

    while(1){ 

        // ---- General Control process ----
        imu.GetPitchRollYaw(pitch,roll,yaw);

        // cambio signo para igualar sentido de giro de los motores y del sensor
        pitch = -pitch;
        roll  = -roll;

        double pitchError = targetPose[0] - pitch;
        double rollError  = targetPose[1] - roll;

        double pitchCs = fcPitchVelocity.OutputUpdate(pitchError);
        if (!std::isnormal(pitchCs))
        {
            pitchCs = 0.0;
        }

        double rollCs = fcRollVelocity.OutputUpdate(rollError);
        if (!std::isnormal(rollCs))
        {
            rollCs = 0.0;
        }

        pr2tendons(pitchCs, rollCs, targetVel);

        // -----------------------------------

        // Controller of velocity in M1

        double currentVelM1 = m1.GetVelocityTP();
        double velError1 = targetVel[0] - currentVelM1;

        // Control process
        double cS1 = cntrl1.OutputUpdate(velError1);

        if (!std::isnormal(cS1))
        {
            cS1 = 0.0;
        }

        m1.SetVelocity(cS1);

        // Controller of velocity in M2

        double currentVelM2 = m2.GetVelocityTP();
        double velError2 = targetVel[1] - currentVelM2;

        // Control process
        double cS2 = cntrl2.OutputUpdate(velError2);

        if (!std::isnormal(cS2))
        {
            cS2 = 0.0;
        }

        m2.SetVelocity(cS2);

        // Controller of velocity in M3

        double currentVelM3 = m3.GetVelocityTP();
        double velError3 = targetVel[2] - currentVelM3;

        // Control process
        double cS3 = cntrl3.OutputUpdate(velError3);

        if (!std::isnormal(cS3))
        {
            cS3 = 0.0;
        }

        m3.SetVelocity(cS3);

        auto current = std::chrono::system_clock::now();
        duration = current - start;

        // Escribimos en el archivo
        fprintf(file,"%.4f,", duration.count());
        fprintf(file,"%.4f, %.4f,",  trunkPoss[0], pitch); // pitch target, trunkAxialPoss
        fprintf(file,"%.4f, %.4f,",  trunkPoss[1], roll); // roll target,  trunkFrontalPoss
        fprintf(file,"%.4f, %.4f, %.4f\n", m1.GetVelocity(), m2.GetVelocity(), m3.GetVelocity()); // roll target,  roll sensor

        Ts.WaitSamplingTime();

    }

    m1.SetVelocity(0);
    m2.SetVelocity(0);
    m3.SetVelocity(0);

    m1.ForceSwitchOff();
    m2.ForceSwitchOff();
    m3.ForceSwitchOff();

    return 1;
}
