//////////////////////////////////////////////////////////////////////////////////////////
// A cross platform socket APIs, support ios & android & wp8 & window store
// universal app
//////////////////////////////////////////////////////////////////////////////////////////
/*
The MIT License (MIT)

Copyright (c) 2012-2020 HALX99

HAL: Hardware Abstraction Layer
X99: Intel X99 Mainboard Platform

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#ifndef YASIO__CORE_CPP
#define YASIO__CORE_CPP
#if !defined(YASIO_HEADER_ONLY)
#  include "yasio/yasio.hpp"
#endif
#include <limits>
#include <sstream>
#if defined(_WIN32)
#  include <io.h>
#  define YASIO_O_OPEN_FLAGS O_CREAT | O_RDWR | O_BINARY, S_IWRITE | S_IREAD
#  define ftruncate _chsize
#else
#  include <unistd.h>
#  define YASIO_O_OPEN_FLAGS O_CREAT | O_RDWR, S_IRWXU
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#if defined(YASIO_HAVE_SSL)
#  include <openssl/bio.h>
#  include <openssl/ssl.h>
#  include <openssl/err.h>
#endif

#if defined(YASIO_HAVE_KCP)
#  include "yasio/kcp/ikcp.h"
#endif

#if defined(YASIO_HAVE_CARES)
extern "C" {
#  include "c-ares/ares.h"
}
#endif

#define YASIO_SLOG_IMPL(options, format, ...)                                                      \
  do                                                                                               \
  {                                                                                                \
    if (options.print_)                                                                            \
      options.print_(::yasio::strfmt(127, "[yasio][%lld]" format "\n",                             \
                                     highp_clock<system_clock_t>(), ##__VA_ARGS__)                 \
                         .c_str());                                                                \
    else                                                                                           \
      YASIO_LOG("[%lld]" format, highp_clock<system_clock_t>(), ##__VA_ARGS__);                    \
  } while (false)

#define YASIO_SLOG(format, ...) YASIO_SLOG_IMPL(options_, format, ##__VA_ARGS__)
#if !defined(YASIO_VERBOSE_LOG)
#  define YASIO_SLOGV(fmt, ...) (void)0
#else
#  define YASIO_SLOGV YASIO_SLOG
#endif

#define YASIO_ADDR_ANY(af) (af == AF_INET ? "0.0.0.0" : "::")

#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 6320 6322 4996)
#endif

namespace yasio
{
namespace inet
{
namespace
{
// error code
enum
{
  YERR_OK                   = 0,    // NO ERROR.
  YERR_INVALID_PACKET       = -500, // Invalid packet.
  YERR_DPL_ILLEGAL_PDU      = -499, // Decode pdu length error.
  YERR_RESOLV_HOST_FAILED   = -498, // Resolve host failed.
  YERR_NO_AVAIL_ADDR        = -497, // No available address to connect.
  YERR_LOCAL_SHUTDOWN       = -496, // Local shutdown the connection.
  YERR_SSL_HANDSHAKE_FAILED = -495, // SSL handshake fail
};

// event mask
enum
{
  YEM_POLLIN  = 1,
  YEM_POLLOUT = 2,
  YEM_POLLERR = 4,
};

// op mask
enum
{
  YOPM_OPEN_CHANNEL    = 1,
  YOPM_CLOSE_CHANNEL   = 1 << 1,
  YOPM_CLOSE_TRANSPORT = 1 << 2
};

// dns queries state
enum : u_short
{
  YDQS_READY = 1,
  YDQS_DIRTY,
  YDQS_INPRROGRESS,
  YDQS_FAILED,
  YDQSF_QUERIES_NEEDED = 1 << 8
};

enum
{
  /* whether udp server enable multicast service */
  YCPF_MCAST = 1 << 17,

  /* Whether multicast loopback, if 1, local machine can recv self multicast packet */
  YCPF_MCAST_LOOPBACK = 1 << 18,

  /* Whether ssl client in handshaking */
  YCPF_SSL_HANDSHAKING = 1 << 19,
};

#define YDQS_CHECK_STATE(what, value) ((what & 0x00ff) == value)
#define YDQS_SET_STATE(what, value) (what = (what & 0xff00) | value)
#define YDQS_GET_STATE(what) (what & 0x00ff)

#if defined(_WIN32)
const DWORD MS_VC_EXCEPTION = 0x406D1388;
#  pragma pack(push, 8)
typedef struct _yasio__thread_info
{
  DWORD dwType;     // Must be 0x1000.
  LPCSTR szName;    // Pointer to name (in user addr space).
  DWORD dwThreadID; // Thread ID (-1=caller thread).
  DWORD dwFlags;    // Reserved for future use, must be zero.
} yasio__thread_info;
#  pragma pack(pop)
static void yasio__set_thread_name(const char* threadName)
{
  yasio__thread_info info;
  info.dwType     = 0x1000;
  info.szName     = threadName;
  info.dwThreadID = GetCurrentThreadId();
  info.dwFlags    = 0;
#  if !defined(__MINGW64__) && !defined(__MINGW32__)
  __try
  {
    RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {}
#  endif
}
#elif defined(__linux__)
#  define yasio__set_thread_name(name) pthread_setname_np(pthread_self(), name)
#elif defined(__APPLE__)
#  define yasio__set_thread_name(name) pthread_setname_np(name)
#else
#  define yasio__set_thread_name(name)
#endif

struct yasio__global_state
{
  enum
  {
    INITF_SSL   = 1,
    INITF_CARES = 2,
  };

public:
  yasio__global_state()
  {
    s_max_alloc_size =
        static_cast<int>((std::max)(sizeof(io_transport_tcp), sizeof(io_transport_udp)));
#if defined(YASIO_HAVE_KCP)
    s_max_alloc_size = (std::max)(s_max_alloc_size, static_cast<int>(sizeof(io_transport_kcp)));
#endif
#if defined(YASIO_HAVE_SSL)
    s_max_alloc_size = (std::max)(s_max_alloc_size, static_cast<int>(sizeof(io_transport_ssl)));
    if (OPENSSL_init_ssl(0, NULL) == 1)
      this->init_flags |= INITF_SSL;
#endif
#if defined(YASIO_HAVE_CARES)
    int ares_status = ::ares_library_init(ARES_LIB_INIT_ALL);
    if (ares_status == 0)
      this->init_flags |= INITF_CARES;
    else
      YASIO_LOG("init c-ares library failed, status=%d, detail:%s", ares_status,
                ::ares_strerror(ares_status));
#endif
  }
  ~yasio__global_state()
  {
#if defined(YASIO_HAVE_CARES)
    if (this->init_flags & INITF_CARES)
      ::ares_library_cleanup();
#endif
  }

  int init_flags = 0;
  static int s_max_alloc_size;
};
int yasio__global_state::s_max_alloc_size;
} // namespace

class a_pdu
{
public:
  a_pdu(std::vector<char>&& buffer, std::function<void()>&& handler)
      : rpos_(0), buffer_(std::move(buffer)), handler_(std::move(handler))
  {}

  size_t rpos_;              // read pos from sending buffer
  std::vector<char> buffer_; // sending data buffer
  std::function<void()> handler_;
#if !defined(YASIO_DISABLE_OBJECT_POOL)
  DEFINE_CONCURRENT_OBJECT_POOL_ALLOCATION(a_pdu, 512)
#endif
};

/// highp_timer
void highp_timer::async_wait(timer_cb_t cb) { this->service_.schedule_timer(this, std::move(cb)); }

void highp_timer::cancel()
{
  if (!expired())
    this->service_.remove_timer(this);
}

#if defined(YASIO_HAVE_SSL)
/// ssl_auto_handle
void ssl_auto_handle::destroy()
{
  if (ssl_)
  {
    ::SSL_shutdown(ssl_);
    ::SSL_free(ssl_);
    ssl_ = nullptr;
  }
}
#endif

/// io_channel
io_channel::io_channel(io_service& service, int index) : timer_(service)
{
  socket_.reset(new xxsocket());
  state_             = io_base::state::CLOSED;
  dns_queries_state_ = YDQS_FAILED;
  index_             = index;
  decode_len_        = [=](void* ptr, int len) { return this->__builtin_decode_len(ptr, len); };
}
void io_channel::enable_multicast_group(const ip::endpoint& ep, int loopback)
{
  properties_ |= YCPF_MCAST;
  if (loopback)
    properties_ |= YCPF_MCAST_LOOPBACK;

  multiaddr_ = ep;
}
int io_channel::join_multicast_group()
{
  if (socket_->is_open())
  {
    int loopback = (properties_ & YCPF_MCAST_LOOPBACK) ? 1 : 0;
    socket_->set_optval(multiaddr_.af() == AF_INET ? IPPROTO_IP : IPPROTO_IPV6,
                        multiaddr_.af() == AF_INET ? IP_MULTICAST_LOOP : IPV6_MULTICAST_LOOP,
                        loopback);
    // ttl
    socket_->set_optval(multiaddr_.af() == AF_INET ? IPPROTO_IP : IPPROTO_IPV6,
                        multiaddr_.af() == AF_INET ? IP_MULTICAST_TTL : IPV6_MULTICAST_HOPS,
                        YASIO_DEFAULT_MULTICAST_TTL);

    return configure_multicast_group(true);
  }
  return -1;
}
void io_channel::disable_multicast_group()
{
  properties_ &= ~YCPF_MCAST;
  properties_ &= ~YCPF_MCAST_LOOPBACK;

  if (socket_->is_open())
    configure_multicast_group(false);
}
int io_channel::configure_multicast_group(bool onoff)
{
  if (multiaddr_.af() == AF_INET)
  { // ipv4
    struct ip_mreq mreq;
    mreq.imr_interface.s_addr = 0;
    mreq.imr_multiaddr        = multiaddr_.in4_.sin_addr;
    return socket_->set_optval(IPPROTO_IP, onoff ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, &mreq,
                               (int)sizeof(mreq));
  }
  else
  { // ipv6
    struct ipv6_mreq mreq_v6;
    mreq_v6.ipv6mr_interface = 0;
    mreq_v6.ipv6mr_multiaddr = multiaddr_.in6_.sin6_addr;
    return socket_->set_optval(IPPROTO_IPV6, onoff ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP, &mreq_v6,
                               (int)sizeof(mreq_v6));
  }
}
void io_channel::configure_host(std::string host)
{
  if (this->remote_host_ != host)
  {
    this->remote_host_ = std::move(host);

    this->remote_eps_.clear();

    ip::endpoint ep;
    if (ep.assign(this->remote_host_.c_str(), this->remote_port_))
    {
      this->remote_eps_.push_back(ep);
      this->dns_queries_state_ = YDQS_READY;
    }
    else
      this->dns_queries_state_ = YDQSF_QUERIES_NEEDED | YDQS_DIRTY;
  }
}
void io_channel::configure_port(u_short port)
{
  if (port == 0)
    return;
  if (this->remote_port_ != port)
  {
    this->remote_port_ = port;
    if (!this->remote_eps_.empty())
      for (auto& ep : this->remote_eps_)
        ep.port(port);
  }
}
int io_channel::__builtin_decode_len(void* ud, int n)
{
  if (lfb_.length_field_offset >= 0)
  {
    if (n >= (lfb_.length_field_offset + lfb_.length_field_length))
    {
      int32_t length = -1;
      switch (lfb_.length_field_length)
      {
        case 4:
          length =
              ntohl(*reinterpret_cast<int32_t*>((unsigned char*)ud + lfb_.length_field_offset)) +
              lfb_.length_adjustment;
          break;
        case 3:
          length = 0;
          memcpy(&length, (unsigned char*)ud + lfb_.length_field_offset, 3);
          length = (ntohl(length) >> 8) + lfb_.length_adjustment;
          break;
        case 2:
          length =
              ntohs(*reinterpret_cast<uint16_t*>((unsigned char*)ud + lfb_.length_field_offset)) +
              lfb_.length_adjustment;
          break;
        case 1:
          length = *((unsigned char*)ud + lfb_.length_field_offset) + lfb_.length_adjustment;
          break;
      }
      if (length > lfb_.max_frame_length)
        length = -1;
      return length;
    }
    return 0;
  }
  return n;
}
// -------------------- io_transport ---------------------
io_transport::io_transport(io_channel* ctx, std::shared_ptr<xxsocket>& s) : ctx_(ctx)
{
  static unsigned int s_object_id = 0;
  this->state_                    = io_base::state::OPEN;
  this->id_                       = ++s_object_id;
  this->socket_                   = s;
  this->ud_.ptr                   = nullptr;
}
int io_transport::do_read(int& error)
{
  int n = read_cb_(buffer_ + wpos_, sizeof(buffer_) - wpos_);
  error = n < 0 ? xxsocket::get_last_errno() : 0;
  return n;
}
void io_transport::set_primitives()
{
  this->write_cb_ = [=](const void* data, int len) { return socket_->send(data, len); };
  this->read_cb_  = [=](void* data, int len) { return socket_->recv(data, len, 0); };
}
// -------------------- io_transport_tcp ---------------------
inline io_transport_tcp::io_transport_tcp(io_channel* ctx, std::shared_ptr<xxsocket>& s)
    : io_transport(ctx, s)
{}
int io_transport_tcp::write(std::vector<char>&& buffer, std::function<void()>&& handler)
{
  int n = static_cast<int>(buffer.size());
  send_queue_.emplace(std::make_shared<a_pdu>(std::move(buffer), std::move(handler)));
  get_service().interrupt();
  return n;
}
bool io_transport_tcp::do_write(long long& max_wait_duration)
{
  bool ret = false;
  do
  {
    if (!socket_->is_open())
      break;

    int error = -1;
    auto wrap = send_queue_.peek();
    if (wrap)
    {
      auto v                 = *wrap;
      auto outstanding_bytes = static_cast<int>(v->buffer_.size() - v->rpos_);
      int n                  = write_cb_(v->buffer_.data() + v->rpos_, outstanding_bytes);
      if (n == outstanding_bytes)
      { // All pdu bytes sent.
        send_queue_.pop();
#if defined(YASIO_VERBOSE_LOG)
        YASIO_SLOG_IMPL(get_service().options_,
                        "[index: %d] do_write ok, A packet sent "
                        "success, packet size:%d",
                        cindex(), static_cast<int>(v->buffer_.size()),
                        socket_->local_endpoint().to_string().c_str(),
                        socket_->peer_endpoint().to_string().c_str());
#endif
        if (v->handler_)
          v->handler_();
      }
      else if (n > 0)
      {
        // #performance: change offset only, remain data will be send next loop.
        v->rpos_ += n;
        outstanding_bytes = static_cast<int>(v->buffer_.size() - v->rpos_);
      }
      else
      { // n <= 0
        error = xxsocket::get_last_errno();
        if (SHOULD_CLOSE_1(n, error))
        {
          if (((ctx_->properties_ & YCM_UDP) == 0) || error != EPERM)
          { // Fix issue: #126, simply ignore EPERM for UDP
            set_last_errno(error);
            break;
          }
        }
      }
    }

    // If still have work to do.
    if (!send_queue_.empty())
      max_wait_duration = error != EWOULDBLOCK ? 0 : YASIO_WOULDBLOCK_WAIT_DURATION;

    ret = true;
  } while (false);

  return ret;
}
// ----------------------- io_transport_ssl ----------------
#if defined(YASIO_HAVE_SSL)
io_transport_ssl::io_transport_ssl(io_channel* ctx, std::shared_ptr<xxsocket>& s)
    : io_transport_tcp(ctx, s), ssl_(std::move(ctx->ssl_))
{
  ctx->properties_ &= ~YCPF_SSL_HANDSHAKING;
}
void io_transport_ssl::set_primitives()
{
  this->read_cb_  = [=](void* data, int len) { return ::SSL_read(ssl_, data, len); };
  this->write_cb_ = [=](const void* data, int len) { return ::SSL_write(ssl_, data, len); };
}
#endif
// ----------------------- io_transport_udp ----------------
io_transport_udp::io_transport_udp(io_channel* ctx, std::shared_ptr<xxsocket>& s)
    : io_transport(ctx, s)
{}
io_transport_udp::~io_transport_udp() {}
ip::endpoint io_transport_udp::peer_endpoint() const
{
  ensure_peer();

  if (!connected_)
    return this->peer_;
  else
    return socket_->peer_endpoint();
}
const ip::endpoint& io_transport_udp::ensure_peer() const
{
  if (this->peer_.af() == AF_UNSPEC && !ctx_->remote_eps_.empty())
    this->peer_ = ctx_->remote_eps_[0];
  return this->peer_;
}
int io_transport_udp::confgure_remote(const ip::endpoint& peer, bool should_connect)
{
  if (connected_) // connected, update peer is pointless and useless
    return -1;
  this->peer_ = peer;
  if (should_connect)
    return this->connect();
  return 0;
}
int io_transport_udp::connect()
{
  if (connected_)
    return 0;

  if (this->peer_.af() == AF_UNSPEC)
  {
    if (ctx_->remote_eps_.empty())
      return -1;
    this->peer_ = ctx_->remote_eps_[0];
  }

  int retval = this->socket_->connect_n(this->peer_);
  connected_ = (retval == 0);

  set_primitives();
  return retval;
}
int io_transport_udp::write_to(std::vector<char>&& buffer, const ip::endpoint& peer)
{
  this->confgure_remote(peer, false);
  return this->write(std::move(buffer), nullptr);
}
int io_transport_udp::write(std::vector<char>&& buffer, std::function<void()>&& cb)
{
  int n = write_cb_(buffer.data(), static_cast<int>(buffer.size()));
  if (n > 0)
    return n;

  int error = xxsocket::get_last_errno();
  if (SHOULD_CLOSE_1(n, error))
  {
    if (error != EPERM)
    { // Fix issue: #126, simply ignore EPERM for UDP
      set_last_errno(error);
      // finally, trigger transport close
      get_service().close(this);
      return -1; // failed, transport should be close
    }
  }

  return 0; // No error
}
void io_transport_udp::set_primitives()
{
  if (connected_)
    io_transport::set_primitives();
  else
  {
    this->write_cb_ = [=](const void* data, int len) {
      return socket_->sendto(data, len, ensure_peer());
    };
    this->read_cb_ = [=](void* data, int len) {
      ip::endpoint peer;
      int n = socket_->recvfrom(data, len, peer);
      if (n > 0)
        this->peer_ = peer;
      return n;
    };
  }
}
bool io_transport_udp::do_write(long long& max_wait_duration)
{
  return !((opmask_ | ctx_->opmask_) & YOPM_CLOSE_TRANSPORT);
}

#if defined(YASIO_HAVE_KCP)
// ----------------------- io_transport_kcp ------------------
io_transport_kcp::io_transport_kcp(io_channel* ctx, std::shared_ptr<xxsocket>& s)
    : io_transport_udp(ctx, s)
{
  this->kcp_ = ::ikcp_create(0, this);
  ::ikcp_nodelay(this->kcp_, 1, 10 /*MAX_WAIT_DURATION / 1000*/, 2, 1);
  ::ikcp_setoutput(this->kcp_, [](const char* buf, int len, ::ikcpcb* /*kcp*/, void* user) {
    auto t = (io_transport_kcp*)user;
    return t->write_cb_(buf, len);
  });
}
io_transport_kcp::~io_transport_kcp() { ::ikcp_release(this->kcp_); }

int io_transport_kcp::write(std::vector<char>&& buffer, std::function<void()>&& /*handler*/)
{
  std::lock_guard<std::recursive_mutex> lck(send_mtx_);
  int retval = ::ikcp_send(kcp_, buffer.data(), static_cast<int>(buffer.size()));
  get_service().interrupt();
  return retval;
}
int io_transport_kcp::do_read(int& error)
{
  char sbuf[YASIO_INET_BUFFER_SIZE];
  int n = read_cb_(sbuf, sizeof(sbuf));
  if (n > 0)
  { // ikcp in event always in service thread, so no need to lock, TODO: confirm.
    // 0: ok, -1: again, -3: error
    if (0 == ::ikcp_input(kcp_, sbuf, n))
    {
      n = ::ikcp_recv(kcp_, buffer_ + wpos_, sizeof(buffer_) - wpos_);
      if (n < 0) // EAGAIN/EWOULDBLOCK
      {
        n     = -1;
        error = EWOULDBLOCK;
      }
    }
    else
    { // current, simply regards -1,-3 as error and trigger connection lost event.
      n     = 0;
      error = YERR_INVALID_PACKET;
    }
  }
  else
    error = xxsocket::get_last_errno();
  return n;
}
bool io_transport_kcp::do_write(long long& max_wait_duration)
{
  std::lock_guard<std::recursive_mutex> lck(send_mtx_);

  auto current = static_cast<IUINT32>(highp_clock() / 1000);
  ::ikcp_update(kcp_, current);

  auto expire_time        = ::ikcp_check(kcp_, current);
  long long wait_duration = (long long)(expire_time - current) * 1000;
  if (wait_duration < 0)
    wait_duration = 0;

  if (max_wait_duration > wait_duration)
    max_wait_duration = wait_duration;

  return true;
}
#endif

// ------------------------ io_service ------------------------
io_service::io_service() { this->init(nullptr, 1); }
io_service::io_service(int channel_count) { this->init(nullptr, channel_count); }
io_service::io_service(const io_hostent& channel_ep) { this->init(&channel_ep, 1); }
io_service::io_service(const io_hostent* channel_eps, int channel_count)
{
  this->init(channel_eps, channel_count);
}
io_service::~io_service()
{
  stop_service();
  dispose();
}
void io_service::start_service(io_event_cb_t cb)
{
  if (state_ == io_service::state::IDLE)
  {
    static yasio__global_state __global_state;

    if (cb)
      options_.on_event_ = std::move(cb);

    this->state_ = io_service::state::RUNNING;
    if (!options_.no_new_thread_)
    {
      this->worker_    = std::thread(&io_service::run, this);
      this->worker_id_ = worker_.get_id();
    }
    else
    {
      this->worker_id_               = std::this_thread::get_id();
      this->options_.deferred_event_ = false;
      run();
      on_service_stopped();
    }
  }
}
void io_service::stop_service()
{
  if (this->state_ == io_service::state::RUNNING)
  {
    this->state_ = io_service::state::STOPPING;

    this->interrupt();
    this->join();
  }
  else if (this->state_ == io_service::state::STOPPING)
    this->join();
}
void io_service::on_service_stopped()
{
  clear_transports();
  this->state_ = io_service::state::IDLE;
}
void io_service::join()
{
  if (this->worker_.joinable())
  {
    if (std::this_thread::get_id() != this->worker_id_)
    {
      this->worker_.join();
      on_service_stopped();
    }
    else
      errno = EAGAIN;
  }
}
void io_service::init(const io_hostent* channel_eps, int channel_count)
{
  if (this->state_ != io_service::state::UNINITIALIZED)
    return;
  if (channel_count <= 0)
    return;

  FD_ZERO(&fds_array_[read_op]);
  FD_ZERO(&fds_array_[write_op]);
  FD_ZERO(&fds_array_[except_op]);

  this->max_nfds_ = 0;

  options_.resolv_ = [=](std::vector<ip::endpoint>& eps, const char* host, unsigned short port) {
    return this->__builtin_resolv(eps, host, port);
  };

  register_descriptor(interrupter_.read_descriptor(), YEM_POLLIN);

  // Create channels
  create_channels(channel_eps, channel_count);

  this->state_ = io_service::state::IDLE;
}
void io_service::dispose()
{
  if (this->state_ == io_service::state::IDLE)
  {
    clear_channels();
    this->events_.clear();
    this->timer_queue_.clear();

    unregister_descriptor(interrupter_.read_descriptor(), YEM_POLLIN);

    options_.on_event_ = nullptr;
    options_.resolv_   = nullptr;
    options_.print_    = nullptr;

    /// purge transport pool memory
    for (auto o : tpool_)
      ::operator delete(o);
    tpool_.clear();

    this->state_ = io_service::state::UNINITIALIZED;
  }
}
void io_service::create_channels(const io_hostent* channel_eps, int channel_count)
{
  for (auto i = 0; i < channel_count; ++i)
  {
    auto channel = new io_channel(*this, i);
    if (channel_eps != nullptr)
      channel->configure_address(channel_eps[i].host_, channel_eps[i].port_);
    channels_.push_back(channel);
  }
}
void io_service::clear_channels()
{
  this->channel_ops_.clear();
  for (auto channel : channels_)
  {
    channel->timer_.cancel();
    cleanup_io(channel);
    delete channel;
  }
  channels_.clear();
}
void io_service::clear_transports()
{
  for (auto transport : transports_)
  {
    cleanup_io(transport);
    transport->~io_transport();
    this->tpool_.push_back(transport);
  }
  transports_.clear();
}
void io_service::dispatch(int count)
{
  if (options_.on_event_)
    this->events_.consume(count, options_.on_event_);
}
void io_service::run()
{
  yasio__set_thread_name("yasio");

#if defined(YASIO_HAVE_SSL)
  init_ssl_context();
#endif

#if defined(YASIO_HAVE_CARES)
  init_ares_channel();
#endif

  // Call once at startup
  this->ipsv_ = static_cast<u_short>(xxsocket::getipsv());

  // event loop
  fd_set fds_array[max_ops];
  long long max_wait_duration = YASIO_MAX_WAIT_DURATION;
  for (; this->state_ == io_service::state::RUNNING;)
  {
    int retval = do_select(fds_array, max_wait_duration);
    if (this->state_ != io_service::state::RUNNING)
      break;

    max_wait_duration = YASIO_MAX_WAIT_DURATION;

    if (retval == -1)
    {
      int ec = xxsocket::get_last_errno();
      YASIO_SLOG("do_select failed, ec=%d, detail:%s\n", ec, io_service::strerror(ec));
      if (ec == EBADF)
        goto _L_end;
      continue; // just continue.
    }

    if (retval == 0)
      YASIO_SLOGV("%s", "do_select is timeout, process_timers()");

    // Reset the interrupter.
    else if (retval > 0 && FD_ISSET(this->interrupter_.read_descriptor(), &(fds_array[read_op])))
    {
      interrupter_.reset();
      --retval;
    }

#if defined(YASIO_HAVE_CARES)
    // process possible async resolve requests.
    process_ares_requests(fds_array);
#endif

    // process active transports
    process_transports(fds_array, max_wait_duration);

    // process active channels
    process_channels(fds_array);

    // process timeout timers
    process_timers();
  }

_L_end:
  (void)0; // ONLY for xcode compiler happy.

#if defined(YASIO_HAVE_CARES)
  cleanup_ares_channel();
#endif
#if defined(YASIO_HAVE_SSL)
  cleanup_ssl_context();
#endif
}
void io_service::process_transports(fd_set* fds_array, long long& max_wait_duration)
{
  // preform transports
  for (auto iter = transports_.begin(); iter != transports_.end();)
  {
    auto transport = *iter;
    if (do_read(transport, fds_array, max_wait_duration) && do_write(transport, max_wait_duration))
      ++iter;
    else
    {
      handle_close(transport);
      iter = transports_.erase(iter);
    }
  }

  /*
    Because Bind() the client socket to the socket address of the listening socket.  On Linux this
    essentially passes the responsibility for receiving data for the client session from the
    well-known listening socket, to the newly allocated client socket.  It is important to note
    that this behavior is not the same on other platforms, like Windows (unfortunately), detail
    see:
    https://blog.grijjy.com/2018/08/29/creating-high-performance-udp-servers-on-windows-and-linux
    https://cloud.tencent.com/developer/article/1004555
    So we emulate thus by ourself.
    since v3.33, the udp not use send_queue, so we only check status instead do write.
  */
#if defined(_WIN32)
  for (auto iter = dgram_clients_.begin(); iter != dgram_clients_.end();)
  {
    auto transport = iter->second;
    if (transport->do_write(max_wait_duration))
      ++iter;
    else
    {
      transport->set_last_errno(YERR_LOCAL_SHUTDOWN);
      handle_close(transport);
      iter = dgram_clients_.erase(iter);
    }
  }
#endif
}
void io_service::process_channels(fd_set* fds_array)
{
  if (!this->channel_ops_.empty())
  {
    // perform active channels
    std::lock_guard<std::recursive_mutex> lck(this->channel_ops_mtx_);
    for (auto iter = this->channel_ops_.begin(); iter != this->channel_ops_.end();)
    {
      auto ctx    = *iter;
      bool finish = true;
      if (ctx->properties_ & YCM_CLIENT)
      { // resolving, opening
        if (ctx->opmask_ & YOPM_OPEN_CHANNEL)
        {
          switch (this->query_ares_state(ctx))
          {
            case YDQS_READY:
              do_nonblocking_connect(ctx);
              break;
            case YDQS_FAILED:
              handle_connect_failed(ctx, YERR_RESOLV_HOST_FAILED);
              break;
            default:; // YDQS_INPRROGRESS
          }
        }
        else if (ctx->state_ == io_base::state::OPENING)
          do_nonblocking_connect_completion(ctx, fds_array);

        finish = ctx->error_ != EINPROGRESS && (ctx->opmask_ & YOPM_OPEN_CHANNEL) == 0;
      }
      else if (ctx->properties_ & YCM_SERVER)
      {
        auto opmask = ctx->opmask_;
        if (opmask & YOPM_CLOSE_CHANNEL)
          cleanup_io(ctx);

        if (opmask & YOPM_OPEN_CHANNEL)
          do_nonblocking_accept(ctx);

        finish = (ctx->state_ != io_base::state::OPEN);
        if (!finish)
          do_nonblocking_accept_completion(ctx, fds_array);
      }

      if (finish)
        iter = this->channel_ops_.erase(iter);
      else
        ++iter;
    }
  }
}
void io_service::close(int cindex)
{
  // Gets channel context
  auto channel = cindex_to_handle(cindex);
  if (!channel)
    return;

  if (!(channel->opmask_ & YOPM_CLOSE_CHANNEL))
  {
    if (close_internal(channel))
      this->interrupt();
  }
}
void io_service::close(transport_handle_t transport)
{
  if (transport->is_open() && !(transport->opmask_ & YOPM_CLOSE_TRANSPORT))
  {
    transport->opmask_ |= YOPM_CLOSE_TRANSPORT;
    if (transport->ctx_->properties_ & YCM_TCP)
      transport->socket_->shutdown();
    this->interrupt();
  }
}
bool io_service::is_open(transport_handle_t transport) const { return transport->is_open(); }
bool io_service::is_open(int cindex) const
{
  auto ctx = cindex_to_handle(cindex);
  return ctx != nullptr && ctx->state_ == io_base::state::OPEN;
}
void io_service::reopen(transport_handle_t transport)
{
  auto ctx = transport->ctx_;
  if (ctx->properties_ & YCM_CLIENT) // Only client channel support reopen by transport
    open_internal(ctx);
}
void io_service::open(size_t cindex, int kind)
{
  assert((kind > 0 && kind <= 0xff) && ((kind & (kind - 1)) != 0));

  auto ctx = cindex_to_handle(cindex);
  if (ctx != nullptr)
  {
    ctx->properties_ = (ctx->properties_ & (~(uint32_t)0xff)) | kind;
    if (kind & YCM_TCP)
      ctx->protocol_ = SOCK_STREAM;
    else if (kind & YCM_UDP)
      ctx->protocol_ = SOCK_DGRAM;

    open_internal(ctx);
  }
}
io_channel* io_service::cindex_to_handle(size_t cindex) const
{
  if (cindex < channels_.size())
    return channels_[cindex];
  return nullptr;
}
void io_service::handle_close(transport_handle_t thandle)
{
  auto ctx = thandle->ctx_;
  auto ec  = thandle->error_;
  // @Because we can't retrive peer endpoint when connect reset by peer, so use id to trace.
  YASIO_SLOG("[index: %d] the connection #%u is lost, ec=%d, detail:%s", ctx->index_, thandle->id_,
             ec, io_service::strerror(ec));

  cleanup_io(thandle, false);

  deallocate_transport(thandle);

  // @Update context state for client
  if (ctx->properties_ & YCM_CLIENT)
  {
    ctx->error_ = 0;
    ctx->opmask_ &= ~YOPM_CLOSE_TRANSPORT;
    ctx->state_ = io_base::state::CLOSED;
    ctx->properties_ &= (uint32_t)0xffff; // clear private flags
  }

  // @Notify connection lost
  this->handle_event(event_ptr(new io_event(ctx->index_, YEK_CONNECTION_LOST, ec, thandle)));
}
void io_service::register_descriptor(const socket_native_type fd, int flags)
{
  if ((flags & YEM_POLLIN) != 0)
    FD_SET(fd, &(fds_array_[read_op]));

  if ((flags & YEM_POLLOUT) != 0)
    FD_SET(fd, &(fds_array_[write_op]));

  if ((flags & YEM_POLLERR) != 0)
    FD_SET(fd, &(fds_array_[except_op]));

  if (max_nfds_ < static_cast<int>(fd) + 1)
    max_nfds_ = static_cast<int>(fd) + 1;
}
void io_service::unregister_descriptor(const socket_native_type fd, int flags)
{
  if ((flags & YEM_POLLIN) != 0)
    FD_CLR(fd, &(fds_array_[read_op]));

  if ((flags & YEM_POLLOUT) != 0)
    FD_CLR(fd, &(fds_array_[write_op]));

  if ((flags & YEM_POLLERR) != 0)
    FD_CLR(fd, &(fds_array_[except_op]));
}
int io_service::write(transport_handle_t transport, std::vector<char> buffer,
                      std::function<void()> handler)
{
  if (transport && transport->is_open())
  {
    if (!buffer.empty())
      return transport->write(std::move(buffer), std::move(handler));

    return 0;
  }
  else
  {
    YASIO_SLOG("[transport: %p] send failed, the connection not ok!", (void*)transport);
    return -1;
  }
}
int io_service::write_to(transport_handle_t transport, std::vector<char> buffer,
                         const ip::endpoint& to)
{
  if (transport && transport->is_open())
  {
    if (!buffer.empty())
      return transport->write_to(std::move(buffer), to);

    return 0;
  }
  else
  {
    YASIO_SLOG("[transport: %p] send failed, the connection not ok!", (void*)transport);
    return -1;
  }
}
void io_service::handle_event(event_ptr event)
{
  if (options_.deferred_event_)
    events_.emplace(std::move(event));
  else
    options_.on_event_(std::move(event));
}
void io_service::do_nonblocking_connect(io_channel* ctx)
{
  assert(YDQS_CHECK_STATE(ctx->dns_queries_state_, YDQS_READY));
  if (this->ipsv_ == 0)
    this->ipsv_ = static_cast<u_short>(xxsocket::getipsv());
  if (ctx->socket_->is_open())
    cleanup_io(ctx);

  ctx->opmask_ &= ~YOPM_OPEN_CHANNEL;

  if (ctx->remote_eps_.empty())
  {
    this->handle_connect_failed(ctx, YERR_NO_AVAIL_ADDR);
    return;
  }

  ctx->state_ = io_base::state::OPENING;
  auto& ep    = ctx->remote_eps_[0];
  YASIO_SLOG("[index: %d] connecting server %s:%u...", ctx->index_, ctx->remote_host_.c_str(),
             ctx->remote_port_);

  if (ctx->socket_->open(ep.af(), ctx->protocol_))
  {
    int ret = 0;
    if (ctx->properties_ & YCF_REUSEADDR)
      ctx->socket_->reuse_address(true);
    if (ctx->properties_ & YCF_EXCLUSIVEADDRUSE)
      ctx->socket_->reuse_address(false);

    if ((ctx->local_port_ != 0 || ctx->properties_ & YCM_UDP))
      ctx->socket_->bind(YASIO_ADDR_ANY(ep.af()), ctx->local_port_);

    // tcp connect directly, for udp do not need to connect.
    if (ctx->properties_ & YCM_TCP)
      ret = xxsocket::connect_n(ctx->socket_->native_handle(), ep);
    else // udp, we should set non-blocking mode manually
      ctx->socket_->set_nonblocking(true);

    // join the multicast group for udp
    if (ctx->properties_ & YCPF_MCAST)
      ctx->join_multicast_group();

    if (ret < 0)
    { // setup no blocking connect
      int error = xxsocket::get_last_errno();
      if (error != EINPROGRESS && error != EWOULDBLOCK)
      {
        this->handle_connect_failed(ctx, error);
      }
      else
      {
        ctx->set_last_errno(EINPROGRESS);
        register_descriptor(ctx->socket_->native_handle(), YEM_POLLIN | YEM_POLLOUT);
        ctx->timer_.expires_from_now(std::chrono::microseconds(options_.connect_timeout_));
        ctx->timer_.async_wait_once([this, ctx]() {
          if (ctx->state_ != io_base::state::OPEN)
            handle_connect_failed(ctx, ETIMEDOUT);
        });
      }
    }
    else if (ret == 0)
    { // connect server successful immediately.
      register_descriptor(ctx->socket_->native_handle(), YEM_POLLIN);
      handle_connect_succeed(ctx, ctx->socket_);
    } // !!!NEVER GO HERE
  }
  else
    this->handle_connect_failed(ctx, xxsocket::get_last_errno());
}

void io_service::do_nonblocking_connect_completion(io_channel* ctx, fd_set* fds_array)
{
  assert((ctx->properties_ & YCM_TCP) && (ctx->properties_ & YCM_CLIENT));
  assert(ctx->state_ == io_base::state::OPENING);

  if (ctx->state_ == io_base::state::OPENING)
  {
#if !defined(YASIO_HAVE_SSL)
    int error = -1;
    if (FD_ISSET(ctx->socket_->native_handle(), &fds_array[write_op]) ||
        FD_ISSET(ctx->socket_->native_handle(), &fds_array[read_op]))
    {
      socklen_t len = sizeof(error);
      if (::getsockopt(ctx->socket_->native_handle(), SOL_SOCKET, SO_ERROR, (char*)&error, &len) >=
              0 &&
          error == 0)
      {
        // The nonblocking tcp handshake complete, remove write event avoid high-CPU occupation
        unregister_descriptor(ctx->socket_->native_handle(), YEM_POLLOUT);
        handle_connect_succeed(ctx, ctx->socket_);
      }
      else
        handle_connect_failed(ctx, error);

      ctx->timer_.cancel();
    }
#else
    if ((ctx->properties_ & YCPF_SSL_HANDSHAKING) == 0)
    {
      int error = -1;
      if (FD_ISSET(ctx->socket_->native_handle(), &fds_array[write_op]) ||
          FD_ISSET(ctx->socket_->native_handle(), &fds_array[read_op]))
      {
        socklen_t len = sizeof(error);
        if (::getsockopt(ctx->socket_->native_handle(), SOL_SOCKET, SO_ERROR, (char*)&error,
                         &len) >= 0 &&
            error == 0)
        {
          // The nonblocking tcp handshake complete, remove write event avoid high-CPU occupation
          unregister_descriptor(ctx->socket_->native_handle(), YEM_POLLOUT);
          if ((ctx->properties_ & YCM_SSL) == 0)
            handle_connect_succeed(ctx, ctx->socket_);
          else
            do_ssl_handshake(ctx);
        }
        else
          handle_connect_failed(ctx, error);
      }
    }
    else
      do_ssl_handshake(ctx);

    if (ctx->state_ != io_base::state::OPENING)
      ctx->timer_.cancel();
#endif
  }
}
#if defined(YASIO_HAVE_SSL)
void io_service::init_ssl_context()
{
  ssl_ctx_ = ::SSL_CTX_new(SSLv23_client_method());

  if (!this->options_.capath_.empty())
  {
    ::SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_PEER, ::SSL_CTX_get_verify_callback(ssl_ctx_));
    if (::SSL_CTX_load_verify_locations(ssl_ctx_, this->options_.capath_.c_str(), nullptr) != 1)
      YASIO_LOG("load ca certifaction file failed!");
  }
  SSL_CTX_set_mode(ssl_ctx_, SSL_MODE_ENABLE_PARTIAL_WRITE);
}
SSL_CTX* io_service::get_ssl_context() { return ssl_ctx_; }
void io_service::cleanup_ssl_context()
{
  if (ssl_ctx_)
  {
    SSL_CTX_free((SSL_CTX*)ssl_ctx_);
    ssl_ctx_ = nullptr;
  }
}
void io_service::do_ssl_handshake(io_channel* ctx)
{
  if (!ctx->ssl_)
  {
    auto ssl = ::SSL_new(get_ssl_context());
    ::SSL_set_fd(ssl, ctx->socket_->native_handle());
    ::SSL_set_connect_state(ssl);
    ctx->properties_ |= YCPF_SSL_HANDSHAKING; // start ssl handshake
    ctx->ssl_.reset(ssl);
  }

  int ret = ::SSL_do_handshake(ctx->ssl_);
  if (ret != 1)
  {
    int error = ::SSL_get_error(ctx->ssl_, ret);
    /*
    When using a non-blocking socket, nothing is to be done, but select() can be used to check for
    the required condition: https://www.openssl.org/docs/manmaster/man3/SSL_do_handshake.html
    */
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
      ; // Nothing need to do
    else
    {
      YASIO_LOG("SSL_do_handshake fail with ret=%d,error=%d, errno=%d, detail:%s\n", ret, error,
                errno, strerror(errno));

      ctx->ssl_.destroy();
      handle_connect_failed(ctx, YERR_SSL_HANDSHAKE_FAILED);
    }
  }
  else
    handle_connect_succeed(ctx, ctx->socket_);
}
#endif
#if defined(YASIO_HAVE_CARES)
void io_service::ares_getaddrinfo_cb(void* arg, int status, int timeouts, ares_addrinfo* answerlist)
{
  auto ctx              = (io_channel*)arg;
  auto& current_service = ctx->get_service();

  ctx->timer_.cancel();
  current_service.ares_work_finished();

  if (status == ARES_SUCCESS)
  {
    if (answerlist != nullptr)
    {
      for (auto ai = answerlist->nodes; ai != nullptr; ai = ai->ai_next)
      {
        if (ai->ai_family == AF_INET6 || ai->ai_family == AF_INET)
        {
          ctx->remote_eps_.push_back(ip::endpoint(ai->ai_addr));
          break;
        }
      }
    }
  }

  if (!ctx->remote_eps_.empty())
  {
    ctx->dns_queries_state_     = YDQS_READY;
    ctx->dns_queries_timestamp_ = highp_clock();
#  if defined(YASIO_ENABLE_ARES_PROFILER)
    YASIO_SLOG_IMPL(current_service.options_,
                    "[index: %d] ares_getaddrinfo_cb: resolve %s succeed, cost:%g(ms)", ctx->index_,
                    ctx->remote_host_.c_str(),
                    (ctx->dns_queries_timestamp_ - ctx->ares_start_time_) / 1000.0);
#  endif
  }
  else
  {
    ctx->set_last_errno(YERR_RESOLV_HOST_FAILED);
    YDQS_SET_STATE(ctx->dns_queries_state_, YDQS_FAILED);
    YASIO_SLOG_IMPL(current_service.options_,
                    "[index: %d] ares_getaddrinfo_cb: resolve %s failed, status=%d, detail:%s",
                    ctx->index_, ctx->remote_host_.c_str(), status, ::ares_strerror(status));
  }

  current_service.interrupt();
}
void io_service::process_ares_requests(fd_set* fds_array)
{
  if (this->ares_outstanding_work_ > 0)
  {
    ares_socket_t socks[ARES_GETSOCK_MAXNUM] = {0};
    int bitmask = ::ares_getsock(this->ares_, socks, ARES_GETSOCK_MAXNUM);

    for (int i = 0; i < ARES_GETSOCK_MAXNUM; ++i)
    {
      if (ARES_GETSOCK_READABLE(bitmask, i) || ARES_GETSOCK_WRITABLE(bitmask, i))
      {
        auto fd = socks[i];
        ::ares_process_fd(this->ares_, FD_ISSET(fd, &(fds_array[read_op])) ? fd : ARES_SOCKET_BAD,
                          FD_ISSET(fd, &(fds_array[write_op])) ? fd : ARES_SOCKET_BAD);
      }
      else
        break;
    }
  }
}
void io_service::init_ares_channel()
{
  auto status = ::ares_init(&ares_);
  if (::ares_init(&ares_) == ARES_SUCCESS)
  {
    YASIO_LOG("init c-ares channel succeed");

    // list all dns servers for resov problem diagnosis
    ares_addr_node* name_servers = nullptr;
    if (::ares_get_servers(ares_, &name_servers) == ARES_SUCCESS)
    {
      std::stringstream dns_info;
      dns_info << "the c-ares name servers are:";
      int flags = 0;
      for (auto name_server = name_servers; name_server != nullptr; name_server = name_server->next)
      {
        switch (name_server->family)
        {
          case AF_INET:
            if (!IN4_IS_ADDR_LOOPBACK((in_addr*)&name_server->addr) &&
                !IN4_IS_ADDR_LINKLOCAL((in_addr*)&name_server->addr))
              flags |= ipsv_ipv4;
            break;
          case AF_INET6:
            if (IN6_IS_ADDR_GLOBAL((in6_addr*)&name_server->addr))
              flags |= ipsv_ipv6;
            break;
        }
        dns_info << yasio::inet::endpoint::ip(name_server->family, &name_server->addr) << "; ";
      }
      if (flags == 0) // if no valid name server, set to 8.8.8.8 as workaround
        ::ares_set_servers_csv(ares_, "8.8.8.8");
      YASIO_LOG("%s", dns_info.str().c_str());
      ::ares_free_data(name_servers);
    }
  }
  else
    YASIO_LOG("init c-ares channel failed, status=%d, detail:%s", status, ::ares_strerror(status));
}
void io_service::cleanup_ares_channel()
{
  if (ares_ != nullptr)
  {
    ::ares_destroy(this->ares_);
    this->ares_ = nullptr;
  }
}
#endif
void io_service::do_nonblocking_accept(io_channel* ctx)
{ // channel is server
  cleanup_io(ctx);

  // server: don't need resolve, don't use remote_eps_
  ip::endpoint ep(ctx->remote_host_.c_str(), ctx->remote_port_);
  if (ctx->socket_->open(ipsv_ & ipsv_ipv4 ? AF_INET : AF_INET6, ctx->protocol_))
  {
    int error = 0;
    if (ctx->properties_ & YCF_REUSEADDR)
      ctx->socket_->reuse_address(true);
    if (ctx->properties_ & YCF_EXCLUSIVEADDRUSE)
      ctx->socket_->reuse_address(false);
    if (ctx->socket_->bind(ep) != 0)
    {
      error = xxsocket::get_last_errno();
      YASIO_SLOG("[index: %d] bind failed, ec=%d, detail:%s", ctx->index_, error,
                 io_service::strerror(error));
      ctx->socket_->close();
      ctx->state_ = io_base::state::CLOSED;
      return;
    }

    if ((ctx->properties_ & YCM_UDP) || ctx->socket_->listen(YASIO_SOMAXCONN) == 0)
    {
      ctx->state_ = io_base::state::OPEN;
      ctx->socket_->set_nonblocking(true);

      if (ctx->properties_ & YCM_UDP)
      {
        if (ctx->properties_ & YCPF_MCAST)
          ctx->join_multicast_group();

        ctx->buffer_.resize(YASIO_INET_BUFFER_SIZE);
      }
      register_descriptor(ctx->socket_->native_handle(), YEM_POLLIN);
      YASIO_SLOG("[index: %d] socket.fd=%d listening at %s...", ctx->index_,
                 (int)ctx->socket_->native_handle(), ep.to_string().c_str());
    }
    else
    {
      error = xxsocket::get_last_errno();
      YASIO_SLOG("[index: %d] socket.fd=%d listening failed, ec=%d, detail:%s", ctx->index_,
                 (int)ctx->socket_->native_handle(), error, io_service::strerror(error));
      ctx->socket_->close();
      ctx->state_ = io_base::state::CLOSED;
    }
  }
}
void io_service::do_nonblocking_accept_completion(io_channel* ctx, fd_set* fds_array)
{
  if (ctx->state_ == io_base::state::OPEN)
  {
    int error = -1;
    if (FD_ISSET(ctx->socket_->native_handle(), &fds_array[read_op]))
    {
      socklen_t len = sizeof(error);
      if (::getsockopt(ctx->socket_->native_handle(), SOL_SOCKET, SO_ERROR, (char*)&error, &len) >=
              0 &&
          error == 0)
      {
        if (ctx->properties_ & YCM_TCP)
        {
          socket_native_type sockfd;
          error = ctx->socket_->accept_n(sockfd);
          if (error == 0)
            handle_connect_succeed(ctx, std::make_shared<xxsocket>(sockfd));
          else // The non blocking tcp accept failed can be ignored.
            YASIO_SLOGV("[index: %d] socket.fd=%d, accept failed, ec=%u", ctx->index(),
                        (int)ctx->socket_->native_handle(), error);
        }
        else // YCM_UDP
        {
          ip::endpoint peer;
          int n = ctx->socket_->recvfrom(&ctx->buffer_.front(),
                                         static_cast<int>(ctx->buffer_.size()), peer);
          if (n > 0)
          {
            YASIO_SLOGV("recvfrom peer: %s succeed.", peer.to_string().c_str());

            /* make a transport local --> peer udp session, just like tcp accept */
#if !defined(_WIN32)
            auto transport = do_dgram_accept(ctx, peer);
#else
            // for win32, we manage dgram clients by ourself, and perfrom write operation only in
            // dgram_transports, the read operation still dispatch by channel.
            auto it = this->dgram_clients_.find(peer);
            auto transport =
                it != this->dgram_clients_.end() ? it->second : do_dgram_accept(ctx, peer);
#endif
            if (transport)
            {
              this->handle_event(event_ptr(new io_event(
                  transport->cindex(), YEK_PACKET,
                  std::vector<char>(&ctx->buffer_.front(), &ctx->buffer_.front() + n), transport)));
            }
          }
          else
          {
            error = xxsocket::get_last_errno();
            if (SHOULD_CLOSE_0(n, error))
            {
              YASIO_SLOG("[index: %d] recvfrom failed, ec=%d", ctx->index_, error);
              close(ctx->index_);
            }
          }
        }
      }
    }
  }
}
transport_handle_t io_service::do_dgram_accept(io_channel* ctx, const ip::endpoint& peer)
{
  auto client_sock = std::make_shared<xxsocket>();
  if (client_sock->open(peer.af(), SOCK_DGRAM, 0))
  {
    if (ctx->properties_ & YCF_REUSEADDR)
      client_sock->reuse_address(true);
    if (ctx->properties_ & YCF_EXCLUSIVEADDRUSE)
      client_sock->reuse_address(false);
    int error = client_sock->bind(YASIO_ADDR_ANY(peer.af()), 0);
    if (error == 0)
    {
      auto transport =
          static_cast<io_transport_udp*>(allocate_transport(ctx, std::move(client_sock)));

      // We always establish 4 tuple with clients
      transport->confgure_remote(peer, true);
#if !defined(_WIN32)
      handle_connect_succeed(transport);
#else
      notify_connect_succeed(transport);
      this->dgram_clients_.emplace(peer, transport);
#endif
      return transport;
    }
    else
    {
      error = xxsocket::get_last_errno();
      YASIO_SLOG("udp-server: bind address failed, ec=%d, detail:%s", error,
                 xxsocket::strerror(error));
    }
  }

  return nullptr;
}
void io_service::handle_connect_succeed(transport_handle_t transport)
{
  this->transports_.push_back(transport);
  auto ctx = transport->ctx_;
  ctx->set_last_errno(0); // clear errno, value may be EINPROGRESS
  auto& connection = transport->socket_;
  if (ctx->properties_ & YCM_CLIENT)
    ctx->state_ = io_base::state::OPEN;
  else
  { // tcp/udp server, accept a new client session
    connection->set_nonblocking(true);
    register_descriptor(connection->native_handle(), YEM_POLLIN);
  }
  if (ctx->properties_ & YCM_TCP)
  {
#if defined(__APPLE__) || defined(__linux__)
    if (ctx->properties_ & YCM_TCP)
      connection->set_optval(SOL_SOCKET, SO_NOSIGPIPE, (int)1);
#endif
    // apply tcp keepalive options
    if (options_.tcp_keepalive_.onoff)
      connection->set_keepalive(options_.tcp_keepalive_.onoff, options_.tcp_keepalive_.idle,
                                options_.tcp_keepalive_.interval, options_.tcp_keepalive_.probs);
  }

  notify_connect_succeed(transport);
}
void io_service::notify_connect_succeed(transport_handle_t transport)
{
  auto ctx = transport->ctx_;

  YASIO_SLOG("[index: %d] the connection #%u [%s] --> [%s] is established.", ctx->index_,
             transport->id_, transport->local_endpoint().to_string().c_str(),
             transport->peer_endpoint().to_string().c_str());
  this->handle_event(event_ptr(new io_event(ctx->index_, YEK_CONNECT_RESPONSE, 0, transport)));
}
transport_handle_t io_service::allocate_transport(io_channel* ctx, std::shared_ptr<xxsocket> socket)
{
  transport_handle_t transport;
  void* vp;
  if (!tpool_.empty())
  { // allocate from pool
    vp = tpool_.back();
    tpool_.pop_back();
  }
  else
    vp = ::operator new(yasio__global_state::s_max_alloc_size);
#if defined(YASIO_HAVE_SSL)
  if (ctx->properties_ & YCM_SSL)
    transport = new (vp) io_transport_ssl(ctx, socket);
  else
#endif
      if (ctx->properties_ & YCM_TCP)
    transport = new (vp) io_transport_tcp(ctx, socket);
#if defined(YASIO_HAVE_KCP)
  else if (ctx->properties_ & YCM_KCP)
    transport = new (vp) io_transport_kcp(ctx, socket);
#endif
  else /* it's ok to always regard as UDP transport */
    transport = new (vp) io_transport_udp(ctx, socket);

  transport->set_primitives();

  return transport;
}
void io_service::deallocate_transport(transport_handle_t t)
{
  if (t && t->is_valid())
  {
    t->invalid();
    yasio::invoke_dtor(t);
    this->tpool_.push_back(t);
  }
}
void io_service::handle_connect_failed(io_channel* ctx, int error)
{
#if defined(YASIO_HAVE_SSL)
  // Remove tmp flags
  ctx->properties_ &= ~YCPF_SSL_HANDSHAKING;
#endif

  cleanup_io(ctx);

  YASIO_SLOG("[index: %d] connect server %s:%u failed, ec=%d, detail:%s", ctx->index_,
             ctx->remote_host_.c_str(), ctx->remote_port_, error, io_service::strerror(error));
  this->handle_event(event_ptr(new io_event(ctx->index_, YEK_CONNECT_RESPONSE, error, nullptr)));
}
bool io_service::do_read(transport_handle_t transport, fd_set* fds_array,
                         long long& max_wait_duration)
{
  bool ret = false;
  do
  {
    if (!transport->socket_->is_open())
      break;
    if ((transport->opmask_ | transport->ctx_->opmask_) & YOPM_CLOSE_TRANSPORT)
    {
      if (!transport->error_) // If no reason, just set reason: local shutdown
        transport->set_last_errno(YERR_LOCAL_SHUTDOWN);
      break;
    }

    int n = -1, error = EWOULDBLOCK;
    if (FD_ISSET(transport->socket_->native_handle(), &(fds_array[read_op])))
    {
      n = transport->do_read(error);
    }
    if (n > 0 || !SHOULD_CLOSE_0(n, error))
    {
      YASIO_SLOGV("[index: %d] do_read status ok, ec=%d, detail:%s", transport->cindex(), error,
                  io_service::strerror(error));
      if (n == -1)
        n = 0;
#if defined(YASIO_VERBOSE_LOG)
      if (n > 0)
      {
        YASIO_SLOG("[index: %d] do_read ok, received data len: %d, "
                   "buffer data "
                   "len: %d",
                   transport->cindex(), n, n + transport->offset_);
      }
#endif
      if (transport->expected_size_ == -1)
      { // decode length
        int length = transport->ctx_->decode_len_(transport->buffer_, transport->wpos_ + n);
        if (length > 0)
        {
          int bytes_to_strip =
              ::yasio::clamp(transport->ctx_->lfb_.initial_bytes_to_strip, 0, length - 1);
          transport->expected_size_ = length;
          transport->expected_packet_.reserve(
              (std::min)(length - bytes_to_strip,
                         YASIO_MAX_PDU_BUFFER_SIZE)); // #perfomance, avoid memory reallocte.
          unpack(transport, transport->expected_size_, n, bytes_to_strip, max_wait_duration);
        }
        else if (length == 0) // header insufficient, wait readfd ready at next event step.
          transport->wpos_ += n;
        else
        {
          transport->set_last_errno(YERR_DPL_ILLEGAL_PDU);
          break;
        }
      }
      else
      { // process incompleted pdu
        unpack(transport,
               transport->expected_size_ - static_cast<int>(transport->expected_packet_.size()), n,
               0, max_wait_duration);
      }
    }
    else
    { // n == 0: The return value will be 0 when the peer has performed an orderly shutdown.
      transport->set_last_errno(error);
      break;
    }

    ret = true;
  } while (false);

  return ret;
}
void io_service::unpack(transport_handle_t transport, int bytes_expected, int bytes_transferred,
                        int bytes_to_strip, long long& max_wait_duration)
{
  auto bytes_available = bytes_transferred + transport->wpos_;
  transport->expected_packet_.insert(
      transport->expected_packet_.end(), transport->buffer_ + bytes_to_strip,
      transport->buffer_ + (std::min)(bytes_expected, bytes_available));

  // set wpos to bytes of remain buffer
  transport->wpos_ = bytes_available - bytes_expected;
  if (transport->wpos_ >= 0)
  { /* pdu received properly */
    if (transport->wpos_ > 0)
    { /* move remain data to head of buffer and hold wpos. */
      ::memmove(transport->buffer_, transport->buffer_ + bytes_expected, transport->wpos_);
      max_wait_duration = 0;
    }
    // move properly pdu to ready queue, the other thread who care about will retrieve it.
    YASIO_SLOGV("[index: %d] received a properly packet from peer, "
                "packet size:%d",
                transport->cindex(), transport->expected_size_);
    this->handle_event(event_ptr(
        new io_event(transport->cindex(), YEK_PACKET, transport->fetch_packet(), transport)));
  }
  else /* all buffer consumed, set wpos to ZERO, pdu incomplete, continue recv remain data. */
    transport->wpos_ = 0;
}
highp_timer_ptr io_service::schedule(const std::chrono::microseconds& duration, timer_cb_t cb)
{
  auto timer = std::make_shared<highp_timer>(*this);
  timer->expires_from_now(duration);
  /*!important, hold on `timer` by lambda expression */
#if YASIO__HAS_CXX17
  timer->async_wait([timer, cb = std::move(cb)]() { return cb(); });
#else
  timer->async_wait([timer, cb]() { return cb(); });
#endif
  return timer;
}
void io_service::schedule_timer(highp_timer* timer_ctl, timer_cb_t&& timer_cb)
{
  // pitfall: this service only hold the weak pointer of the timer
  // object, so before dispose the timer object need call
  // cancel_timer to cancel it.
  if (timer_ctl == nullptr)
    return;

  std::lock_guard<std::recursive_mutex> lck(this->timer_queue_mtx_);
  auto timer_it = this->find_timer(timer_ctl);
  if (timer_it == timer_queue_.end())
  {
    this->timer_queue_.emplace_back(timer_ctl, std::move(timer_cb));
    this->sort_timers();

    // If the new timer is earliest, wakup
    if (timer_ctl == this->timer_queue_.rbegin()->first)
      this->interrupt();
  }
  else // always replace timer_cb
    timer_it->second = std::move(timer_cb);
}
void io_service::remove_timer(highp_timer* timer)
{
  std::lock_guard<std::recursive_mutex> lck(this->timer_queue_mtx_);

  auto iter = this->find_timer(timer);
  if (iter != timer_queue_.end())
  {
    timer_queue_.erase(iter);
    if (!timer_queue_.empty())
    {
      this->sort_timers();
      this->interrupt();
    }
  }
}
void io_service::open_internal(io_channel* ctx, bool ignore_state)
{
  if (ctx->state_ == io_base::state::OPENING && !ignore_state)
  { // in-opening, do nothing
    YASIO_SLOG("[index: %d] the channel is in opening!", ctx->index_);
    return;
  }

  close_internal(ctx);

  ctx->opmask_ |= YOPM_OPEN_CHANNEL;

  this->channel_ops_mtx_.lock();
  if (std::find(this->channel_ops_.begin(), this->channel_ops_.end(), ctx) ==
      this->channel_ops_.end())
    this->channel_ops_.push_back(ctx);
  this->channel_ops_mtx_.unlock();

  this->interrupt();
}
bool io_service::close_internal(io_channel* ctx)
{
  if (ctx->socket_->is_open())
  {
    if (ctx->properties_ & YCM_CLIENT)
    {
      ctx->opmask_ |= YOPM_CLOSE_TRANSPORT;
      if (ctx->properties_ & YCM_TCP)
        ctx->socket_->shutdown();
    }
    else
      ctx->opmask_ |= YOPM_CLOSE_CHANNEL;
    return true;
  }
  return false;
}
void io_service::process_timers()
{
  if (this->timer_queue_.empty())
    return;

  std::lock_guard<std::recursive_mutex> lck(this->timer_queue_mtx_);

  int n = 0; // the count expired loop timers
  while (!this->timer_queue_.empty())
  {
    auto timer_ctl = timer_queue_.back().first;
    if (timer_ctl->expired())
    {
      // fetch timer
      auto timer_impl = std::move(timer_queue_.back());
      timer_queue_.pop_back();

      if (!timer_impl.second())
      { // reschedule if the timer want wait again
        timer_ctl->expires_from_now();
        timer_queue_.push_back(std::move(timer_impl));
        ++n;
      }
    }
    else
      break;
  }
  if (n)
    sort_timers();
}
int io_service::do_select(fd_set* fdsa, long long max_wait_duration)
{
  int retval = 1;

  ::memcpy(fdsa, this->fds_array_, sizeof(this->fds_array_));

  auto wait_duration = get_wait_duration(max_wait_duration);
  if (wait_duration > 0)
  {
    timeval waitd_tv = {(decltype(timeval::tv_sec))(wait_duration / 1000000),
                        (decltype(timeval::tv_usec))(wait_duration % 1000000)};

#if defined(YASIO_HAVE_CARES)
    int nfds = -1;
    if (this->ares_outstanding_work_ > 0 &&
        (nfds = ::ares_fds(this->ares_, &fdsa[read_op], &fdsa[write_op])) > 0)
    {
      ::ares_timeout(this->ares_, &waitd_tv, &waitd_tv);
      if (this->max_nfds_ < nfds)
        this->max_nfds_ = nfds;
    }
#endif

    YASIO_SLOGV("socket.select maxfdp:%d waiting... %ld milliseconds", maxfdp_,
                waitd_tv.tv_sec * 1000 + waitd_tv.tv_usec / 1000);
    retval = ::select(this->max_nfds_, &(fdsa[read_op]), &(fdsa[write_op]), nullptr, &waitd_tv);
    YASIO_SLOGV("socket.select waked up, retval=%d", retval);
  }

  return retval;
}
long long io_service::get_wait_duration(long long usec)
{
  if (this->timer_queue_.empty())
    return usec;

  std::lock_guard<std::recursive_mutex> lck(this->timer_queue_mtx_);
  auto earliest = timer_queue_.back().first;

  // microseconds
  auto duration = earliest->wait_duration();
  if (std::chrono::microseconds(usec) > duration)
    return duration.count();
  else
    return usec;
}
bool io_service::cleanup_io(io_base* obj, bool clear_state)
{
  obj->error_  = 0;
  obj->opmask_ = 0;
  if (clear_state)
    obj->state_ = io_base::state::CLOSED;
  if (obj->socket_->is_open())
  {
    unregister_descriptor(obj->socket_->native_handle(), YEM_POLLIN | YEM_POLLOUT);
    obj->socket_->close();
    return true;
  }
  return false;
}
u_short io_service::query_ares_state(io_channel* ctx)
{
  if ((ctx->dns_queries_state_ & YDQSF_QUERIES_NEEDED) &&
      !YDQS_CHECK_STATE(ctx->dns_queries_state_, YDQS_INPRROGRESS))
  {
    auto diff = (highp_clock() - ctx->dns_queries_timestamp_);
    if (YDQS_CHECK_STATE(ctx->dns_queries_state_, YDQS_READY) &&
        diff >= options_.dns_cache_timeout_)
      YDQS_SET_STATE(ctx->dns_queries_state_, YDQS_DIRTY);

    if (YDQS_CHECK_STATE(ctx->dns_queries_state_, YDQS_DIRTY))
      start_resolve(ctx);
  }

  return YDQS_GET_STATE(ctx->dns_queries_state_);
}
void io_service::start_resolve(io_channel* ctx)
{ // Only call at event-loop thread, so
  // no need to consider thread safe.
  assert(YDQS_CHECK_STATE(ctx->dns_queries_state_, YDQS_DIRTY));
  ctx->set_last_errno(EINPROGRESS);
  YDQS_SET_STATE(ctx->dns_queries_state_, YDQS_INPRROGRESS);

  YASIO_SLOG("[index: %d] resolving %s", ctx->index_, ctx->remote_host_.c_str());
  ctx->remote_eps_.clear();

#if defined(YASIO_ENABLE_ARES_PROFILER)
  ctx->ares_start_time_ = highp_clock();
#endif
#if !defined(YASIO_HAVE_CARES)
  std::thread async_resolv_thread([=] {
    int error = options_.resolv_(ctx->remote_eps_, ctx->remote_host_.c_str(), ctx->remote_port_);
    if (error == 0)
    {
      ctx->dns_queries_state_     = YDQS_READY;
      ctx->dns_queries_timestamp_ = highp_clock();
#  if defined(YASIO_ENABLE_ARES_PROFILER)
      YASIO_SLOG("[index: %d] resolve %s succeed, cost: %g(ms)", ctx->index_,
                 ctx->remote_host_.c_str(),
                 (ctx->dns_queries_timestamp_ - ctx->ares_start_time_) / 1000.0);
#  endif
    }
    else
    {
      YASIO_SLOG("[index: %d] resolve %s failed, ec=%d, detail:%s", ctx->index_,
                 ctx->remote_host_.c_str(), error, xxsocket::gai_strerror(error));
      YDQS_SET_STATE(ctx->dns_queries_state_, YDQS_FAILED);
    }
    /*
    The getaddrinfo behavior at win32 is strange:
    If the channel 0 is in non-blocking connect, and waiting at select, than
    channel 1 request connect(need dns queries), it's wake up the select call,
    do resolve with getaddrinfo. After resolved, the channel 0 call FD_ISSET
    without select call, FD_ISSET will always return true, even through the
    TCP connection handshake is not complete.

    Try write data to a incomplete TCP will trigger error: 10057
    Another result at this situation is: Try get local endpoint by getsockname
    will return 0.0.0.0
    */
    this->interrupt();
  });
  async_resolv_thread.detach();
#else
  ares_addrinfo_hints hint;
  memset(&hint, 0x0, sizeof(hint));
  hint.ai_family = (this->ipsv_ & ipsv_ipv4) ? hint.ai_family = AF_INET : AF_INET6;
  char sport[sizeof "65535"] = {'\0'};
  const char* service = nullptr;
  if (ctx->remote_port_ > 0)
  {
    sprintf(sport, "%u", ctx->remote_port_); // It's enough for unsigned short, so use sprintf ok.
    service = sport;
  }

  ctx->timer_.expires_from_now(std::chrono::microseconds(options_.dns_queries_timeout_));
  ctx->timer_.async_wait_once([=]() {
    ::ares_cancel(this->ares_);
    handle_connect_failed(ctx, YERR_RESOLV_HOST_FAILED);
  });
  ares_work_started();
  ::ares_getaddrinfo(this->ares_, ctx->remote_host_.c_str(), service, &hint,
                     io_service::ares_getaddrinfo_cb, ctx);
#endif
}
int io_service::__builtin_resolv(std::vector<ip::endpoint>& endpoints, const char* hostname,
                                 unsigned short port)
{
  if (this->ipsv_ & ipsv_ipv4)
    return xxsocket::resolve_v4(endpoints, hostname, port);
  else if (this->ipsv_ & ipsv_ipv6) // localhost is IPv6_only network
    return xxsocket::resolve_v6(endpoints, hostname, port) != 0
               ? xxsocket::resolve_v4to6(endpoints, hostname, port)
               : 0;
  return -1;
}
void io_service::interrupt() { interrupter_.interrupt(); }
const char* io_service::strerror(int error)
{
  switch (error)
  {
    case 0:
      return "No error.";
    case YERR_DPL_ILLEGAL_PDU:
      return "Decode frame length failed!";
    case YERR_RESOLV_HOST_FAILED:
      return "Resolve host failed!";
    case YERR_NO_AVAIL_ADDR:
      return "No available address!";
    case YERR_LOCAL_SHUTDOWN:
      return "An existing connection was shutdown by local host!";
    case YERR_INVALID_PACKET:
      return "Invalid packet!";
    case YERR_SSL_HANDSHAKE_FAILED:
      return "SSL handeshake failed!";
    case -1:
      return "Unknown error!";
    default:
      return xxsocket::strerror(error);
  }
}
void io_service::set_option(int opt, ...)
{
  va_list ap;
  va_start(ap, opt);
  set_option_internal(opt, ap);
  va_end(ap);
}
void io_service::set_option_internal(int opt, va_list ap) // lgtm [cpp/poorly-documented-function]
{
  switch (opt)
  {
    case YOPT_S_DEFERRED_EVENT:
      options_.deferred_event_ = !!va_arg(ap, int);
      break;
    case YOPT_S_TCP_KEEPALIVE:
      options_.tcp_keepalive_.onoff    = 1;
      options_.tcp_keepalive_.idle     = va_arg(ap, int);
      options_.tcp_keepalive_.interval = va_arg(ap, int);
      options_.tcp_keepalive_.probs    = va_arg(ap, int);
      break;
    case YOPT_S_RESOLV_FN:
      options_.resolv_ = *va_arg(ap, resolv_fn_t*);
      break;
    case YOPT_S_PRINT_FN:
      this->options_.print_ = *va_arg(ap, print_fn_t*);
      break;
    case YOPT_S_NO_NEW_THREAD:
      this->options_.no_new_thread_ = !!va_arg(ap, int);
      break;
#if defined(YASIO_HAVE_SSL)
    case YOPT_S_SSL_CACERT:
      this->options_.capath_ = va_arg(ap, const char*);
      break;
#endif
    case YOPT_S_CONNECT_TIMEOUT:
      options_.connect_timeout_ = static_cast<highp_time_t>(va_arg(ap, int)) * std::micro::den;
      break;
    case YOPT_S_DNS_CACHE_TIMEOUT:
      options_.dns_cache_timeout_ = static_cast<highp_time_t>(va_arg(ap, int)) * std::micro::den;
      break;
    case YOPT_S_DNS_QUERIES_TIMEOUT:
      options_.dns_queries_timeout_ = static_cast<highp_time_t>(va_arg(ap, int)) * std::micro::den;
      break;
    case YOPT_C_LFBFD_PARAMS: {
      auto channel = cindex_to_handle(static_cast<size_t>(va_arg(ap, int)));
      if (channel)
      {
        channel->lfb_.max_frame_length    = va_arg(ap, int);
        channel->lfb_.length_field_offset = va_arg(ap, int);
        channel->lfb_.length_field_length = va_arg(ap, int);
        channel->lfb_.length_adjustment   = va_arg(ap, int);
      }
      break;
    }
    case YOPT_C_LFBFD_IBTS: {
      auto channel = cindex_to_handle(static_cast<size_t>(va_arg(ap, int)));
      if (channel)
        channel->lfb_.initial_bytes_to_strip = ::yasio::clamp(va_arg(ap, int), 0, YASIO_MAX_IBTS);
      break;
    }
    case YOPT_S_EVENT_CB:
      options_.on_event_ = *va_arg(ap, io_event_cb_t*);
      break;
    case YOPT_C_LFBFD_FN: {
      auto channel = cindex_to_handle(static_cast<size_t>(va_arg(ap, int)));
      if (channel)
        channel->decode_len_ = *va_arg(ap, decode_len_fn_t*);
      break;
    }
    case YOPT_C_LOCAL_PORT: {
      auto channel = cindex_to_handle(static_cast<size_t>(va_arg(ap, int)));
      if (channel)
        channel->local_port_ = (u_short)va_arg(ap, int);
      break;
    }
    case YOPT_C_REMOTE_HOST: {
      auto channel = cindex_to_handle(static_cast<size_t>(va_arg(ap, int)));
      if (channel)
        channel->configure_host(va_arg(ap, const char*));
      break;
    }
    case YOPT_C_REMOTE_PORT: {
      auto channel = cindex_to_handle(static_cast<size_t>(va_arg(ap, int)));
      if (channel)
        channel->configure_port((u_short)va_arg(ap, int));
      break;
    }
    case YOPT_C_REMOTE_ENDPOINT: {
      auto channel = cindex_to_handle(static_cast<size_t>(va_arg(ap, int)));
      if (channel)
      {
        channel->configure_host(va_arg(ap, const char*));
        channel->configure_port((u_short)va_arg(ap, int));
      }
      break;
    }
    case YOPT_C_ENABLE_MCAST: {
      auto channel = cindex_to_handle(static_cast<size_t>(va_arg(ap, int)));
      if (channel)
      {
        const char* addr = va_arg(ap, const char*);
        int loopback     = va_arg(ap, int);
        channel->enable_multicast_group(ip::endpoint(addr, 0), loopback);
        if (channel->socket_->is_open())
        { // client join directly
          channel->join_multicast_group();
        }
      }
      break;
    }
    case YOPT_C_DISABLE_MCAST: {
      auto channel = cindex_to_handle(static_cast<size_t>(va_arg(ap, int)));
      if (channel)
        channel->disable_multicast_group();
      break;
    }
    case YOPT_C_MOD_FLAGS: {
      auto channel = cindex_to_handle(static_cast<size_t>(va_arg(ap, int)));
      if (channel)
      {
        channel->properties_ |= (uint32_t)va_arg(ap, int);
        channel->properties_ &= ~(uint32_t)va_arg(ap, int);
      }
      break;
    }
    case YOPT_SOCKOPT: {
      auto obj = va_arg(ap, io_base*);
      if (obj && obj->socket_ && obj->socket_->is_open())
      {
        auto optlevel = va_arg(ap, int);
        auto optname  = va_arg(ap, int);
        auto optval   = va_arg(ap, void*);
        auto optlen   = va_arg(ap, int);
        obj->socket_->set_optval(optlevel, optname, optval, optlen);
      }
      break;
    }
  }
}
} // namespace inet
} // namespace yasio

#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#endif
