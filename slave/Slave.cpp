/*
 * Copyright 2012 Tuomas Kulve, <tuomas.kulve@snowcap.fi>
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "Slave.h"
#include "Transmitter.h"
#include "VideoSender.h"

#include <QCoreApplication>
#include <QPluginLoader>

#include <stdlib.h>                     /* getenv */

// For traditional serial port handling
#include <termios.h>
#include <sys/stat.h>
#include <fcntl.h>

Slave::Slave(int &argc, char **argv):
  QCoreApplication(argc, argv), transmitter(NULL),
  vs(NULL), status(0), hardware(NULL), cb(NULL), camera(NULL),
  oldSpeed(0), oldTurn(0)
{
}



Slave::~Slave()
{

  // Delete the control board, if any
  if (cb) {
	delete cb;
	cb = NULL;
  }

  // Delete the transmitter, if any
  if (transmitter) {
	delete transmitter;
	transmitter = NULL;
  }

  // Delete hardware
  if (hardware) {
	delete hardware;
	hardware = NULL;
  }
}



bool Slave::init(void)
{
  // Check on which hardware we are running based on the info in /proc/cpuinfo.
  // Defaulting to Generic X86
  QString hardwareName("generic_x86");
  QFile cpuinfo("/proc/cpuinfo");
  if (cpuinfo.open(QIODevice::ReadOnly | QIODevice::Text)) {
	qDebug() << "Reading cpuinfo";
	QByteArray content = cpuinfo.readAll();

	// Check for supported hardwares
	if (content.contains("Gumstix Overo")) {
	  qDebug() << "Detected Gumstix Overo";
	  hardwareName = "gumstix_overo";
	} else if (content.contains("BCM2708")) {
	  qDebug() << "Detected Broadcom based Raspberry Pi";
	  hardwareName = "raspberry_pi";
	} else if (content.contains("grouper")) {
	  qDebug() << "Detected Tegra 3 based Nexus 7";
	  hardwareName = "tegra3";
	} else if (content.contains("cardhu")) {
	  qDebug() << "Detected Tegra 3 based Cardhu or Ouya";
	  hardwareName = "tegra3";
	} else if (content.contains("jetson-tk1")) {
	  qDebug() << "Detected Tegra K1 based Jetson TK1";
	  hardwareName = "tegrak1";
	}

	cpuinfo.close();
  }


  qDebug() << "Initialising hardware object:" << hardwareName;

  hardware = new Hardware(hardwareName);

  // FIXME: get serial device path from hardware plugin?
  // FIXME: or env variable?
  cb = new ControlBoard("/dev/ttyACM0");
  // FIXME: if the init fails, wait for a signal that is has succeeded (or wait it always?)
  if (!cb->init()) {
	qCritical("Failed to initialize ControlBoard");
	// CHECKME: to return false or not to return false (and do clean up)?
  } else {
	// Set ControlBoard frequency to 50Hz to match standard servos
	cb->setPWMFreq(50);
  }

  camera = new Camera();
  if (camera->init()) {
	camera->setBrightness(0);
  }

  // Start a timer for sending ping to the control board
  QTimer *cbPingTimer = new QTimer();
  QObject::connect(cbPingTimer, SIGNAL(timeout()), this, SLOT(sendCBPing()));
  cbPingTimer->setSingleShot(false);
  cbPingTimer->start(100);

  return true;
}



void Slave::connect(QString host, quint16 port)
{

  // Delete old transmitter if any
  if (transmitter) {
	delete transmitter;
  }

  // Create a new transmitter
  transmitter = new Transmitter(host, port);

  // Connect the incoming data signals
  QObject::connect(transmitter, SIGNAL(value(quint8, quint16)), this, SLOT(updateValue(quint8, quint16)));
  QObject::connect(transmitter, SIGNAL(connectionStatusChanged(int)), this, SLOT(updateConnectionStatus(int)));

  transmitter->initSocket();

  // Send ping every second (unless other high priority packet are sent)
  transmitter->enableAutoPing(true);

  // Start timer for sending system statistics (wlan signal, cpu load) peridiocally
  QTimer *statsTimer = new QTimer();
  QObject::connect(statsTimer, SIGNAL(timeout()), this, SLOT(sendSystemStats()));
  statsTimer->setSingleShot(false);
  statsTimer->start(1000);

  // Create and enable sending video
  if (vs) {
	delete vs;
  }
  vs = new VideoSender(hardware);

  QObject::connect(vs, SIGNAL(media(QByteArray*)), transmitter, SLOT(sendMedia(QByteArray*)));

  QObject::connect(cb, SIGNAL(debug(QString*)), transmitter, SLOT(sendDebug(QString*)));
  QObject::connect(cb, SIGNAL(distance(quint16)), this, SLOT(cbDistance(quint16)));
  QObject::connect(cb, SIGNAL(temperature(quint16)), this, SLOT(cbTemperature(quint16)));
  QObject::connect(cb, SIGNAL(current(quint16)), this, SLOT(cbCurrent(quint16)));
  QObject::connect(cb, SIGNAL(voltage(quint16)), this, SLOT(cbVoltage(quint16)));
}



void Slave::sendSystemStats(void)
{
  {
	QFile file("/sys/class/net/wlan0/wireless/link");
	if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {

	  QByteArray line = file.readLine();
	  uint link = line.trimmed().toUInt();
	  // Link is 0-70, convert to 0-100%
	  quint16 signal = (quint16)(link/70.0 * 100);

	  transmitter->sendPeriodicValue(MSG_SUBTYPE_SIGNAL_STRENGTH, signal);
	  file.close();
	} else {
	  QFile file("/proc/net/wireless");
	  if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {

		QString line = file.readLine();
		line = file.readLine();
		line = file.readLine();
		quint16 signal = 0;
		if (line.length() > 0) {
		  QStringList list = line.split(" ", QString::SkipEmptyParts);
		  list[3].chop(1);
		  signal = (quint16)(list[3].toUInt());
		}
		transmitter->sendPeriodicValue(MSG_SUBTYPE_SIGNAL_STRENGTH, signal);
		file.close();
	  }
	}
  }

  {
	QFile file("/proc/loadavg");
	if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {

	  QByteArray line = file.readLine();
	  double loadavg = line.left(line.indexOf(" ")).toDouble();
	  // Load avg is double, send as 100x integer
	  quint16 signal = (quint16)(loadavg * 100);

	  transmitter->sendPeriodicValue(MSG_SUBTYPE_CPU_USAGE, signal);
	  file.close();
	}
  }

  {
	QFile file("/proc/uptime");
	if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {

	  QByteArray line = file.readLine();
	  double uptime = line.left(line.indexOf(" ")).toDouble();
	  // Uptime is seconds as double, send seconds as uint
	  quint16 signal = (quint16)(uptime);

	  transmitter->sendPeriodicValue(MSG_SUBTYPE_UPTIME, signal);
	  file.close();
	}
  }

  {
	QFile file("/sys/devices/virtual/hwmon/hwmon0/temp1_input");
	if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {

	  QByteArray line = file.readLine();
	  uint mtemp = line.trimmed().toUInt();
	  // Temperature is in millicelsius, convert to hundreds of celsius
	  quint16 temp = (quint16)(mtemp / 10.0);

	  transmitter->sendPeriodicValue(MSG_SUBTYPE_TEMPERATURE, temp);
	  file.close();
	}
  }
}



void Slave::updateValue(quint8 type, quint16 value)
{
  qDebug() << "in" << __FUNCTION__ << ", type:" << Message::getSubTypeStr(type) << ", value:" << value;

  switch (type) {
  case MSG_SUBTYPE_ENABLE_LED:
	cb->setGPIO(CB_GPIO_LED1, value);
	cb->setGPIO(CB_GPIO_HEAD_LIGHTS, value);
	break;
  case MSG_SUBTYPE_ENABLE_VIDEO:
	parseSendVideo(value);
	break;
  case MSG_SUBTYPE_VIDEO_SOURCE:
	vs->setVideoSource(value);
	break;
  case MSG_SUBTYPE_CAMERA_XY:
	parseCameraXY(value);
	break;
  case MSG_SUBTYPE_CAMERA_ZOOM:
	camera->setZoom(value);
	break;
  case MSG_SUBTYPE_CAMERA_FOCUS:
	camera->setFocus(value);
	break;
  case MSG_SUBTYPE_SPEED_TURN:
	parseSpeedTurn(value);
	break;
  case MSG_SUBTYPE_VIDEO_QUALITY:
	parseVideoQuality(value);
	break;
  default:
    qWarning() << __FUNCTION__ << "Unknown type: " << Message::getSubTypeStr(type);
  }
}


void Slave::updateConnectionStatus(int status)
{
  qDebug() << "in" << __FUNCTION__ << ", status:" << status;

  if (status == CONNECTION_STATUS_LOST) {

	// Stop moving if lost connection to the controller
	if (oldSpeed != 0) {
	  cb->setPWMDuty(CB_PWM_SPEED, 750);
	  cb->stopPWM(CB_PWM_SPEED);
	  qDebug() << "in" << __FUNCTION__ << ", Speed PWM:" << 750;
	  oldSpeed = 0;
	}

	if (oldTurn != 0) {
	  cb->setPWMDuty(CB_PWM_TURN, 750);
	  cb->stopPWM(CB_PWM_TURN);
	  qDebug() << "in" << __FUNCTION__ << ", Turn PWM:" << 750;
	  oldTurn = 0;
	}

	// Stop sending video
	parseSendVideo(0);
  }

  // FIXME: if connection restored (or just ok for the first time), send status to controller?
}


void Slave::cbTemperature(quint16 value)
{
  transmitter->sendPeriodicValue(MSG_SUBTYPE_TEMPERATURE, value);
}



void Slave::cbDistance(quint16 value)
{
  transmitter->sendPeriodicValue(MSG_SUBTYPE_DISTANCE, value);
}



void Slave::cbCurrent(quint16 value)
{
  transmitter->sendPeriodicValue(MSG_SUBTYPE_BATTERY_CURRENT, value);
}



void Slave::cbVoltage(quint16 value)
{
  transmitter->sendPeriodicValue(MSG_SUBTYPE_BATTERY_VOLTAGE, value);
}



void Slave::parseSendVideo(quint16 value)
{
  vs->enableSending(value ? true : false);
  // FIXME: what's the point of maintaining "status"? Better to ask from vs?
  if (value) {
	status ^= STATUS_VIDEO_ENABLED;
  } else {
	status &= ~STATUS_VIDEO_ENABLED;
  }
}



void Slave::parseVideoQuality(quint16 value)
{
  vs->setVideoQuality(value);
}



void Slave::parseCameraXY(quint16 value)
{
  quint16 x, y;
  static quint16 oldx = 0, oldy = 0;

  // Value is a 16 bit containing 2x 8bit values that are doubled percentages
  x = (value >> 8);
  y = (value & 0x00ff);

  // Control board expects percentages to be x100 integers
  // Servos expect 5-10% duty cycle for the 1-2ms pulses
  x = static_cast<quint16>(x * (5 / 2.0)) + 500;
  y = static_cast<quint16>(y * (5 / 2.0)) + 500;

  // Update servo positions only if value has changed
  if (x != oldx) {
	cb->setPWMDuty(CB_PWM_CAMERA_X, x);
	qDebug() << "in" << __FUNCTION__ << ", Camera X PWM:" << x;
	oldx = x;
  }

  if (y != oldy) {
	cb->setPWMDuty(CB_PWM_CAMERA_Y, y);
	qDebug() << "in" << __FUNCTION__ << ", Camera Y PWM:" << y;
	oldy = y;
  }
}



void Slave::parseSpeedTurn(quint16 value)
{
  quint16 speed, turn;

  // Value is a 16 bit containing 2x 8bit values that are shifted by 100
  speed = (value >> 8);
  turn = (value & 0x00ff);

  // Control board expects percentages to be x100 integers
  // Servos expect 5-10% duty cycle for the 1-2ms pulses
  speed = static_cast<quint16>(speed * (5 / 2.0)) + 500;
  turn = static_cast<quint16>(turn * (5 / 2.0)) + 500;

  // Update servo/ESC positions only if value has changed
  if (speed != oldSpeed) {
	cb->setPWMDuty(CB_PWM_SPEED, speed);

	if (speed < oldSpeed) {
	  // Start a timer for turning of rear lights
	  static QTimer *cbRearLightTimer = NULL;
	  if (cbRearLightTimer == NULL) {
		cbRearLightTimer = new QTimer();
		QObject::connect(cbRearLightTimer, SIGNAL(timeout()), this, SLOT(turnOffRearLight()));
		cbRearLightTimer->setSingleShot(true);
	  }
	  cbRearLightTimer->start(2000);

	  cb->setGPIO(CB_GPIO_REAR_LIGHTS, 1);
	}
	qDebug() << "in" << __FUNCTION__ << ", Speed PWM:" << speed;
	oldSpeed = speed;
  }

  if (turn != oldTurn) {
	// The rear wheels must be turned vice versa compared to front wheels
	// 500-1000 -> 0-500 -> 500-0 -> 1000-500
	quint16 turn2 = (500 - (turn - 500)) + 500;

	// Reversing front and rear based on experiments
	cb->setPWMDuty(CB_PWM_TURN, turn);
	qDebug() << "in" << __FUNCTION__ << ", Turn PWM1:" << turn;
	oldTurn = turn;

	cb->setPWMDuty(CB_PWM_TURN2, turn2);
	qDebug() << "in" << __FUNCTION__ << ", Turn PWM2:" << turn2;
  }

}



void Slave::turnOffRearLight(void)
{
  cb->setGPIO(CB_GPIO_REAR_LIGHTS, 0);
}



void Slave::sendCBPing(void)
{
  cb->sendPing();
}
