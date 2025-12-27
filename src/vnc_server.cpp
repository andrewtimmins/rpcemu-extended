/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2025 Andrew Timmins

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifdef RPCEMU_VNC

#include <cstring>
#include <QHostAddress>

#include "vnc_server.h"
#include "rpc-qt5.h"
#include "keyboard.h"

// Global VNC server instance
VncServer *g_vncServer = nullptr;

// Keysym to Acorn scan code mapping table
// Maps X11/VNC keysyms to Acorn keyboard scan codes
// Acorn uses a row/column encoding

// Forward declarations of callbacks
void vnc_kbd_callback(rfbBool down, rfbKeySym keysym, rfbClientPtr cl);
void vnc_ptr_callback(int buttonMask, int x, int y, rfbClientPtr cl);
enum rfbNewClientAction vnc_new_client_callback(rfbClientPtr cl);
void vnc_client_gone_callback(rfbClientPtr cl);

VncServer::VncServer(Emulator *emulator, QObject *parent)
    : QObject(parent)
    , emulator(emulator)
    , rfbScreen(nullptr)
    , eventTimer(nullptr)
    , currentWidth(640)
    , currentHeight(480)
    , listenPort(5900)
    , running(false)
{
    clientCount.storeRelease(0);

    // Set up event processing timer
    eventTimer = new QTimer(this);
    eventTimer->setInterval(20); // 50 Hz - process VNC events
    connect(eventTimer, &QTimer::timeout, this, &VncServer::processEvents);

    // Connect input injection signals to emulator slots
    connect(this, &VncServer::injectKeyPress,
            emulator, [emulator](unsigned int scanCode) {
                emit emulator->key_press_signal(scanCode);
            }, Qt::QueuedConnection);

    connect(this, &VncServer::injectKeyRelease,
            emulator, [emulator](unsigned int scanCode) {
                emit emulator->key_release_signal(scanCode);
            }, Qt::QueuedConnection);

    connect(this, &VncServer::injectMouseMove,
            emulator, [emulator](int x, int y) {
                emit emulator->mouse_move_signal(x, y);
            }, Qt::QueuedConnection);

    connect(this, &VncServer::injectMouseButton,
            emulator, [emulator](int buttons) {
                // VNC uses: 1=left, 2=middle, 4=right
                // Acorn uses same bit pattern
                static int lastButtons = 0;
                int pressed = buttons & ~lastButtons;
                int released = lastButtons & ~buttons;

                if (pressed) {
                    emit emulator->mouse_press_signal(pressed);
                }
                if (released) {
                    emit emulator->mouse_release_signal(released);
                }
                lastButtons = buttons;
            }, Qt::QueuedConnection);
}

VncServer::~VncServer()
{
    stop();
}

bool VncServer::start(int port, const QString &password)
{
    QMutexLocker locker(&mutex);

    if (running) {
        return true; // Already running
    }

    // Store password for the duration of the server
    currentPassword = password;
    passwordList[0] = nullptr;
    passwordList[1] = nullptr;

    // Create the VNC screen
    int argc = 0;
    char *argv[] = { nullptr };
    
    rfbScreen = rfbGetScreen(&argc, argv, currentWidth, currentHeight, 8, 3, 4);
    if (!rfbScreen) {
        qWarning("VNC: Failed to create RFB screen");
        return false;
    }

    // Configure the screen
    rfbScreen->desktopName = "RPCEmu - RISC OS";
    rfbScreen->alwaysShared = TRUE;
    rfbScreen->port = port;
    rfbScreen->ipv6port = port;

    // Allocate framebuffer
    rfbScreen->frameBuffer = (char *)malloc(currentWidth * currentHeight * 4);
    if (!rfbScreen->frameBuffer) {
        rfbScreenCleanup(rfbScreen);
        rfbScreen = nullptr;
        qWarning("VNC: Failed to allocate framebuffer");
        return false;
    }
    memset(rfbScreen->frameBuffer, 0, currentWidth * currentHeight * 4);

    // Set pixel format - RGBX (matches Qt's RGB32)
    rfbScreen->serverFormat.redShift = 16;
    rfbScreen->serverFormat.greenShift = 8;
    rfbScreen->serverFormat.blueShift = 0;
    rfbScreen->serverFormat.redMax = 255;
    rfbScreen->serverFormat.greenMax = 255;
    rfbScreen->serverFormat.blueMax = 255;
    rfbScreen->serverFormat.bitsPerPixel = 32;
    rfbScreen->serverFormat.depth = 24;

    // Set callbacks
    rfbScreen->kbdAddEvent = vnc_kbd_callback;
    rfbScreen->ptrAddEvent = vnc_ptr_callback;
    rfbScreen->newClientHook = vnc_new_client_callback;

    // Store 'this' pointer for callbacks
    rfbScreen->screenData = this;

    // Configure password authentication if password is set
    if (!currentPassword.isEmpty()) {
        QByteArray pwdBytes = currentPassword.toUtf8();
        passwordList[0] = strdup(pwdBytes.constData());
        passwordList[1] = nullptr;
        rfbScreen->authPasswdData = passwordList;
        rfbScreen->passwordCheck = rfbCheckPasswordByList;
        qInfo("VNC: Password authentication enabled");
    } else {
        rfbScreen->authPasswdData = nullptr;
        qInfo("VNC: No password set - authentication disabled");
    }

    // Initialize the server
    rfbInitServer(rfbScreen);

    listenPort = port;
    running = true;

    // Start event processing
    eventTimer->start();

    qInfo("VNC: Server started on port %d", port);
    emit statusChanged(true, port);

    return true;
}

void VncServer::stop()
{
    QMutexLocker locker(&mutex);

    if (!running) {
        return;
    }

    eventTimer->stop();

    if (rfbScreen) {
        rfbShutdownServer(rfbScreen, TRUE);
        if (rfbScreen->frameBuffer) {
            free(rfbScreen->frameBuffer);
        }
        rfbScreenCleanup(rfbScreen);
        rfbScreen = nullptr;
    }

    // Free password if allocated
    if (passwordList[0]) {
        free(passwordList[0]);
        passwordList[0] = nullptr;
    }
    currentPassword.clear();

    running = false;
    clientCount.storeRelease(0);

    qInfo("VNC: Server stopped");
    emit statusChanged(false, 0);
}

bool VncServer::isRunning() const
{
    return running;
}

int VncServer::getPort() const
{
    return running ? listenPort : 0;
}

int VncServer::getClientCount() const
{
    return clientCount.loadAcquire();
}

void VncServer::updateFramebuffer(const uint32_t *buffer, int width, int height, int yl, int yh)
{
    if (!running || !rfbScreen || !buffer) {
        return;
    }

    QMutexLocker locker(&mutex);

    // Check if we need to resize
    if (width != currentWidth || height != currentHeight) {
        if (!resizeFramebuffer(width, height)) {
            return;
        }
    }

    if (!rfbScreen->frameBuffer) {
        return;
    }

    // Copy the dirty region
    // Note: Both buffers are RGB32/XRGB format, so direct copy works
    int bytesPerRow = width * 4;
    int startY = qMax(0, yl);
    int endY = qMin(height, yh + 1);

    for (int y = startY; y < endY; y++) {
        memcpy(rfbScreen->frameBuffer + (y * bytesPerRow),
               buffer + (y * width),
               bytesPerRow);
    }

    // Mark the region as modified
    rfbMarkRectAsModified(rfbScreen, 0, startY, width, endY);
}

void VncServer::processEvents()
{
    if (!running || !rfbScreen) {
        return;
    }

    // Process VNC events with a short timeout (non-blocking)
    rfbProcessEvents(rfbScreen, 0);
}

bool VncServer::resizeFramebuffer(int width, int height)
{
    // This is called with mutex held
    if (!rfbScreen) {
        return false;
    }

    qInfo("VNC: Resizing framebuffer to %dx%d", width, height);

    char *newBuffer = (char *)realloc(rfbScreen->frameBuffer, width * height * 4);
    if (!newBuffer) {
        qWarning("VNC: Failed to resize framebuffer");
        return false;
    }

    rfbScreen->frameBuffer = newBuffer;
    memset(rfbScreen->frameBuffer, 0, width * height * 4);

    // Update screen dimensions
    rfbNewFramebuffer(rfbScreen, rfbScreen->frameBuffer, width, height, 8, 3, 4);

    currentWidth = width;
    currentHeight = height;

    return true;
}

// Map VNC/X11 keysym to Acorn scan code
// This is a simplified mapping - full implementation would need complete table
unsigned int VncServer::keysymToScanCode(rfbKeySym keysym)
{
    // Basic ASCII mapping
    // Acorn keyboard scan codes are based on the keyboard matrix position
    // For now, we'll use a simple direct mapping that works for most keys

    switch (keysym) {
        // Function keys
        case XK_Escape:     return 0x00;
        case XK_F1:         return 0x01;
        case XK_F2:         return 0x02;
        case XK_F3:         return 0x03;
        case XK_F4:         return 0x04;
        case XK_F5:         return 0x05;
        case XK_F6:         return 0x06;
        case XK_F7:         return 0x07;
        case XK_F8:         return 0x08;
        case XK_F9:         return 0x09;
        case XK_F10:        return 0x0a;
        case XK_F11:        return 0x0b;
        case XK_F12:        return 0x0c;
        case XK_Print:      return 0x0d;
        case XK_Scroll_Lock: return 0x0e;
        case XK_Pause:      return 0x0f;

        // Number row
        case XK_grave:
        case XK_asciitilde: return 0x10;
        case XK_1:
        case XK_exclam:     return 0x11;
        case XK_2:
        case XK_at:         return 0x12;
        case XK_3:
        case XK_numbersign: return 0x13;
        case XK_4:
        case XK_dollar:     return 0x14;
        case XK_5:
        case XK_percent:    return 0x15;
        case XK_6:
        case XK_asciicircum: return 0x16;
        case XK_7:
        case XK_ampersand:  return 0x17;
        case XK_8:
        case XK_asterisk:   return 0x18;
        case XK_9:
        case XK_parenleft:  return 0x19;
        case XK_0:
        case XK_parenright: return 0x1a;
        case XK_minus:
        case XK_underscore: return 0x1b;
        case XK_equal:
        case XK_plus:       return 0x1c;
        case XK_sterling:   return 0x1d; // £ key on UK keyboard
        case XK_BackSpace:  return 0x1e;
        case XK_Insert:     return 0x1f;

        // QWERTY row
        case XK_Tab:        return 0x26;
        case XK_q:
        case XK_Q:          return 0x27;
        case XK_w:
        case XK_W:          return 0x28;
        case XK_e:
        case XK_E:          return 0x29;
        case XK_r:
        case XK_R:          return 0x2a;
        case XK_t:
        case XK_T:          return 0x2b;
        case XK_y:
        case XK_Y:          return 0x2c;
        case XK_u:
        case XK_U:          return 0x2d;
        case XK_i:
        case XK_I:          return 0x2e;
        case XK_o:
        case XK_O:          return 0x2f;
        case XK_p:
        case XK_P:          return 0x30;
        case XK_bracketleft:
        case XK_braceleft:  return 0x31;
        case XK_bracketright:
        case XK_braceright: return 0x32;
        case XK_backslash:
        case XK_bar:        return 0x33;
        case XK_Delete:     return 0x34;
        case XK_End:        return 0x36;
        case XK_Page_Down:  return 0x37;

        // ASDF row
        case XK_Caps_Lock:  return 0x3c;
        case XK_a:
        case XK_A:          return 0x3d;
        case XK_s:
        case XK_S:          return 0x3e;
        case XK_d:
        case XK_D:          return 0x3f;
        case XK_f:
        case XK_F:          return 0x40;
        case XK_g:
        case XK_G:          return 0x41;
        case XK_h:
        case XK_H:          return 0x42;
        case XK_j:
        case XK_J:          return 0x43;
        case XK_k:
        case XK_K:          return 0x44;
        case XK_l:
        case XK_L:          return 0x45;
        case XK_semicolon:
        case XK_colon:      return 0x46;
        case XK_apostrophe:
        case XK_quotedbl:   return 0x47;
        case XK_Return:     return 0x48;
        case XK_Home:       return 0x20;
        case XK_Page_Up:    return 0x21;
        case XK_Num_Lock:   return 0x22;

        // ZXCV row
        case XK_Shift_L:    return 0x4c;
        case XK_z:
        case XK_Z:          return 0x4e;
        case XK_x:
        case XK_X:          return 0x4f;
        case XK_c:
        case XK_C:          return 0x50;
        case XK_v:
        case XK_V:          return 0x51;
        case XK_b:
        case XK_B:          return 0x52;
        case XK_n:
        case XK_N:          return 0x53;
        case XK_m:
        case XK_M:          return 0x54;
        case XK_comma:
        case XK_less:       return 0x55;
        case XK_period:
        case XK_greater:    return 0x56;
        case XK_slash:
        case XK_question:   return 0x57;
        case XK_Shift_R:    return 0x58;
        case XK_Up:         return 0x59;

        // Bottom row
        case XK_Control_L:  return 0x5c;
        case XK_Alt_L:      return 0x5e;
        case XK_space:      return 0x5f;
        case XK_Alt_R:      return 0x60;
        case XK_Control_R:  return 0x61;
        case XK_Left:       return 0x62;
        case XK_Down:       return 0x63;
        case XK_Right:      return 0x64;

        // Numpad
        case XK_KP_Divide:  return 0x23;
        case XK_KP_Multiply: return 0x24;
        case XK_KP_Subtract: return 0x25; // Actually hash on Acorn
        case XK_KP_7:
        case XK_KP_Home:    return 0x38;
        case XK_KP_8:
        case XK_KP_Up:      return 0x39;
        case XK_KP_9:
        case XK_KP_Page_Up: return 0x3a;
        case XK_KP_Add:     return 0x3b;
        case XK_KP_4:
        case XK_KP_Left:    return 0x48; // Reused
        case XK_KP_5:       return 0x49;
        case XK_KP_6:
        case XK_KP_Right:   return 0x4a;
        case XK_KP_1:
        case XK_KP_End:     return 0x5a;
        case XK_KP_2:
        case XK_KP_Down:    return 0x5b;
        case XK_KP_3:
        case XK_KP_Page_Down: return 0x65;
        case XK_KP_Enter:   return 0x67;
        case XK_KP_0:
        case XK_KP_Insert:  return 0x68;
        case XK_KP_Decimal:
        case XK_KP_Delete:  return 0x69;

        default:
            // For unmapped keys, return invalid code
            return 0xff;
    }
}

// VNC keyboard callback
void vnc_kbd_callback(rfbBool down, rfbKeySym keysym, rfbClientPtr cl)
{
    VncServer *server = static_cast<VncServer *>(cl->screen->screenData);
    if (!server) {
        return;
    }

    unsigned int scanCode = server->keysymToScanCode(keysym);
    if (scanCode == 0xff) {
        // Unmapped key
        return;
    }

    if (down) {
        emit server->injectKeyPress(scanCode);
    } else {
        emit server->injectKeyRelease(scanCode);
    }
}

// VNC pointer (mouse) callback
void vnc_ptr_callback(int buttonMask, int x, int y, rfbClientPtr cl)
{
    VncServer *server = static_cast<VncServer *>(cl->screen->screenData);
    if (!server) {
        return;
    }

    // Send mouse position
    emit server->injectMouseMove(x, y);

    // Send button state
    // VNC buttons: 1=left, 2=middle, 4=right
    // This matches Acorn's button encoding
    emit server->injectMouseButton(buttonMask);
}

// VNC new client callback
enum rfbNewClientAction vnc_new_client_callback(rfbClientPtr cl)
{
    VncServer *server = static_cast<VncServer *>(cl->screen->screenData);
    if (!server) {
        return RFB_CLIENT_REFUSE;
    }

    // Set up client gone callback
    cl->clientGoneHook = vnc_client_gone_callback;

    // Increment client count
    server->clientCount.fetchAndAddRelease(1);

    // Get client address
    QString address;
    if (cl->host) {
        address = QString::fromLatin1(cl->host);
    }

    qInfo("VNC: Client connected from %s", qPrintable(address));
    emit server->clientConnected(address);

    return RFB_CLIENT_ACCEPT;
}

// VNC client disconnection callback
void vnc_client_gone_callback(rfbClientPtr cl)
{
    VncServer *server = static_cast<VncServer *>(cl->screen->screenData);
    if (!server) {
        return;
    }

    // Decrement client count
    server->clientCount.fetchAndAddRelease(-1);

    // Get client address
    QString address;
    if (cl->host) {
        address = QString::fromLatin1(cl->host);
    }

    qInfo("VNC: Client disconnected from %s", qPrintable(address));
    emit server->clientDisconnected(address);
}

#endif // RPCEMU_VNC