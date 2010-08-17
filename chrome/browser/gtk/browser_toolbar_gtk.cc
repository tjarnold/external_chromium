// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/gtk/browser_toolbar_gtk.h"

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <X11/XF86keysym.h>

#include "app/gtk_dnd_util.h"
#include "app/l10n_util.h"
#include "app/menus/accelerator_gtk.h"
#include "app/resource_bundle.h"
#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/i18n/rtl.h"
#include "base/keyboard_codes_posix.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/singleton.h"
#include "chrome/app/chrome_dll_resource.h"
#include "chrome/browser/browser.h"
#include "chrome/browser/browser_theme_provider.h"
#include "chrome/browser/encoding_menu_controller.h"
#include "chrome/browser/gtk/accelerators_gtk.h"
#include "chrome/browser/gtk/back_forward_button_gtk.h"
#include "chrome/browser/gtk/browser_actions_toolbar_gtk.h"
#include "chrome/browser/gtk/browser_window_gtk.h"
#include "chrome/browser/gtk/cairo_cached_surface.h"
#include "chrome/browser/gtk/custom_button.h"
#include "chrome/browser/gtk/gtk_chrome_button.h"
#include "chrome/browser/gtk/gtk_theme_provider.h"
#include "chrome/browser/gtk/gtk_util.h"
#include "chrome/browser/gtk/location_bar_view_gtk.h"
#include "chrome/browser/gtk/reload_button_gtk.h"
#include "chrome/browser/gtk/rounded_window.h"
#include "chrome/browser/gtk/tabs/tab_strip_gtk.h"
#include "chrome/browser/gtk/view_id_util.h"
#include "chrome/browser/net/url_fixer_upper.h"
#include "chrome/browser/pref_service.h"
#include "chrome/browser/profile.h"
#include "chrome/browser/tab_contents/tab_contents.h"
#include "chrome/browser/upgrade_detector.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/notification_details.h"
#include "chrome/common/notification_service.h"
#include "chrome/common/notification_type.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/url_constants.h"
#include "gfx/canvas_skia_paint.h"
#include "gfx/gtk_util.h"
#include "gfx/skbitmap_operations.h"
#include "grit/chromium_strings.h"
#include "grit/generated_resources.h"
#include "grit/theme_resources.h"

namespace {

// Height of the toolbar in pixels (not counting padding).
const int kToolbarHeight = 29;

// Padding within the toolbar above the buttons and location bar.
const int kTopPadding = 4;

// Height of the toolbar in pixels when we only show the location bar.
const int kToolbarHeightLocationBarOnly = kToolbarHeight - 2;

// Interior spacing between toolbar widgets.
const int kToolbarWidgetSpacing = 2;

// Amount of rounding on top corners of toolbar. Only used in Gtk theme mode.
const int kToolbarCornerSize = 3;

// The offset in pixels of the upgrade dot on the app menu.
const int kUpgradeDotOffset = 11;

// The duration of the upgrade notification animation (actually the duration
// of a half-throb).
const int kThrobDuration = 1000;

}  // namespace

// BrowserToolbarGtk, public ---------------------------------------------------

BrowserToolbarGtk::BrowserToolbarGtk(Browser* browser, BrowserWindowGtk* window)
    : toolbar_(NULL),
      location_bar_(new LocationBarViewGtk(browser)),
      model_(browser->toolbar_model()),
      wrench_menu_model_(this, browser),
      browser_(browser),
      window_(window),
      profile_(NULL),
      upgrade_reminder_animation_(this),
      upgrade_reminder_canceled_(false) {
  browser_->command_updater()->AddCommandObserver(IDC_BACK, this);
  browser_->command_updater()->AddCommandObserver(IDC_FORWARD, this);
  browser_->command_updater()->AddCommandObserver(IDC_HOME, this);
  browser_->command_updater()->AddCommandObserver(IDC_BOOKMARK_PAGE, this);

  registrar_.Add(this,
                 NotificationType::BROWSER_THEME_CHANGED,
                 NotificationService::AllSources());
  registrar_.Add(this,
                 NotificationType::UPGRADE_RECOMMENDED,
                 NotificationService::AllSources());

  upgrade_reminder_animation_.SetThrobDuration(kThrobDuration);

  ActiveWindowWatcherX::AddObserver(this);
  MaybeShowUpgradeReminder();
}

BrowserToolbarGtk::~BrowserToolbarGtk() {
  ActiveWindowWatcherX::RemoveObserver(this);

  browser_->command_updater()->RemoveCommandObserver(IDC_BACK, this);
  browser_->command_updater()->RemoveCommandObserver(IDC_FORWARD, this);
  browser_->command_updater()->RemoveCommandObserver(IDC_HOME, this);
  browser_->command_updater()->RemoveCommandObserver(IDC_BOOKMARK_PAGE, this);

  offscreen_entry_.Destroy();

  app_menu_.reset();
  app_menu_button_.Destroy();
  app_menu_image_.Destroy();
}

void BrowserToolbarGtk::Init(Profile* profile,
                             GtkWindow* top_level_window) {
  // Make sure to tell the location bar the profile before calling its Init.
  SetProfile(profile);

  theme_provider_ = GtkThemeProvider::GetFrom(profile);
  offscreen_entry_.Own(gtk_entry_new());

  show_home_button_.Init(prefs::kShowHomeButton, profile->GetPrefs(), this);
  home_page_.Init(prefs::kHomePage, profile->GetPrefs(), this);
  home_page_is_new_tab_page_.Init(prefs::kHomePageIsNewTabPage,
                                  profile->GetPrefs(), this);

  event_box_ = gtk_event_box_new();
  // Make the event box transparent so themes can use transparent toolbar
  // backgrounds.
  if (!theme_provider_->UseGtkTheme())
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(event_box_), FALSE);

  toolbar_ = gtk_hbox_new(FALSE, 0);
  alignment_ = gtk_alignment_new(0.0, 0.0, 1.0, 1.0);
  UpdateForBookmarkBarVisibility(false);
  g_signal_connect(alignment_, "expose-event",
                   G_CALLBACK(&OnAlignmentExposeThunk), this);
  gtk_container_add(GTK_CONTAINER(event_box_), alignment_);
  gtk_container_add(GTK_CONTAINER(alignment_), toolbar_);

  toolbar_left_ = gtk_hbox_new(FALSE, 0);

  GtkWidget* back_forward_hbox_ = gtk_hbox_new(FALSE, 0);
  back_.reset(new BackForwardButtonGtk(browser_, false));
  g_signal_connect(back_->widget(), "clicked",
                   G_CALLBACK(OnButtonClickThunk), this);
  gtk_box_pack_start(GTK_BOX(back_forward_hbox_), back_->widget(), FALSE,
                     FALSE, 0);

  forward_.reset(new BackForwardButtonGtk(browser_, true));
  g_signal_connect(forward_->widget(), "clicked",
                   G_CALLBACK(OnButtonClickThunk), this);
  gtk_box_pack_start(GTK_BOX(back_forward_hbox_), forward_->widget(), FALSE,
                     FALSE, 0);

  gtk_box_pack_start(GTK_BOX(toolbar_left_), back_forward_hbox_, FALSE,
                     FALSE, kToolbarWidgetSpacing);

  reload_.reset(new ReloadButtonGtk(location_bar_.get(), browser_));
  gtk_box_pack_start(GTK_BOX(toolbar_left_), reload_->widget(), FALSE, FALSE,
                     0);

  home_.reset(BuildToolbarButton(IDR_HOME, IDR_HOME_P, IDR_HOME_H, 0,
                                 IDR_BUTTON_MASK,
                                 l10n_util::GetStringUTF8(IDS_TOOLTIP_HOME),
                                 GTK_STOCK_HOME, kToolbarWidgetSpacing));
  gtk_util::SetButtonTriggersNavigation(home_->widget());

  gtk_box_pack_start(GTK_BOX(toolbar_), toolbar_left_, FALSE, FALSE, 0);

  location_hbox_ = gtk_hbox_new(FALSE, 0);
  location_bar_->Init(ShouldOnlyShowLocation());
  gtk_box_pack_start(GTK_BOX(location_hbox_), location_bar_->widget(), TRUE,
                     TRUE, 0);

  g_signal_connect(location_hbox_, "expose-event",
                   G_CALLBACK(OnLocationHboxExposeThunk), this);
  gtk_box_pack_start(GTK_BOX(toolbar_), location_hbox_, TRUE, TRUE,
      kToolbarWidgetSpacing + (ShouldOnlyShowLocation() ? 1 : 0));

  toolbar_right_ = gtk_hbox_new(FALSE, 0);

  if (!ShouldOnlyShowLocation()) {
    actions_toolbar_.reset(new BrowserActionsToolbarGtk(browser_));
    gtk_box_pack_start(GTK_BOX(toolbar_right_), actions_toolbar_->widget(),
                       FALSE, FALSE, 0);
  }

  // We need another hbox for the menu buttons so we can place them together,
  // but still have some padding to their collective left/right.
  GtkWidget* menus_hbox = gtk_hbox_new(FALSE, 0);
  GtkWidget* chrome_menu = BuildToolbarMenuButton(
      l10n_util::GetStringFUTF8(IDS_APPMENU_TOOLTIP,
          WideToUTF16(l10n_util::GetString(IDS_PRODUCT_NAME))),
      &app_menu_button_);
  app_menu_image_.Own(gtk_image_new_from_pixbuf(
      theme_provider_->GetRTLEnabledPixbufNamed(IDR_TOOLS)));
  gtk_container_add(GTK_CONTAINER(chrome_menu), app_menu_image_.get());
  g_signal_connect_after(app_menu_image_.get(), "expose-event",
                         G_CALLBACK(OnAppMenuImageExposeThunk), this);

  app_menu_.reset(new MenuGtk(this, &wrench_menu_model_));
  gtk_box_pack_start(GTK_BOX(menus_hbox), chrome_menu, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(toolbar_right_), menus_hbox, FALSE, FALSE,
                     kToolbarWidgetSpacing);
  g_signal_connect(app_menu_->widget(), "show",
                   G_CALLBACK(OnAppMenuShowThunk), this);

  gtk_box_pack_start(GTK_BOX(toolbar_), toolbar_right_, FALSE, FALSE, 0);

  if (ShouldOnlyShowLocation()) {
    gtk_widget_show(event_box_);
    gtk_widget_show(alignment_);
    gtk_widget_show(toolbar_);
    gtk_widget_show_all(location_hbox_);
    gtk_widget_hide(reload_->widget());
  } else {
    gtk_widget_show_all(event_box_);
    if (actions_toolbar_->button_count() == 0)
      gtk_widget_hide(actions_toolbar_->widget());
  }
  // Initialize pref-dependent UI state.
  NotifyPrefChanged(NULL);

  // Because the above does a recursive show all on all widgets we need to
  // update the icon visibility to hide them.
  location_bar_->UpdateContentSettingsIcons();

  SetViewIDs();
  theme_provider_->InitThemesFor(this);
}

void BrowserToolbarGtk::SetViewIDs() {
  ViewIDUtil::SetID(widget(), VIEW_ID_TOOLBAR);
  ViewIDUtil::SetID(back_->widget(), VIEW_ID_BACK_BUTTON);
  ViewIDUtil::SetID(forward_->widget(), VIEW_ID_FORWARD_BUTTON);
  ViewIDUtil::SetID(reload_->widget(), VIEW_ID_RELOAD_BUTTON);
  ViewIDUtil::SetID(home_->widget(), VIEW_ID_HOME_BUTTON);
  ViewIDUtil::SetID(location_bar_->widget(), VIEW_ID_LOCATION_BAR);
  ViewIDUtil::SetID(app_menu_button_.get(), VIEW_ID_APP_MENU);
}

void BrowserToolbarGtk::Show() {
  gtk_widget_show(toolbar_);
}

void BrowserToolbarGtk::Hide() {
  gtk_widget_hide(toolbar_);
}

LocationBar* BrowserToolbarGtk::GetLocationBar() const {
  return location_bar_.get();
}

void BrowserToolbarGtk::UpdateForBookmarkBarVisibility(
    bool show_bottom_padding) {
  gtk_alignment_set_padding(GTK_ALIGNMENT(alignment_),
      ShouldOnlyShowLocation() ? 0 : kTopPadding,
      !show_bottom_padding || ShouldOnlyShowLocation() ? 0 : kTopPadding,
      0, 0);
}

void BrowserToolbarGtk::ShowAppMenu() {
  app_menu_->Cancel();
  gtk_chrome_button_set_paint_state(GTK_CHROME_BUTTON(app_menu_button_.get()),
                                    GTK_STATE_ACTIVE);
  app_menu_->PopupAsFromKeyEvent(app_menu_button_.get());
}

// CommandUpdater::CommandObserver ---------------------------------------------

void BrowserToolbarGtk::EnabledStateChangedForCommand(int id, bool enabled) {
  GtkWidget* widget = NULL;
  switch (id) {
    case IDC_BACK:
      widget = back_->widget();
      break;
    case IDC_FORWARD:
      widget = forward_->widget();
      break;
    case IDC_HOME:
      if (home_.get())
        widget = home_->widget();
      break;
  }
  if (widget) {
    if (!enabled && GTK_WIDGET_STATE(widget) == GTK_STATE_PRELIGHT) {
      // If we're disabling a widget, GTK will helpfully restore it to its
      // previous state when we re-enable it, even if that previous state
      // is the prelight.  This looks bad.  See the bug for a simple repro.
      // http://code.google.com/p/chromium/issues/detail?id=13729
      gtk_widget_set_state(widget, GTK_STATE_NORMAL);
    }
    gtk_widget_set_sensitive(widget, enabled);
  }
}

// MenuGtk::Delegate -----------------------------------------------------------

void BrowserToolbarGtk::StoppedShowing() {
  // Without these calls, the hover state can get stuck since the leave-notify
  // event is not sent when clicking a button brings up the menu.
  gtk_chrome_button_set_hover_state(
      GTK_CHROME_BUTTON(app_menu_button_.get()), 0.0);
  gtk_chrome_button_unset_paint_state(
      GTK_CHROME_BUTTON(app_menu_button_.get()));
}

GtkIconSet* BrowserToolbarGtk::GetIconSetForId(int idr) {
  return theme_provider_->GetIconSetForId(idr);
}

// menus::SimpleMenuModel::Delegate

bool BrowserToolbarGtk::IsCommandIdEnabled(int id) const {
  return browser_->command_updater()->IsCommandEnabled(id);
}

bool BrowserToolbarGtk::IsCommandIdChecked(int id) const {
  if (!profile_)
    return false;

  EncodingMenuController controller;
  if (id == IDC_SHOW_BOOKMARK_BAR) {
    return profile_->GetPrefs()->GetBoolean(prefs::kShowBookmarkBar);
  } else if (controller.DoesCommandBelongToEncodingMenu(id)) {
    TabContents* tab_contents = browser_->GetSelectedTabContents();
    if (tab_contents) {
      return controller.IsItemChecked(profile_, tab_contents->encoding(),
                                      id);
    }
  }

  return false;
}

void BrowserToolbarGtk::ExecuteCommand(int id) {
  browser_->ExecuteCommand(id);
}

bool BrowserToolbarGtk::GetAcceleratorForCommandId(
    int id,
    menus::Accelerator* accelerator) {
  const menus::AcceleratorGtk* accelerator_gtk =
      Singleton<AcceleratorsGtk>()->GetPrimaryAcceleratorForCommand(id);
  if (accelerator_gtk)
    *accelerator = *accelerator_gtk;
  return !!accelerator_gtk;
}

// NotificationObserver --------------------------------------------------------

void BrowserToolbarGtk::Observe(NotificationType type,
                                const NotificationSource& source,
                                const NotificationDetails& details) {
  if (type == NotificationType::PREF_CHANGED) {
    NotifyPrefChanged(Details<std::wstring>(details).ptr());
  } else if (type == NotificationType::BROWSER_THEME_CHANGED) {
    // Update the spacing around the menu buttons
    bool use_gtk = theme_provider_->UseGtkTheme();
    int border = use_gtk ? 0 : 2;
    gtk_container_set_border_width(
        GTK_CONTAINER(app_menu_button_.get()), border);

    // Update the menu button image.
    gtk_image_set_from_pixbuf(GTK_IMAGE(app_menu_image_.get()),
        theme_provider_->GetRTLEnabledPixbufNamed(IDR_TOOLS));

    // Force the height of the toolbar so we get the right amount of padding
    // above and below the location bar. We always force the size of the hboxes
    // to either side of the location box, but we only force the location box
    // size in chrome-theme mode because that's the only time we try to control
    // the font size.
    int toolbar_height = ShouldOnlyShowLocation() ?
                         kToolbarHeightLocationBarOnly : kToolbarHeight;
    gtk_widget_set_size_request(toolbar_left_, -1, toolbar_height);
    gtk_widget_set_size_request(toolbar_right_, -1, toolbar_height);
    gtk_widget_set_size_request(location_hbox_, -1,
                                use_gtk ? -1 : toolbar_height);

    // When using the GTK+ theme, we need to have the event box be visible so
    // buttons don't get a halo color from the background.  When using Chromium
    // themes, we want to let the background show through the toolbar.
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(event_box_), use_gtk);

    UpdateRoundedness();
  } else if (type == NotificationType::UPGRADE_RECOMMENDED) {
    MaybeShowUpgradeReminder();
  } else {
    NOTREACHED();
  }
}

// BrowserToolbarGtk, public ---------------------------------------------------

void BrowserToolbarGtk::SetProfile(Profile* profile) {
  if (profile == profile_)
    return;

  profile_ = profile;
  location_bar_->SetProfile(profile);
}

void BrowserToolbarGtk::UpdateTabContents(TabContents* contents,
                                          bool should_restore_state) {
  location_bar_->Update(should_restore_state ? contents : NULL);

  if (actions_toolbar_.get())
    actions_toolbar_->Update();
}

// BrowserToolbarGtk, private --------------------------------------------------

CustomDrawButton* BrowserToolbarGtk::BuildToolbarButton(
    int normal_id, int active_id, int highlight_id, int depressed_id,
    int background_id, const std::string& localized_tooltip,
    const char* stock_id, int spacing) {
  CustomDrawButton* button = new CustomDrawButton(
      GtkThemeProvider::GetFrom(profile_),
      normal_id, active_id, highlight_id, depressed_id, background_id, stock_id,
      GTK_ICON_SIZE_SMALL_TOOLBAR);

  gtk_widget_set_tooltip_text(button->widget(),
                              localized_tooltip.c_str());
  g_signal_connect(button->widget(), "clicked",
                   G_CALLBACK(OnButtonClickThunk), this);

  gtk_box_pack_start(GTK_BOX(toolbar_left_), button->widget(), FALSE, FALSE,
                     spacing);
  return button;
}

GtkWidget* BrowserToolbarGtk::BuildToolbarMenuButton(
    const std::string& localized_tooltip,
    OwnedWidgetGtk* owner) {
  GtkWidget* button = theme_provider_->BuildChromeButton();
  owner->Own(button);

  gtk_widget_set_tooltip_text(button, localized_tooltip.c_str());
  g_signal_connect(button, "button-press-event",
                   G_CALLBACK(OnMenuButtonPressEventThunk), this);
  GTK_WIDGET_UNSET_FLAGS(button, GTK_CAN_FOCUS);

  return button;
}

void BrowserToolbarGtk::SetUpDragForHomeButton(bool enable) {
  if (enable) {
    gtk_drag_dest_set(home_->widget(), GTK_DEST_DEFAULT_ALL,
                      NULL, 0, GDK_ACTION_COPY);
    static const int targets[] = { gtk_dnd_util::TEXT_PLAIN,
                                   gtk_dnd_util::TEXT_URI_LIST, -1 };
    gtk_dnd_util::SetDestTargetList(home_->widget(), targets);

    drop_handler_.reset(new GtkSignalRegistrar());
    drop_handler_->Connect(home_->widget(), "drag-data-received",
                           G_CALLBACK(OnDragDataReceivedThunk), this);
  } else {
    gtk_drag_dest_unset(home_->widget());
    drop_handler_.reset(NULL);
  }
}

bool BrowserToolbarGtk::UpdateRoundedness() {
  // We still round the corners if we are in chrome theme mode, but we do it by
  // drawing theme resources rather than changing the physical shape of the
  // widget.
  bool should_be_rounded = theme_provider_->UseGtkTheme() &&
      window_->ShouldDrawContentDropShadow();

  if (should_be_rounded == gtk_util::IsActingAsRoundedWindow(alignment_))
    return false;

  if (should_be_rounded) {
    gtk_util::ActAsRoundedWindow(alignment_, GdkColor(), kToolbarCornerSize,
                                 gtk_util::ROUNDED_TOP,
                                 gtk_util::BORDER_NONE);
  } else {
    gtk_util::StopActingAsRoundedWindow(alignment_);
  }

  return true;
}

gboolean BrowserToolbarGtk::OnAlignmentExpose(GtkWidget* widget,
                                              GdkEventExpose* e) {
  // We may need to update the roundedness of the toolbar's top corners. In
  // this case, don't draw; we'll be called again soon enough.
  if (UpdateRoundedness())
    return TRUE;

  // We don't need to render the toolbar image in GTK mode.
  if (theme_provider_->UseGtkTheme())
    return FALSE;

  cairo_t* cr = gdk_cairo_create(GDK_DRAWABLE(widget->window));
  gdk_cairo_rectangle(cr, &e->area);
  cairo_clip(cr);

  gfx::Point tabstrip_origin =
      window_->tabstrip()->GetTabStripOriginForWidget(widget);
  // Fill the entire region with the toolbar color.
  GdkColor color = theme_provider_->GetGdkColor(
      BrowserThemeProvider::COLOR_TOOLBAR);
  gdk_cairo_set_source_color(cr, &color);
  cairo_fill(cr);

  // The horizontal size of the top left and right corner images.
  const int kCornerWidth = 4;
  // The thickness of the shadow outside the toolbar's bounds; the offset
  // between the edge of the toolbar and where we anchor the corner images.
  const int kShadowThickness = 2;

  gfx::Rect area(e->area);
  gfx::Rect right(widget->allocation.x + widget->allocation.width -
                      kCornerWidth,
                  widget->allocation.y - kShadowThickness,
                  kCornerWidth,
                  widget->allocation.height + kShadowThickness);
  gfx::Rect left(widget->allocation.x - kShadowThickness,
                 widget->allocation.y - kShadowThickness,
                 kCornerWidth,
                 widget->allocation.height + kShadowThickness);

  if (window_->ShouldDrawContentDropShadow()) {
    // Leave room to draw rounded corners.
    area = area.Subtract(right).Subtract(left);
  }

  CairoCachedSurface* background = theme_provider_->GetSurfaceNamed(
      IDR_THEME_TOOLBAR, widget);
  background->SetSource(cr, tabstrip_origin.x(), tabstrip_origin.y());
  cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_REPEAT);
  cairo_rectangle(cr, area.x(), area.y(), area.width(), area.height());
  cairo_fill(cr);

  if (!window_->ShouldDrawContentDropShadow()) {
    // The rest of this function is for rounded corners. Our work is done here.
    cairo_destroy(cr);
    return FALSE;
  }

  bool draw_left_corner = left.Intersects(gfx::Rect(e->area));
  bool draw_right_corner = right.Intersects(gfx::Rect(e->area));

  if (draw_left_corner || draw_right_corner) {
    // Create a mask which is composed of the left and/or right corners.
    cairo_surface_t* target = cairo_surface_create_similar(
        cairo_get_target(cr),
        CAIRO_CONTENT_COLOR_ALPHA,
        widget->allocation.x + widget->allocation.width,
        widget->allocation.y + widget->allocation.height);
    cairo_t* copy_cr = cairo_create(target);

    cairo_set_operator(copy_cr, CAIRO_OPERATOR_SOURCE);
    if (draw_left_corner) {
      CairoCachedSurface* left_corner = theme_provider_->GetSurfaceNamed(
          IDR_CONTENT_TOP_LEFT_CORNER_MASK, widget);
      left_corner->SetSource(copy_cr, left.x(), left.y());
      cairo_paint(copy_cr);
    }
    if (draw_right_corner) {
      CairoCachedSurface* right_corner = theme_provider_->GetSurfaceNamed(
          IDR_CONTENT_TOP_RIGHT_CORNER_MASK, widget);
      right_corner->SetSource(copy_cr, right.x(), right.y());
      // We fill a path rather than just painting because we don't want to
      // overwrite the left corner.
      cairo_rectangle(copy_cr, right.x(), right.y(),
                      right.width(), right.height());
      cairo_fill(copy_cr);
    }

    // Draw the background. CAIRO_OPERATOR_IN uses the existing pixel data as
    // an alpha mask.
    background->SetSource(copy_cr, tabstrip_origin.x(), tabstrip_origin.y());
    cairo_set_operator(copy_cr, CAIRO_OPERATOR_IN);
    cairo_pattern_set_extend(cairo_get_source(copy_cr), CAIRO_EXTEND_REPEAT);
    cairo_paint(copy_cr);
    cairo_destroy(copy_cr);

    // Copy the temporary surface to the screen.
    cairo_set_source_surface(cr, target, 0, 0);
    cairo_paint(cr);
    cairo_surface_destroy(target);
  }

  cairo_destroy(cr);

  return FALSE;  // Allow subwidgets to paint.
}

gboolean BrowserToolbarGtk::OnLocationHboxExpose(GtkWidget* location_hbox,
                                                 GdkEventExpose* e) {
  if (theme_provider_->UseGtkTheme()) {
    gtk_util::DrawTextEntryBackground(offscreen_entry_.get(),
                                      location_hbox, &e->area,
                                      &location_hbox->allocation);
  }

  return FALSE;
}

void BrowserToolbarGtk::OnButtonClick(GtkWidget* button) {
  if ((button == back_->widget()) || (button == forward_->widget())) {
    if (gtk_util::DispositionForCurrentButtonPressEvent() == CURRENT_TAB)
      location_bar_->Revert();
    return;
  }

  DCHECK(home_.get() && button == home_->widget()) <<
      "Unexpected button click callback";
  browser_->Home(gtk_util::DispositionForCurrentButtonPressEvent());
}

gboolean BrowserToolbarGtk::OnMenuButtonPressEvent(GtkWidget* button,
                                                   GdkEventButton* event) {
  if (event->button != 1)
    return FALSE;

  gtk_chrome_button_set_paint_state(GTK_CHROME_BUTTON(button),
                                    GTK_STATE_ACTIVE);
  app_menu_->Popup(button, reinterpret_cast<GdkEvent*>(event));

  return TRUE;
}

void BrowserToolbarGtk::OnDragDataReceived(GtkWidget* widget,
    GdkDragContext* drag_context, gint x, gint y,
    GtkSelectionData* data, guint info, guint time) {
  if (info != gtk_dnd_util::TEXT_PLAIN) {
    NOTIMPLEMENTED() << "Only support plain text drops for now, sorry!";
    return;
  }

  GURL url(reinterpret_cast<char*>(data->data));
  if (!url.is_valid())
    return;

  bool url_is_newtab = url.spec() == chrome::kChromeUINewTabURL;
  home_page_is_new_tab_page_.SetValue(url_is_newtab);
  if (!url_is_newtab)
    home_page_.SetValue(url.spec());
}

void BrowserToolbarGtk::NotifyPrefChanged(const std::wstring* pref) {
  if (!pref || *pref == prefs::kShowHomeButton) {
    if (show_home_button_.GetValue() && !ShouldOnlyShowLocation()) {
      gtk_widget_show(home_->widget());
    } else {
      gtk_widget_hide(home_->widget());
    }
  }

  if (!pref ||
      *pref == prefs::kHomePage ||
      *pref == prefs::kHomePageIsNewTabPage)
    SetUpDragForHomeButton(!home_page_.IsManaged() &&
                           !home_page_is_new_tab_page_.IsManaged());
}

void BrowserToolbarGtk::MaybeShowUpgradeReminder() {
  // Only show the upgrade reminder animation for the currently active window.
  if (window_->IsActive() &&
      Singleton<UpgradeDetector>::get()->notify_upgrade() &&
      !upgrade_reminder_canceled_) {
    upgrade_reminder_animation_.StartThrobbing(-1);
  } else {
    upgrade_reminder_animation_.Reset();
  }
}

bool BrowserToolbarGtk::ShouldOnlyShowLocation() const {
  // If we're a popup window, only show the location bar (omnibox).
  return browser_->type() != Browser::TYPE_NORMAL;
}

void BrowserToolbarGtk::AnimationEnded(const Animation* animation) {
  DCHECK_EQ(animation, &upgrade_reminder_animation_);
  gtk_widget_queue_draw(app_menu_image_.get());
}

void BrowserToolbarGtk::AnimationProgressed(const Animation* animation) {
  DCHECK_EQ(animation, &upgrade_reminder_animation_);
  if (UpgradeAnimationIsFaded())
    gtk_widget_queue_draw(app_menu_image_.get());
}

void BrowserToolbarGtk::AnimationCanceled(const Animation* animation) {
  AnimationEnded(animation);
}

void BrowserToolbarGtk::ActiveWindowChanged(GdkWindow* active_window) {
  MaybeShowUpgradeReminder();
}

void BrowserToolbarGtk::OnAppMenuShow(GtkWidget* sender) {
  if (upgrade_reminder_animation_.is_animating()) {
    upgrade_reminder_canceled_ = true;
    MaybeShowUpgradeReminder();
  }
}

gboolean BrowserToolbarGtk::OnAppMenuImageExpose(GtkWidget* sender,
                                                 GdkEventExpose* expose) {
  if (!Singleton<UpgradeDetector>::get()->notify_upgrade())
    return FALSE;

  SkBitmap badge;
  if (UpgradeAnimationIsFaded()) {
    badge = SkBitmapOperations::CreateBlendedBitmap(
        *theme_provider_->GetBitmapNamed(IDR_UPGRADE_DOT_ACTIVE),
        *theme_provider_->GetBitmapNamed(IDR_UPGRADE_DOT_INACTIVE),
        upgrade_reminder_animation_.GetCurrentValue());
  } else {
    badge = *theme_provider_->GetBitmapNamed(IDR_UPGRADE_DOT_INACTIVE);
  }

  // Draw the chrome app menu icon onto the canvas.
  gfx::CanvasSkiaPaint canvas(expose, false);
  int x_offset = base::i18n::IsRTL() ?
      sender->allocation.width - kUpgradeDotOffset - badge.width() :
      kUpgradeDotOffset;
  canvas.DrawBitmapInt(
      badge,
      sender->allocation.x + x_offset,
      sender->allocation.y + sender->allocation.height - badge.height());

  return FALSE;
}

bool BrowserToolbarGtk::UpgradeAnimationIsFaded() {
  return upgrade_reminder_animation_.cycles_remaining() > 0 &&
      // This funky looking math makes the badge throb for 2 seconds once
      // every 8 seconds.
      ((upgrade_reminder_animation_.cycles_remaining() - 1) / 2) % 4 == 0;
}