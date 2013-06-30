/*
 * librouter is a virtual router software that moves packets back and forth to
 * SLIRP, taking care of virtual ARP resolution and DHCP management in the
 * process.
 *
 * I created it specifically for making DOSemu networking easier, but it might
 * have other interesting use cases, too.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation,  either version 3 of the License,  or (at your option)  any later
 * version.
 *
 * This program is  distributed in the hope that it will be useful,  but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Copyright (C) Mateusz Viste 2013. All rights reserved.
 * Contact: <mateusz$viste-family.net> (replace the $ sign by a @)
 */

#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <sys/select.h>
#include "emu.h"
#include "slirp.h"
#include "netparse.h"
#include "arp.h"
#include "forgehdr.h"
#include "processpkt.h"
#include "librouter.h"  /* include self for control */

#define printf pd_printf


/* returns a socket that can be monitored for new incoming packets. Returns -1 on error. */
int librouter_init(char *slirpexec) {
  uint8_t mymac[] = {0xF0,0xDE,0xF1,0x29,0x07,0x85};
  uint32_t myip[] = {(10 << 24) | (0 << 16) | (2 << 8) | 1,  /* me as the gateway */
                     (10 << 24) | (0 << 16) | (2 << 8) | 2,  /* me as the host */
                     (10 << 24) | (0 << 16) | (2 << 8) | 3,  /* me as the dns server */
                     0};                                     /* last entry is marked as '0' */
  uint32_t mynetmask = 0xFFFFFF00;

  struct arptabletype *arptable = NULL;
  #define buffmaxlen 4096
  uint8_t buff[buffmaxlen];
  int x, bufflen;
  int slirpfdarr[2], sockpair[2];
  int slirpfd, clientfd, highestfd;
  pid_t child;
  fd_set rfds;
  struct timeval tv;

  /* open the communication channel with SLIRP */
  if (slirp_open(slirpexec, slirpfdarr) != 0) {
    printf("Error: failed to invoke '%s'.\n", slirpexec);
    return(-1);
  }
  /* printf("open SLIRP channel (%s): success.\n", slirpexec); */

  /* add all my IP addresses to the ARP cache, to avoid distributing them later to clients via DHCP */
  for (x = 0; myip[x] != 0; x++) {
    arptable = arp_learn(arptable, mymac, myip[x]);
  }
  slirpfd = slirpfdarr[0];

  /* now that slirp is open, we compute a socketpair, return one end of the pair to the caller, and fork off to work on the other end */

  /* note: do NOT use SOCK_DGRAM with socketpair, because DGRAM (being purely non-connected) doesn't notify you when the socket gets closed, so select() won't detect when the socket gets closed on the other end. I can't use SOCK_STREAM here because I badly need to preserve message boundaries (since these are nothing else than raw eth frames). Fortunately Linux provides us with a cool alternative: SOCK_SEQPACKET. Preserves messages boundaries AND notifies on close. */
  if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockpair) < 0) {
    pd_printf("opening stream socket pair failed");
    return(-1);
  }

  /* fork off */
  if ((child = fork()) == -1) {
      printf("fork error: %s\n", strerror(errno));
      return(-1);
    } else if (child != 0) {     /* This is the parent. */
      close(sockpair[0]);
      return(sockpair[1]);
   } else {     /* This is the child - it stays active and keep processing incoming/outgoing stuff */
      close(sockpair[1]); /* but first close the parent's end of the socket, to be sure we won't write there by mistake */
      clientfd = sockpair[0];
      for (;;) {
        /* wait for activity on sockets */
        FD_ZERO(&rfds);
        FD_SET(clientfd, &rfds);
        FD_SET(slirpfd, &rfds);
        if (slirpfd > clientfd) {
            highestfd = slirpfd;
          } else {
            highestfd = clientfd;
        }
        /* Wait up to one second. */
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        /* wait for something to happen */
        select(highestfd+1, &rfds, NULL, NULL, &tv);
        /* is there something on the client fd? */
        if (FD_ISSET(clientfd, &rfds) != 0) {
          bufflen = read(clientfd, buff, buffmaxlen);
          if (bufflen < 0) { /* error on socket - most probably dosemu is gone */
            close(clientfd);
            close(slirpfd);
            exit(0);
          }
          /* puts("Got packet from clientfd"); */
          processpkt(buff, bufflen, myip, mynetmask, mymac, slirpfd, &arptable, clientfd);
        }

        /* is there something on the slirp iface? */
        if (FD_ISSET(slirpfd, &rfds) != 0) {
          uint8_t *buffptr;
          buffptr = buff + 128; /* leaving some place before data, for headers */
          bufflen = slirp_read(buffptr, slirpfd);
          /* printf("Got %d bytes on the slirp iface.\n", bufflen); */
          if (bufflen < 0) { /* slirp is gone (probably because the dosemu parent has been terminated) */
            close(clientfd);
            close(slirpfd);
            exit(0);
          }
          if (bufflen > 0) { /* ignore 0-len packets (these are generated by SLIP) */
            uint8_t *dstmac;
            uint8_t *ipsrcarr, *ipdstarr;
            uint32_t ipdst;
            int ipprotocol, dscp, ttl, id, fragoffset, morefragsflag;
            uint8_t ethbroadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            /* analyze the IP header */
            parse_ipv4(buffptr, bufflen, &ipsrcarr, &ipdstarr, &ipprotocol, &dscp, &ttl, &id, &fragoffset, &morefragsflag);
            ipdst = 0;
            for (x = 0; x < 4; x++) {
              ipdst <<= 8;
              ipdst |= ipdstarr[x];
            }
            /* check if the destination belongs to my network */
            if ((ipdst & mynetmask) == (myip[0] & mynetmask)) {
              /* compute the dst mac */
              dstmac = arp_getmac(arptable, ipdst);
              if (dstmac == NULL) {
                printf("Could not find the mac for %u.%u.%u.%u in the ARP cache. Broadcasting then!\n", ipdstarr[0], ipdstarr[1], ipdstarr[2], ipdstarr[3]);
                dstmac = ethbroadcast;
              }
              /* encapsulate the data into an eth frame and relay it */
              forge_eth(&buffptr, &bufflen, mymac, dstmac);
              write(clientfd, buffptr, bufflen);
            }
          }


        } /* for (;;) */
     }
  }
}