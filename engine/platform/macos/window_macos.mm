#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <astra/platform/window.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <utility>

@interface AstraWindowDelegate : NSObject <NSWindowDelegate> {
@public
  std::atomic_bool *openFlag;
  std::vector<astra::platform::WindowEvent> *eventQueue;
}
- (void)menuItemSelected:(NSMenuItem *)sender;
@end

@implementation AstraWindowDelegate
- (void)menuItemSelected:(NSMenuItem *)sender {
  if (eventQueue != nullptr) {
    eventQueue->push_back(astra::platform::make_menu_action_event(
        static_cast<std::uint32_t>(sender.tag)));
  }
}
- (BOOL)windowShouldClose:(NSWindow *)sender {
  (void)sender;
  return YES;
}
- (void)windowWillClose:(NSNotification *)notification {
  (void)notification;
  if (openFlag != nullptr) {
    openFlag->store(false, std::memory_order_release);
  }
}
@end

@interface AstraMetalView : NSView
@property(nonatomic, assign) BOOL usesHighDPI;
@end

@implementation AstraMetalView
- (BOOL)acceptsFirstResponder {
  return YES;
}
- (void)keyDown:(NSEvent *)event {
  // The platform layer translates hardware key events in Window::poll_events.
  // NSResponder's default implementation beeps when the game view has no text
  // command to handle, so consume the event after it has been captured there.
  (void)event;
}
- (CALayer *)makeBackingLayer {
  CAMetalLayer *metalLayer = [CAMetalLayer layer];
  metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  metalLayer.framebufferOnly = YES;
  return metalLayer;
}
- (void)viewDidChangeBackingProperties {
  [super viewDidChangeBackingProperties];
  [self updateDrawableSize];
}
- (void)setFrameSize:(NSSize)newSize {
  [super setFrameSize:newSize];
  [self updateDrawableSize];
}
- (void)updateDrawableSize {
  if (![self.layer isKindOfClass:[CAMetalLayer class]]) {
    return;
  }
  CAMetalLayer *metalLayer = (CAMetalLayer *)self.layer;
  const CGFloat scale = self.usesHighDPI && self.window.backingScaleFactor > 0.0
                            ? self.window.backingScaleFactor
                            : 1.0;
  metalLayer.contentsScale = scale;
  metalLayer.drawableSize = CGSizeMake(self.bounds.size.width * scale,
                                       self.bounds.size.height * scale);
}
@end

namespace astra::platform {

struct Window::Impl final {
  WindowConfig config;
  __strong NSWindow *window{};
  __strong AstraMetalView *view{};
  __strong AstraWindowDelegate *delegate{};
  std::atomic_bool open{true};
  std::uint32_t pixel_width{};
  std::uint32_t pixel_height{};
  bool close_event_sent{};
  std::vector<WindowEvent> events;

  ~Impl() {
    if (window != nil) {
      delegate->openFlag = nullptr;
      delegate->eventQueue = nullptr;
      window.delegate = nil;
      [window close];
    }
  }
};

namespace {

std::uint32_t pixel_dimension(CGFloat points, CGFloat scale) noexcept {
  const auto pixels = std::max(0.0, std::round(points * scale));
  return static_cast<std::uint32_t>(pixels);
}

std::uint32_t modifiers_for_event(NSEventModifierFlags flags) noexcept {
  std::uint32_t result = 0;
  if ((flags & NSEventModifierFlagShift) != 0)
    result |= 1U << 0U;
  if ((flags & NSEventModifierFlagControl) != 0)
    result |= 1U << 1U;
  if ((flags & NSEventModifierFlagOption) != 0)
    result |= 1U << 2U;
  if ((flags & NSEventModifierFlagCommand) != 0)
    result |= 1U << 3U;
  if ((flags & NSEventModifierFlagCapsLock) != 0)
    result |= 1U << 4U;
  return result;
}

} // namespace

Window::Window(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Window::~Window() = default;

Window::Window(Window &&) noexcept = default;
Window &Window::operator=(Window &&) noexcept = default;

std::expected<Window, WindowError> Window::create(WindowConfig config) {
  if (![NSThread isMainThread]) {
    return std::unexpected(WindowError{
        .code = WindowErrorCode::not_main_thread,
        .message = "AppKit windows must be created on the main thread.",
    });
  }
  if (config.width == 0 || config.height == 0) {
    return std::unexpected(WindowError{
        .code = WindowErrorCode::creation_failed,
        .message = "Window dimensions must be greater than zero.",
    });
  }

  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    auto impl = std::make_unique<Impl>();
    impl->config = std::move(config);

    NSWindowStyleMask style = NSWindowStyleMaskTitled |
                              NSWindowStyleMaskClosable |
                              NSWindowStyleMaskMiniaturizable;
    if (impl->config.resizable) {
      style |= NSWindowStyleMaskResizable;
    }

    const NSRect contentRect =
        NSMakeRect(0.0, 0.0, impl->config.width, impl->config.height);
    impl->window = [[NSWindow alloc] initWithContentRect:contentRect
                                               styleMask:style
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
    if (impl->window == nil) {
      return std::unexpected(WindowError{
          .code = WindowErrorCode::creation_failed,
          .message = "AppKit could not create the native window.",
      });
    }

    impl->view = [[AstraMetalView alloc] initWithFrame:contentRect];
    impl->view.usesHighDPI = impl->config.high_dpi;
    impl->view.wantsLayer = YES;
    impl->window.contentView = impl->view;
    impl->window.title =
        [NSString stringWithUTF8String:impl->config.title.c_str()];
    impl->window.releasedWhenClosed = NO;
    impl->window.acceptsMouseMovedEvents = YES;

    impl->delegate = [[AstraWindowDelegate alloc] init];
    impl->delegate->openFlag = &impl->open;
    impl->delegate->eventQueue = &impl->events;
    impl->window.delegate = impl->delegate;

    [impl->window center];
    if (impl->config.visible) {
      [impl->window makeKeyAndOrderFront:nil];
      [impl->window makeFirstResponder:impl->view];
      [NSApp activateIgnoringOtherApps:YES];
    }
    [impl->view updateDrawableSize];

    const CGFloat scale =
        impl->config.high_dpi ? impl->window.backingScaleFactor : 1.0;
    impl->pixel_width = pixel_dimension(impl->view.bounds.size.width, scale);
    impl->pixel_height = pixel_dimension(impl->view.bounds.size.height, scale);
    return Window{std::move(impl)};
  }
}

bool Window::poll_events() {
  if (!impl_ || ![NSThread isMainThread]) {
    return false;
  }

  impl_->events.clear();
  if (!impl_->open.load(std::memory_order_acquire)) {
    if (!impl_->close_event_sent) {
      impl_->events.push_back(
          WindowEvent{.type = WindowEventType::close_requested});
      impl_->close_event_sent = true;
      return true;
    }
    return false;
  }

  @autoreleasepool {
    for (;;) {
      NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                          untilDate:[NSDate distantPast]
                                             inMode:NSDefaultRunLoopMode
                                            dequeue:YES];
      if (event == nil) {
        break;
      }
      if (event.window != impl_->window) {
        [NSApp sendEvent:event];
        continue;
      }

      WindowEvent translated{};
      bool recognized = true;
      translated.modifiers = modifiers_for_event(event.modifierFlags);
      switch (event.type) {
      case NSEventTypeKeyDown:
        translated.type = WindowEventType::key_down;
        translated.key_code = event.keyCode;
        translated.repeat = event.isARepeat;
        break;
      case NSEventTypeKeyUp:
        translated.type = WindowEventType::key_up;
        translated.key_code = event.keyCode;
        break;
      case NSEventTypeLeftMouseDown:
      case NSEventTypeRightMouseDown:
      case NSEventTypeOtherMouseDown:
        translated.type = WindowEventType::mouse_down;
        translated.mouse_button = static_cast<std::uint8_t>(event.buttonNumber);
        break;
      case NSEventTypeLeftMouseUp:
      case NSEventTypeRightMouseUp:
      case NSEventTypeOtherMouseUp:
        translated.type = WindowEventType::mouse_up;
        translated.mouse_button = static_cast<std::uint8_t>(event.buttonNumber);
        break;
      case NSEventTypeMouseMoved:
      case NSEventTypeLeftMouseDragged:
      case NSEventTypeRightMouseDragged:
      case NSEventTypeOtherMouseDragged:
        translated.type = WindowEventType::mouse_moved;
        break;
      case NSEventTypeScrollWheel:
        translated.type = WindowEventType::mouse_scrolled;
        translated.delta_x = event.scrollingDeltaX;
        translated.delta_y = event.scrollingDeltaY;
        break;
      default:
        recognized = false;
        break;
      }

      if (recognized) {
        const NSPoint point = [impl_->view convertPoint:event.locationInWindow
                                               fromView:nil];
        translated.x = point.x;
        translated.y = point.y;
        impl_->events.push_back(translated);
      }
      // Game keyboard input has already been translated above. Do not forward
      // those events into AppKit's text-input/command interpretation path:
      // an unhandled character key can trigger the system alert beep even
      // though the game correctly received its hardware key code.
      const bool game_keyboard_event = event.type == NSEventTypeKeyDown ||
                                       event.type == NSEventTypeKeyUp;
      if (!game_keyboard_event)
        [NSApp sendEvent:event];
    }
    [NSApp updateWindows];

    const CGFloat scale =
        impl_->config.high_dpi ? impl_->window.backingScaleFactor : 1.0;
    const auto new_width =
        pixel_dimension(impl_->view.bounds.size.width, scale);
    const auto new_height =
        pixel_dimension(impl_->view.bounds.size.height, scale);
    if (new_width != impl_->pixel_width || new_height != impl_->pixel_height) {
      impl_->pixel_width = new_width;
      impl_->pixel_height = new_height;
      impl_->events.push_back(WindowEvent{
          .type = WindowEventType::resized,
          .width = new_width,
          .height = new_height,
      });
    }
  }

  if (!impl_->open.load(std::memory_order_acquire) &&
      !impl_->close_event_sent) {
    impl_->events.push_back(
        WindowEvent{.type = WindowEventType::close_requested});
    impl_->close_event_sent = true;
  }
  return true;
}

std::vector<WindowEvent> Window::take_events() {
  if (!impl_) {
    return {};
  }
  auto events = std::move(impl_->events);
  impl_->events.clear();
  return events;
}

void Window::request_close() noexcept {
  if (impl_ && impl_->window != nil && [NSThread isMainThread]) {
    [impl_->window performClose:nil];
  }
}

void Window::set_title(std::string title) {
  if (!impl_ || ![NSThread isMainThread])
    return;
  impl_->config.title = std::move(title);
  impl_->window.title = [NSString stringWithUTF8String:impl_->config.title.c_str()];
}

void Window::set_menu_items(std::span<const WindowMenuItem> items) {
  if (!impl_ || ![NSThread isMainThread])
    return;
  NSMenu *application_menu = [[NSMenu alloc] initWithTitle:@"Astra"];
  for (const auto &entry : items) {
    if (entry.separator) {
      [application_menu addItem:[NSMenuItem separatorItem]];
      continue;
    }
    NSString *label = [NSString stringWithUTF8String:entry.label.c_str()];
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:label
                                                  action:@selector(menuItemSelected:)
                                           keyEquivalent:@""];
    item.target = impl_->delegate;
    item.tag = static_cast<NSInteger>(entry.command_id);
    [application_menu addItem:item];
  }
  NSMenuItem *root = [[NSMenuItem alloc] initWithTitle:@"Settings"
                                                action:nil
                                         keyEquivalent:@""];
  root.submenu = application_menu;
  NSMenu *main_menu = [[NSMenu alloc] initWithTitle:@"Main Menu"];
  [main_menu addItem:root];
  [NSApp setMainMenu:main_menu];
}

void Window::set_display_sync(bool enabled) noexcept {
  if (!impl_ || ![NSThread isMainThread] ||
      ![impl_->view.layer isKindOfClass:[CAMetalLayer class]])
    return;
  CAMetalLayer *metal_layer = (CAMetalLayer *)impl_->view.layer;
  metal_layer.displaySyncEnabled = enabled ? YES : NO;
}

bool Window::is_open() const noexcept {
  return impl_ && impl_->open.load(std::memory_order_acquire);
}

std::uint32_t Window::width() const noexcept {
  return impl_ ? impl_->pixel_width : 0;
}

std::uint32_t Window::height() const noexcept {
  return impl_ ? impl_->pixel_height : 0;
}

const WindowConfig &Window::config() const noexcept {
  static const WindowConfig empty{};
  return impl_ ? impl_->config : empty;
}

void *Window::native_render_surface() const noexcept {
  if (!impl_ || impl_->view == nil) {
    return nullptr;
  }
  return (__bridge void *)impl_->view.layer;
}

} // namespace astra::platform
