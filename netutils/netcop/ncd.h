/****************************************************************************
 * apps/netutils/netcop/ncd.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * netcopd's insides: one thread owns the link to the coprocessor (the UART)
 * and /dev/usrsock, and nothing here is called from anywhere else.
 ****************************************************************************/

#ifndef __APPS_NETUTILS_NETCOP_NCD_H
#define __APPS_NETUTILS_NETCOP_NCD_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <syslog.h>

#include "nc_host.h"
#include "netutils/netcop.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NCD_NCHAN       256              /* Socket ids 1..255: channels */
#define NCD_NCTRL       8                /* Control sockets, ids 256.. */
#define NCD_CTRL0       NCD_NCHAN
#define NCD_NSOCK       (NCD_NCHAN + NCD_NCTRL)

#define NCD_CTRL_QMAX   8192             /* Bytes queued per control socket */

/* Socket kinds */

#define NCD_TCP         1
#define NCD_DGRAM       2                /* UDP, ICMP */
#define NCD_SSH         3
#define NCD_CTRL        4

#define ncd_info(...)   syslog(LOG_INFO, "netcop: " __VA_ARGS__)
#define ncd_err(...)    syslog(LOG_ERR, "netcop: " __VA_ARGS__)

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct ncd_msg                           /* A control socket's queue */
{
  struct ncd_msg *next;
  uint16_t        len;
  uint8_t         data[];                /* struct ncx_msg + body */
};

struct ncd_sock
{
  uint16_t  id;                          /* usrsock's id; = the channel */
  uint16_t  gen;                         /* Tells reuses apart */
  uint8_t   kind;                        /* NCD_* */
  uint8_t   family;                      /* AF_INET... */
  bool      open;                        /* The cop's channel is open */
  bool      aborted;                     /* The link went down under it */
  bool      closing;                     /* Closed; CHAN_CLOSE pending */
  bool      eof;                         /* EV_CHAN_EOF */
  int32_t   err;                         /* EV_CHAN_ERROR (positive) */
  bool      sig_recv;                    /* usrsock knows: data to read */
  bool      sig_send;                    /* ... room to send */
  bool      listening;
  uint16_t  accept_pending;              /* EV_CHAN_ACCEPT's count */

  /* The usrsock request held in progress (one per socket at a time) */

  uint32_t  pend_xid;                    /* 0: none */
  uint8_t   pend_req;                    /* USRSOCK_REQUEST_* */
  uint16_t  pend_max;                    /* Its value/address room */
  uint16_t  pend_new;                    /* ACCEPT: the new socket */

  /* Received data: a ring of 'cap' bytes; datagrams as records
   * (u16 length, the DGRAM payload: addr then bytes)
   */

  uint8_t  *rx;
  uint32_t  cap;
  uint32_t  head;
  uint32_t  len;

  struct nc_addr peer;                   /* TCP: for recvfrom's address */
  bool      have_peer;

  /* A raw ICMP socket (stock ping) on the cop's ICMP datagram socket: the
   * IP header added to what it receives, the identifier put back
   */

  bool      icmp_raw;
  uint16_t  icmp_id;

  /* SSH */

  uint8_t  *hostkey;                     /* EV_SSH_HOSTKEY body */
  uint16_t  hostkey_n;
  uint8_t  *info;                        /* SSH_OPEN response body */
  uint16_t  info_n;
  uint8_t  *exitb;                       /* EV_SSH_EXIT body */
  uint16_t  exit_n;
  bool      ssh_opening;                 /* SSH_OPEN sent, not answered */
  uint8_t  *ssh_req;                     /* NCX_SSH_OPEN's body, for connect */
  uint16_t  ssh_req_n;

  /* Control socket */

  struct ncd_msg *qhead;
  struct ncd_msg *qtail;
  uint32_t  qbytes;
  uint32_t  evmask;                      /* NCX_EVM_* */
};

struct ncd_s
{
  struct nc_host       h;                /* Link, requests, channels */
  int                  uart;
  int                  usock;
  bool                 ready;            /* HELLO answered */
  uint32_t             ups;
  bool                 restarted;
  uint32_t             baud;
  struct nc_hello_rsp  hello;
  struct ncd_sock     *sock[NCD_NSOCK];
  uint16_t             gen;
  uint32_t             window;           /* Receive window per channel */

  /* What the cop shows, as last heard */

  bool                 have_wifi;
  struct nc_wifi_status wifi;
  uint8_t              wifi_body[160];   /* ... as it came */
  uint16_t             wifi_n;
  uint8_t              nble;
  struct nc_ble_dev    ble[8];

  /* Time */

  bool                 clock_valid;      /* Worth giving the cop */
  bool                 ntp_running;
  bool                 ntp_synced;
  int64_t              ntp_last;
  int64_t              ntp_offset;
  unsigned int         ntp_nsamples;
  uint32_t             ntp_check_ms;
  uint32_t             timeset_ms;       /* When TIME_SET was sent */

  /* The request being read from /dev/usrsock */

  uint8_t              req[NC_MTU_MAX + 128];
  uint8_t              out[NC_MTU_MAX + 128];
  uint8_t              tmp[NC_MTU_MAX];     /* Request bodies */
};

extern struct ncd_s *g_ncd;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* netcopd.c */

uint32_t ncd_now(void);
void ncd_ctrl_event(uint32_t mask, uint16_t code, const void *body,
                    size_t n);
int  ncd_ctrl_request(struct ncd_sock *s, const uint8_t *p, size_t n);
void ncd_ctrl_free(struct ncd_sock *s);
bool ncd_ctrl_take(struct ncd_sock *s, struct ncd_msg **m);

/* ncd_sock.c: usrsock */

void ncd_usrsock_request(void);
void ncd_sock_data(uint8_t chan, uint8_t type, const uint8_t *p, size_t n);
void ncd_sock_credit(uint8_t chan);
void ncd_sock_event(uint8_t chan, uint16_t code, const uint8_t *p,
                    size_t n);
void ncd_sock_link_down(void);
void ncd_sock_poll(void);
void ncd_us_event(uint16_t id, uint16_t events);

/* ncd_hid.c: BLE input */

void ncd_hid_init(void);
void ncd_hid_event(uint16_t code, const uint8_t *p, size_t n);

#endif /* __APPS_NETUTILS_NETCOP_NCD_H */
