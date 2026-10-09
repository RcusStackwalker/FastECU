#include "src/platform/desktop/common/serial/serial_backend_host.h"

#include <QCoreApplication>
#include "src/platform/desktop/common/serial/serial_backend.h"

SerialBackendHost::SerialBackendHost()
{
    m_thread_.setObjectName("SerialIoThread");
    m_context_ = new QObject();
    m_context_->moveToThread(&m_thread_);
    m_thread_.start();
}

SerialBackendHost::~SerialBackendHost()
{
    Q_ASSERT(QThread::currentThread() != &m_thread_);
    if (m_backend_)
    {
        SerialBackend *b = m_backend_;
        m_backend_ = nullptr;
        QMetaObject::invokeMethod(m_context_, [b] { delete b; }, Qt::BlockingQueuedConnection);
    }
    m_thread_.quit();
    m_thread_.wait();
    delete m_context_; // safe: its thread has finished
}

SerialBackend *SerialBackendHost::createBackend(const std::function<SerialBackend *()>& factory)
{
    QMetaObject::invokeMethod(m_context_, [this, &factory] { m_backend_ = factory(); }, Qt::BlockingQueuedConnection);
    return m_backend_;
}
