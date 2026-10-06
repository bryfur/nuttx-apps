/****************************************************************************
 * apps/include/netutils/netcop.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The network coprocessor (docs/netcop.md) as programs see it.  netcopd
 * (apps/netutils/netcop) makes ordinary sockets work through it, so most
 * programs need nothing from here.  What is here, all of it through
 * sockets (usrsock), so that it works from any task and any loadable
 * module and closes itself when the program ends:
 *
 *   - TLS on a connected TCP socket: setsockopt(SOL_TLS, NC_TLS_*), the
 *     coprocessor doing the TLS (ncx_tls_start() does the lot);
 *   - SSH sessions: socket(AF_INET, SOCK_STREAM, NCX_PROTO_SSH),
 *     setsockopt(SOL_NCX, NCX_SSH_OPEN) with an SSH_OPEN body, then
 *     connect() (any address) opens the session; the socket is then the
 *     session's stdin and stdout;
 *   - the control socket, socket(AF_INET, SOCK_RAW, NCX_PROTO_CTRL): a
 *     datagram socket over which any netcop request goes to the
 *     coprocessor (WiFi, BLE, time, SSH keys, ...) and comes back with its
 *     answers, and over which the events a program subscribes to arrive
 *     (ncx_*() below).
 *
 * The bodies are netcop's own (nc_msg.h: nc_enc_*() / nc_dec_*()).
 ****************************************************************************/

#ifndef __APPS_INCLUDE_NETUTILS_NETCOP_H
#define __APPS_INCLUDE_NETUTILS_NETCOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <nuttx/compiler.h>

#include "nc_proto.h"
#include "nc_msg.h"

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* socket(AF_INET, ..., protocol) that netcopd takes for its own */

#define NCX_PROTO_SSH       253   /* SOCK_STREAM: an SSH session */
#define NCX_PROTO_CTRL      254   /* SOCK_RAW: the control socket */

/* TLS offload: the coprocessor's options, passed through (5.7) */

#ifndef SOL_TLS
#  define SOL_TLS           NC_SOL_TLS
#endif

/* netcopd's own options, on an SSH socket */

#define SOL_NCX             283

#define NCX_SSH_OPEN        1     /* set: SSH_OPEN body (nc_enc_ssh_open),
                                   * for connect(), which returns when the
                                   * session runs, or fails with ENOKEY:
                                   * the host key needs a decision */
#define NCX_SSH_HOSTKEY     2     /* get: EV_SSH_HOSTKEY body; set: u8
                                   * NC_SSH_HK_*, returns when the session
                                   * runs or has failed; then connect()
                                   * again (it marks the socket connected) */
#define NCX_SSH_RESIZE      3     /* set: u16 cols, u16 rows */
#define NCX_SSH_INFO        4     /* get: SSH_OPEN response body */
#define NCX_SSH_EXIT        5     /* get: EV_SSH_EXIT body (ENODATA until
                                   * the session has ended) */

/* Control socket: a request is struct ncx_req + body, an answer or event
 * struct ncx_msg + body, one datagram each.  The op is a netcop op (sent
 * on channel 0) or one of netcopd's own:
 */

#define NCX_OP_STATE        0xff01  /* -> struct ncx_state */
#define NCX_OP_SUBSCRIBE    0xff02  /* u32 NCX_EVM_* -> 0 */
#define NCX_OP_WIFI         0xff03  /* -> the last WiFi status (as
                                     * EV_WIFI_STATE), -ENOENT none yet */
#define NCX_OP_BLE          0xff04  /* -> the bonded BLE devices as a list
                                     * (as BLE_LIST), kept up to date by
                                     * EV_BLE_CONN */
#define NCX_OP_NTP          0xff05  /* -> struct ncx_ntp */

/* netcopd's own event: the link to the coprocessor */

#define NCX_EV_LINK         0xff81  /* struct ncx_state */

/* NCX_OP_SUBSCRIBE: which events a control socket gets */

#define NCX_EVM_LINK        0x01    /* NCX_EV_LINK */
#define NCX_EVM_WIFI        0x02    /* EV_WIFI_STATE */
#define NCX_EVM_BLE         0x04    /* EV_BLE_PASSKEY, _CONN, _BATTERY */
#define NCX_EVM_INPUT       0x08    /* EV_BLE_KEY, _MOUSE, _GAMEPAD */
#define NCX_EVM_LOG         0x10    /* EV_LOG */

/* struct ncx_msg kind */

#define NCX_MSG_PART        1     /* A list's entries, more follow */
#define NCX_MSG_FINAL       2     /* The answer */
#define NCX_MSG_EVENT       3     /* An event (tag 0) */

/* struct ncx_state link */

#define NCX_LINK_NONE       0     /* No coprocessor answers */
#define NCX_LINK_SYNC       1     /* Up, saying HELLO */
#define NCX_LINK_READY      2     /* Up and introduced */

#define NCX_MAXMSG          2100  /* Largest control datagram */

/****************************************************************************
 * Public Types
 ****************************************************************************/

begin_packed_struct struct ncx_req
{
  uint16_t tag;                 /* Echoed in the answers */
  uint16_t op;
} end_packed_struct;

begin_packed_struct struct ncx_msg
{
  uint8_t  kind;                /* NCX_MSG_* */
  uint8_t  reserved;
  uint16_t tag;
  int32_t  status;              /* FINAL: >= 0 or -errno */
  uint16_t code;                /* The op, or the event */
  uint16_t reserved2;
} end_packed_struct;

begin_packed_struct struct ncx_state
{
  uint8_t  link;                /* NCX_LINK_* */
  uint8_t  restarted;           /* The coprocessor restarted (last up) */
  uint8_t  reserved[2];
  uint32_t caps;                /* HELLO's NC_CAP_* (READY) */
  uint32_t ups;                 /* Times the link came up */
  uint32_t baud;
  uint8_t  max_sockets;
  uint8_t  max_tls;
  uint8_t  max_ssh;
  uint8_t  max_ble;
  uint8_t  mac[6];
  uint8_t  reserved3[2];
  char     fw_version[NC_NAME_MAX + 1];
  char     platform[NC_NAME_MAX + 1];
} end_packed_struct;

begin_packed_struct struct ncx_ntp
{
  uint8_t  running;             /* The NTP client was started */
  uint8_t  synced;              /* ... and has set the clock */
  uint8_t  reserved[2];
  int64_t  last;                /* When it last set it (unix seconds) */
  int64_t  offset_us;           /* By how much */
  char     server[64];
} end_packed_struct;

/* The answer to ncx_call() */

struct ncx_rsp
{
  int32_t  status;
  size_t   n;                   /* Body bytes kept */
  uint8_t  body[NCX_MAXMSG];
};

/* A list's entries: each RSP_PART body ({u8 n, n entries}) */

typedef void (*ncx_part_cb)(FAR void *arg, FAR const uint8_t *body,
                            size_t n);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* The control socket: a file descriptor, or -1 with errno (ENETDOWN or
 * EAFNOSUPPORT: no netcopd)
 */

int ncx_open(void);

/* A request and its answer: the status (>= 0, or -errno), the final body
 * in rsp (may be NULL), each part to part (may be NULL).  Events that
 * arrive meanwhile are dropped.  timeout_ms 0: 30 s.
 */

int ncx_call(int fd, uint16_t op, FAR const void *body, size_t n,
             FAR struct ncx_rsp *rsp, ncx_part_cb part, FAR void *arg,
             int timeout_ms);

/* The same, opening and closing a control socket for it */

int ncx_call1(uint16_t op, FAR const void *body, size_t n,
              FAR struct ncx_rsp *rsp, ncx_part_cb part, FAR void *arg,
              int timeout_ms);

/* Asynchronously: send a request (tag is the caller's), then take what
 * comes, one message at a time (ncx_recv: its body length, -1 with errno
 * EAGAIN when the socket is non-blocking and nothing is there)
 */

int ncx_send(int fd, uint16_t tag, uint16_t op, FAR const void *body,
             size_t n);
ssize_t ncx_recv(int fd, FAR struct ncx_msg *m, FAR void *body, size_t cap);

/* Events: NCX_EVM_* */

int ncx_subscribe(int fd, uint32_t mask);

/* The link: struct ncx_state; 0, or -errno (-ENETDOWN: no netcopd) */

int ncx_state(FAR struct ncx_state *st);

/* TLS on a connected TCP socket (host: the name verified and sent as
 * SNI; alpn may be NULL; verify false: accept any certificate).  0, or
 * -errno (ETIME, EKEYEXPIRED, EKEYREJECTED, EPROTO...); info (may be
 * NULL) is filled in either way when the coprocessor says.
 */

int ncx_tls_start(int sd, FAR const char *host, FAR const char *alpn,
                  bool verify, FAR struct nc_tls_info *info);

/* Names */

FAR const char *ncx_wifi_state_str(uint8_t state);
FAR const char *ncx_wifi_auth_str(uint8_t auth);
FAR const char *ncx_wifi_reason_str(uint8_t reason);
FAR const char *ncx_tls_verify_str(uint8_t verify);
int ncx_rssi_bars(int8_t rssi);                     /* 0..4 */

#ifdef __cplusplus
}
#endif

#endif /* __APPS_INCLUDE_NETUTILS_NETCOP_H */
