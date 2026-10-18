#pragma once
// SPDX-License-Identifier: Apache-2.0
// A Qt 6 widget hosting a VTK render window in a QOpenGLWidget.
//
// VTK's own Qt module (GUISupportQt) is optional in VTK builds and distribution packages often
// build it against Qt 5, so OpenMine carries this small adapter instead. VTK renders into its
// own offscreen framebuffers (vtkOpenGLRenderWindow) and blits the result into the framebuffer
// Qt binds for paintGL(); the context is Qt's (VTK never creates or swaps one). Mouse, wheel and
// key events are forwarded to a vtkGenericRenderWindowInteractor so standard and custom
// interactor styles work unchanged. Rendering is always driven by Qt (update()/paintGL), never
// by VTK directly, which keeps several viewports in one window well behaved.

#include <QOpenGLWidget>
#include <QSurfaceFormat>

#include <vtkSmartPointer.h>

class vtkGenericOpenGLRenderWindow;
class vtkGenericRenderWindowInteractor;
class vtkObject;

namespace openmine::app {

class QtVtkWidget : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit QtVtkWidget(QWidget* parent = nullptr);
    ~QtVtkWidget() override;

    /// Surface format VTK needs (OpenGL 3.2 core, depth 24, stencil 8, no multisampling in the
    /// default framebuffer — VTK multisamples its own framebuffer).
    static QSurfaceFormat defaultFormat();

    vtkGenericOpenGLRenderWindow* renderWindow() const;
    vtkGenericRenderWindowInteractor* interactor() const;

    /// Requests a repaint (coalesced by Qt).
    void requestRender() { update(); }

    /// Device pixel ratio used to map Qt logical coordinates to VTK display coordinates.
    double pixelRatio() const { return devicePixelRatioF(); }
    /// Qt widget position → VTK display coordinates (origin bottom-left, physical pixels).
    void toDisplay(const QPoint& p, int& x, int& y) const;

    /// True once the GL context was created and VTK initialised.
    bool isInitialized() const { return initialized_; }
    /// Last OpenGL problem reported during initialisation (empty when fine).
    const QString& glError() const { return glError_; }

signals:
    /// Emitted after VTK finished a frame (inside paintGL).
    void frameRendered();
    /// Emitted once after the GL context became available.
    void initialised();

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;

    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;
    bool event(QEvent* e) override;

private:
    void onVtkEvent(vtkObject* caller, unsigned long eventId, void* callData);
    void setEventPosition(const QPointF& pos, Qt::KeyboardModifiers mods, int repeat = 0);
    void cleanupGL();

    vtkSmartPointer<vtkGenericOpenGLRenderWindow> window_;
    vtkSmartPointer<vtkGenericRenderWindowInteractor> interactor_;
    bool initialized_ = false;
    bool inPaint_ = false;
    QString glError_;
};

} // namespace openmine::app
