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

#ifndef VNC_SERVER_H
#define VNC_SERVER_H

#ifdef RPCEMU_VNC

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QImage>
#include <QTimer>
#include <QAtomicInt>

#include <rfb/rfb.h>
#include <rfb/keysym.h>

class Emulator;

/**
 * VNC Server wrapper class
 * 
 * Provides remote desktop access to the emulated RISC OS display.
 * Uses libvncserver for the RFB protocol implementation.
 */
class VncServer : public QObject
{
    Q_OBJECT

public:
    explicit VncServer(Emulator *emulator, QObject *parent = nullptr);
    ~VncServer();

    /**
     * Start the VNC server on the specified port
     * @param port TCP port to listen on (default 5900)
     * @param password Optional password for authentication (empty = no auth)
     * @return true on success
     */
    bool start(int port = 5900, const QString &password = QString());

    /**
     * Stop the VNC server
     */
    void stop();

    /**
     * Check if the VNC server is running
     * @return true if running
     */
    bool isRunning() const;

    /**
     * Get the port the VNC server is listening on
     * @return port number, or 0 if not running
     */
    int getPort() const;

    /**
     * Get the number of connected clients
     * @return client count
     */
    int getClientCount() const;

    /**
     * Update the framebuffer from the emulator
     * Called from the video update path
     * @param buffer Pointer to RGB32 pixel data
     * @param width Image width
     * @param height Image height
     * @param yl Y low of dirty region
     * @param yh Y high of dirty region
     */
    void updateFramebuffer(const uint32_t *buffer, int width, int height, int yl, int yh);

signals:
    /**
     * Emitted when a client connects
     * @param clientAddress IP address of connecting client
     */
    void clientConnected(const QString &clientAddress);

    /**
     * Emitted when a client disconnects
     * @param clientAddress IP address of disconnecting client
     */
    void clientDisconnected(const QString &clientAddress);

    /**
     * Emitted when server status changes
     * @param running true if server is now running
     * @param port port number if running
     */
    void statusChanged(bool running, int port);

    // Internal signals for thread-safe input injection
    void injectKeyPress(unsigned int scanCode);
    void injectKeyRelease(unsigned int scanCode);
    void injectMouseMove(int x, int y);
    void injectMouseButton(int buttonMask);

private slots:
    void processEvents();

private:
    // libvncserver callback friends
    friend void vnc_kbd_callback(rfbBool down, rfbKeySym keysym, rfbClientPtr cl);
    friend void vnc_ptr_callback(int buttonMask, int x, int y, rfbClientPtr cl);
    friend enum rfbNewClientAction vnc_new_client_callback(rfbClientPtr cl);
    friend void vnc_client_gone_callback(rfbClientPtr cl);

    // Convert X11 keysym to Acorn scan code
    unsigned int keysymToScanCode(rfbKeySym keysym);

    // Resize the VNC framebuffer if needed
    bool resizeFramebuffer(int width, int height);

    Emulator *emulator;
    rfbScreenInfoPtr rfbScreen;
    QTimer *eventTimer;
    QMutex mutex;

    int currentWidth;
    int currentHeight;
    int listenPort;
    QAtomicInt clientCount;
    bool running;
    char *passwordList[2];  // For libvncserver auth
    QString currentPassword;
};

// Global VNC server instance (set when VNC is enabled)
extern VncServer *g_vncServer;

#endif // RPCEMU_VNC

#endif // VNC_SERVER_H
