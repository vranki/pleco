#include <QCoreApplication>
#include <QStringList>

#include "Transmitter.h"
#include "Slave.h"

int main(int argc, char *argv[])
{
  Slave slave(argc, argv);

  QStringList args = QCoreApplication::arguments();

  if (args.contains("--help")
   || args.contains("-h")) {
    printf("Usage: %s [ip of relay server]\n",
      qPrintable(QFileInfo(argv[0]).baseName()));
    return 0;
  }

  if (!slave.init()) {
	qFatal("Failed to init slave class. Exiting..");
	return 1;
  }

  QString relay = "127.0.0.1";
  if (args.length() > 1) {
	relay = args.at(1);
  }

  QHostInfo info = QHostInfo::fromName(relay);

  if (info.addresses().isEmpty()) {
	qWarning() << "Failed to get IP for" << relay;
	return 0;
  }

  QHostAddress address = info.addresses().first();

  slave.connect(address.toString(), 8500);

  return slave.exec();
}
