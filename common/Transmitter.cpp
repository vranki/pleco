
#include "Transmitter.h"
#include "Message.h"


Transmitter::Transmitter(QString host, quint16 port):
  socket(), relayHost(host), relayPort(port), resendTimeout(RESEND_TIMEOUT)
{
  qDebug() << "in" << __FUNCTION__;

  // Add a signal mapper for resending packages
  resendSignalMapper = new QSignalMapper(this);
  connect(resendSignalMapper, SIGNAL(mapped(QObject *)),
		  this, SLOT(sendMessage(QObject *)));

  // Zero arrays
  for (int i = 0; i < Message::MSG_TYPE_MAX; i++)  {
	rtTimers[i] = NULL;
	resendTimers[i] = NULL;
	messageHandlers[i] = NULL;
	resendMessages[i] = NULL;
  }

  // Set message handlers
  messageHandlers[Message::MSG_TYPE_ACK]      = &Transmitter::handleACK;
  messageHandlers[Message::MSG_TYPE_PING]     = &Transmitter::handlePing;
  messageHandlers[Message::MSG_TYPE_STATS]    = &Transmitter::handleStats;
}


Transmitter::~Transmitter()
{
  qDebug() << "in" << __FUNCTION__;

  delete resendSignalMapper;

  // Delete the timers 
  for (int i = 0; i < Message::MSG_TYPE_MAX; i++)  {
	delete rtTimers[i];
	rtTimers[i] = NULL;

	delete resendTimers[i];
	resendTimers[i] = NULL;

	messageHandlers[i] = NULL;
  }

}

void Transmitter::initSocket()
{
  qDebug() << "in " << __FUNCTION__;

  socket.bind(QHostAddress::Any, 0,QUdpSocket::ShareAddress);
  
  qDebug() << "Local address:" << socket.localAddress().toString();
  qDebug() << "Local port   :" << socket.localPort();

  connect(&socket, SIGNAL(readyRead()),
		  this, SLOT(readPendingDatagrams()));
  connect(&socket, SIGNAL(error(QAbstractSocket::SocketError)), 
		  this, SLOT(printError(QAbstractSocket::SocketError)));
}



void Transmitter::sendPing()
{
  qDebug() << "in" << __FUNCTION__;

  Message *msg = new Message(Message::MSG_TYPE_PING);
  sendMessage(msg);
}



void Transmitter::sendStats(QList <int> *stats)
{
  qDebug() << "in" << __FUNCTION__;

  Message *msg = new Message(Message::MSG_TYPE_STATS);

  QByteArray *data = msg->data();

  for (int i = 0; i < stats->size(); i++) {
	// FIXME: no hardcoded offset
	(*data)[2 + i] = (uint8_t)stats->at(i);
  }

  sendMessage(msg);
}



void Transmitter::sendMessage(Message *msg)
{
  socket.writeDatagram(*msg->data(), relayHost, relayPort);

  // If not a high priority package, all is done
  if (!msg->isHighPriority()) {
	delete msg;
	return;
  }

  // Store pointer to message until it's acked
  resendMessages[msg->type()] = msg;

  // Start high priority package resend
  startResendTimer(msg);

  // Start round trip timer
  startRTTimer(msg);
}



void Transmitter::sendMessage(QObject *msg)
{
  sendMessage(dynamic_cast<Message *>(msg));
}



void Transmitter::startResendTimer(Message *msg)
{
  Message::MSG_TYPE type = msg->type();

  // Restart existing timer, or create a new one
  if (resendTimers[type]) {
	resendTimers[type]->start();
  } else {
	resendTimers[type] = new QTimer(this);

	// Add resend timeout through a signal mapper
	connect(resendTimers[type], SIGNAL(timeout()), resendSignalMapper, SLOT(map()));
	resendSignalMapper->setMapping(resendTimers[type], msg);

	resendTimers[type]->start(resendTimeout);
  }
}



void Transmitter::startRTTimer(Message *msg)
{
  Message::MSG_TYPE type = msg->type();

  // Restart existing stopwatch, or create a new one
  if (rtTimers[type]) {
	rtTimers[type]->restart();
  } else {
	rtTimers[type] = new QTime();
	rtTimers[type]->start();
  }
}



void Transmitter::readPendingDatagrams()
{
  while (socket.hasPendingDatagrams()) {
	QByteArray datagram;
	QHostAddress sender;
	quint16 senderPort;

	qDebug() << "in" << __FUNCTION__;

	datagram.resize(socket.pendingDatagramSize());
	
	socket.readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
	
	qDebug() << "Sender:" << sender.toString() << ", port:" << senderPort;
	printData(datagram);

	parseData(datagram);
  }
}


void Transmitter::printError(QAbstractSocket::SocketError error)
{
  qDebug() << "Socket error (" << error << "):" << socket.errorString();
}



void Transmitter::printData(QByteArray data)
{
  qDebug() << "in" << __FUNCTION__ << ", data:" << data.size();

  qDebug() << data.toHex();
}



void Transmitter::parseData(QByteArray data)
{
  qDebug() << "in" << __FUNCTION__;

  Message msg(data);

  // isValid() also checks that the packet is exactly as long as expected
  if (!msg.isValid()) {
	qWarning() << "Package not valid, ignoring";
	return;
  }

  qDebug() << __FUNCTION__ << ": type:" << (int)msg.type();

  // Check, whether to ACK the packet
  if (msg.isHighPriority()) {
	sendACK(msg);
  }

  // Handle different message types in different methods
  if (messageHandlers[msg.type()]) {
	messageHandler func = messageHandlers[msg.type()];
	(this->*func)(msg);
  } else {
	qWarning() << "No message handler for type" << msg.type() << ", ignoring";
  }  
}



void Transmitter::sendACK(Message &incoming)
{
  qDebug() << "in" << __FUNCTION__;

  Message *msg = new Message(Message::MSG_TYPE_ACK);

  msg->setACK(incoming.type());

  sendMessage(msg);
}



void Transmitter::handleACK(Message &msg)
{
  qDebug() << "in" << __FUNCTION__;

  Message::MSG_TYPE ackedType = msg.getAckedType();

  // Stop and delete resend timer
  if (resendTimers[ackedType]) {
	resendTimers[ackedType]->stop();
	resendSignalMapper->removeMappings(resendTimers[ackedType]);
	delete resendTimers[ackedType];
	resendTimers[ackedType] = NULL;
  } else {
	  qWarning() << "No Resend timer running for type" << ackedType;
  }


  // Send RTT signal and delete RT timer
  if (rtTimers[ackedType]) {
	int rttMs = rtTimers[ackedType]->elapsed();

	emit(rtt(rttMs));

	// Adjust resend timeout but keep it always > 20ms.
	// If the doubled round trip time is less than current timeout, decrease resendTimeout by 10%.
	// if the doubled round trip time is greater that current resendTimeout, increase resendTimeout to 2x rtt
	if (2 * rttMs < resendTimeout) {
	  resendTimeout -= (int)(0.1 * resendTimeout);
	} else {
	  resendTimeout = 2 * rttMs;
	}

	if (resendTimeout < 20) {
	  resendTimeout = 20;
	}

	qDebug() << "New resend timeout:" << resendTimeout;

	delete rtTimers[ackedType];
	rtTimers[ackedType] = NULL;
  } else {
	  qWarning() << "No RT timer running for type" << ackedType;
  }

  // Delete the message waiting for resend
  if (resendMessages[ackedType]) {
	delete resendMessages[ackedType];
	resendMessages[ackedType] = NULL;
  }
}



void Transmitter::handlePing(Message &)
{
  qDebug() << "in" << __FUNCTION__;

  // We don't do anything with ping (ACKing it is enough).
  // This handler is here to avoid missing handler warning.
}



void Transmitter::handleStats(Message &msg)
{
  qDebug() << "in" << __FUNCTION__;

  QList <int> stats;
  
  // FIXME: no hardcoded limit
  for (int i = 0; i < 3; i++) {
	// FIXME: no hardcoded offset
	stats.append((int)((uint8_t)msg.data()->at(2 + i)));
  }

  // FIXME: no hardcoded indexes
  emit(uptime(stats[0] * 60)); // Uptime is sent as minutes, but
							   // seconds is normal representation
  emit(loadAvg(float(stats[1]) / 10)); // Load avg is sent 10x
  emit(wlan(stats[2]));        // WLAN signal is 0-100%
}

