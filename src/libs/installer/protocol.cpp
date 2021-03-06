/**************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
**
** This file is part of the Qt Installer Framework.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file. Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** As a special exception, The Qt Company gives you certain additional
** rights. These rights are described in The Qt Company LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
**
** $QT_END_LICENSE$
**
**************************************************************************/

#include "protocol.h"
#include <QIODevice>

namespace QInstaller {

typedef qint32 PackageSize;

/*!
    Write a packet containing \a command and \a data to \a device.

    \note Both client and server need to have the same endianness.
 */
void sendPacket(QIODevice *device, const QByteArray &command, const QByteArray &data)
{
    // use aliasing for writing payload size into bytes
    char payloadBytes[sizeof(PackageSize)];
    PackageSize *payloadSize = reinterpret_cast<PackageSize*>(&payloadBytes);
    *payloadSize = command.size() + sizeof(char) + data.size();

    QByteArray packet;
    packet.reserve(sizeof(PackageSize) + *payloadSize);
    packet.append(payloadBytes, sizeof(PackageSize));
    packet.append(command);
    packet.append('\0');
    packet.append(data);

    forever {
        const int bytesWritten = device->write(packet);
        Q_ASSERT(bytesWritten >= 0);
        if (bytesWritten == packet.size())
            break;
        packet.remove(0, bytesWritten);
    }
    // needed for big packages over TCP on Windows
    device->waitForBytesWritten(-1);
}

/*!
    Reads a packet from \a device, and stores its content into \a command and \a data.

    Returns \c false if the packet in the device buffer is yet incomplete, \c true otherwise.

    \note Both client and server need to have the same endianness.
 */
bool receivePacket(QIODevice *device, QByteArray *command, QByteArray *data)
{
    if (device->bytesAvailable() < static_cast<qint64>(sizeof(PackageSize)))
        return false;

    // read payload size
    char payloadBytes[sizeof(PackageSize)];
    PackageSize *payloadSize = reinterpret_cast<PackageSize*>(&payloadBytes);
    device->read(payloadBytes, sizeof(PackageSize));

    // not enough data yet? back off ...
    if (device->bytesAvailable() < *payloadSize) {
        for (int i = sizeof(PackageSize) - 1; i >= 0; --i)
            device->ungetChar(payloadBytes[i]);
        return false;
    }

    const QByteArray payload = device->read(*payloadSize);
    int separator = payload.indexOf('\0');

    *command = payload.left(separator);
    *data = payload.right(payload.size() - separator - 1);
    return true;
}

} // namespace QInstaller
