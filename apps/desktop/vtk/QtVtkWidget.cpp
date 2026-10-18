// SPDX-License-Identifier: Apache-2.0
#include "QtVtkWidget.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QWheelEvent>

#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkGenericRenderWindowInteractor.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkOpenGLState.h>
#include <vtkRendererCollection.h>

#include <cmath>

namespace openmine::app {

namespace {

const char* keySym(int key) {
    switch (key) {
    case Qt::Key_Escape: return "Escape";
    case Qt::Key_Return: return "Return";
    case Qt::Key_Enter: return "KP_Enter";
    case Qt::Key_Tab: return "Tab";
    case Qt::Key_Backspace: return "BackSpace";
    case Qt::Key_Delete: return "Delete";
    case Qt::Key_Insert: return "Insert";
    case Qt::Key_Home: return "Home";
    case Qt::Key_End: return "End";
    case Qt::Key_PageUp: return "Prior";
    case Qt::Key_PageDown: return "Next";
    case Qt::Key_Left: return "Left";
    case Qt::Key_Right: return "Right";
    case Qt::Key_Up: return "Up";
    case Qt::Key_Down: return "Down";
    case Qt::Key_Shift: return "Shift_L";
    case Qt::Key_Control: return "Control_L";
    case Qt::Key_Alt: return "Alt_L";
    case Qt::Key_Space: return "space";
    case Qt::Key_Plus: return "plus";
    case Qt::Key_Minus: return "minus";
    case Qt::Key_F1: return "F1";
    case Qt::Key_F2: return "F2";
    case Qt::Key_F3: return "F3";
    case Qt::Key_F4: return "F4";
    case Qt::Key_F5: return "F5";
    case Qt::Key_F6: return "F6";
    case Qt::Key_F7: return "F7";
    case Qt::Key_F8: return "F8";
    case Qt::Key_F9: return "F9";
    case Qt::Key_F10: return "F10";
    case Qt::Key_F11: return "F11";
    case Qt::Key_F12: return "F12";
    default: break;
    }
    static thread_local char buf[2] = {0, 0};
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        buf[0] = static_cast<char>('a' + (key - Qt::Key_A));
        return buf;
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        buf[0] = static_cast<char>('0' + (key - Qt::Key_0));
        return buf;
    }
    return nullptr;
}

} // namespace

QSurfaceFormat QtVtkWidget::defaultFormat() {
    QSurfaceFormat f;
    f.setRenderableType(QSurfaceFormat::OpenGL);
    f.setVersion(3, 2);
    f.setProfile(QSurfaceFormat::CoreProfile);
    f.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    f.setRedBufferSize(8);
    f.setGreenBufferSize(8);
    f.setBlueBufferSize(8);
    f.setAlphaBufferSize(8);
    f.setDepthBufferSize(24);
    f.setStencilBufferSize(8);
    f.setSamples(0);
    f.setStereo(false);
    return f;
}

QtVtkWidget::QtVtkWidget(QWidget* parent) : QOpenGLWidget(parent) {
    setFormat(defaultFormat());
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
    setMinimumSize(120, 90);

    window_ = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
    window_->SetReadyForRendering(false);
    window_->SetFrameBlitModeToBlitToCurrent();
    window_->SetOwnContext(false);
    window_->SetMultiSamples(4);
    interactor_ = vtkSmartPointer<vtkGenericRenderWindowInteractor>::New();
    interactor_->SetRenderWindow(window_);
    interactor_->EnableRenderOff(); // renders are requested from Qt (RenderEvent → update())
    vtkNew<vtkInteractorStyleTrackballCamera> style;
    interactor_->SetInteractorStyle(style);

    auto cb = vtkSmartPointer<vtkCallbackCommand>::New();
    cb->SetClientData(this);
    cb->SetCallback([](vtkObject* caller, unsigned long id, void* client, void* callData) {
        static_cast<QtVtkWidget*>(client)->onVtkEvent(caller, id, callData);
    });
    for (unsigned long ev : {vtkCommand::WindowMakeCurrentEvent, vtkCommand::WindowIsCurrentEvent, vtkCommand::WindowFrameEvent,
                             vtkCommand::WindowSupportsOpenGLEvent, vtkCommand::WindowIsDirectEvent, vtkCommand::CursorChangedEvent})
        window_->AddObserver(ev, cb);
    interactor_->AddObserver(vtkCommand::RenderEvent, cb);
}

QtVtkWidget::~QtVtkWidget() {
    makeCurrent();
    cleanupGL();
    doneCurrent();
}

vtkGenericOpenGLRenderWindow* QtVtkWidget::renderWindow() const { return window_; }
vtkGenericRenderWindowInteractor* QtVtkWidget::interactor() const { return interactor_; }

void QtVtkWidget::onVtkEvent(vtkObject*, unsigned long id, void* callData) {
    switch (id) {
    case vtkCommand::WindowMakeCurrentEvent:
        if (context() && QOpenGLContext::currentContext() != context()) makeCurrent();
        break;
    case vtkCommand::WindowIsCurrentEvent:
        if (callData) *static_cast<bool*>(callData) = context() && QOpenGLContext::currentContext() == context();
        break;
    case vtkCommand::WindowSupportsOpenGLEvent:
        if (callData) *static_cast<int*>(callData) = 1;
        break;
    case vtkCommand::WindowIsDirectEvent:
        if (callData) *static_cast<int*>(callData) = 1;
        break;
    case vtkCommand::WindowFrameEvent:
    case vtkCommand::RenderEvent:
        if (!inPaint_) update();
        break;
    case vtkCommand::CursorChangedEvent: {
        const int shape = callData ? *static_cast<int*>(callData) : VTK_CURSOR_DEFAULT;
        Qt::CursorShape qs = Qt::ArrowCursor;
        switch (shape) {
        case VTK_CURSOR_CROSSHAIR: qs = Qt::CrossCursor; break;
        case VTK_CURSOR_SIZEALL: qs = Qt::SizeAllCursor; break;
        case VTK_CURSOR_SIZEWE: qs = Qt::SizeHorCursor; break;
        case VTK_CURSOR_SIZENS: qs = Qt::SizeVerCursor; break;
        case VTK_CURSOR_HAND: qs = Qt::PointingHandCursor; break;
        default: break;
        }
        setCursor(qs);
        break;
    }
    default: break;
    }
}

void QtVtkWidget::initializeGL() {
    if (!context()) return;
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] {
        makeCurrent();
        cleanupGL();
        doneCurrent();
    });
    const auto fmt = context()->format();
    if (fmt.majorVersion() < 3 || (fmt.majorVersion() == 3 && fmt.minorVersion() < 2)) {
        glError_ = QStringLiteral("OpenGL %1.%2 context obtained; VTK needs OpenGL 3.2 or later.").arg(fmt.majorVersion()).arg(fmt.minorVersion());
    }
    window_->SetMapped(1);
    window_->SetIsCurrent(true);
    window_->SetForceMaximumHardwareLineWidth(1);
    window_->InitializeFromCurrentContext();
    const double dpr = devicePixelRatioF();
    const int w = static_cast<int>(std::lround(width() * dpr)), h = static_cast<int>(std::lround(height() * dpr));
    window_->SetSize(w, h);
    interactor_->SetSize(w, h);
    if (!interactor_->GetInitialized()) interactor_->Initialize();
    initialized_ = true;
    emit initialised();
}

void QtVtkWidget::resizeGL(int w, int h) {
    const double dpr = devicePixelRatioF();
    const int pw = static_cast<int>(std::lround(w * dpr)), ph = static_cast<int>(std::lround(h * dpr));
    window_->SetSize(pw, ph);
    interactor_->SetSize(pw, ph);
    interactor_->InvokeEvent(vtkCommand::ConfigureEvent, nullptr);
}

void QtVtkWidget::paintGL() {
    if (!initialized_) return;
    inPaint_ = true;
    // Qt changed GL state behind VTK's back (it bound the widget framebuffer): resynchronise VTK's
    // state cache, render, blit into the currently bound (Qt) framebuffer, and restore Qt's state.
    auto* state = window_->GetState();
    state->Reset();
    state->Push();
    window_->SetReadyForRendering(true);
    window_->Render();
    window_->SetReadyForRendering(false);
    state->Pop();
    inPaint_ = false;
    emit frameRendered();
}

void QtVtkWidget::cleanupGL() {
    if (!initialized_) return;
    initialized_ = false;
    window_->SetReadyForRendering(false);
    // Release GPU resources of every renderer while the context is current.
    window_->ReleaseGraphicsResources(window_);
    window_->Finalize();
}

void QtVtkWidget::toDisplay(const QPoint& p, int& x, int& y) const {
    const double dpr = devicePixelRatioF();
    x = static_cast<int>(std::lround(p.x() * dpr));
    y = static_cast<int>(std::lround((height() - p.y() - 1) * dpr));
}

void QtVtkWidget::setEventPosition(const QPointF& pos, Qt::KeyboardModifiers mods, int repeat) {
    const double dpr = devicePixelRatioF();
    interactor_->SetEventInformationFlipY(static_cast<int>(std::lround(pos.x() * dpr)), static_cast<int>(std::lround(pos.y() * dpr)),
                                          (mods & Qt::ControlModifier) ? 1 : 0, (mods & Qt::ShiftModifier) ? 1 : 0, 0, repeat);
    interactor_->SetAltKey((mods & Qt::AltModifier) ? 1 : 0);
}

void QtVtkWidget::mousePressEvent(QMouseEvent* e) {
    setFocus(Qt::MouseFocusReason);
    setEventPosition(e->position(), e->modifiers());
    switch (e->button()) {
    case Qt::LeftButton: interactor_->InvokeEvent(vtkCommand::LeftButtonPressEvent, e); break;
    case Qt::MiddleButton: interactor_->InvokeEvent(vtkCommand::MiddleButtonPressEvent, e); break;
    case Qt::RightButton: interactor_->InvokeEvent(vtkCommand::RightButtonPressEvent, e); break;
    default: break;
    }
}

void QtVtkWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    setEventPosition(e->position(), e->modifiers(), 1);
    switch (e->button()) {
    case Qt::LeftButton: interactor_->InvokeEvent(vtkCommand::LeftButtonDoubleClickEvent, e); break;
    case Qt::MiddleButton: interactor_->InvokeEvent(vtkCommand::MiddleButtonDoubleClickEvent, e); break;
    case Qt::RightButton: interactor_->InvokeEvent(vtkCommand::RightButtonDoubleClickEvent, e); break;
    default: break;
    }
}

void QtVtkWidget::mouseReleaseEvent(QMouseEvent* e) {
    setEventPosition(e->position(), e->modifiers());
    switch (e->button()) {
    case Qt::LeftButton: interactor_->InvokeEvent(vtkCommand::LeftButtonReleaseEvent, e); break;
    case Qt::MiddleButton: interactor_->InvokeEvent(vtkCommand::MiddleButtonReleaseEvent, e); break;
    case Qt::RightButton: interactor_->InvokeEvent(vtkCommand::RightButtonReleaseEvent, e); break;
    default: break;
    }
}

void QtVtkWidget::mouseMoveEvent(QMouseEvent* e) {
    setEventPosition(e->position(), e->modifiers());
    interactor_->InvokeEvent(vtkCommand::MouseMoveEvent, e);
}

void QtVtkWidget::wheelEvent(QWheelEvent* e) {
    setEventPosition(e->position(), e->modifiers());
    const int dy = e->angleDelta().y();
    if (dy > 0) interactor_->InvokeEvent(vtkCommand::MouseWheelForwardEvent, e);
    else if (dy < 0) interactor_->InvokeEvent(vtkCommand::MouseWheelBackwardEvent, e);
    e->accept();
}

void QtVtkWidget::keyPressEvent(QKeyEvent* e) {
    const QPoint p = mapFromGlobal(QCursor::pos());
    setEventPosition(p, e->modifiers(), e->isAutoRepeat() ? 1 : 0);
    const QByteArray text = e->text().toLatin1();
    interactor_->SetKeyCode(text.isEmpty() ? 0 : text[0]);
    interactor_->SetKeySym(keySym(e->key()));
    interactor_->InvokeEvent(vtkCommand::KeyPressEvent, e);
    if (!text.isEmpty()) interactor_->InvokeEvent(vtkCommand::CharEvent, e);
}

void QtVtkWidget::keyReleaseEvent(QKeyEvent* e) {
    const QPoint p = mapFromGlobal(QCursor::pos());
    setEventPosition(p, e->modifiers());
    const QByteArray text = e->text().toLatin1();
    interactor_->SetKeyCode(text.isEmpty() ? 0 : text[0]);
    interactor_->SetKeySym(keySym(e->key()));
    interactor_->InvokeEvent(vtkCommand::KeyReleaseEvent, e);
}

void QtVtkWidget::enterEvent(QEnterEvent* e) {
    setEventPosition(e->position(), Qt::NoModifier);
    interactor_->InvokeEvent(vtkCommand::EnterEvent, e);
}

void QtVtkWidget::leaveEvent(QEvent* e) { interactor_->InvokeEvent(vtkCommand::LeaveEvent, e); }

bool QtVtkWidget::event(QEvent* e) {
    // Let Tab/Backtab reach keyPressEvent (VTK styles and drawing tools use them).
    if (e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() == Qt::Key_Tab || k->key() == Qt::Key_Backtab) {
            keyPressEvent(k);
            return true;
        }
    }
    return QOpenGLWidget::event(e);
}

} // namespace openmine::app
