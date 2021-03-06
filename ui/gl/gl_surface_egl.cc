// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This include must be here so that the includes provided transitively
// by gl_surface_egl.h don't make it impossible to compile this code.
#include "third_party/mesa/src/include/GL/osmesa.h"

#include "ui/gl/gl_surface_egl.h"

#if defined(OS_ANDROID)
#include <android/native_window_jni.h>
#include "base/android/sys_utils.h"
#endif

#include "base/command_line.h"
#include "base/debug/trace_event.h"
#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "base/message_loop/message_loop.h"
#include "build/build_config.h"
#include "ui/gl/egl_util.h"
#include "ui/gl/gl_context.h"
#include "ui/gl/gl_implementation.h"
#include "ui/gl/gl_surface_osmesa.h"
#include "ui/gl/gl_surface_stub.h"
#include "ui/gl/gl_switches.h"
#include "ui/gl/scoped_make_current.h"
#include "ui/gl/sync_control_vsync_provider.h"

#if defined(USE_X11)
extern "C" {
#include <X11/Xlib.h>
}
#endif

#if defined (USE_OZONE)
#include "ui/gfx/ozone/surface_factory_ozone.h"
#endif

// From ANGLE's egl/eglext.h.
#if !defined(EGL_D3D11_ELSE_D3D9_DISPLAY_ANGLE)
#define EGL_D3D11_ELSE_D3D9_DISPLAY_ANGLE \
    reinterpret_cast<EGLNativeDisplayType>(-2)
#endif

using ui::GetLastEGLErrorString;

namespace gfx {

namespace {

EGLConfig g_config;
EGLDisplay g_display;
EGLNativeDisplayType g_native_display;

const char* g_egl_extensions = NULL;
bool g_egl_create_context_robustness_supported = false;
bool g_egl_sync_control_supported = false;
bool g_egl_surfaceless_context_supported = false;

class EGLSyncControlVSyncProvider
    : public gfx::SyncControlVSyncProvider {
 public:
  explicit EGLSyncControlVSyncProvider(EGLSurface surface)
      : SyncControlVSyncProvider(),
        surface_(surface) {
  }

  virtual ~EGLSyncControlVSyncProvider() { }

 protected:
  virtual bool GetSyncValues(int64* system_time,
                             int64* media_stream_counter,
                             int64* swap_buffer_counter) OVERRIDE {
    uint64 u_system_time, u_media_stream_counter, u_swap_buffer_counter;
    bool result = eglGetSyncValuesCHROMIUM(
        g_display, surface_, &u_system_time,
        &u_media_stream_counter, &u_swap_buffer_counter) == EGL_TRUE;
    if (result) {
      *system_time = static_cast<int64>(u_system_time);
      *media_stream_counter = static_cast<int64>(u_media_stream_counter);
      *swap_buffer_counter = static_cast<int64>(u_swap_buffer_counter);
    }
    return result;
  }

  virtual bool GetMscRate(int32* numerator, int32* denominator) OVERRIDE {
    return false;
  }

 private:
  EGLSurface surface_;

  DISALLOW_COPY_AND_ASSIGN(EGLSyncControlVSyncProvider);
};

bool ValidateEglConfig(EGLDisplay display,
                       const EGLint* config_attribs,
                       EGLint* num_configs) {
  if (!eglChooseConfig(display,
                       config_attribs,
                       NULL,
                       0,
                       num_configs)) {
    LOG(ERROR) << "eglChooseConfig failed with error "
               << GetLastEGLErrorString();
    return false;
  }
  if (*num_configs == 0) {
    LOG(ERROR) << "No suitable EGL configs found.";
    return false;
  }
  return true;
}

}  // namespace

GLSurfaceEGL::GLSurfaceEGL() {}

bool GLSurfaceEGL::InitializeOneOff() {
  static bool initialized = false;
  if (initialized)
    return true;

#if defined(USE_X11)
  g_native_display = base::MessagePumpForUI::GetDefaultXDisplay();
#elif defined(OS_WIN)
  g_native_display = EGL_DEFAULT_DISPLAY;
  if (!CommandLine::ForCurrentProcess()->HasSwitch(switches::kDisableD3D11) &&
      CommandLine::ForCurrentProcess()->HasSwitch(switches::kEnableD3D11)) {
    g_native_display = EGL_D3D11_ELSE_D3D9_DISPLAY_ANGLE;
  }
#elif defined(USE_OZONE)
  gfx::SurfaceFactoryOzone* surface_factory =
      gfx::SurfaceFactoryOzone::GetInstance();
  if (surface_factory->InitializeHardware() !=
      gfx::SurfaceFactoryOzone::INITIALIZED) {
    LOG(ERROR) << "OZONE failed to initialize hardware";
    return false;
  }
  g_native_display = reinterpret_cast<EGLNativeDisplayType>(
      surface_factory->GetNativeDisplay());
#else
  g_native_display = EGL_DEFAULT_DISPLAY;
#endif
  g_display = eglGetDisplay(g_native_display);
  if (!g_display) {
    LOG(ERROR) << "eglGetDisplay failed with error " << GetLastEGLErrorString();
    return false;
  }

  if (!eglInitialize(g_display, NULL, NULL)) {
    LOG(ERROR) << "eglInitialize failed with error " << GetLastEGLErrorString();
    return false;
  }

  // Choose an EGL configuration.
  // On X this is only used for PBuffer surfaces.
  static EGLint config_attribs_8888[] = {
    EGL_BUFFER_SIZE, 32,
    EGL_ALPHA_SIZE, 8,
    EGL_BLUE_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_RED_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
    EGL_NONE
  };

#if defined(OS_ANDROID)
  static EGLint config_attribs_565[] = {
    EGL_BUFFER_SIZE, 16,
    EGL_BLUE_SIZE, 5,
    EGL_GREEN_SIZE, 6,
    EGL_RED_SIZE, 5,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
    EGL_NONE
  };
#endif
  EGLint* choose_attributes = config_attribs_8888;

#if defined(OS_ANDROID)
  if (base::android::SysUtils::IsLowEndDevice()) {
    choose_attributes = config_attribs_565;
  }
#endif

#if defined(USE_OZONE)
  const EGLint* config_attribs =
      surface_factory->GetEGLSurfaceProperties(choose_attributes);
#else
  const EGLint* config_attribs = choose_attributes;
#endif

  EGLint num_configs;
  EGLint config_size = 1;
  EGLConfig* config_data = &g_config;
  // Validate if there are any configs for given atrribs.
  if (!ValidateEglConfig(g_display,
                         config_attribs,
                         &num_configs)) {
    return false;
  }

#if defined(OS_ANDROID)
  scoped_ptr<EGLConfig[]> matching_configs(new EGLConfig[num_configs]);
  if (base::android::SysUtils::IsLowEndDevice()) {
    config_size = num_configs;
    config_data = matching_configs.get();
  }
#endif

  if (!eglChooseConfig(g_display,
                       config_attribs,
                       config_data,
                       config_size,
                       &num_configs)) {
    LOG(ERROR) << "eglChooseConfig failed with error "
               << GetLastEGLErrorString();
    return false;
  }

#if defined(OS_ANDROID)
  if (base::android::SysUtils::IsLowEndDevice()) {
    // Because of the EGL config sort order, we have to iterate
    // through all of them (it'll put higher sum(R,G,B) bits
    // first with the above attribs).
    bool match_found = false;
    for (int i = 0; i < num_configs; i++) {
      EGLBoolean success;
      EGLint red, green, blue;
      // Read the relevent attributes of the EGLConfig.
      success = eglGetConfigAttrib(g_display, matching_configs[i],
                                   EGL_RED_SIZE, &red);
      success &= eglGetConfigAttrib(g_display, matching_configs[i],
                                    EGL_BLUE_SIZE, &blue);
      success &= eglGetConfigAttrib(g_display, matching_configs[i],
                                    EGL_GREEN_SIZE, &green);
      if ((success == EGL_TRUE) && (red == 5) &&
          (green == 6) && (blue == 5)) {
        g_config = matching_configs[i];
        match_found = true;
        break;
      }
    }
    if (!match_found) {
      // To fall back to default 32 bit format, choose with
      // the right attributes again.
      if (!ValidateEglConfig(g_display,
                             config_attribs_8888,
                             &num_configs)) {
        return false;
      }
      if (!eglChooseConfig(g_display,
                           config_attribs_8888,
                           &g_config,
                           1,
                           &num_configs)) {
        LOG(ERROR) << "eglChooseConfig failed with error "
                   << GetLastEGLErrorString();
        return false;
      }
    }
  }

#endif

  g_egl_extensions = eglQueryString(g_display, EGL_EXTENSIONS);
  g_egl_create_context_robustness_supported =
      HasEGLExtension("EGL_EXT_create_context_robustness");
  g_egl_sync_control_supported =
      HasEGLExtension("EGL_CHROMIUM_sync_control");

  // Check if SurfacelessEGL is supported.
  g_egl_surfaceless_context_supported =
      HasEGLExtension("EGL_KHR_surfaceless_context");
  if (g_egl_surfaceless_context_supported) {
    // EGL_KHR_surfaceless_context is supported but ensure
    // GL_OES_surfaceless_context is also supported. We need a current context
    // to query for supported GL extensions.
    scoped_refptr<GLSurface> surface = new SurfacelessEGL(Size(1, 1));
    scoped_refptr<GLContext> context = GLContext::CreateGLContext(
      NULL, surface.get(), PreferIntegratedGpu);
    if (!context->MakeCurrent(surface.get()))
      g_egl_surfaceless_context_supported = false;

    // Ensure context supports GL_OES_surfaceless_context.
    if (g_egl_surfaceless_context_supported) {
      g_egl_surfaceless_context_supported = context->HasExtension(
          "GL_OES_surfaceless_context");
      context->ReleaseCurrent(surface.get());
    }
  }

  initialized = true;

  return true;
}

EGLDisplay GLSurfaceEGL::GetDisplay() {
  return g_display;
}

EGLDisplay GLSurfaceEGL::GetHardwareDisplay() {
  return g_display;
}

EGLNativeDisplayType GLSurfaceEGL::GetNativeDisplay() {
  return g_native_display;
}

const char* GLSurfaceEGL::GetEGLExtensions() {
  return g_egl_extensions;
}

bool GLSurfaceEGL::HasEGLExtension(const char* name) {
  return ExtensionsContain(GetEGLExtensions(), name);
}

bool GLSurfaceEGL::IsCreateContextRobustnessSupported() {
  return g_egl_create_context_robustness_supported;
}

GLSurfaceEGL::~GLSurfaceEGL() {}

NativeViewGLSurfaceEGL::NativeViewGLSurfaceEGL(gfx::AcceleratedWidget window)
    : window_(window),
      surface_(NULL),
      supports_post_sub_buffer_(false),
      config_(NULL) {
#if defined(OS_ANDROID)
  if (window)
    ANativeWindow_acquire(window);
#endif
}

bool NativeViewGLSurfaceEGL::Initialize() {
  return Initialize(scoped_ptr<VSyncProvider>());
}

bool NativeViewGLSurfaceEGL::Initialize(
    scoped_ptr<VSyncProvider> sync_provider) {
  DCHECK(!surface_);

  if (window_ == kNullAcceleratedWidget) {
    LOG(ERROR) << "Trying to create surface without window.";
    return false;
  }

  if (!GetDisplay()) {
    LOG(ERROR) << "Trying to create surface with invalid display.";
    return false;
  }

  static const EGLint egl_window_attributes_sub_buffer[] = {
    EGL_POST_SUB_BUFFER_SUPPORTED_NV, EGL_TRUE,
    EGL_NONE
  };

  // Create a surface for the native window.
  surface_ = eglCreateWindowSurface(
      GetDisplay(),
      GetConfig(),
      window_,
      gfx::g_driver_egl.ext.b_EGL_NV_post_sub_buffer ?
          egl_window_attributes_sub_buffer :
          NULL);

  if (!surface_) {
    LOG(ERROR) << "eglCreateWindowSurface failed with error "
               << GetLastEGLErrorString();
    Destroy();
    return false;
  }

  EGLint surfaceVal;
  EGLBoolean retVal = eglQuerySurface(GetDisplay(),
                                      surface_,
                                      EGL_POST_SUB_BUFFER_SUPPORTED_NV,
                                      &surfaceVal);
  supports_post_sub_buffer_ = (surfaceVal && retVal) == EGL_TRUE;

  if (sync_provider)
    vsync_provider_.reset(sync_provider.release());
  else if (g_egl_sync_control_supported)
    vsync_provider_.reset(new EGLSyncControlVSyncProvider(surface_));
  return true;
}

void NativeViewGLSurfaceEGL::Destroy() {
  if (surface_) {
    if (!eglDestroySurface(GetDisplay(), surface_)) {
      LOG(ERROR) << "eglDestroySurface failed with error "
                 << GetLastEGLErrorString();
    }
    surface_ = NULL;
  }
}

EGLConfig NativeViewGLSurfaceEGL::GetConfig() {
#if !defined(USE_X11)
  return g_config;
#else
  if (!config_) {
    // Get a config compatible with the window
    DCHECK(window_);
    XWindowAttributes win_attribs;
    if (!XGetWindowAttributes(GetNativeDisplay(), window_, &win_attribs)) {
      return NULL;
    }

    // Try matching the window depth with an alpha channel,
    // because we're worried the destination alpha width could
    // constrain blending precision.
    const int kBufferSizeOffset = 1;
    const int kAlphaSizeOffset = 3;
    EGLint config_attribs[] = {
      EGL_BUFFER_SIZE, ~0,
      EGL_ALPHA_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_RED_SIZE, 8,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
      EGL_NONE
    };
    config_attribs[kBufferSizeOffset] = win_attribs.depth;

    EGLint num_configs;
    if (!eglChooseConfig(g_display,
                         config_attribs,
                         &config_,
                         1,
                         &num_configs)) {
      LOG(ERROR) << "eglChooseConfig failed with error "
                 << GetLastEGLErrorString();
      return NULL;
    }

    if (num_configs) {
      EGLint config_depth;
      if (!eglGetConfigAttrib(g_display,
                              config_,
                              EGL_BUFFER_SIZE,
                              &config_depth)) {
        LOG(ERROR) << "eglGetConfigAttrib failed with error "
                   << GetLastEGLErrorString();
        return NULL;
      }

      if (config_depth == win_attribs.depth) {
        return config_;
      }
    }

    // Try without an alpha channel.
    config_attribs[kAlphaSizeOffset] = 0;
    if (!eglChooseConfig(g_display,
                         config_attribs,
                         &config_,
                         1,
                         &num_configs)) {
      LOG(ERROR) << "eglChooseConfig failed with error "
                 << GetLastEGLErrorString();
      return NULL;
    }

    if (num_configs == 0) {
      LOG(ERROR) << "No suitable EGL configs found.";
      return NULL;
    }
  }
  return config_;
#endif
}

bool NativeViewGLSurfaceEGL::IsOffscreen() {
  return false;
}

bool NativeViewGLSurfaceEGL::SwapBuffers() {
  TRACE_EVENT2("gpu", "NativeViewGLSurfaceEGL:RealSwapBuffers",
      "width", GetSize().width(),
      "height", GetSize().height());

  if (!eglSwapBuffers(GetDisplay(), surface_)) {
    DVLOG(1) << "eglSwapBuffers failed with error "
             << GetLastEGLErrorString();
    return false;
  }

  return true;
}

gfx::Size NativeViewGLSurfaceEGL::GetSize() {
  EGLint width;
  EGLint height;
  if (!eglQuerySurface(GetDisplay(), surface_, EGL_WIDTH, &width) ||
      !eglQuerySurface(GetDisplay(), surface_, EGL_HEIGHT, &height)) {
    NOTREACHED() << "eglQuerySurface failed with error "
                 << GetLastEGLErrorString();
    return gfx::Size();
  }

  return gfx::Size(width, height);
}

bool NativeViewGLSurfaceEGL::Resize(const gfx::Size& size) {
  if (size == GetSize())
    return true;

  scoped_ptr<ui::ScopedMakeCurrent> scoped_make_current;
  GLContext* current_context = GLContext::GetCurrent();
  bool was_current =
      current_context && current_context->IsCurrent(this);
  if (was_current) {
    scoped_make_current.reset(
        new ui::ScopedMakeCurrent(current_context, this));
    current_context->ReleaseCurrent(this);
  }

  Destroy();

  if (!Initialize()) {
    LOG(ERROR) << "Failed to resize window.";
    return false;
  }

  return true;
}

bool NativeViewGLSurfaceEGL::Recreate() {
  Destroy();
  if (!Initialize()) {
    LOG(ERROR) << "Failed to create surface.";
    return false;
  }
  return true;
}

EGLSurface NativeViewGLSurfaceEGL::GetHandle() {
  return surface_;
}

std::string NativeViewGLSurfaceEGL::GetExtensions() {
  std::string extensions = GLSurface::GetExtensions();
  if (supports_post_sub_buffer_) {
    extensions += extensions.empty() ? "" : " ";
    extensions += "GL_CHROMIUM_post_sub_buffer";
  }
  return extensions;
}

bool NativeViewGLSurfaceEGL::PostSubBuffer(
    int x, int y, int width, int height) {
  DCHECK(supports_post_sub_buffer_);
  if (!eglPostSubBufferNV(GetDisplay(), surface_, x, y, width, height)) {
    DVLOG(1) << "eglPostSubBufferNV failed with error "
             << GetLastEGLErrorString();
    return false;
  }
  return true;
}

VSyncProvider* NativeViewGLSurfaceEGL::GetVSyncProvider() {
  return vsync_provider_.get();
}

NativeViewGLSurfaceEGL::~NativeViewGLSurfaceEGL() {
  Destroy();
#if defined(OS_ANDROID)
  if (window_)
    ANativeWindow_release(window_);
#endif
}

void NativeViewGLSurfaceEGL::SetHandle(EGLSurface surface) {
  surface_ = surface;
}

PbufferGLSurfaceEGL::PbufferGLSurfaceEGL(const gfx::Size& size)
    : size_(size),
      surface_(NULL) {
}

bool PbufferGLSurfaceEGL::Initialize() {
  EGLSurface old_surface = surface_;

  EGLDisplay display = GetDisplay();
  if (!display) {
    LOG(ERROR) << "Trying to create surface with invalid display.";
    return false;
  }

  if (size_.GetArea() == 0) {
    LOG(ERROR) << "Error: surface has zero area "
               << size_.width() << " x " << size_.height();
    return false;
  }

  // Allocate the new pbuffer surface before freeing the old one to ensure
  // they have different addresses. If they have the same address then a
  // future call to MakeCurrent might early out because it appears the current
  // context and surface have not changed.
  const EGLint pbuffer_attribs[] = {
    EGL_WIDTH, size_.width(),
    EGL_HEIGHT, size_.height(),
    EGL_NONE
  };

  EGLSurface new_surface = eglCreatePbufferSurface(display,
                                                   GetConfig(),
                                                   pbuffer_attribs);
  if (!new_surface) {
    LOG(ERROR) << "eglCreatePbufferSurface failed with error "
               << GetLastEGLErrorString();
    return false;
  }

  if (old_surface)
    eglDestroySurface(display, old_surface);

  surface_ = new_surface;
  return true;
}

void PbufferGLSurfaceEGL::Destroy() {
  if (surface_) {
    if (!eglDestroySurface(GetDisplay(), surface_)) {
      LOG(ERROR) << "eglDestroySurface failed with error "
                 << GetLastEGLErrorString();
    }
    surface_ = NULL;
  }
}

EGLConfig PbufferGLSurfaceEGL::GetConfig() {
  return g_config;
}

bool PbufferGLSurfaceEGL::IsOffscreen() {
  return true;
}

bool PbufferGLSurfaceEGL::SwapBuffers() {
  NOTREACHED() << "Attempted to call SwapBuffers on a PbufferGLSurfaceEGL.";
  return false;
}

gfx::Size PbufferGLSurfaceEGL::GetSize() {
  return size_;
}

bool PbufferGLSurfaceEGL::Resize(const gfx::Size& size) {
  if (size == size_)
    return true;

  scoped_ptr<ui::ScopedMakeCurrent> scoped_make_current;
  GLContext* current_context = GLContext::GetCurrent();
  bool was_current =
      current_context && current_context->IsCurrent(this);
  if (was_current) {
    scoped_make_current.reset(
        new ui::ScopedMakeCurrent(current_context, this));
  }

  size_ = size;

  if (!Initialize()) {
    LOG(ERROR) << "Failed to resize pbuffer.";
    return false;
  }

  return true;
}

EGLSurface PbufferGLSurfaceEGL::GetHandle() {
  return surface_;
}

void* PbufferGLSurfaceEGL::GetShareHandle() {
#if defined(OS_ANDROID)
  NOTREACHED();
  return NULL;
#else
  if (!gfx::g_driver_egl.ext.b_EGL_ANGLE_query_surface_pointer)
    return NULL;

  if (!gfx::g_driver_egl.ext.b_EGL_ANGLE_surface_d3d_texture_2d_share_handle)
    return NULL;

  void* handle;
  if (!eglQuerySurfacePointerANGLE(g_display,
                                   GetHandle(),
                                   EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE,
                                   &handle)) {
    return NULL;
  }

  return handle;
#endif
}

PbufferGLSurfaceEGL::~PbufferGLSurfaceEGL() {
  Destroy();
}

SurfacelessEGL::SurfacelessEGL(const gfx::Size& size)
    : size_(size) {
}

bool SurfacelessEGL::Initialize() {
  return true;
}

void SurfacelessEGL::Destroy() {
}

EGLConfig SurfacelessEGL::GetConfig() {
  return g_config;
}

bool SurfacelessEGL::IsOffscreen() {
  return true;
}

bool SurfacelessEGL::SwapBuffers() {
  LOG(ERROR) << "Attempted to call SwapBuffers with SurfacelessEGL.";
  return false;
}

gfx::Size SurfacelessEGL::GetSize() {
  return size_;
}

bool SurfacelessEGL::Resize(const gfx::Size& size) {
  size_ = size;
  return true;
}

EGLSurface SurfacelessEGL::GetHandle() {
  return EGL_NO_SURFACE;
}

void* SurfacelessEGL::GetShareHandle() {
  return NULL;
}

SurfacelessEGL::~SurfacelessEGL() {
}

#if defined(ANDROID) || defined(USE_OZONE)

// A thin subclass of |GLSurfaceOSMesa| that can be used in place
// of a native hardware-provided surface when a native surface
// provider is not available.
class GLSurfaceOSMesaHeadless : public GLSurfaceOSMesa {
 public:
  explicit GLSurfaceOSMesaHeadless(gfx::AcceleratedWidget window);

  virtual bool IsOffscreen() OVERRIDE;
  virtual bool SwapBuffers() OVERRIDE;

 protected:
  virtual ~GLSurfaceOSMesaHeadless();

 private:

  DISALLOW_COPY_AND_ASSIGN(GLSurfaceOSMesaHeadless);
};

bool GLSurfaceOSMesaHeadless::IsOffscreen() { return false; }

bool GLSurfaceOSMesaHeadless::SwapBuffers() { return true; }

GLSurfaceOSMesaHeadless::GLSurfaceOSMesaHeadless(gfx::AcceleratedWidget window)
    : GLSurfaceOSMesa(OSMESA_BGRA, gfx::Size(1, 1)) {
  DCHECK(window);
}

GLSurfaceOSMesaHeadless::~GLSurfaceOSMesaHeadless() { Destroy(); }

// static
bool GLSurface::InitializeOneOffInternal() {
  switch (GetGLImplementation()) {
    case kGLImplementationEGLGLES2:
      if (!GLSurfaceEGL::InitializeOneOff()) {
        LOG(ERROR) << "GLSurfaceEGL::InitializeOneOff failed.";
        return false;
      }
    default:
      break;
  }
  return true;
}

// static
scoped_refptr<GLSurface>
GLSurface::CreateViewGLSurface(gfx::AcceleratedWidget window) {

  if (GetGLImplementation() == kGLImplementationOSMesaGL) {
    scoped_refptr<GLSurface> surface(new GLSurfaceOSMesaHeadless(window));
    if (!surface->Initialize())
      return NULL;
    return surface;
  }
  DCHECK(GetGLImplementation() == kGLImplementationEGLGLES2);
  if (window) {
    scoped_refptr<NativeViewGLSurfaceEGL> surface;
    scoped_ptr<VSyncProvider> sync_provider;
#if defined(USE_OZONE)
    window = gfx::SurfaceFactoryOzone::GetInstance()->RealizeAcceleratedWidget(
        window);
    sync_provider =
        gfx::SurfaceFactoryOzone::GetInstance()->CreateVSyncProvider(window);
#endif
    surface = new NativeViewGLSurfaceEGL(window);
    if(surface->Initialize(sync_provider.Pass()))
      return surface;
  } else {
    scoped_refptr<GLSurface> surface = new GLSurfaceStub();
    if (surface->Initialize())
      return surface;
  }
  return NULL;
}

// static
scoped_refptr<GLSurface>
GLSurface::CreateOffscreenGLSurface(const gfx::Size& size) {
  switch (GetGLImplementation()) {
    case kGLImplementationOSMesaGL: {
      scoped_refptr<GLSurface> surface(new GLSurfaceOSMesa(1, size));
      if (!surface->Initialize())
        return NULL;

      return surface;
    }
    case kGLImplementationEGLGLES2: {
      scoped_refptr<GLSurface> surface;
      if (g_egl_surfaceless_context_supported &&
         (size.width() == 0 && size.height() == 0)) {
        surface = new SurfacelessEGL(size);
      } else
        surface = new PbufferGLSurfaceEGL(size);

      if (!surface->Initialize())
        return NULL;
      return surface;
    }
    default:
      NOTREACHED();
      return NULL;
  }
}

#endif

}  // namespace gfx
