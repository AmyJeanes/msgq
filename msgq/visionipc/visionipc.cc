#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include "msgq/visionipc/visionipc.h"

#ifdef _WIN32
#include <winsock2.h>
#include <afunix.h>
#include <windows.h>
#include <stdint.h>
#include <mutex>

// Windows has AF_UNIX stream sockets but no SCM_RIGHTS, so the sender duplicates
// the buffers' section handles into the peer process (the socket knows its pid)
// and sends the handle values. Messages are length-prefixed because
// SOCK_SEQPACKET does not exist here.
static void wsa_init() {
  static std::once_flag once;
  std::call_once(once, []() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  });
}

static bool send_all(SOCKET s, const void *buf, size_t len) {
  const char *p = (const char *)buf;
  while (len > 0) {
    int n = ::send(s, p, (int)len, 0);
    if (n <= 0) return false;
    p += n;
    len -= n;
  }
  return true;
}

static bool recv_all(SOCKET s, void *buf, size_t len) {
  char *p = (char *)buf;
  while (len > 0) {
    int n = ::recv(s, p, (int)len, 0);
    if (n <= 0) return false;
    p += n;
    len -= n;
  }
  return true;
}

int ipc_connect(const char* socket_path) {
  wsa_init();
  SOCKET sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock == INVALID_SOCKET) return -1;
  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);
  if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    closesocket(sock);
    return -1;
  }
  return (int)sock;
}

int ipc_bind(const char* socket_path) {
  wsa_init();
  unlink(socket_path);
  SOCKET sock = socket(AF_UNIX, SOCK_STREAM, 0);
  assert(sock != INVALID_SOCKET);
  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);
  int err = bind(sock, (struct sockaddr *)&addr, sizeof(addr));
  assert(err == 0);
  err = listen(sock, 3);
  assert(err == 0);
  return (int)sock;
}

void ipc_close(int fd) {
  closesocket((SOCKET)fd);
}

int ipc_sendrecv_with_fds(bool send, int fd, void *buf, size_t buf_size, int* fds, int num_fds,
                          int *out_num_fds) {
  SOCKET sock = (SOCKET)fd;
  if (send) {
    uint32_t len = (uint32_t)buf_size, n = (uint32_t)num_fds;
    if (!send_all(sock, &len, sizeof(len)) || !send_all(sock, buf, buf_size) || !send_all(sock, &n, sizeof(n))) return -1;
    if (n > 0) {
      DWORD peer_pid = 0, bytes = 0;
      if (WSAIoctl(sock, SIO_AF_UNIX_GETPEERPID, NULL, 0, &peer_pid, sizeof(peer_pid), &bytes, NULL, NULL) != 0) return -1;
      HANDLE peer = OpenProcess(PROCESS_DUP_HANDLE, FALSE, peer_pid);
      if (peer == NULL) return -1;
      for (uint32_t i = 0; i < n; i++) {
        HANDLE dup = NULL;
        BOOL ok = DuplicateHandle(GetCurrentProcess(), (HANDLE)(intptr_t)fds[i], peer, &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
        uint32_t handle = (uint32_t)(uintptr_t)dup;  // kernel handles fit in 32 bits
        if (!ok || !send_all(sock, &handle, sizeof(handle))) {
          CloseHandle(peer);
          return -1;
        }
      }
      CloseHandle(peer);
    }
    return (int)buf_size;
  } else {
    uint32_t len = 0, n = 0;
    if (!recv_all(sock, &len, sizeof(len)) || len > buf_size || !recv_all(sock, buf, len) || !recv_all(sock, &n, sizeof(n))) {
      errno = ECONNRESET;
      return -1;
    }
    if (n > 0) {
      assert(fds && (int)n <= num_fds);
      for (uint32_t i = 0; i < n; i++) {
        uint32_t handle = 0;
        if (!recv_all(sock, &handle, sizeof(handle))) {
          errno = ECONNRESET;
          return -1;
        }
        fds[i] = (int)handle;
      }
    }
    if (fds) {
      assert(out_num_fds);
      *out_num_fds = (int)n;
    }
    return (int)len;
  }
}

#else
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>

#ifdef __APPLE__
#define getsocket() socket(AF_UNIX, SOCK_STREAM, 0)
#else
#define getsocket() socket(AF_UNIX, SOCK_SEQPACKET, 0)
#endif

int ipc_connect(const char* socket_path) {
  int err;

  int sock = getsocket();

  if (sock < 0) return -1;
  struct sockaddr_un addr = {
    .sun_family = AF_UNIX,
  };
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);
  err = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
  if (err != 0) {
    close(sock);
    return -1;
  }

  return sock;
}

int ipc_bind(const char* socket_path) {
  int err;

  unlink(socket_path);

  int sock = getsocket();

  struct sockaddr_un addr = {
    .sun_family = AF_UNIX,
  };
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);
  err = bind(sock, (struct sockaddr *)&addr, sizeof(addr));
  assert(err == 0);

  err = listen(sock, 3);
  assert(err == 0);

  return sock;
}

void ipc_close(int fd) {
  close(fd);
}


int ipc_sendrecv_with_fds(bool send, int fd, void *buf, size_t buf_size, int* fds, int num_fds,
                          int *out_num_fds) {
  char control_buf[CMSG_SPACE(sizeof(int) * num_fds)];
  memset(control_buf, 0, CMSG_SPACE(sizeof(int) * num_fds));

  struct iovec iov = {
    .iov_base = buf,
    .iov_len = buf_size,
  };
  struct msghdr msg = {
    .msg_iov = &iov,
    .msg_iovlen = 1,
  };

  if (num_fds > 0) {
    assert(fds);

    msg.msg_control = control_buf;
    msg.msg_controllen = CMSG_SPACE(sizeof(int) * num_fds);
  }

  if (send) {
    if (num_fds) {
      struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
      assert(cmsg);
      cmsg->cmsg_level = SOL_SOCKET;
      cmsg->cmsg_type = SCM_RIGHTS;
      cmsg->cmsg_len = CMSG_LEN(sizeof(int) * num_fds);
      memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * num_fds);
    }
    return sendmsg(fd, &msg, 0);
  } else {
    int r = recvmsg(fd, &msg, 0);
    if (r < 0) return r;

    int recv_fds = 0;
    if (msg.msg_controllen > 0) {
      struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
      assert(cmsg);
      assert(cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS);
      recv_fds = (cmsg->cmsg_len - CMSG_LEN(0));
      assert(recv_fds > 0 && (recv_fds % sizeof(int)) == 0);
      recv_fds /= sizeof(int);

      assert(fds && recv_fds <= num_fds);
      memcpy(fds, CMSG_DATA(cmsg), sizeof(int) * recv_fds);
    }

    if (msg.msg_flags) {
      for (int i=0; i<recv_fds; i++) {
        close(fds[i]);
      }
      return -1;
    }

    if (fds) {
      assert(out_num_fds);
      *out_num_fds = recv_fds;
    }
    return r;
  }
}
#endif
