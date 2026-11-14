/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QByteArray>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct wl_display;
struct wl_registry;
struct wl_seat;
struct ext_data_control_manager_v1;
struct ext_data_control_device_v1;
struct ext_data_control_offer_v1;
struct ext_data_control_source_v1;

namespace deskflow {

//! Wayland clipboard access through ext-data-control-v1
/*!
Fallback for compositors whose portal backend has no Clipboard interface
(e.g. Hyprland with xdg-desktop-portal-hyprland). It opens its own Wayland
connection and does every protocol call on one private thread, so callers on
any thread only ever touch plain data.
*/
class DataControlClipboard
{
public:
  //! Returns bytes for a mime type, called on the clipboard thread
  using Provider = std::function<QByteArray(const std::string &mime)>;

  //! Connect and bind ext_data_control_manager_v1; nullptr if unavailable
  static std::unique_ptr<DataControlClipboard> create();

  ~DataControlClipboard();

  DataControlClipboard(const DataControlClipboard &) = delete;
  DataControlClipboard &operator=(const DataControlClipboard &) = delete;

  //! Mime types offered by the current selection (empty if none)
  std::vector<std::string> selectionMimeTypes() const;

  //! True while our own source is the selection
  bool ownsSelection() const;

  //! Read the current selection in \p mime, up to \p maxBytes
  QByteArray read(const std::string &mime, qint64 maxBytes);

  //! Become the selection, serving \p mimes from \p provider on demand
  void setSelection(const std::vector<std::string> &mimes, Provider provider);

private:
  struct Offer
  {
    ext_data_control_offer_v1 *proxy = nullptr;
    std::vector<std::string> mimes;
  };

  DataControlClipboard() = default;
  bool connect();
  void run();
  bool post(std::function<void()> task);
  void drainTasks();
  void destroyOffer(Offer *offer);

  // Wayland listener trampolines
  static void onGlobal(void *data, wl_registry *, uint32_t name, const char *interface, uint32_t version);
  static void onGlobalRemove(void *, wl_registry *, uint32_t);
  static void onDataOffer(void *data, ext_data_control_device_v1 *, ext_data_control_offer_v1 *offer);
  static void onSelection(void *data, ext_data_control_device_v1 *, ext_data_control_offer_v1 *offer);
  static void onFinished(void *data, ext_data_control_device_v1 *);
  static void onPrimarySelection(void *, ext_data_control_device_v1 *, ext_data_control_offer_v1 *offer);
  static void onOfferMime(void *data, ext_data_control_offer_v1 *, const char *mime);
  static void onSourceSend(void *data, ext_data_control_source_v1 *source, const char *mime, int32_t fd);
  static void onSourceCancelled(void *data, ext_data_control_source_v1 *source);

  wl_display *m_display = nullptr;
  wl_registry *m_registry = nullptr;
  wl_seat *m_seat = nullptr;
  ext_data_control_manager_v1 *m_manager = nullptr;
  ext_data_control_device_v1 *m_device = nullptr;

  // Clipboard-thread state
  std::vector<std::unique_ptr<Offer>> m_pendingOffers;
  std::unique_ptr<Offer> m_selection;
  ext_data_control_source_v1 *m_source = nullptr;
  Provider m_provider;

  // Shared with callers, guarded by m_mutex
  mutable std::mutex m_mutex;
  std::vector<std::string> m_selectionMimes;
  bool m_ownsSelection = false;
  std::deque<std::function<void()>> m_tasks;
  bool m_running = false;

  int m_wakeFds[2] = {-1, -1};
  std::atomic_bool m_stop = false;
  std::thread m_thread;
};

} // namespace deskflow
