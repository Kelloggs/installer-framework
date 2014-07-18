/**************************************************************************
**
** Copyright (C) 2014 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the Qt Installer Framework.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
**************************************************************************/

#include "console.h"

# include <qt_windows.h>
# include <wincon.h>


# ifndef ENABLE_INSERT_MODE
#  define ENABLE_INSERT_MODE 0x0020
# endif

# ifndef ENABLE_QUICK_EDIT_MODE
#  define ENABLE_QUICK_EDIT_MODE 0x0040
# endif

# ifndef ENABLE_EXTENDED_FLAGS
#  define ENABLE_EXTENDED_FLAGS 0x0080
# endif


Console::Console()
{
    parentConsole = AttachConsole(ATTACH_PARENT_PROCESS);
    if (parentConsole)
        return;

    AllocConsole();
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle != INVALID_HANDLE_VALUE) {
        COORD largestConsoleWindowSize = GetLargestConsoleWindowSize(handle);
        largestConsoleWindowSize.X -= 3;
        largestConsoleWindowSize.Y = 5000;
        SetConsoleScreenBufferSize(handle, largestConsoleWindowSize);
    }

    handle = GetStdHandle(STD_INPUT_HANDLE);
    if (handle != INVALID_HANDLE_VALUE)
        SetConsoleMode(handle, ENABLE_INSERT_MODE | ENABLE_QUICK_EDIT_MODE | ENABLE_EXTENDED_FLAGS);

    m_oldCin = std::cin.rdbuf();
    m_newCin.open("CONIN$");
    std::cin.rdbuf(m_newCin.rdbuf());

    m_oldCout = std::cout.rdbuf();
    m_newCout.open("CONOUT$");
    std::cout.rdbuf(m_newCout.rdbuf());

    m_oldCerr = std::cerr.rdbuf();
    m_newCerr.open("CONOUT$");
    std::cerr.rdbuf(m_newCerr.rdbuf());
# ifndef Q_CC_MINGW
    HMENU systemMenu = GetSystemMenu(GetConsoleWindow(), FALSE);
    if (systemMenu != NULL)
        RemoveMenu(systemMenu, SC_CLOSE, MF_BYCOMMAND);
    DrawMenuBar(GetConsoleWindow());
# endif
}

Console::~Console()
{
    if (!parentConsole) {
        system("PAUSE");

        std::cin.rdbuf(m_oldCin);
        std::cerr.rdbuf(m_oldCerr);
        std::cout.rdbuf(m_oldCout);
    }

    FreeConsole();
}