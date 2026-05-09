#include "hyprview.hpp"
#include <algorithm>
#include <any>
#include <numeric>
#include <ranges>
#include <unordered_set>
#define private public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/animation/AnimationManager.hpp>
#include <hyprland/src/managers/animation/DesktopAnimationManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/devices/ITouch.hpp>
#include <hyprland/src/render/Renderer.hpp>
#undef private
#include "HyprViewPassElement.hpp"
#include "PlacementAlgorithms.hpp"

// Helper to find the CHyprView instance for a given animation variable
CHyprView *findInstanceForAnimation(
    WP<Hyprutils::Animation::CBaseAnimatedVariable> thisptr) {
  for (auto &[monitor, instance] : g_pHyprViewInstances) {
    if (instance && (instance->size.get() == thisptr.lock().get() ||
                     instance->pos.get() == thisptr.lock().get() ||
                     instance->scale.get() == thisptr.lock().get())) {
      return instance.get();
    }
  }
  return nullptr;
}

void damageMonitor(WP<Hyprutils::Animation::CBaseAnimatedVariable> thisptr) {
  auto *instance = findInstanceForAnimation(thisptr);
  if (instance)
    instance->damage();
}

static float lerp(const float &from, const float &to, const float perc) {
  return (to - from) * perc + from;
}

static Vector2D lerp(const Vector2D &from, const Vector2D &to, const float perc) {
  return Vector2D{lerp(from.x, to.x, perc), lerp(from.y, to.y, perc)};
}

CHyprView::~CHyprView() {

  // If close() wasn't called, do cleanup here
  if (!closing) {
    // Restore all windows to their original workspaces
    for (const auto &image : images) {
      auto window = image.pWindow.lock();
      if (window && image.originalWorkspace &&
          window->m_workspace != image.originalWorkspace) {
        Log::logger->log(Log::INFO,
                   "[hyprview] ~CHyprView(): Restoring window '{}' from workspace {} to {}",
                   window->m_title, window->m_workspace->m_id,
                   image.originalWorkspace->m_id);
        window->moveToWorkspace(image.originalWorkspace);
      }
    }
  }

  // Always cleanup resources in destructor if they haven't been cleaned yet
  if (!images.empty() || bgFramebuffer.m_size.x > 0) {
    Log::logger->log(Log::INFO, "[hyprview] ~CHyprView(): Cleaning up remaining resources");
    g_pHyprRenderer->makeEGLCurrent();
    images.clear();
    bgFramebuffer.release();
    g_pInputManager->restoreCursorIconToApp();
    g_pHyprOpenGL->markBlurDirtyForMonitor(pMonitor.lock());
  }
}

void CHyprView::setupWindowImages(std::vector<PHLWINDOW> &windowsToRender) {
  g_pHyprRenderer->makeEGLCurrent();

  g_pHyprRenderer->m_bBlockSurfaceFeedback = true;

  // Save original workspaces BEFORE moving
  std::unordered_map<PHLWINDOW, PHLWORKSPACE> originalWorkspaces;
  for (auto &window : windowsToRender) {
    originalWorkspaces[window] = window->m_workspace;
  }

  // Move windows to active workspace so they have valid surfaces for rendering
  for (auto &window : windowsToRender) {
    if (window->m_workspace != pMonitor->m_activeWorkspace) {
      Log::logger->log(Log::INFO, "[hyprview] Moving window '{}' from workspace {} to {}",
                 window->m_title, window->m_workspace->m_id,
                 pMonitor->m_activeWorkspace->m_id);
      window->moveToWorkspace(pMonitor->m_activeWorkspace);
    }
  }

  // Render all windows to framebuffers using the box positions set by placement
  // algorithm
  for (size_t i = 0; i < images.size(); ++i) {
    SWindowImage &image = images[i];
    auto &window = windowsToRender[i];

    image.pWindow = window;
    image.originalPos = window->m_realPosition->value();
    image.originalSize = window->m_realSize->value();
    image.originalWorkspace = originalWorkspaces[window];

    const auto RENDERSIZE =
        (window->m_realSize->value() * pMonitor->m_scale).floor();
    image.fb.alloc(std::max(1.0, RENDERSIZE.x), std::max(1.0, RENDERSIZE.y),
                   pMonitor->m_output->state->state().drmFormat);

    CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};

    const auto REALPOS = window->m_realPosition->value();

    // Temporarily move window to monitor position for rendering
    window->m_realPosition->setValue(pMonitor->m_position);

    g_pHyprRenderer->beginRender(pMonitor.lock(), fakeDamage,
                                 RENDER_MODE_FULL_FAKE, nullptr, &image.fb);

    // Clear FB to opaque black so transparent regions of the captured window
    // show solid black instead of stale GPU memory ("noise" on opacity windows).
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (window && window->m_isMapped) {
      g_pHyprRenderer->renderWindow(window, pMonitor.lock(), Time::steadyNow(),
                                    false, RENDER_PASS_MAIN, false, false);
    }

    g_pHyprOpenGL->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->endRender();

    // Restore original position
    window->m_realPosition->setValue(REALPOS);
  }

  // Setup scale animation
  Vector2D fullMonitorSize = pMonitor->m_pixelSize;

  g_pAnimationManager->createAnimation(
      1.0f, scale, g_pConfigManager->getAnimationPropertyConfig("windowsMove"),
      AVARDAMAGE_NONE);

  // Keep size and pos for potential future use (or swipe gestures)
  g_pAnimationManager->createAnimation(
      fullMonitorSize, size,
      g_pConfigManager->getAnimationPropertyConfig("windowsMove"),
      AVARDAMAGE_NONE);
  g_pAnimationManager->createAnimation(
      Vector2D{0, 0}, pos,
      g_pConfigManager->getAnimationPropertyConfig("windowsMove"),
      AVARDAMAGE_NONE);

  scale->setUpdateCallback(damageMonitor);
  size->setUpdateCallback(damageMonitor);
  pos->setUpdateCallback(damageMonitor);

  // Set to initial value and warp (no animation yet)
  scale->setValueAndWarp(0.0f);
  size->setValueAndWarp(fullMonitorSize);
  pos->setValueAndWarp({0, 0});

  // For keyboard shortcut, animate to target; for swipe, gesture controls it
  if (!swipe) {
    *scale = 1.0f;
    *size = fullMonitorSize;
    *pos = {0, 0};
  }

  // Set openedID to first window (for swipe gestures)
  openedID = images.empty() ? -1 : 0;
}

void CHyprView::captureBackground() {
  auto monitor = pMonitor.lock();
  if (!monitor)
    return;

  // Capture background at full monitor size to avoid recursive layers
  const auto MONITOR_SIZE = monitor->m_pixelSize;
  const auto FORMAT = monitor->m_output->state->state().drmFormat;

  // Allocate the background framebuffer at full size
  bgFramebuffer.alloc(MONITOR_SIZE.x, MONITOR_SIZE.y, FORMAT);

  // Get current workspace
  auto activeWorkspace = monitor->m_activeWorkspace;
  if (!activeWorkspace)
    return;

  // Temporarily hide all windows that are visible on this monitor
  // This includes windows assigned to this monitor AND windows from other
  // monitors that "leak" into this one
  std::vector<PHLWINDOW> hiddenWindows;
  std::vector<bool> originalHiddenStates;

  CBox monitorBox = {monitor->m_position.x, monitor->m_position.y,
                     MONITOR_SIZE.x, MONITOR_SIZE.y};

  for (auto &w : g_pCompositor->m_windows) {
    if (!w->m_isMapped || w->isHidden())
      continue;

    // Check if window geometry intersects with this monitor
    CBox windowBox = {w->m_realPosition->value().x,
                      w->m_realPosition->value().y, w->m_realSize->value().x,
                      w->m_realSize->value().y};

    // Check if window overlaps with this monitor using overlaps() method
    if (windowBox.overlaps(monitorBox)) {
      hiddenWindows.push_back(w);
      originalHiddenStates.push_back(w->m_hidden);
      w->m_hidden = true;
    }
  }

  // Capture the background with hidden windows
  CRegion fullRegion{0, 0, (int)MONITOR_SIZE.x, (int)MONITOR_SIZE.y};
  g_pHyprRenderer->beginRender(monitor, fullRegion, RENDER_MODE_FULL_FAKE,
                               nullptr, &bgFramebuffer);
  // Render the workspace with hidden windows (showing just the
  // wallpaper/background)
  g_pHyprRenderer->renderWorkspace(
      monitor, activeWorkspace, std::chrono::steady_clock::now(),
      CBox{0, 0, (int)MONITOR_SIZE.x, (int)MONITOR_SIZE.y});
  g_pHyprRenderer->endRender();

  // Restore all windows to original hidden state
  for (size_t i = 0; i < hiddenWindows.size(); ++i) {
    hiddenWindows[i]->m_hidden = originalHiddenStates[i];
  }

  bgCaptured = true;
}

CHyprView::CHyprView(PHLMONITOR pMonitor_, PHLWORKSPACE startedOn_, bool swipe_,
                     EWindowCollectionMode mode, const std::string &placement,
                     bool explicitOn)
    : pMonitor(pMonitor_), startedOn(startedOn_), swipe(swipe_),
      m_collectionMode(mode), m_placement(placement), stickyOn(explicitOn) {

  // Capture the background BEFORE moving windows for the overview
  captureBackground();

  // Block rendering until we finish moving windows to active workspace
  // This ensures the overview layer is created AFTER workspace migration
  blockOverviewRendering = true;

  originalFocusedWindow = Desktop::focusState()->window();
  userExplicitlySelected = false;
  currentHoveredIndex = -1;
  visualHoveredIndex = -1;

  auto origWindow = originalFocusedWindow.lock();
  Log::logger->log(Log::INFO, "[hyprview] CHyprView(): Saved original focused window: {}",
             (void *)origWindow.get());

  static auto *const *PMARGIN =
      (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(
          PHANDLE, "plugin:hyprview:margin")
          ->getDataStaticPtr();

  MARGIN = **PMARGIN;

  static auto *const *PACTIVEBORDERCOL =
      (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(
          PHANDLE, "plugin:hyprview:active_border_color")
          ->getDataStaticPtr();
  static auto *const *PINACTIVEBORDERCOL =
      (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(
          PHANDLE, "plugin:hyprview:inactive_border_color")
          ->getDataStaticPtr();
  static auto *const *PBORDERWIDTH =
      (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(
          PHANDLE, "plugin:hyprview:border_width")
          ->getDataStaticPtr();
  static auto *const *PBORDERRADIUS =
      (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(
          PHANDLE, "plugin:hyprview:border_radius")
          ->getDataStaticPtr();
  static auto *const *PBGDIM =
      (Hyprlang::FLOAT *const *)HyprlandAPI::getConfigValue(
          PHANDLE, "plugin:hyprview:bg_dim")
          ->getDataStaticPtr();
  static auto *const *PWORKSPACEINDICATORENABLED =
      (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(
          PHANDLE, "plugin:hyprview:workspace_indicator_enabled")
          ->getDataStaticPtr();
  static auto *const *PWORKSPACEINDICATORFONTSIZE =
      (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(
          PHANDLE, "plugin:hyprview:workspace_indicator_font_size")
          ->getDataStaticPtr();
  static auto PWORKSPACEINDICATORPOSITION_VAL = HyprlandAPI::getConfigValue(
      PHANDLE, "plugin:hyprview:workspace_indicator_position");
  static auto *const *PWORKSPACEINDICATORBGOPACITY =
      (Hyprlang::FLOAT *const *)HyprlandAPI::getConfigValue(
          PHANDLE, "plugin:hyprview:workspace_indicator_bg_opacity")
          ->getDataStaticPtr();
  static auto *const *PWINDOWNAMEENABLED =
      (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(
          PHANDLE, "plugin:hyprview:window_name_enabled")
          ->getDataStaticPtr();
  static auto *const *PWINDOWNAMEFONTSIZE =
      (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(
          PHANDLE, "plugin:hyprview:window_name_font_size")
          ->getDataStaticPtr();
  static auto *const *PWINDOWNAMEBGOPACITY =
      (Hyprlang::FLOAT *const *)HyprlandAPI::getConfigValue(
          PHANDLE, "plugin:hyprview:window_name_bg_opacity")
          ->getDataStaticPtr();
  static auto *const *PWINDOWTEXTCOLOR =
      (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(
          PHANDLE, "plugin:hyprview:window_text_color")
          ->getDataStaticPtr();

  ACTIVE_BORDER_COLOR = **PACTIVEBORDERCOL;
  INACTIVE_BORDER_COLOR = **PINACTIVEBORDERCOL;
  BORDER_WIDTH = **PBORDERWIDTH;
  BORDER_RADIUS = **PBORDERRADIUS;
  BG_DIM = **PBGDIM;
  WORKSPACE_INDICATOR_ENABLED = **PWORKSPACEINDICATORENABLED != 0;
  WORKSPACE_INDICATOR_FONT_SIZE = **PWORKSPACEINDICATORFONTSIZE;
  WORKSPACE_INDICATOR_BG_OPACITY = **PWORKSPACEINDICATORBGOPACITY;
  WORKSPACE_INDICATOR_POSITION = "";
  WINDOW_NAME_ENABLED = **PWINDOWNAMEENABLED != 0;
  WINDOW_NAME_FONT_SIZE = **PWINDOWNAMEFONTSIZE;
  WINDOW_NAME_BG_OPACITY = **PWINDOWNAMEBGOPACITY;
  WINDOW_TEXT_COLOR = **PWINDOWTEXTCOLOR;

  try {
    if (PWORKSPACEINDICATORPOSITION_VAL) {
      if (auto strPtr =
              (Hyprlang::STRING const *)
                  PWORKSPACEINDICATORPOSITION_VAL->getDataStaticPtr()) {
        if (*strPtr) {
          WORKSPACE_INDICATOR_POSITION = *strPtr;
        }
      }
    }
  } catch (...) {
    // Keep default on any exception
  }

  std::vector<PHLWINDOW> windowsToRender;

  PHLWORKSPACE activeWorkspace = pMonitor->m_activeWorkspace;

  if (!activeWorkspace)
    return;

  Log::logger->log(Log::INFO,
             "[hyprview] CHyprView(): Collecting windows for monitor '{}', "
             "workspace ID {}, mode={}",
             pMonitor->m_description, activeWorkspace->m_id,
             (int)m_collectionMode);

  // Lambda to check if window should be included based on collection mode
  auto shouldIncludeWindow = [&](PHLWINDOW w) -> bool {
    auto windowWorkspace = w->m_workspace;
    if (!windowWorkspace)
      return false;

    auto windowMonitor = w->m_monitor.lock();
    if (!windowMonitor || windowMonitor != pMonitor.lock())
      return false;

    switch (m_collectionMode) {
    case EWindowCollectionMode::CURRENT_ONLY:
      // Only current workspace
      return windowWorkspace == activeWorkspace;

    case EWindowCollectionMode::ALL_WORKSPACES:
      // All workspaces on monitor, excluding special
      return !windowWorkspace->m_isSpecialWorkspace;

    case EWindowCollectionMode::WITH_SPECIAL:
      // Current workspace + special workspace
      return windowWorkspace == activeWorkspace ||
             windowWorkspace->m_isSpecialWorkspace;

    case EWindowCollectionMode::ALL_WITH_SPECIAL:
      // All workspaces on monitor including special
      return true;
    }
    return false;
  };

  for (auto &w : g_pCompositor->m_windows) {
    if (!w->m_isMapped || w->isHidden())
      continue;

    // Skip fullscreen windows to prevent problems and crashes
    if (w->isFullscreen())
      continue;

    if (!shouldIncludeWindow(w))
      continue;

    windowsToRender.push_back(w);
  }

  // Sort windows: current workspace first, then by X then Y
  std::stable_sort(
      windowsToRender.begin(), windowsToRender.end(),
      [&activeWorkspace](const PHLWINDOW &a, const PHLWINDOW &b) {
        auto wsA = a->m_workspace;
        auto wsB = b->m_workspace;

        // Priority 1: Current workspace first
        bool aIsCurrent = (wsA == activeWorkspace);
        bool bIsCurrent = (wsB == activeWorkspace);
        if (aIsCurrent != bIsCurrent)
          return aIsCurrent; // Current workspace windows come first

        // Priority 2: Within same workspace group, sort by workspace ID
        if (wsA != wsB)
          return wsA->m_id < wsB->m_id;

        // Priority 3: Within same workspace, sort by X then Y (changed from Y
        // then X)
        if (a->m_realPosition->value().x != b->m_realPosition->value().x)
          return a->m_realPosition->value().x < b->m_realPosition->value().x;
        return a->m_realPosition->value().y < b->m_realPosition->value().y;
      });

  // Prepare input for pure placement algorithm
  std::vector<WindowInfo> windowInfos;
  windowInfos.reserve(windowsToRender.size());
  for (size_t i = 0; i < windowsToRender.size(); ++i) {
    auto &w = windowsToRender[i];
    windowInfos.push_back({
        i,                        // id
        w->m_realSize->value().x, // width
        w->m_realSize->value().y  // height
    });
  }

  // Prepare screen info (available area after reserved regions)
  Vector2D reservedTopLeft = Vector2D(pMonitor->m_reservedArea.left(), pMonitor->m_reservedArea.top());
  Vector2D reservedBottomRight = Vector2D(pMonitor->m_reservedArea.right(), pMonitor->m_reservedArea.bottom());
  Vector2D fullMonitorSize = pMonitor->m_pixelSize;

  // Calculate extra bottom margin needed for window names if enabled
  double bottomMarginAdjustment = 0.0;
  if (WINDOW_NAME_ENABLED) {
    // Reserve space for text: font size + padding + background padding
    // Approximate height: font_size * 1.5 (for rendering) + top/bottom padding (8px total)
    bottomMarginAdjustment = WINDOW_NAME_FONT_SIZE * 1.5 + 8.0;
  }

  // Optional max-width clamp (centers the layout on ultrawide screens)
  static auto *const *PMAXWIDTH =
      (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(
          PHANDLE, "plugin:hyprview:max_width")
          ->getDataStaticPtr();
  const int MAX_WIDTH = **PMAXWIDTH;

  double availableWidth = fullMonitorSize.x - reservedTopLeft.x - reservedBottomRight.x;
  double offsetX = reservedTopLeft.x;
  if (MAX_WIDTH > 0 && (double)MAX_WIDTH < availableWidth) {
    offsetX = reservedTopLeft.x + (availableWidth - (double)MAX_WIDTH) / 2.0;
    availableWidth = (double)MAX_WIDTH;
  }

  ScreenInfo screenInfo = {
      availableWidth,                                                                         // width
      fullMonitorSize.y - reservedTopLeft.y - reservedBottomRight.y - bottomMarginAdjustment, // height (adjusted for window names)
      offsetX,                                                                                // offsetX
      reservedTopLeft.y,                                                                      // offsetY
      (double)MARGIN                                                                          // margin
  };

  // Call the placement function based on m_placement
  PlacementResult placementResult;
  if (m_placement == "spiral") {
    placementResult = spiralPlacement(windowInfos, screenInfo);
  } else if (m_placement == "flow") {
    placementResult = flowPlacement(windowInfos, screenInfo);
  } else if (m_placement == "adaptive") {
    placementResult = adaptivePlacement(windowInfos, screenInfo);
  } else if (m_placement == "wide") {
    placementResult = widePlacement(windowInfos, screenInfo);
  } else if (m_placement == "scale") {
    placementResult = scalePlacement(windowInfos, screenInfo);
  } else {
    // Default to grid placement
    placementResult = gridPlacement(windowInfos, screenInfo);
  }

  // Apply placement results to images
  images.resize(placementResult.tiles.size());
  for (size_t i = 0; i < placementResult.tiles.size(); ++i) {
    images[i].box = {placementResult.tiles[i].x, placementResult.tiles[i].y,
                     placementResult.tiles[i].width,
                     placementResult.tiles[i].height};
  }

  Log::logger->log(
      Log::INFO,
      "[hyprview] Placement algorithm '{}' generated {}x{} grid with {} tiles",
      m_placement, placementResult.gridCols, placementResult.gridRows,
      placementResult.tiles.size());

  // Now call common setup to handle window rendering
  setupWindowImages(windowsToRender);

  g_pHyprRenderer->m_bBlockSurfaceFeedback = false;

  // removed in 0.54: no direct cursor-icon-by-name setter
  // g_pInputManager->setCursorImageUntilUnset("left_ptr");

  lastMousePosLocal =
      g_pInputManager->getMouseCoordsInternal() - pMonitor->m_position;

  auto onCursorMove = [this](Event::SCallbackInfo &info) {
    if (closing)
      return;

    // Check if mouse is actually on this monitor BEFORE cancelling
    Vector2D globalMousePos = g_pInputManager->getMouseCoordsInternal();
    Vector2D monitorPos = pMonitor->m_position;
    Vector2D fullMonitorSize = pMonitor->m_pixelSize;

    bool mouseOnThisMonitor =
        (globalMousePos.x >= monitorPos.x &&
         globalMousePos.x < monitorPos.x + fullMonitorSize.x &&
         globalMousePos.y >= monitorPos.y &&
         globalMousePos.y < monitorPos.y + fullMonitorSize.y);

    if (!mouseOnThisMonitor) {
      return; // Mouse is on a different monitor - don't cancel event
    }

    lastMousePosLocal = globalMousePos - monitorPos;

    if (!images.empty()) {
      int tileIndex = getWindowIndexFromMousePos(lastMousePosLocal);
      updateHoverState(tileIndex);
    }

    // Cancel move events in overview mode
    info.cancelled = true;
  };

  auto onCursorSelect = [this](Event::SCallbackInfo &info) {
    if (closing)
      return;

    // Check if mouse is on this monitor BEFORE cancelling
    Vector2D globalMousePos = g_pInputManager->getMouseCoordsInternal();
    Vector2D monitorPos = pMonitor->m_position;
    Vector2D fullMonitorSize = pMonitor->m_pixelSize;

    bool mouseOnThisMonitor =
        (globalMousePos.x >= monitorPos.x &&
         globalMousePos.x < monitorPos.x + fullMonitorSize.x &&
         globalMousePos.y >= monitorPos.y &&
         globalMousePos.y < monitorPos.y + fullMonitorSize.y);

    if (!mouseOnThisMonitor) {
      return; // Mouse is on a different monitor - don't cancel event
    }

    // If explicitly turned on, project click to real window
    if (stickyOn) {
      info.cancelled = true;

      Vector2D localMousePos = globalMousePos - monitorPos;
      int tileIndex = getWindowIndexFromMousePos(localMousePos);

      if (tileIndex >= 0 && tileIndex < (int)images.size()) {
        auto window = images[tileIndex].pWindow.lock();
        if (window && window->m_isMapped) {
          // Focus the window first
          Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_OTHER);

          // Calculate mouse position relative to tile
          const CBox &tileBox = images[tileIndex].box;
          Vector2D mousePosInTile = {localMousePos.x - tileBox.x,
                                     localMousePos.y - tileBox.y};

          // Calculate scale factor from tile to real window
          Vector2D realWindowSize = window->m_realSize->value();
          Vector2D scaleFactors = {realWindowSize.x / tileBox.width,
                                   realWindowSize.y / tileBox.height};

          // Project to real window coordinates
          Vector2D projectedPos = {mousePosInTile.x * scaleFactors.x,
                                   mousePosInTile.y * scaleFactors.y};

          // Warp cursor to projected position on real window
          Vector2D realWindowPos = window->m_realPosition->value();
          Vector2D targetGlobalPos = realWindowPos + projectedPos;

          // Use InputManager to move mouse to projected position
          g_pInputManager->mouseMoveUnified(0, true, true, targetGlobalPos);
        }
      }

      return;
    }

    // Normal mode: cancel click, select window, and close ALL overviews except
    // forced ones
    info.cancelled = true;
    selectHoveredWindow();

    // Close all overview instances except those with stickyOn=true
    for (auto &[monitor, instance] : g_pHyprViewInstances) {
      if (instance && !instance->stickyOn) {
        instance->close();
      }
    }
  };

  auto onMouseAxis = [this](Event::SCallbackInfo &info) {
    if (closing)
      return;

    // Check if mouse is on this monitor
    Vector2D globalMousePos = g_pInputManager->getMouseCoordsInternal();
    Vector2D monitorPos = pMonitor->m_position;
    Vector2D fullMonitorSize = pMonitor->m_pixelSize;

    bool mouseOnThisMonitor =
        (globalMousePos.x >= monitorPos.x &&
         globalMousePos.x < monitorPos.x + fullMonitorSize.x &&
         globalMousePos.y >= monitorPos.y &&
         globalMousePos.y < monitorPos.y + fullMonitorSize.y);

    if (!mouseOnThisMonitor) {
      return; // Mouse is on a different monitor - don't interfere
    }

    // If explicitly on, ensure the window under the cursor is focused for
    // scroll
    if (stickyOn) {
      Vector2D localMousePos = globalMousePos - monitorPos;
      int tileIndex = getWindowIndexFromMousePos(localMousePos);

      if (tileIndex >= 0 && tileIndex < (int)images.size()) {
        auto window = images[tileIndex].pWindow.lock();
        if (window && window->m_isMapped) {
          // Make sure this window is focused so scroll events go to it
          Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_OTHER);
          // Don't cancel - let scroll event pass through to the focused window
          return;
        }
      }
    }

    // In normal (non-explicit) overview mode, don't do anything special with
    // scroll The focused window from hover will receive it
  };

  mouseMoveHook = Event::bus()->m_events.input.mouse.move.listen(
      [onCursorMove](const Vector2D &, Event::SCallbackInfo &info) { onCursorMove(info); });
  touchMoveHook = Event::bus()->m_events.input.touch.motion.listen(
      [onCursorMove](const ITouch::SMotionEvent &, Event::SCallbackInfo &info) { onCursorMove(info); });

  mouseButtonHook = Event::bus()->m_events.input.mouse.button.listen(
      [onCursorSelect](const IPointer::SButtonEvent &, Event::SCallbackInfo &info) { onCursorSelect(info); });
  mouseAxisHook = Event::bus()->m_events.input.mouse.axis.listen(
      [onMouseAxis](const IPointer::SAxisEvent &, Event::SCallbackInfo &info) { onMouseAxis(info); });
  touchDownHook = Event::bus()->m_events.input.touch.down.listen(
      [onCursorSelect](const ITouch::SDownEvent &, Event::SCallbackInfo &info) { onCursorSelect(info); });

  // NOW unblock rendering - workspace migration is complete
  // The overview layer will be created on the next render pass
  blockOverviewRendering = false;
  Log::logger->log(
      Log::INFO, "[hyprview] CHyprView(): Constructor complete, unblocked rendering");
}

void CHyprView::selectHoveredWindow() {
  if (closing)
    return;

  // Use the currently tracked hovered index for consistency
  closeOnID = currentHoveredIndex;

  // Safety validation - ensure we have a valid window to select
  if (closeOnID < 0 || closeOnID >= (int)images.size()) {
    Log::logger->log(Log::WARN,
               "[hyprview] selectHoveredWindow(): Invalid currentHoveredIndex "
               "{}, recalculating from mouse position",
               closeOnID);

    // Fallback: recalculate from current mouse position
    closeOnID = getWindowIndexFromMousePos(lastMousePosLocal);

    // Final fallback: use bounds-clamped value
    if (closeOnID < 0 || closeOnID >= (int)images.size()) {
      closeOnID = std::max(0, std::min((int)images.size() - 1, 0));
    }
  }

  // Verify the selected window is valid
  if (closeOnID >= 0 && closeOnID < (int)images.size()) {
    auto selectedWindow = images[closeOnID].pWindow.lock();
    if (!selectedWindow || !selectedWindow->m_isMapped) {
      Log::logger->log(Log::WARN,
                 "[hyprview] selectHoveredWindow(): Selected window at index "
                 "{} is invalid",
                 closeOnID);
    }
  }

  userExplicitlySelected = true;

  Log::logger->log(Log::INFO,
             "[hyprview] selectHoveredWindow(): User explicitly selected "
             "window at index {} (from currentHoveredIndex {})",
             closeOnID, currentHoveredIndex);
}

void CHyprView::redrawID(int id, bool forcelowres) {
  blockOverviewRendering = true;

  g_pHyprRenderer->makeEGLCurrent();

  if (id >= (int)images.size())
    id = images.size() - 1;
  if (id < 0)
    id = 0;

  auto &image = images[id];
  auto window = image.pWindow.lock();
  if (!window) {
    blockOverviewRendering = false;
    return;
  }

  const auto RENDERSIZE =
      (window->m_realSize->value() * pMonitor->m_scale).floor();
  if (RENDERSIZE.x < 1 || RENDERSIZE.y < 1) {
    blockOverviewRendering = false;
    return;
  }

  if (image.fb.m_size.x != RENDERSIZE.x || image.fb.m_size.y != RENDERSIZE.y) {
    image.fb.release();
    image.fb.alloc(RENDERSIZE.x, RENDERSIZE.y,
                   pMonitor->m_output->state->state().drmFormat);
  }

  CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};

  const auto REALPOS = window->m_realPosition->value();
  window->m_realPosition->setValue(pMonitor->m_position);

  g_pHyprRenderer->beginRender(pMonitor.lock(), fakeDamage,
                               RENDER_MODE_FULL_FAKE, nullptr, &image.fb);

  // Clear FB to opaque black so transparent windows don't show GPU-memory noise
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  if (window->m_isMapped) {
    g_pHyprRenderer->renderWindow(window, pMonitor.lock(), Time::steadyNow(),
                                  false, RENDER_PASS_MAIN, false, false);
  }

  g_pHyprOpenGL->m_renderData.blockScreenShader = true;
  g_pHyprRenderer->endRender();

  window->m_realPosition->setValue(REALPOS);

  blockOverviewRendering = false;
}

void CHyprView::redrawAll(bool forcelowres) {
  for (size_t i = 0; i < images.size(); ++i) {
    redrawID(i, forcelowres);
  }
}

void CHyprView::damage() {
  blockDamageReporting = true;
  g_pHyprRenderer->damageMonitor(pMonitor.lock());
  blockDamageReporting = false;
}

void CHyprView::onDamageReported() {
  damageDirty = true;

  // Damage the entire overview area
  damage();

  // If there's a focused window, damage its tile specifically
  if (openedID >= 0 && openedID < (int)images.size()) {
    CBox tileBox = images[openedID].box;
    tileBox.translate(pMonitor->m_position);
    blockDamageReporting = true;
    g_pHyprRenderer->damageBox(tileBox);
    blockDamageReporting = false;
  }

  g_pCompositor->scheduleFrameForMonitor(pMonitor.lock());
}

void CHyprView::close() {

  if (closing) {
    Log::logger->log(Log::INFO, "[hyprview] close(): already closing, returning");
    return;
  }

  closing = true;

  // Save the selected window before cleanup
  PHLWINDOW selectedWindow = nullptr;
  if (userExplicitlySelected && closeOnID >= 0 &&
      closeOnID < (int)images.size()) {
    selectedWindow = images[closeOnID].pWindow.lock();
  }

  // STEP 1: Restore ALL windows to their original workspaces
  // The overview layer continues rendering during this process to hide the movement
  Log::logger->log(
      Log::INFO, "[hyprview] close(): Restoring all windows to original workspaces");
  for (const auto &image : images) {
    auto window = image.pWindow.lock();
    if (window && image.originalWorkspace &&
        window->m_workspace != image.originalWorkspace) {
      Log::logger->log(
          Log::INFO,
          "[hyprview] close(): Restoring window '{}' from workspace {} to {}",
          window->m_title, window->m_workspace->m_id,
          image.originalWorkspace->m_id);
      window->moveToWorkspace(image.originalWorkspace);
    }
  }

  // STEP 2: Start closing animationi - animate scale back to 0
  Log::logger->log(Log::INFO, "[hyprview] close(): Start closing animation");
  *scale = 0.0f;

  // STEP 3: Focus the selected window to trigger all lifecycle events
  if (userExplicitlySelected && selectedWindow) {
    Desktop::focusState()->fullWindowFocus(selectedWindow, Desktop::FOCUS_REASON_OTHER);
    g_pKeybindManager->alterZOrder("top");
  }
}

void CHyprView::onPreRender() {
  // Re-capture window framebuffers only when truly dirty AND not closing.
  // During close animation, cached textures are sufficient — re-rendering
  // every window every frame is what made the close lag visible.
  if (damageDirty && !closing) {
    damageDirty = false;
    redrawAll(false);
  } else if (closing) {
    damageDirty = false; // consume; never re-capture during close
  }

  // If we're closing and animation has finished, mark for cleanup.
  // Heavy GPU teardown (images.clear() destroys ~10 framebuffers,
  // bgFramebuffer.release() drops the full-monitor capture) caused
  // a single-frame stall right at the end of the close animation
  // — the "lag into place" jank. Defer all that to the destructor,
  // which runs when the next toggle erases the instance.
  if (closing && scale->value() <= 0.01f && !readyForCleanup) {
    Log::logger->log(Log::INFO, "[hyprview] onPreRender(): Closing animation complete");
    readyForCleanup = true;
    g_pInputManager->restoreCursorIconToApp();
    g_pHyprOpenGL->markBlurDirtyForMonitor(pMonitor.lock());
    g_pHyprRenderer->damageMonitor(pMonitor.lock());
  }
}

void CHyprView::onWorkspaceChange() {}

void CHyprView::render() {
  g_pHyprRenderer->m_renderPass.add(makeUnique<CHyprViewPassElement>(this));
}

void CHyprView::fullRender() {
  // Get the current scale value for smooth scale animation
  const float currentScale = scale->value();
  const float currentAlpha = 1.0f; // Keep alpha fixed, removing all fade animations

  // Render the captured background instead of a solid color
  if (bgCaptured && bgFramebuffer.m_size.x > 0 && bgFramebuffer.m_size.y > 0) {
    Vector2D fullMonitorSize = pMonitor->m_pixelSize;
    CBox monitorBox = {0, 0, fullMonitorSize.x, fullMonitorSize.y};
    CRegion damage{0, 0, INT16_MAX, INT16_MAX};
    g_pHyprOpenGL->renderTextureInternal(
        bgFramebuffer.getTexture(), monitorBox,
        {.damage = &damage, .a = 1.0, .round = 0});

    // Add a dim overlay that fades in with the overview.
    // SRectRenderData.damage is a non-null required pointer in 0.54 —
    // passing {} crashes the compositor with malloc heap-corruption.
    CHyprOpenGLImpl::SRectRenderData dimData;
    dimData.damage = &damage;
    g_pHyprOpenGL->renderRect(
        monitorBox, CHyprColor(0.0, 0.0, 0.0, BG_DIM * currentAlpha), dimData);
  }

  // If no windows, show centered message
  if (images.empty()) {
    Vector2D fullMonitorSize = pMonitor->m_pixelSize;
    std::string emptyMessage = "Overview (no windows)";
    int fontSize = 32;

    auto textTexture = g_pHyprOpenGL->renderText(
        emptyMessage, CHyprColor(1.0, 1.0, 1.0, currentAlpha), fontSize, false,
        "sans-serif");

    if (textTexture) {
      double textWidth = textTexture->m_size.x * 0.8;
      double textHeight = textTexture->m_size.y * 0.8;

      double centerX = (fullMonitorSize.x - textWidth) / 2.0;
      double centerY = (fullMonitorSize.y - textHeight) / 2.0;

      CBox textBox = {centerX, centerY, textWidth, textHeight};

      // Render subtle background box
      CBox bgBox = {centerX - 30, centerY - 20, textWidth + 60,
                    textHeight + 40};
      CRegion bgBoxDamage{0, 0, INT16_MAX, INT16_MAX};
      CHyprOpenGLImpl::SRectRenderData bgData;
      bgData.round = 12;
      bgData.damage = &bgBoxDamage;
      g_pHyprOpenGL->renderRect(
          bgBox, CHyprColor(0.0, 0.0, 0.0, 0.5 * currentAlpha), bgData);

      // Render the text
      CRegion damage{0, 0, INT16_MAX, INT16_MAX};
      g_pHyprOpenGL->renderTextureInternal(
          textTexture, textBox,
          {.damage = &damage, .a = currentAlpha, .round = 0});
    }

    return;
  }

  const auto PLASTWINDOW = Desktop::focusState()->window();
  const auto PLASTWORKSPACE = Desktop::focusState()->window();

  // Floating windows are rendered on top of tiled windows.
  // Z-order of windows from other workspaces does not matter.
  std::unordered_map<Desktop::View::CWindow *, size_t> zOrderMap;
  zOrderMap.reserve(g_pCompositor->m_windows.size());
  size_t zOrder = 0;
  for (auto &window : g_pCompositor->m_windows) {
    if (window->m_workspace == PLASTWORKSPACE && !window->m_isFloating)
      zOrderMap.try_emplace(window.get(), zOrder++);
  }
  for (auto &window : g_pCompositor->m_windows) {
    if (window->m_workspace == PLASTWORKSPACE && window->m_isFloating)
      zOrderMap.try_emplace(window.get(), zOrder++);
  }

  std::vector<size_t> renderOrder(images.size());
  std::iota(renderOrder.begin(), renderOrder.end(), 0);

  std::stable_sort(renderOrder.begin(), renderOrder.end(), [this, &zOrderMap](size_t a, size_t b) {
    auto winA = images[a].pWindow;
    auto winB = images[b].pWindow;
    if (!winA || !winB)
      return false;
    auto itA = zOrderMap.find(winA.get());
    auto itB = zOrderMap.find(winB.get());
    // don't care when one window is from a different workspace
    // (so, not in the map): already handled in the constructor
    if (itA == zOrderMap.end() || itB == zOrderMap.end())
      return false;
    return itA->second < itB->second;
  });

  for (auto i : renderOrder) {
    const Vector2D &textureSize = images[i].fb.m_size;

    if (textureSize.x < 1 || textureSize.y < 1)
      continue;

    // Use the EXACT box position calculated by the placement algorithm
    // No modifications, no centering - the placement algorithm is authoritative
    CBox tileBox = images[i].box;

    // Calculate aspect-ratio-preserving size within the tile
    const double textureAspect = textureSize.x / textureSize.y;
    const double cellAspect = tileBox.width / tileBox.height;

    Vector2D newSize;
    if (textureAspect > cellAspect) {
      newSize.x = tileBox.width;
      newSize.y = newSize.x / textureAspect;
    } else {
      newSize.y = tileBox.height;
      newSize.x = newSize.y * textureAspect;
    }

    // Center the window within its tile
    const double offsetX = (tileBox.width - newSize.x) / 2.0;
    const double offsetY = (tileBox.height - newSize.y) / 2.0;
    Vector2D newPos = {tileBox.x + offsetX, tileBox.y + offsetY};

    // Interpolate position and size for move animation
    Vector2D originalPosLocal = images[i].originalPos - pMonitor->m_position;
    Vector2D animPos = lerp(originalPosLocal, newPos, currentScale);
    Vector2D animSize = lerp(images[i].originalSize, newSize, currentScale);

    CBox windowBox = {animPos.x, animPos.y, animSize.x, animSize.y};
    CBox borderBox = {
        windowBox.x - BORDER_WIDTH,
        windowBox.y - BORDER_WIDTH,
        windowBox.width + 2 * BORDER_WIDTH,
        windowBox.height + 2 * BORDER_WIDTH};

    const bool ISACTIVE = images[i].pWindow.lock() == PLASTWINDOW;
    const auto &BORDERCOLOR =
        ISACTIVE ? ACTIVE_BORDER_COLOR : INACTIVE_BORDER_COLOR;

    // Translate both boxes for the overview animation
    borderBox.translate(pos->value());
    borderBox.round();
    windowBox.translate(pos->value());
    windowBox.round();

    // Apply alpha to border color for smooth fade
    CHyprColor fadedBorderColor = BORDERCOLOR;
    fadedBorderColor.a *= currentAlpha;

    // Skip border renderRect entirely when border is invisible
    // (user config has border_width=0). Saves N renderRect calls per
    // frame for N tiles AND avoids a Hyprland 0.54 crash path that
    // appears tied to renderRect under heavy tile counts.
    if (BORDER_WIDTH > 0) {
      CRegion borderDamage{0, 0, INT16_MAX, INT16_MAX};
      CHyprOpenGLImpl::SRectRenderData data;
      data.round = BORDER_RADIUS;
      data.damage = &borderDamage;
      g_pHyprOpenGL->renderRect(borderBox, fadedBorderColor, data);
    }

    // Hover grow: scale the hovered tile up slightly (centered on tile).
    // Gives clear visual feedback even with borders disabled, mimicking
    // macOS Mission Control / Wayfire scale's "raise on hover" affordance.
    const bool ISHOVERED = ((int)i == visualHoveredIndex);
    if (ISHOVERED) {
      constexpr double HOVER_SCALE = 1.05;
      const double dw = windowBox.width  * (HOVER_SCALE - 1.0);
      const double dh = windowBox.height * (HOVER_SCALE - 1.0);
      windowBox.x -= dw / 2.0;
      windowBox.y -= dh / 2.0;
      windowBox.width  *= HOVER_SCALE;
      windowBox.height *= HOVER_SCALE;
    }

    CRegion damage{0, 0, INT16_MAX, INT16_MAX};
    g_pHyprOpenGL->renderTextureInternal(
        images[i].fb.getTexture(), windowBox,
        {.damage = &damage, .a = currentAlpha, .round = BORDER_RADIUS});

    // Labels (workspace ID + window name) are heavy: each tile rasterizes
    // ~13 cairo text textures per frame because of the binary-search
    // truncation in renderWindowName. With 10 tiles × 60fps that's ~7800
    // text rasters/sec — kills both animation smoothness and hover.
    // Only render labels when the overview is fully open (scale ≈ 1.0)
    // and not closing. During open/close animation, skip labels entirely.
    const bool LABELS_VISIBLE = !closing && currentScale >= 0.98f;
    if (LABELS_VISIBLE) {
      // Render workspace number indicator (if enabled and window names are disabled)
      // When window names are enabled, the workspace ID is integrated into the window name
      if (WORKSPACE_INDICATOR_ENABLED && !WINDOW_NAME_ENABLED) {
        auto window = images[i].pWindow.lock();
        if (window && images[i].originalWorkspace) {
          renderWorkspaceIndicator(i, borderBox, damage, ISACTIVE);
        }
      }

      // Render window name (if enabled)
      if (WINDOW_NAME_ENABLED) {
        renderWindowName(images[i], borderBox);
      }
    }
  }
}

void CHyprView::renderWorkspaceIndicator(size_t i, const CBox &borderBox,
                                         const CRegion &damage,
                                         const bool ISACTIVE) {
  int workspaceID = images[i].originalWorkspace->m_id;
  std::string workspaceText = "wsid:" + std::to_string(workspaceID);
  // Use border color based on whether window is active
  const auto &INDICATOR_COLOR =
      ISACTIVE ? ACTIVE_BORDER_COLOR : INACTIVE_BORDER_COLOR;
  auto textTexture = g_pHyprOpenGL->renderText(workspaceText, INDICATOR_COLOR,
                                               WORKSPACE_INDICATOR_FONT_SIZE,
                                               false, "sans-serif");

  if (textTexture) {
    double textPadding = 15.0;
    double textX, textY;

    // Calculate position based on configured position
    if (WORKSPACE_INDICATOR_POSITION == "top-left") {
      textX = borderBox.x + textPadding;
      textY = borderBox.y + textPadding;
    } else if (WORKSPACE_INDICATOR_POSITION == "bottom-left") {
      textX = borderBox.x + textPadding;
      textY = borderBox.y + borderBox.height - (textTexture->m_size.y * 0.8) -
              textPadding;
    } else if (WORKSPACE_INDICATOR_POSITION == "bottom-right") {
      textX = borderBox.x + borderBox.width - (textTexture->m_size.x * 0.8) -
              textPadding;
      textY = borderBox.y + borderBox.height - (textTexture->m_size.y * 0.8) -
              textPadding;
    } else {
      textX = borderBox.x + borderBox.width - (textTexture->m_size.x * 0.8) -
              textPadding;
      textY = borderBox.y + textPadding;
    }

    // Scale text size appropriately
    double textWidth = textTexture->m_size.x * 0.8;
    double textHeight = textTexture->m_size.y * 0.8;

    CBox textBox = {textX, textY, textWidth, textHeight};

    // Render background for text with configured opacity
    CBox textBgBox = {textX - 8, textY - 8, textWidth + 16, textHeight + 16};
    CHyprOpenGLImpl::SRectRenderData bgData;
    bgData.round = 8;
    bgData.damage = &damage;
    g_pHyprOpenGL->renderRect(
        textBgBox, CHyprColor(0.0, 0.0, 0.0, WORKSPACE_INDICATOR_BG_OPACITY),
        bgData);

    // Render the text on top
    g_pHyprOpenGL->renderTextureInternal(
        textTexture, textBox, {.damage = &damage, .a = 1.0, .round = 0});
  }
}

void CHyprView::renderWindowName(const SWindowImage &image,
                                 const CBox &borderBox) {
  auto window = image.pWindow.lock();
  if (!window)
    return;

  // Build separate strings for workspace ID and window info
  std::string workspaceText;
  std::string windowText = window->m_initialClass + " • " + window->m_title;

  // Determine workspace text color based on whether window is active
  const auto PLASTWINDOW = Desktop::focusState()->window();
  const bool ISACTIVE = window == PLASTWINDOW;
  const auto &WORKSPACE_COLOR = ISACTIVE ? ACTIVE_BORDER_COLOR : INACTIVE_BORDER_COLOR;

  // Include workspace ID if workspace indicator is enabled
  if (WORKSPACE_INDICATOR_ENABLED && image.originalWorkspace) {
    workspaceText = "[" + std::to_string(image.originalWorkspace->m_id) + "] ";
  }

  // Render workspace text to get its width
  SP<CTexture> workspaceTexture;
  double workspaceWidth = 0.0;
  if (!workspaceText.empty()) {
    workspaceTexture = g_pHyprOpenGL->renderText(
        workspaceText, WORKSPACE_COLOR, WINDOW_NAME_FONT_SIZE, false, "sans-serif");
    if (workspaceTexture) {
      workspaceWidth = workspaceTexture->m_size.x * 0.8;
    }
  }

  // Calculate available width for the window text
  double bgPadding = 4.0;
  double availableWidth = borderBox.width - workspaceWidth - (2 * bgPadding);

  // Helper function to truncate string with smart ellipsis
  auto truncateWithEllipsis = [&](const std::string &text, double maxWidth) -> std::string {
    // First check if truncation is needed
    auto fullTexture = g_pHyprOpenGL->renderText(
        text, WINDOW_TEXT_COLOR, WINDOW_NAME_FONT_SIZE, false, "sans-serif");
    if (!fullTexture)
      return text;

    double fullWidth = fullTexture->m_size.x * 0.8;
    if (fullWidth <= maxWidth)
      return text; // No truncation needed

    // Calculate how many characters we can fit
    // Use binary search approach with ellipsis " ... "
    std::string ellipsis = " ... ";
    auto ellipsisTexture = g_pHyprOpenGL->renderText(
        ellipsis, WINDOW_TEXT_COLOR, WINDOW_NAME_FONT_SIZE, false, "sans-serif");
    double ellipsisWidth = ellipsisTexture ? ellipsisTexture->m_size.x * 0.8 : 30.0;

    // Reserve space for ellipsis
    double targetWidth = maxWidth - ellipsisWidth;
    if (targetWidth <= 0)
      return ellipsis; // Too small, just show ellipsis

    // Try to fit approximately equal parts from start and end
    size_t textLen = text.length();
    size_t startChars = textLen / 3; // Take roughly 1/3 from start
    size_t endChars = textLen / 3;   // Take roughly 1/3 from end

    // Binary search to find optimal lengths
    for (int attempts = 0; attempts < 10; attempts++) {
      if (startChars + endChars >= textLen)
        break;

      std::string truncated = text.substr(0, startChars) + ellipsis +
                              text.substr(textLen - endChars);

      auto testTexture = g_pHyprOpenGL->renderText(
          truncated, WINDOW_TEXT_COLOR, WINDOW_NAME_FONT_SIZE, false, "sans-serif");
      if (!testTexture)
        break;

      double testWidth = testTexture->m_size.x * 0.8;

      if (testWidth <= maxWidth) {
        // Try to add more characters
        startChars = std::min(startChars + 2, textLen / 2);
        endChars = std::min(endChars + 2, textLen / 2);
      } else {
        // Too wide, reduce
        if (startChars > 3)
          startChars -= 1;
        if (endChars > 3)
          endChars -= 1;
        if (startChars <= 3 && endChars <= 3)
          break;
      }
    }

    // Ensure we have at least a few characters
    startChars = std::max(size_t(3), std::min(startChars, textLen / 2));
    endChars = std::max(size_t(3), std::min(endChars, textLen / 2));

    return text.substr(0, startChars) + ellipsis + text.substr(textLen - endChars);
  };

  // Truncate window text if necessary
  if (availableWidth > 50) { // Only truncate if we have reasonable space
    windowText = truncateWithEllipsis(windowText, availableWidth);
  }

  // Render window text
  auto windowTexture = g_pHyprOpenGL->renderText(
      windowText, WINDOW_TEXT_COLOR, WINDOW_NAME_FONT_SIZE, false, "sans-serif");

  if (windowTexture) {
    double windowWidth = windowTexture->m_size.x * 0.8;
    double textHeight = windowTexture->m_size.y * 0.8;

    // Calculate total width
    double totalWidth = workspaceWidth + windowWidth;

    // Center the entire label horizontally over the bottom border
    double startX = borderBox.x + (borderBox.width - totalWidth) / 2.0;

    // Position vertically: center over the bottom border
    double textY = borderBox.y + borderBox.height - (textHeight + 2 * bgPadding) / 2.0;

    // Render background for entire text
    CBox textBgBox = {startX - bgPadding, textY - bgPadding,
                      totalWidth + 2 * bgPadding, textHeight + 2 * bgPadding};
    CRegion textBgDamage{0, 0, INT16_MAX, INT16_MAX};
    CHyprOpenGLImpl::SRectRenderData bgData;
    bgData.round = 4;
    bgData.damage = &textBgDamage;
    g_pHyprOpenGL->renderRect(
        textBgBox, CHyprColor(0.0, 0.0, 0.0, WINDOW_NAME_BG_OPACITY), bgData);

    CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};

    // Render workspace text first (if present)
    if (workspaceTexture) {
      CBox workspaceBox = {startX, textY, workspaceWidth, textHeight};
      g_pHyprOpenGL->renderTextureInternal(
          workspaceTexture, workspaceBox, {.damage = &fakeDamage, .a = 1.0, .round = 0});
    }

    // Render window text after workspace text
    CBox windowBox = {startX + workspaceWidth, textY, windowWidth, textHeight};
    g_pHyprOpenGL->renderTextureInternal(
        windowTexture, windowBox, {.damage = &fakeDamage, .a = 1.0, .round = 0});
  }
}

void CHyprView::setClosing(bool closing_) { closing = closing_; }

void CHyprView::resetSwipe() { swipeWasCommenced = false; }

void CHyprView::onSwipeUpdate(double delta) {
  m_isSwiping = true;

  if (swipeWasCommenced)
    return;

  static auto *const *PDISTANCE =
      (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(
          PHANDLE, "plugin:hyprview:gesture_distance")
          ->getDataStaticPtr();

  // Calculate progress percentage based on swipe direction
  // For opening: delta 0 -> distance means scale 0 -> 1 (original -> tile)
  // For closing: delta 0 -> distance means scale 1 -> 0 (tile -> original)
  const float PERC = std::clamp(delta / (double)**PDISTANCE, 0.0, 1.0);
  scale->setValueAndWarp(closing ? (1.0f - PERC) : PERC);
}

void CHyprView::onSwipeEnd() {
  // Check if swipe crossed the halfway threshold
  const float currentScale = scale->value();

  if (closing || currentScale < 0.5f) {
    // Swipe finished in closing direction - close the overview
    *scale = 0.0f;
    close();
    return;
  }

  // Swipe cancelled - animate back to full visibility
  *scale = 1.0f;

  swipeWasCommenced = true;
  m_isSwiping = false;
}

int CHyprView::getWindowIndexFromMousePos(const Vector2D &mousePos) {
  if (images.empty())
    return -1;

  // Generic approach: iterate through all tiles and check if mouse is within
  // their boxes This works with ANY placement algorithm, not just grids The
  // placement algorithm has already calculated EXACT positions - we trust them
  // completely
  for (size_t i = 0; i < images.size(); ++i) {
    const CBox &tileBox = images[i].box;

    // Check if mouse is within this tile's bounds
    if (mousePos.x >= tileBox.x && mousePos.x <= tileBox.x + tileBox.width &&
        mousePos.y >= tileBox.y && mousePos.y <= tileBox.y + tileBox.height) {
      return i;
    }
  }

  // Mouse is not over any tile
  return -1;
}

bool CHyprView::isMouseOverValidTile(const Vector2D &mousePos) {
  return getWindowIndexFromMousePos(mousePos) != -1;
}

void CHyprView::updateHoverState(int newIndex) {
  // Early-exit if hover hasn't actually moved to a different tile.
  // Mouse can move many pixels within the same tile and we'd otherwise
  // call fullWindowFocus + damage on every event (~100/sec on ultrawide),
  // making hover laggy.
  if (newIndex == currentHoveredIndex)
    return;

  // Update visual hover state and trigger one redraw for the size change.
  visualHoveredIndex = newIndex;
  currentHoveredIndex = newIndex;
  damage();

  // Focus the newly-hovered window once. fullWindowFocus is heavy
  // (fires signals, repaints decorations); only do it on actual change.
  if (newIndex >= 0 && newIndex < (int)images.size()) {
    auto window = images[newIndex].pWindow.lock();
    if (window && window->m_isMapped) {
      Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_OTHER);
      lastHoveredWindow = window;
    }
  }
}
