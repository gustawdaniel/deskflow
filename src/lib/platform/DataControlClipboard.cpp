/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/DataControlClipboard.h"

#include "base/Log.h"

#include "ext-data-control-v1-client-protocol.h"
#include <wayland-client.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <future>
#include <poll.h>
#include <unistd.h>

namespace deskflow {

static constexpr int kReadTimeoutMs = 200;
static constexpr int kWriteTimeoutMs = 200;
static constexpr qint64 kChunkBytes = 64 * 1024;

std::unique_ptr<DataControlClipboard> DataControlClipboard::create()
{
  std::unique_ptr<DataControlClipboard> clipboard(new DataControlClipboard());
  if (!clipboard->connect())
    return nullptr;

  clipboard->m_running = true;
  clipboard->m_thread = std::thread(&DataControlClipboard::run, clipboard.get());
  return clipboard;
}

bool DataControlClipboard::connect()
{
  m_display = wl_display_connect(nullptr);
  if (!m_display) {
    LOG_DEBUG("data-control clipboard: no wayland display");
    return false;
  }

  static const wl_registry_listener registryListener = {
      &DataControlClipboard::onGlobal, &DataControlClipboard::onGlobalRemove
  };
  m_registry = wl_display_get_registry(m_display);
  wl_registry_add_listener(m_registry, &registryListener, this);
  wl_display_roundtrip(m_display);

  if (!m_manager || !m_seat) {
    LOG_DEBUG("data-control clipboard: compositor has no ext_data_control_manager_v1");
    return false;
  }

  static const ext_data_control_device_v1_listener deviceListener = {
      &DataControlClipboard::onDataOffer, &DataControlClipboard::onSelection, &DataControlClipboard::onFinished,
      &DataControlClipboard::onPrimarySelection
  };
  m_device = ext_data_control_manager_v1_get_data_device(m_manager, m_seat);
  ext_data_control_device_v1_add_listener(m_device, &deviceListener, this);

  // Picks up the current selection before the thread starts.
  wl_display_roundtrip(m_display);

  if (pipe2(m_wakeFds, O_CLOEXEC | O_NONBLOCK) != 0) {
    LOG_WARN("data-control clipboard: failed to create wake pipe: %s", strerror(errno));
    return false;
  }

  LOG_INFO("using wayland data-control for clipboard sharing");
  return true;
}

DataControlClipboard::~DataControlClipboard()
{
  if (m_thread.joinable()) {
    post([this] { m_stop = true; });
    m_thread.join();
  }

  for (auto &offer : m_pendingOffers)
    destroyOffer(offer.get());
  if (m_selection)
    destroyOffer(m_selection.get());
  if (m_source)
    ext_data_control_source_v1_destroy(m_source);
  if (m_device)
    ext_data_control_device_v1_destroy(m_device);
  if (m_manager)
    ext_data_control_manager_v1_destroy(m_manager);
  if (m_seat)
    wl_seat_destroy(m_seat);
  if (m_registry)
    wl_registry_destroy(m_registry);
  if (m_display)
    wl_display_disconnect(m_display);

  for (int fd : m_wakeFds) {
    if (fd >= 0)
      ::close(fd);
  }
}

std::vector<std::string> DataControlClipboard::selectionMimeTypes() const
{
  std::lock_guard lock(m_mutex);
  return m_selectionMimes;
}

bool DataControlClipboard::ownsSelection() const
{
  std::lock_guard lock(m_mutex);
  return m_ownsSelection;
}

QByteArray DataControlClipboard::read(const std::string &mime, qint64 maxBytes)
{
  auto readFd = std::make_shared<std::promise<int>>();
  auto future = readFd->get_future();

  const bool posted = post([this, mime, readFd] {
    if (!m_selection) {
      readFd->set_value(-1);
      return;
    }

    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
      LOG_WARN("data-control clipboard: failed to create pipe: %s", strerror(errno));
      readFd->set_value(-1);
      return;
    }

    ext_data_control_offer_v1_receive(m_selection->proxy, mime.c_str(), fds[1]);
    wl_display_flush(m_display);
    ::close(fds[1]);
    readFd->set_value(fds[0]);
  });
  if (!posted)
    return {};

  const int fd = future.get();
  if (fd < 0)
    return {};

  QByteArray contents;
  char buf[kChunkBytes];
  while (contents.size() < maxBytes) {
    pollfd pfd{fd, POLLIN, 0};
    if (poll(&pfd, 1, kReadTimeoutMs) <= 0)
      break;

    const auto n = ::read(fd, buf, static_cast<size_t>(std::min<qint64>(kChunkBytes, maxBytes - contents.size())));
    if (n <= 0)
      break;

    contents.append(buf, n);
  }

  ::close(fd);
  return contents;
}

void DataControlClipboard::setSelection(const std::vector<std::string> &mimes, Provider provider)
{
  post([this, mimes, provider = std::move(provider)]() mutable {
    static const ext_data_control_source_v1_listener sourceListener = {
        &DataControlClipboard::onSourceSend, &DataControlClipboard::onSourceCancelled
    };

    // The compositor may have finished the device (onFinished) while this task
    // was queued; the final drain in run() still executes it.
    if (!m_device || !m_manager)
      return;

    if (m_source)
      ext_data_control_source_v1_destroy(m_source);

    m_source = ext_data_control_manager_v1_create_data_source(m_manager);
    ext_data_control_source_v1_add_listener(m_source, &sourceListener, this);
    for (const auto &mime : mimes)
      ext_data_control_source_v1_offer(m_source, mime.c_str());

    m_provider = std::move(provider);
    ext_data_control_device_v1_set_selection(m_device, m_source);

    std::lock_guard lock(m_mutex);
    m_ownsSelection = true;
  });
}

bool DataControlClipboard::post(std::function<void()> task)
{
  {
    std::lock_guard lock(m_mutex);
    if (!m_running)
      return false;
    m_tasks.push_back(std::move(task));
  }

  const char byte = 1;
  [[maybe_unused]] auto n = ::write(m_wakeFds[1], &byte, 1);
  return true;
}

void DataControlClipboard::drainTasks()
{
  char buf[64];
  while (::read(m_wakeFds[0], buf, sizeof(buf)) > 0) {
  }

  std::deque<std::function<void()>> tasks;
  {
    std::lock_guard lock(m_mutex);
    tasks.swap(m_tasks);
  }
  for (auto &task : tasks)
    task();
}

void DataControlClipboard::run()
{
  const int displayFd = wl_display_get_fd(m_display);

  while (!m_stop) {
    while (wl_display_prepare_read(m_display) != 0)
      wl_display_dispatch_pending(m_display);
    wl_display_flush(m_display);

    pollfd fds[2] = {{displayFd, POLLIN, 0}, {m_wakeFds[0], POLLIN, 0}};
    if (poll(fds, 2, -1) < 0) {
      wl_display_cancel_read(m_display);
      if (errno == EINTR)
        continue;
      LOG_ERR("data-control clipboard: poll failed: %s", strerror(errno));
      break;
    }

    if (fds[0].revents & (POLLERR | POLLHUP)) {
      wl_display_cancel_read(m_display);
      LOG_ERR("data-control clipboard: wayland connection lost");
      break;
    }

    if (fds[0].revents & POLLIN)
      wl_display_read_events(m_display);
    else
      wl_display_cancel_read(m_display);
    wl_display_dispatch_pending(m_display);

    if (fds[1].revents & POLLIN)
      drainTasks();
  }

  // Refuse new tasks, then run what was queued so no reader is left waiting.
  {
    std::lock_guard lock(m_mutex);
    m_running = false;
  }
  drainTasks();
}

void DataControlClipboard::destroyOffer(Offer *offer)
{
  if (offer && offer->proxy) {
    ext_data_control_offer_v1_destroy(offer->proxy);
    offer->proxy = nullptr;
  }
}

void DataControlClipboard::onGlobal(
    void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version
)
{
  auto *self = static_cast<DataControlClipboard *>(data);
  if (strcmp(interface, wl_seat_interface.name) == 0 && !self->m_seat) {
    self->m_seat = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
  } else if (strcmp(interface, ext_data_control_manager_v1_interface.name) == 0) {
    self->m_manager = static_cast<ext_data_control_manager_v1 *>(
        wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, std::min<uint32_t>(version, 1))
    );
  }
}

void DataControlClipboard::onGlobalRemove(void *, wl_registry *, uint32_t)
{
  // nothing to do
}

void DataControlClipboard::onDataOffer(void *data, ext_data_control_device_v1 *, ext_data_control_offer_v1 *proxy)
{
  static const ext_data_control_offer_v1_listener offerListener = {&DataControlClipboard::onOfferMime};

  auto *self = static_cast<DataControlClipboard *>(data);
  auto offer = std::make_unique<Offer>();
  offer->proxy = proxy;
  ext_data_control_offer_v1_add_listener(proxy, &offerListener, offer.get());
  self->m_pendingOffers.push_back(std::move(offer));
}

void DataControlClipboard::onSelection(void *data, ext_data_control_device_v1 *, ext_data_control_offer_v1 *proxy)
{
  auto *self = static_cast<DataControlClipboard *>(data);

  if (self->m_selection)
    self->destroyOffer(self->m_selection.get());
  self->m_selection.reset();

  auto it = std::find_if(self->m_pendingOffers.begin(), self->m_pendingOffers.end(), [proxy](const auto &offer) {
    return offer->proxy == proxy;
  });
  if (proxy && it != self->m_pendingOffers.end()) {
    self->m_selection = std::move(*it);
    self->m_pendingOffers.erase(it);
  }

  std::lock_guard lock(self->m_mutex);
  self->m_selectionMimes = self->m_selection ? self->m_selection->mimes : std::vector<std::string>{};
}

void DataControlClipboard::onFinished(void *data, ext_data_control_device_v1 *)
{
  auto *self = static_cast<DataControlClipboard *>(data);
  LOG_WARN("data-control clipboard: device finished, clipboard sharing stopped");
  ext_data_control_device_v1_destroy(self->m_device);
  self->m_device = nullptr;
  self->m_stop = true;
}

void DataControlClipboard::onPrimarySelection(void *data, ext_data_control_device_v1 *, ext_data_control_offer_v1 *proxy)
{
  // Only the regular clipboard is shared; drop primary-selection offers.
  auto *self = static_cast<DataControlClipboard *>(data);
  if (!proxy)
    return;

  auto it = std::find_if(self->m_pendingOffers.begin(), self->m_pendingOffers.end(), [proxy](const auto &offer) {
    return offer->proxy == proxy;
  });
  if (it != self->m_pendingOffers.end()) {
    self->destroyOffer(it->get());
    self->m_pendingOffers.erase(it);
  }
}

void DataControlClipboard::onOfferMime(void *data, ext_data_control_offer_v1 *, const char *mime)
{
  static_cast<Offer *>(data)->mimes.emplace_back(mime);
}

void DataControlClipboard::onSourceSend(void *data, ext_data_control_source_v1 *, const char *mime, int32_t fd)
{
  auto *self = static_cast<DataControlClipboard *>(data);
  const auto bytes = self->m_provider ? self->m_provider(mime) : QByteArray();

  qint64 written = 0;
  while (written < bytes.size()) {
    pollfd pfd{fd, POLLOUT, 0};
    if (poll(&pfd, 1, kWriteTimeoutMs) <= 0) {
      LOG_ERR("data-control clipboard: timed out writing selection");
      break;
    }

    const auto n = ::write(fd, bytes.constData() + written, static_cast<size_t>(bytes.size() - written));
    if (n <= 0) {
      LOG_ERR("data-control clipboard: write failed: %s", strerror(errno));
      break;
    }
    written += n;
  }

  ::close(fd);
  LOG_DEBUG("data-control clipboard: served %s, bytes: %lld", mime, static_cast<long long>(written));
}

void DataControlClipboard::onSourceCancelled(void *data, ext_data_control_source_v1 *source)
{
  auto *self = static_cast<DataControlClipboard *>(data);
  ext_data_control_source_v1_destroy(source);
  if (self->m_source == source) {
    self->m_source = nullptr;
    self->m_provider = nullptr;

    std::lock_guard lock(self->m_mutex);
    self->m_ownsSelection = false;
  }
}

} // namespace deskflow
