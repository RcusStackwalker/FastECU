#include "serial_backend_host.h"

#include <QCoreApplication>
#include "serial_backend.h"

SerialBackendHost::SerialBackendHost()
{
    m_thread.setObjectName("SerialIoThread");
    m_context = new QObject();
    m_context->moveToThread(&m_thread);
    m_thread.start();
}

SerialBackendHost::~SerialBackendHost()
{
    Q_ASSERT(QThread::currentThread() != &m_thread);
    if (m_backend)
    {
        SerialBackend *b = m_backend;
        m_backend = nullptr;
        QMetaObject::invokeMethod(m_context, [b]
                                  { delete b; }, Qt::BlockingQueuedConnection);
    }
    m_thread.quit();
    m_thread.wait();
    delete m_context; // safe: its thread has finished
}

SerialBackend *SerialBackendHost::createBackend(const std::function<SerialBackend *()>& factory)
{
    QMetaObject::invokeMethod(m_context, [this, &factory]
                              { m_backend = factory(); }, Qt::BlockingQueuedConnection);
    return m_backend;
}
