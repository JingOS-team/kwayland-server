/********************************************************************
KWin - the KDE window manager
This file is part of the KDE project.

Copyright (C) 2014 Martin Gräßlin <mgraesslin@kde.org>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*********************************************************************/
// Qt
#include <QtTest/QtTest>
// KWin
#include "../../wayland_client/connection_thread.h"
// Wayland
#include <wayland-client-protocol.h>

class TestWaylandConnectionThread : public QObject
{
    Q_OBJECT
public:
    explicit TestWaylandConnectionThread(QObject *parent = nullptr);
private Q_SLOTS:
    void init();
    void cleanup();

    void testInitConnectionNoThread();
    void testConnectionFailure();
    void testConnectionDieing();
    void testConnectionThread();

private:
    QProcess *m_westonProcess;
};

static const QString s_socketName = QStringLiteral("kwin-test-wayland-connection-0");

TestWaylandConnectionThread::TestWaylandConnectionThread(QObject *parent)
    : QObject(parent)
    , m_westonProcess(nullptr)
{
}

void TestWaylandConnectionThread::init()
{
    QVERIFY(!m_westonProcess);
    // starts weston
    m_westonProcess = new QProcess(this);
    m_westonProcess->setProgram(QStringLiteral("weston"));

    m_westonProcess->setArguments(QStringList({QStringLiteral("--socket=%1").arg(s_socketName), QStringLiteral("--use-pixman")}));
    m_westonProcess->start();
    QVERIFY(m_westonProcess->waitForStarted());

    // wait for the socket to appear
    QDir runtimeDir(qgetenv("XDG_RUNTIME_DIR"));
    if (runtimeDir.exists(s_socketName)) {
        return;
    }
    QFileSystemWatcher *socketWatcher = new QFileSystemWatcher(QStringList({runtimeDir.absolutePath()}), this);
    QSignalSpy socketSpy(socketWatcher, SIGNAL(directoryChanged(QString)));

    // limit to maximum of 10 waits
    for (int i = 0; i < 10; ++i) {
        QVERIFY(socketSpy.wait());
        if (runtimeDir.exists(s_socketName)) {
            delete socketWatcher;
            return;
        }
    }
}

void TestWaylandConnectionThread::cleanup()
{
    // terminates weston
    m_westonProcess->terminate();
    QVERIFY(m_westonProcess->waitForFinished());
    delete m_westonProcess;
    m_westonProcess = nullptr;
}

void TestWaylandConnectionThread::testInitConnectionNoThread()
{
    if (m_westonProcess->state() != QProcess::Running) {
        QSKIP("This test requires a running wayland server");
    }
    QScopedPointer<KWin::Wayland::ConnectionThread> connection(new KWin::Wayland::ConnectionThread);
    QCOMPARE(connection->socketName(), QStringLiteral("wayland-0"));
    connection->setSocketName(s_socketName);
    QCOMPARE(connection->socketName(), s_socketName);

    QSignalSpy connectedSpy(connection.data(), SIGNAL(connected()));
    QSignalSpy failedSpy(connection.data(), SIGNAL(failed()));
    connection->initConnection();
    QVERIFY(connectedSpy.wait());
    QCOMPARE(connectedSpy.count(), 1);
    QCOMPARE(failedSpy.count(), 0);
    QVERIFY(connection->display());
}

void TestWaylandConnectionThread::testConnectionFailure()
{
    if (m_westonProcess->state() != QProcess::Running) {
        QSKIP("This test requires a running wayland server");
    }
    QScopedPointer<KWin::Wayland::ConnectionThread> connection(new KWin::Wayland::ConnectionThread);
    connection->setSocketName(QStringLiteral("kwin-test-socket-does-not-exist"));

    QSignalSpy connectedSpy(connection.data(), SIGNAL(connected()));
    QSignalSpy failedSpy(connection.data(), SIGNAL(failed()));
    connection->initConnection();
    QVERIFY(failedSpy.wait());
    QCOMPARE(connectedSpy.count(), 0);
    QCOMPARE(failedSpy.count(), 1);
    QVERIFY(!connection->display());
}

static void registryHandleGlobal(void *data, struct wl_registry *registry,
                                 uint32_t name, const char *interface, uint32_t version)
{
    Q_UNUSED(data)
    Q_UNUSED(registry)
    Q_UNUSED(name)
    Q_UNUSED(interface)
    Q_UNUSED(version)
}

static void registryHandleGlobalRemove(void *data, struct wl_registry *registry, uint32_t name)
{
    Q_UNUSED(data)
    Q_UNUSED(registry)
    Q_UNUSED(name)
}

static const struct wl_registry_listener s_registryListener = {
    registryHandleGlobal,
    registryHandleGlobalRemove
};

void TestWaylandConnectionThread::testConnectionThread()
{
    if (m_westonProcess->state() != QProcess::Running) {
        QSKIP("This test requires a running wayland server");
    }
    QScopedPointer<KWin::Wayland::ConnectionThread> connection(new KWin::Wayland::ConnectionThread);
    connection->setSocketName(s_socketName);

    QThread *connectionThread = new QThread(this);
    connection->moveToThread(connectionThread);
    connectionThread->start();

    QSignalSpy connectedSpy(connection.data(), SIGNAL(connected()));
    QSignalSpy failedSpy(connection.data(), SIGNAL(failed()));
    connection->initConnection();
    QVERIFY(connectedSpy.wait());
    QCOMPARE(connectedSpy.count(), 1);
    QCOMPARE(failedSpy.count(), 0);
    QVERIFY(connection->display());

    // now we have the connection ready, let's get some events
    QSignalSpy eventsSpy(connection.data(), SIGNAL(eventsRead()));
    wl_display *display = connection->display();
    wl_event_queue *queue = wl_display_create_queue(display);

    wl_registry *registry = wl_display_get_registry(display);
    wl_proxy_set_queue((wl_proxy*)registry, queue);

    wl_registry_add_listener(registry, &s_registryListener, this);
    wl_display_flush(display);

    QVERIFY(eventsSpy.wait());

    wl_registry_destroy(registry);
    wl_event_queue_destroy(queue);

    connectionThread->quit();
    connectionThread->wait();
    delete connectionThread;
}

void TestWaylandConnectionThread::testConnectionDieing()
{
    if (m_westonProcess->state() != QProcess::Running) {
        QSKIP("This test requires a running wayland server");
    }
    QScopedPointer<KWin::Wayland::ConnectionThread> connection(new KWin::Wayland::ConnectionThread);
    QSignalSpy connectedSpy(connection.data(), SIGNAL(connected()));
    connection->setSocketName(s_socketName);
    connection->initConnection();
    QVERIFY(connectedSpy.wait());
    QVERIFY(connection->display());

    QSignalSpy diedSpy(connection.data(), SIGNAL(connectionDied()));
    m_westonProcess->terminate();
    QVERIFY(m_westonProcess->waitForFinished());
    QVERIFY(diedSpy.wait());
    QCOMPARE(diedSpy.count(), 1);
    QVERIFY(!connection->display());

    connectedSpy.clear();
    QVERIFY(connectedSpy.isEmpty());
    // restarts the server
    delete m_westonProcess;
    m_westonProcess = nullptr;
    init();
    if (connectedSpy.count() == 0) {
        QVERIFY(connectedSpy.wait());
    }
    QCOMPARE(connectedSpy.count(), 1);
}

QTEST_MAIN(TestWaylandConnectionThread)
#include "test_wayland_connection_thread.moc"
