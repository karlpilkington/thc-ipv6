#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <netdb.h>
#include <pcap.h>
#include "thc-ipv6.h"

#define MAX_SEND 12
#define INCREASE 6
#define SENDS    3
#define POS_SIZE ((SENDS * MAX_SEND) + 2)

extern int debug;
unsigned char buf[4 * 4];
unsigned char *alive[65536];
unsigned char *position[POS_SIZE];
unsigned char *remark[POS_SIZE];
unsigned int rmtu[POS_SIZE];
unsigned char buf2[4];
unsigned short int baseport = 1200;
unsigned int pid = 0;
unsigned int mtu = 0;
unsigned int orig_mtu = 0;
int off = 48;
int buf_len = 16;
int tunnel = 0;

int up_to, complete = 0, type = 0, rawmode = 0;

void help(char *prg) {
  printf("%s %s (c) 2011 by %s %s\n\n", prg, VERSION, AUTHOR, RESOURCE);
  printf("Syntax: %s [-r] [-d] [-t] [-s src6] interface targetaddress [port]\n\n", prg);
  printf("A basic but very fast traceroute6 program.\n");
  printf("If no port is specified, ICMP6 ping requests are used, otherwise TCP SYN\n");
  printf("packets to the specified port. -d resolves the ip6 addresses to DNS.\n");
  printf("Option -t enables tunnel detection mechanism, -s src6 specifies the source ip6\n");
//  printf("Use -r to use raw mode.\n");
  printf("Maximum hop reach: %d\n\n", INCREASE * (SENDS - 1) + MAX_SEND);
  exit(-1);
}

void check_packets(u_char * foo, const struct pcap_pkthdr *header, const unsigned char *data) {
  int i, ok = 0, len = header->caplen;
  unsigned char *ptr = (unsigned char *) data, *ptr2;
  unsigned short int *si;
  unsigned int *ui, new_mtu = 0;
  unsigned char pos = 0;

  if (!rawmode) {
    ptr += 14;
    len -= 14;
  }

  if (debug)
    thc_dump_data((char *) data, header->caplen, "Received Packet");
  complete = 0;

  if (tunnel && ptr[6] == NXT_ICMP6 && ptr[40] == ICMP6_TOOBIG && len >= 100) {
    new_mtu = (ptr[44] << 24) + (ptr[45] << 16) + (ptr[46] << 8) + ptr[47];
    if (new_mtu < mtu) {
      pos = ptr[94];
      if (pos == ptr[93] && pos <= up_to) {
        rmtu[pos - 1] = new_mtu;
        mtu = new_mtu;
        buf_len = mtu - off;
      }
    }
  }

  if (type == 0) {
    if (ptr[6] != NXT_ICMP6)
      return;
    ptr2 = ptr + len - 2 * sizeof(buf2);
    if (memcmp(ptr2, buf2, 4) != 0)     // from a different process?
      return;
    if (ptr[40] == ICMP6_PINGREPLY) {
      pos = ptr[46];
      if (position[pos] != NULL && pos <= up_to && pos == ptr[45]) {
        position[pos] = thc_ipv62notation(ptr + 8);
        remark[pos] = strdup("\t[ping reply received]");
        position[pos + 1] = NULL;
      }
    }
    if (ptr[40] == ICMP6_TTLEXEED && ptr[41] == 0 && len >= 100) {
      pos = ptr[94];
      if (pos == ptr[93] && pos <= up_to)
        position[pos] = thc_ipv62notation(ptr + 8);
    }
    if (ptr[6] == NXT_ICMP6 && ptr[40] == ICMP6_UNREACH) {
      pos = ptr[94];
      if (pos == ptr[93] && pos <= up_to) {
        position[pos] = thc_ipv62notation(ptr + 8);
        remark[pos] = strdup("\t[unreachable message received]");
        if (position[pos + 1][0] == '?')
          position[pos + 1] = NULL;
      }
    }
  } else {
    if (ptr[6] != NXT_ICMP6 && ptr[6] != NXT_TCP)
      return;
    if (ptr[6] == NXT_TCP) {
      si = (unsigned short int *) &ptr[42];
      pos = htons((*si % 65536)) - baseport;
      ui = (unsigned int *) &ptr[48];
      if ((pid + 1) != htonl(*ui))
        return;
      if (position[pos] != NULL && pos <= up_to) {
        position[pos] = thc_ipv62notation(ptr + 8);
        i = ptr[53] & 6;
        switch (i) {
        case 2:
          remark[pos] = strdup("\t[TCP SYN-ACK reply received]");
          break;
        case 4:
          remark[pos] = strdup("\t[TCP RST reply received]");
          break;
        default:
          remark[pos] = strdup("\t[TCP unknown reply received]");
        }
        position[pos + 1] = NULL;
      }
    }
    if (ptr[6] == NXT_ICMP6 && ptr[40] == ICMP6_TTLEXEED && ptr[41] == 0 && len >= 100) {
      si = (unsigned short int *) &ptr[88];
      pos = htons((*si % 65536)) - baseport;
      ui = (unsigned int *) &ptr[92];
      if (pid != htonl(*ui))
        return;
      if (pos <= up_to)
        position[pos] = thc_ipv62notation(ptr + 8);
    }
    if (ptr[6] == NXT_ICMP6 && ptr[40] == ICMP6_UNREACH && len >= 100) {
      si = (unsigned short int *) &ptr[88];
      pos = htons((*si % 65536)) - baseport;
      ui = (unsigned int *) &ptr[92];
      if (pid != htonl(*ui))
        return;
      if (pos <= up_to) {
        position[pos] = thc_ipv62notation(ptr + 8);
        remark[pos] = strdup("\t[unreachable message received]");
        if (position[pos + 1][0] == '?')
          position[pos + 1] = NULL;
      }
    }
  }
  for (i = 1; i <= up_to && position[i] != NULL; i++) {
    if (position[i][0] != '?')
      ok++;
    if (position[ok + 1] == NULL)
      complete = 1;
  }
}

int main(int argc, char *argv[]) {
  unsigned char *pkt = NULL;
  unsigned char *dst6, *src6 = NULL, *mac = NULL, string[64] = "ip6 and dst ";
  int pkt_len = 0, prefer = PREFER_GLOBAL, i, k, m, dport = 0, resolve = 0;
  unsigned int j;
  struct hostent *he;
  unsigned char *interface, *srcmac, buf[2560], dummy[4] = "???", text[32];
  time_t passed;
  pcap_t *p;

  if (argc < 3 || strncmp(argv[1], "-h", 2) == 0)
    help(argv[0]);

  while ((i = getopt(argc, argv, "dtrs:")) >= 0) {
    switch (i) {
    case 'd':
      resolve = 1;
      break;
    case 'r':
      thc_ipv6_rawmode(1);
      rawmode = 1;
      break;
    case 't':
      tunnel = 1;
      break;
    case 's':
      src6 = thc_resolve6(optarg);
      break;
    default:
      fprintf(stderr, "Error: invalid option %c\n", i);
      exit(-1);
    }
  }

  if (argc - optind < 2)
    help(argv[0]);

  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);

  interface = argv[optind];
  dst6 = thc_resolve6(argv[optind + 1]);
  if (src6 == NULL)
    src6 = thc_get_own_ipv6(interface, dst6, prefer);
  srcmac = thc_get_own_mac(interface);
  up_to = MAX_SEND;

  if (argc - optind >= 3 && argv[optind + 2] != NULL) {
    type = 1;
    dport = atoi(argv[optind + 2]);
    if (dport < 0 || dport > 65535) {
      fprintf(stderr, "Error: tcp port (3rd option) is invalid: %s\n", argv[3]);
      exit(-1);
    }
  }

  if (tunnel) {
    if (type == 1)
      off += 12;
    if ((mtu = thc_get_mtu(interface)) > sizeof(buf) + off)
      buf_len = sizeof(buf);
    else
      buf_len = mtu - off;
    orig_mtu = buf_len + off;
  }

  if (src6 == NULL || srcmac == NULL) {
    fprintf(stderr, "Error: interface not valid: %s!\n", interface);
    exit(-1);
  }

  if (rawmode == 0 && (mac = thc_get_mac(interface, src6, dst6)) == NULL) {
    fprintf(stderr, "ERROR: Can not resolve mac address for %s\n", argv[2]);
    exit(-1);
  }
  strcat(string, thc_ipv62notation(src6));

  for (i = 0; i < POS_SIZE; i++) {
    position[i] = dummy;
    remark[i] = strdup("");
    rmtu[i] = 0;
  }

  if ((p = thc_pcap_init(interface, string)) == NULL) {
    fprintf(stderr, "Error: could not capture on interface %s with string %s\n", interface, string);
    exit(-1);
  }

  while (thc_pcap_check(p, (char *) check_packets, NULL) > 0);
  if (type != 0) {
    baseport += getpid() % 60000;
    pid = (getpid() << 16) + getpid();
    if (tunnel == 0)
      buf_len = 0;
  } else {
    buf2[0] = getpid() / 256;
    buf2[1] = getpid() % 256;
    buf2[2] = buf[0];
    buf2[3] = buf[1];
    for (m = 0; m < sizeof(buf) / 4; m++)
      memcpy(buf + m * 4 + (sizeof(buf) % 4), buf2, 4);
  }

  for (k = 0; k < SENDS; k++) {
    if (complete == 0) {
      for (i = 1; i <= up_to; i++) {
        if (position[i] != NULL && position[i][0] == '?') {
          if (type == 0)
            memset((char *) &j, i % 256, 4);
          if ((pkt = thc_create_ipv6(interface, prefer, &pkt_len, src6, dst6, i, 0, 0, 0, 0)) == NULL)
            return -1;
          if (type == 0) {
            if (thc_add_icmp6(pkt, &pkt_len, ICMP6_PINGREQUEST, 0, j, (unsigned char *) &buf, buf_len, 0) < 0)
              return -1;
          } else {
            if (thc_add_tcp(pkt, &pkt_len, baseport + i, dport, pid, 0, TCP_SYN, 5760, 0, NULL, 0, buf_len > 0 ? buf : NULL, buf_len) < 0)
              return -1;
          }
          if (thc_generate_and_send_pkt(interface, srcmac, mac, pkt, &pkt_len) < 0) {
            fprintf(stderr, "Error: Can not send packet, exiting ...\n");
            exit(-1);
          }
          usleep(200);
        } else if (position[i] == NULL)
          up_to = i - 1;
      }
    }

    passed = time(NULL);
    while (passed + k + (k + 1 == SENDS ? 1 : 0) >= time(NULL) && complete == 0)
      thc_pcap_check(p, (char *) check_packets, NULL);

    if (complete == 0 && k + 1 < SENDS && up_to >= MAX_SEND && position[up_to] != NULL && position[up_to][0] != '?')
      up_to += INCREASE;
  }

  thc_pcap_close(p);

  j = 0;
  for (i = 1; i <= up_to && position[i] != NULL; i++)
    if (position[i][0] != '?')
      j++;
  if (j == 0) {
    printf("Trace6 for %s unsuccessful, no packets received.\n", argv[2]);
  } else {
    if (tunnel) {
      sprintf(text, " with starting MTU %d", orig_mtu);
      mtu = orig_mtu;
    } else
      text[0] = 0;
    printf("Trace6 for %s (%s)%s:\n", argv[optind + 1], thc_ipv62notation(dst6), text);
    for (i = 1; i <= up_to && position[i] != NULL; i++) {
      if (tunnel && rmtu[i] > 0 && mtu > rmtu[i]) {
        if (mtu - rmtu[i] < 8)
          sprintf(text, " - new MTU %d", rmtu[i]);
        else if (mtu - rmtu[i] == 20)
          sprintf(text, " - new MTU %d - 6in4 tunnel endpoint", rmtu[i]);
        else if (mtu - rmtu[i] == 28 || mtu - rmtu[i] == 36 || mtu - rmtu[i] == 8 || mtu - rmtu[i] == 16)
          sprintf(text, " - new MTU %d - Teredo tunnel endpoint", rmtu[i]);
        else if (mtu - rmtu[i] == 64)
          sprintf(text, " - new MTU %d - PPTP tunnel endpoint", rmtu[i]);
        else if (mtu - rmtu[i] == 80)
          sprintf(text, " - new MTU %d - AYIYA tunnel endpoint", rmtu[i]);
        else if (mtu - rmtu[i] > 80)
          sprintf(text, " - new MTU %d", rmtu[i]);
        else
          sprintf(text, " - new MTU %d", rmtu[i]);
        mtu = rmtu[i];
      } else
        text[0] = 0;
      if (resolve && position[i][0] != '?') {
        he = gethostbyaddr(thc_resolve6(position[i]), 16, AF_INET6);
        printf(" %2d: %s (%s)%s%s\n", i, position[i], he != NULL ? he->h_name : "", remark[i], text);
      } else
        printf(" %2d: %s%s%s\n", i, position[i], remark[i], text);
    }
    printf("\n");
  }

  return 0;
}
