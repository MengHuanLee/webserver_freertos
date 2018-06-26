/*
 * @brief LWIP FreeRTOS HTTP Webserver example
 *
 * @note
 * Copyright(C) NXP Semiconductors, 2014
 * All rights reserved.
 *
 * @par
 * Software that is described herein is for illustrative purposes only
 * which provides customers with programming information regarding the
 * LPC products.  This software is supplied "AS IS" without any warranties of
 * any kind, and NXP Semiconductors and its licensor disclaim any and
 * all warranties, express or implied, including all implied warranties of
 * merchantability, fitness for a particular purpose and non-infringement of
 * intellectual property rights.  NXP Semiconductors assumes no responsibility
 * or liability for the use of the software, conveys no license or rights under any
 * patent, copyright, mask work right, or any other intellectual property rights in
 * or to any products. NXP Semiconductors reserves the right to make changes
 * in the software without notification. NXP Semiconductors also makes no
 * representation or warranty that such application will be suitable for the
 * specified use without further testing or modification.
 *
 * @par
 * Permission to use, copy, modify, and distribute this software and its
 * documentation is hereby granted, under NXP Semiconductors' and its
 * licensor's relevant copyrights in the software, without fee, provided that it
 * is used in conjunction with NXP Semiconductors microcontrollers.  This
 * copyright, permission, and disclaimer notice must appear in all copies of
 * this code.
 */

#include "lwip/init.h"
#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/memp.h"
#include "lwip/tcpip.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/timers.h"
#include "netif/etharp.h"
#include "lwip/sockets.h"


#if LWIP_DHCP
#include "lwip/dhcp.h"
#endif

#include <string.h>
#include "board.h"
#include "lpc_phy.h"
#include "arch/lpc17xx_40xx_emac.h"
#include "arch/lpc_arch.h"
#include "arch/sys_arch.h"
#include "lpc_phy.h"/* For the PHY monitor support */

#define SENDER_PORT_NUM 6000
#define SENDER_IP_ADDR "192.168.0.1"

#define SERVER_PORT_NUM 6001
#define SERVER_IP_ADDR "192.168.0.3"

/*****************************************************************************
 * Private types/enumerations/variables
 ****************************************************************************/

/* NETIF data */
static struct netif lpc_netif;
static void server_thread(void *arg);
void client();

/*****************************************************************************
 * Public types/enumerations/variables
 ****************************************************************************/

/*****************************************************************************
 * Private functions
 ****************************************************************************/

extern void http_server_netconn_init(void);

/* Sets up system hardware */
static void prvSetupHardware(void)
{
	SystemCoreClockUpdate();
	Board_Init();

	/* LED0 is used for the link status, on = PHY cable detected */
	/* Initial LED state is off to show an unconnected cable state */
	Board_LED_Set(0, false);
}

/* Callback for TCPIP thread to indicate TCPIP init is done */
static void tcpip_init_done_signal(void *arg)
{
	/* Tell main thread TCP/IP init is done */
	*(s32_t *) arg = 1;
}

/* LWIP kickoff and PHY link monitor thread */
static void vSetupIFTask(void *pvParameters) {
	ip_addr_t ipaddr, netmask, gw;
	volatile s32_t tcpipdone = 0;
	uint32_t physts;
	static int prt_ip = 0;

	DEBUGSTR("LWIP HTTP Web Server FreeRTOS Demo...\r\n");

	/* Wait until the TCP/IP thread is finished before
	   continuing or wierd things may happen */
	DEBUGSTR("Waiting for TCPIP thread to initialize...\r\n");
	tcpip_init(tcpip_init_done_signal, (void *) &tcpipdone);
	while (!tcpipdone) {
		msDelay(1);
	}

	DEBUGSTR("Starting LWIP HTTP server...\r\n");

	/* Static IP assignment */
#if LWIP_DHCP
	IP4_ADDR(&gw, 0, 0, 0, 0);
	IP4_ADDR(&ipaddr, 0, 0, 0, 0);
	IP4_ADDR(&netmask, 0, 0, 0, 0);
#else
	IP4_ADDR(&gw, 192, 168, 0, 1);
	IP4_ADDR(&ipaddr, 192, 168, 0, 3);
	IP4_ADDR(&netmask, 255, 255, 255, 0);
#endif
	//printf(&ipaddr);

	/* Add netif interface for lpc17xx_8x */
	memset(&lpc_netif, 0, sizeof(lpc_netif));
	if (!netif_add(&lpc_netif, &ipaddr, &netmask, &gw, NULL, lpc_enetif_init,
			tcpip_input)) {
		DEBUGSTR("Net interface failed to initialize\r\n");
		while(1);			   
	}
	netif_set_default(&lpc_netif);
	netif_set_up(&lpc_netif);

	/* Enable MAC interrupts only after LWIP is ready */
	NVIC_SetPriority(ETHERNET_IRQn, config_ETHERNET_INTERRUPT_PRIORITY);
	NVIC_EnableIRQ(ETHERNET_IRQn);

#if LWIP_DHCP
	dhcp_start(&lpc_netif);
#endif

	/* Initialize and start application */
	http_server_netconn_init();

	/* Start server task*/
	printf("checkpoint before server session...\n");
	sys_thread_new("server_netconn", server_thread, NULL, DEFAULT_THREAD_STACKSIZE + 128, DEFAULT_THREAD_PRIO);
	//sys_thread_new("client_netconn", client, NULL, DEFAULT_THREAD_STACKSIZE + 128, DEFAULT_THREAD_PRIO);


	/* This loop monitors the PHY link and will handle cable events
	   via the PHY driver. */
	while (1) {
		/* Call the PHY status update state machine once in a while
		   to keep the link status up-to-date */
		physts = lpcPHYStsPoll();

		/* Only check for connection state when the PHY status has changed */
		if (physts & PHY_LINK_CHANGED) {
			if (physts & PHY_LINK_CONNECTED) {
				Board_LED_Set(0, true);
				prt_ip = 0;

				/* Set interface speed and duplex */
				if (physts & PHY_LINK_SPEED100) {
					Chip_ENET_Set100Mbps(LPC_ETHERNET);
					NETIF_INIT_SNMP(&lpc_netif, snmp_ifType_ethernet_csmacd, 100000000);
				}
				else {
					Chip_ENET_Set10Mbps(LPC_ETHERNET);
					NETIF_INIT_SNMP(&lpc_netif, snmp_ifType_ethernet_csmacd, 10000000);
				}
				if (physts & PHY_LINK_FULLDUPLX) {
					Chip_ENET_SetFullDuplex(LPC_ETHERNET);
				}
				else {
					Chip_ENET_SetHalfDuplex(LPC_ETHERNET);
				}

				tcpip_callback_with_block((tcpip_callback_fn) netif_set_link_up,
						(void *) &lpc_netif, 1);
			}
			else {
				Board_LED_Set(0, false);
				tcpip_callback_with_block((tcpip_callback_fn) netif_set_link_down,
						(void *) &lpc_netif, 1);
			}

			/* Delay for link detection (250mS) */
			vTaskDelay(configTICK_RATE_HZ / 4);
		}

		/* Print IP address info */
		if (!prt_ip) {
			if (lpc_netif.ip_addr.addr) {
				static char tmp_buff[16];
				DEBUGOUT("IP_ADDR    : %s\r\n", ipaddr_ntoa_r((const ip_addr_t *) &lpc_netif.ip_addr, tmp_buff, 16));
				DEBUGOUT("NET_MASK   : %s\r\n", ipaddr_ntoa_r((const ip_addr_t *) &lpc_netif.netmask, tmp_buff, 16));
				DEBUGOUT("GATEWAY_IP : %s\r\n", ipaddr_ntoa_r((const ip_addr_t *) &lpc_netif.gw, tmp_buff, 16));
				prt_ip = 1;
			}
		}
	}
}


static void server_thread(void *arg){
	printf("Starting server_thread ...\n");

	struct netconn *conn, *newconn;
	struct netbuf *inbuf;
	err_t err, err1;
	LWIP_UNUSED_ARG(arg);

	/* Create a new TCP connection handle */
	conn = netconn_new(NETCONN_TCP);
	LWIP_ERROR("http_server: invalid conn", (conn != NULL), return;);

	/* Bind to port 6001 with default IP address */
	netconn_bind(conn, NULL, 6001);

	/* Put the connection into LISTEN state */
	netconn_listen(conn);
	char data_buffer[80];
	strcpy(data_buffer,"Hello World\n");
	err = netconn_accept(conn, &newconn);
	u16_t buflen;
	char *buf;
	if (err == ERR_OK){
		netconn_write_partly(newconn, "Hello! You have connected to LPC1769! \n", sizeof("Hello! You have connected to LPC1769! \n"), NETCONN_COPY, 0);
		netconn_write_partly(newconn, "Please type in message: \n", sizeof("Please type in message: \n"), NETCONN_COPY, 0);
	}

//	char str[80];
	do {

		if (err == ERR_OK) {
			//	      http_server_netconn_serve(newconn);
//			gets(str);
//			netconn_write_partly(newconn, str, 10, NETCONN_COPY, 0);
			//err1 = netconn_recv(newconn, inbuf);
			err1 = netconn_recv(newconn, &inbuf);
			netbuf_data(inbuf, (void**)&buf, &buflen);
//			printf(*buf);
//			printf(buf);
			netconn_write_partly(newconn, "You typed: ", sizeof("You typed: "), NETCONN_COPY, 0);
			netconn_write_partly(newconn, buf, buflen, NETCONN_COPY, 0);
			//netconn_delete(newconn);
		}
	} while(err == ERR_OK);
	LWIP_DEBUGF(HTTPD_DEBUG,
			("http_server_netconn_thread: netconn_accept received error %d, shutting down",
					err));
	netconn_close(conn);
	netconn_delete(conn);




//	while(1){
//		scanf("%s", str);
//		send(accept_fd, str,sizeof(str),0);
//	}
//
//	if(sent_data < 0 )
//	{
//
//		printf("send failed\n");
//		close(socket_fd);
//		exit(3);
//	}
//	printf("Send Completed...\n");
//
//	close(socket_fd);
}

void client(){
	int socket_fd;
	struct sockaddr_in sa,ra;

	int recv_data; char data_buffer[80]; /* Creates an TCP socket (SOCK_STREAM) with Internet Protocol Family (PF_INET).
	 * Protocol family and Address family related. For example PF_INET Protocol Family and AF_INET family are coupled.
	 */

	socket_fd = socket(PF_INET, SOCK_STREAM, 0);

	if ( socket_fd < 0 )
	{

		printf("socket call failed");
		exit(0);
	}

	memset(&sa, 0, sizeof(struct sockaddr_in));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = inet_addr(SENDER_IP_ADDR);
	sa.sin_port = htons(SENDER_PORT_NUM);


	/* Bind the TCP socket to the port SENDER_PORT_NUM and to the current
	 * machines IP address (Its defined by SENDER_IP_ADDR).
	 * Once bind is successful for UDP sockets application can operate
	 * on the socket descriptor for sending or receiving data.
	 */
	if (bind(socket_fd, (struct sockaddr *)&sa, sizeof(struct sockaddr_in)) == -1)
	{
		printf("Bind to Port Number %d ,IP address %s failed\n",SENDER_PORT_NUM,SENDER_IP_ADDR);
		close(socket_fd);
		exit(1);
	}
	/* Receiver connects to server ip-address. */

	memset(&ra, 0, sizeof(struct sockaddr_in));
	ra.sin_family = AF_INET;
	ra.sin_addr.s_addr = inet_addr(SERVER_IP_ADDR);
	ra.sin_port = htons(SERVER_PORT_NUM);


	if(connect(socket_fd,(struct sockaddr_in*)&ra,sizeof(struct sockaddr_in)) < 0)
	{

		printf("connect failed \n");
		close(socket_fd);
		exit(2);
	}
	recv_data = recv(socket_fd,data_buffer,sizeof(data_buffer),0);
	if(recv_data < 0)
	{

		printf("recv failed \n");
		close(socket_fd);
		exit(2);
	}
	data_buffer[recv_data] = '\0';
	printf("received data: %s\n",data_buffer);

	close(socket_fd);
}

/*****************************************************************************
 * Public functions
 ****************************************************************************/

/**
 * @brief	MilliSecond delay function based on FreeRTOS
 * @param	ms	: Number of milliSeconds to delay
 * @return	Nothing
 * Needed for some functions, do not use prior to FreeRTOS running
 */
void msDelay(uint32_t ms)
{
	vTaskDelay((configTICK_RATE_HZ * ms) / 1000);
}

/**
 * @brief	main routine for example_lwip_tcpecho_freertos_17xx40xx
 * @return	Function should not exit
 */
int main(void)
{
	prvSetupHardware();

	/* Add another thread for initializing physical interface. This
	   is delayed from the main LWIP initialization. */
	xTaskCreate(vSetupIFTask, (signed char *) "SetupIFx",
			configMINIMAL_STACK_SIZE, NULL, (tskIDLE_PRIORITY + 1UL),
			(xTaskHandle *) NULL);


	/* Start the scheduler */
	vTaskStartScheduler();
	/* Should never arrive here */
	return 1;
}

/**
 * @}
 */
