#ifndef _CONTROLLER_H
#define _CONTROLLER_H

#include "Transmitter.h"
#include "VideoReceiver.h"
#include "Plotter.h"
#include "GLWidget.h"

#include <QApplication>
#include <QtGui>

class Controller : public QApplication
{
  Q_OBJECT;

 public:
  Controller(int &argc, char **argv);
  ~Controller(void);
  void createGUI(void);
  void connect(QString host, quint16 port);

 private slots:
  void updateRtt(int ms);
  void updateResendTimeout(int ms);
  void updateResentPackets(quint32 resendCounter);
  void updateUptime(int seconds);
  void updateLoadAvg(float avg);
  void updateWlan(int percent);
  void updateCamera(double x_percent, double y_percent);
  void updateCameraX(int degree);
  void updateCameraY(int degree);
  void updateMotor(QKeyEvent *event);
  void updateMotorRight(int percent);
  void updateMotorLeft(int percent);
  void updateStatus(quint8 status);
  void updateIMU(QByteArray *imu);
  void updateIMURaw(QByteArray *imuraw);
  void sendCameraAndSpeed(void);
  void clickedEnableVideo(bool enabled);
  void selectedVideoSource(int index);
  void updateNetworkRate(int payloadRx, int totalRx, int payloadTx, int totalTx);

 private:
  void prepareSendCameraAndSpeed(void);
  QSlider *createSlider();

  GLWidget *glWidget;
  QSlider *yawSlider;
  QSlider *pitchSlider;
  QSlider *rollSlider;

  Transmitter *transmitter;
  VideoReceiver *vr;

  QWidget *window;
  QLabel *labelRTT;
  QLabel *labelResendTimeout;
  QLabel *labelResentPackets;
  QLabel *labelUptime;
  QLabel *labelLoadAvg;
  QLabel *labelWlan;

  QLabel *labelYaw;
  QLabel *labelPitch;
  QLabel *labelRoll;

  QSlider *horizSlider;
  QSlider *vertSlider;

  QPushButton *buttonEnableVideo;
  QComboBox *comboboxVideoSource;

  QLabel *labelMotorRightSpeed;
  QLabel *labelMotorLeftSpeed;

  QLabel *labelRx;
  QLabel *labelTx;

  int cameraX;
  int cameraY;
  int motorRight;
  int motorLeft;

  Plotter *plotter[9];

  QTimer *cameraAndSpeedTimer;
  QTime *cameraAndSpeedTime;
};

#endif
