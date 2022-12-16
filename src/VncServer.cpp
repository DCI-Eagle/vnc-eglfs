/******************************************************************************
 * VncEGLFS - Copyright (C) 2022 Uwe Rathmann
 * This file may be used under the terms of the 3-clause BSD License
 *****************************************************************************/

#include "VncServer.h"
#include "VncClient.h"

#include <qtcpserver.h>
#include <qopenglcontext.h>
#include <qopenglfunctions.h>
#include <qwindow.h>
#include <qthread.h>
#include <qelapsedtimer.h>
#include <qloggingcategory.h>

#include <qpa/qplatformcursor.h>

Q_LOGGING_CATEGORY( logGrab, "vnceglfs.grab", QtCriticalMsg )
Q_LOGGING_CATEGORY( logConnection, "vnceglfs.connection" )

/*
	There is an issue with EGL and Qt6 which makes it really really difficult
	to grab the working framebuffer. In Qt5, the rendering was done directly
	to a framebuffer, so it was possible to call glReadPixels in afterRendering
	and the completely-rendered, about-to-be-swapped frame was present. In Qt6,
	the scene graph manages the rendering pipeline and so the rendering happens
	in a different buffer, which we have no visibility into. Furthermore, it's
	not clear if the scene graph double-buffers or not. The consequence of all
	this is that in Qt 6, when the VNC server calls glReadPixels, it will read
	stale frame data -- one frame delayed, due to double-buffering.
	
	In experimentation, doing a glReadPixels after frameSwapped (instead of
	afterRendering) did work, although this is highly driver dependent and
	requires the following configuration (which may or may not be the default):
		- EGL_SWAP_BEHAVIOR_PRESERVED_BIT bit set for EGL_SURFACE_TYPE attribute
		when calling eglChooseConfig
		- EGL_SWAP_BEHAVIOR attribute set for EGL_BUFFER_PRESERVED when calling
		eglSurfaceAttrib
	Qt doesn't do either of the above, nor is it accessible, so we are basically
	at the mercy of whatever the driver wants to do.
	
	If the above works, excellent, but if not, there is one other workaround,
	which is to render the entire scene graph again into an offscreen buffer.
	The performance tradeoff will need to be analyzed in the application to see
	if this is faster or slower than glReadPixels. For simple scenes, it is
	probably faster to render the scene graph again.
	
	For the record, the Qt VNC Server commerical add-on for Qt 6.4 does the
	latter option (at least, that's what was gleaned from the API, which
	uses QML and thus requires use of the latter). The latter option also doesn't
	require eglfs, which is probably useful for some.
	
	Either of the two workarounds above can be applied with the following
	definition.
	0 - No workaround applied.
	1 - Execute glReadPixels after frameSwapped instead of afterRendering
	2 - Call QQuickWindow::grabWindow instead of using glReadPixels
*/
#define EGL_STALE_FRAMEBUFFER_WORKAROUND 2

#if EGL_STALE_FRAMEBUFFER_WORKAROUND == 2
#include <QQuickWindow>
#endif

namespace
{
    /*
        Often EGLFS is in combination with a touch screen, where you do not
        have a cursor and all we need is a dummy cursor so that we can make
        use of the mouse in the VNC client.

        But when having a cursor, it might be updated by an OpenGl shader,
        - like Qt::WaitCursor, that is rotating constantly.

        We have to find out how to deal with this all, but for the moment
        we simply go with a workaround, that acts like when having
        static cursor images.
     */
    VncCursor createCursor( Qt::CursorShape shape )
    {
        QPlatformCursorImage platformImage( nullptr, nullptr, 0, 0, 0, 0 );
        platformImage.set( shape );

        return { *platformImage.image(), platformImage.hotspot() };
    }

#if 0
    VncCursor createCursor( const QCursor* cursor )
    {
        const auto shape = cursor ? cursor->shape() : Qt::ArrowCursor;

        if ( shape == Qt::BitmapCursor )
            return { cursor->pixmap().toImage(), cursor->hotSpot() };

        return createCursor( shape );
    }
#endif

    class TcpServer final : public QTcpServer
    {
        Q_OBJECT

      public:
        TcpServer( QObject* parent )
            : QTcpServer( parent )
        {
        }

      Q_SIGNALS:
        void connectionRequested( qintptr );

      protected:
        void incomingConnection( qintptr socketDescriptor ) override
        {
            /*
                We do not want to use QTcpServer::nextPendingConnection to avoid
                QTcpSocket being created in the wrong thread
             */
            Q_EMIT connectionRequested( socketDescriptor );
        }
    };

    class ClientThread : public QThread
    {
      public:
        ClientThread( qintptr socketDescriptor, VncServer* server )
            : QThread( server )
            , m_socketDescriptor( socketDescriptor )
        {
        }

        ~ClientThread()
        {
        }

        void markDirty()
        {
            if ( m_client )
                m_client->markDirty();
        }

        VncClient* client() const { return m_client; }

      protected:
        void run() override
        {
            VncClient client( m_socketDescriptor, qobject_cast< VncServer* >( parent() ) );
            connect( &client, &VncClient::disconnected, this, &QThread::quit );

            m_client = &client;
            QThread::run();
            m_client = nullptr;
        }

      private:
        VncClient* m_client = nullptr;
        const qintptr m_socketDescriptor;
    };
}

VncServer::VncServer( int port, QWindow* window )
    : m_window( window )
    , m_cursor( createCursor( Qt::ArrowCursor ) )
{
    Q_ASSERT( window && window->inherits( "QQuickWindow" ) );

    m_window = window;

    auto tcpServer = new TcpServer( this );
    connect( tcpServer, &TcpServer::connectionRequested, this, &VncServer::addClient );

    m_tcpServer = tcpServer;

    if( m_tcpServer->listen( QHostAddress::Any, port ) )
        qCDebug( logConnection ) << "VncServer created on port" << port;
}

VncServer::~VncServer()
{
    m_window = nullptr;

    for ( auto thread : qAsConst( m_threads ) )
    {
        thread->quit();
        thread->wait( 20 );
    }
}

int VncServer::port() const
{
    return m_tcpServer->serverPort();
}

void VncServer::addClient( qintptr fd )
{
    auto thread = new ClientThread( fd, this );
    m_threads += thread;

    if ( m_window && !m_grabConnectionId )
    {
#if EGL_STALE_FRAMEBUFFER_WORKAROUND == 1
		/*
			The framebuffer will go out-of-scope after the signal ends,
			so we must use DirectConnection here.
		*/
		m_grabConnectionId = QObject::connect( m_window, SIGNAL(frameSwapped()),
            this, SLOT(updateFrameBuffer()), Qt::DirectConnection );
#elif EGL_STALE_FRAMEBUFFER_WORKAROUND == 2
		/*
			QQuickWindow::grabWindow must be called from the GUI thread, and not
			the scene graph rendering thread, so we must use QueuedConnection.
		*/
		m_grabConnectionId = QObject::connect( m_window, SIGNAL(frameSwapped()),
            this, SLOT(updateFrameBuffer()), Qt::QueuedConnection );
#else
        /*
            afterRendering is from the scene graph thread, so we
            need a Qt::DirectConnection to avoid, that the image is
            already gone, when being scheduled from a Qt::QQueuedConnection !
         */
        m_grabConnectionId = QObject::connect( m_window, SIGNAL(afterRendering()),
            this, SLOT(updateFrameBuffer()), Qt::DirectConnection );
#endif

        QMetaObject::invokeMethod( m_window, "update" );
    }

    qCDebug( logConnection ) << "New VNC client attached on port" << m_tcpServer->serverPort()
        << "#clients" << m_threads.count();

    connect( thread, &QThread::finished, this, &VncServer::removeClient );
    thread->start();
}

void VncServer::removeClient()
{
    if ( auto thread = qobject_cast< QThread* >( sender() ) )
    {
        m_threads.removeOne( thread );
        if ( m_threads.isEmpty() && m_grabConnectionId )
            QObject::disconnect( m_grabConnectionId );

        thread->quit();
        thread->wait( 100 );

        delete thread;

        qCDebug( logConnection ) << "VNC client detached on port" << m_tcpServer->serverPort()
            << "#clients:" << m_threads.count();
    }
}

void VncServer::setTimerInterval( int ms )
{
    for ( auto thread : qAsConst( m_threads ) )
    {
        auto client = static_cast< ClientThread* >( thread )->client();
        client->setTimerInterval( ms );
    }
}

static inline bool isOpenGL12orBetter( const QOpenGLContext* context )
{
    if ( context->isOpenGLES() )
        return false;

    const auto fmt = context->format();
    return ( fmt.majorVersion() >= 2 ) || ( fmt.minorVersion() >= 2 );
}

static void grabWindow( QWindow* window, QImage& frameBuffer )
{
    QElapsedTimer timer;

    if ( logGrab().isDebugEnabled() )
        timer.start();

#if 0
    const auto context = QOpenGLContext::currentContext();

    if ( isOpenGL12orBetter( context ) )
    {
        #ifndef GL_BGRA
            #define GL_BGRA 0x80E1
        #endif

        #ifndef GL_UNSIGNED_INT_8_8_8_8_REV
            #define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
        #endif

        context->functions()->glReadPixels(
            0, 0, frameBuffer.width(), frameBuffer.height(),
            GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, frameBuffer.bits() );

        // OpenGL images are vertically flipped.
        frameBuffer = std::move( frameBuffer ).mirrored( false, true );
    }
    else
    {
        QImage image( frameBuffer.size(), QImage::Format_RGBX8888 );
#if 0
        image.fill( Qt::white ); // for debugging only
#endif

        context->functions()->glReadPixels(
            0, 0, frameBuffer.width(), frameBuffer.height(),
            GL_RGBA, GL_UNSIGNED_BYTE, image.bits() );

        // inplace conversion/mirroring in one pass: TODO ...
        if ( image.format() != frameBuffer.format() )
        {
#if QT_VERSION < QT_VERSION_CHECK( 5, 13, 0 )
            image = image.convertToFormat( frameBuffer.format() );
#else
            image.convertTo( frameBuffer.format() );
#endif
        }

        // OpenGL images are vertically flipped.
        frameBuffer = image.mirrored( false, true );
    }
#else
    // avoiding native OpenGL calls

    const auto format = frameBuffer.format();

#if EGL_STALE_FRAMEBUFFER_WORKAROUND == 2
	auto qqWindow = dynamic_cast<QQuickWindow *>(window);
	if (qqWindow != nullptr)
	{
		frameBuffer = qqWindow->grabWindow();
	}
	else
	{
		// If not a QQuickWindow, fallback to the default case.
#else
	Q_UNUSED(window);
#endif

    extern QImage qt_gl_read_framebuffer(
        const QSize&, bool alpha_format, bool include_alpha );
		
    frameBuffer = qt_gl_read_framebuffer( frameBuffer.size(), false, false );

#if EGL_STALE_FRAMEBUFFER_WORKAROUND == 2
	}
#endif

    if ( frameBuffer.format() != format )
    {
#if QT_VERSION < QT_VERSION_CHECK( 5, 13, 0 )
        frameBuffer = frameBuffer.convertToFormat( format );
#else
        frameBuffer.convertTo( format );
#endif
    }

#endif

    if ( logGrab().isDebugEnabled() )
    {
        qCDebug( logGrab ) << "grabWindow:" << timer.elapsed() << "ms";
    }
}

void VncServer::updateFrameBuffer()
{
    {
        QMutexLocker locker( &m_frameBufferMutex );

        const auto size = m_window->size() * m_window->devicePixelRatio();
        if ( size != m_frameBuffer.size() )
        {
            /*
                On EGLFS the window always matches the screen size.

                But when testing the implementation on X11 the window
                might be resized manually later. Should be no problem,
                as most clients indicate being capable of adjustments
                of the framebuffer size. ( "DesktopSize" pseudo encoding )
             */

            m_frameBuffer = QImage( size, QImage::Format_RGB32 );
        }

        grabWindow( m_window, m_frameBuffer );
    }

    const QRect rect( 0, 0, m_frameBuffer.width(), m_frameBuffer.height() );

    for ( auto thread : qAsConst( m_threads ) )
    {
        auto clientThread = static_cast< ClientThread* >( thread );
        clientThread->markDirty();
    }
}

QWindow* VncServer::window() const
{
    return m_window;
}

QImage VncServer::frameBuffer() const
{
    QMutexLocker locker( &m_frameBufferMutex );
    const auto fb = m_frameBuffer;

    return fb;
}

VncCursor VncServer::cursor() const
{
    return m_cursor;
}

#include "VncServer.moc"
#include "moc_VncServer.cpp"
