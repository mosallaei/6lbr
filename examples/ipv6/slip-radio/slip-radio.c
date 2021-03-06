/*
 * Copyright (c) 2011, Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/**
 * \file
 *         Slip-radio driver
 * \author
 *         Niclas Finne <nfi@sics.se>
 *         Joakim Eriksson <joakime@sics.se>
 */
#include "contiki.h"
#include "net/ip/uip.h"
#include "net/ipv6/uip-ds6.h"
#include "dev/slip.h"
#include <string.h>
#include "net/netstack.h"
#include "net/packetbuf.h"
#include "dev/watchdog.h"

#define DEBUG DEBUG_NONE
#include "net/ip/uip-debug.h"
#include "cmd.h"
#include "slip-radio.h"
#include "packetutils.h"
#include "no-framer.h"

#if WITH_TSCH
#include "net/mac/tsch/tsch.h"
#include "net/mac/tsch/tsch-private.h"
#include "net/mac/tsch/tsch-schedule.h"
#include "net/mac/tsch/tsch-log.h"
#include "tsch-rpl.h"
#endif

#ifdef SLIP_RADIO_CONF_SENSORS
extern const struct slip_radio_sensors SLIP_RADIO_CONF_SENSORS;
#endif

void slip_send_packet(const uint8_t *ptr, int len);

 /* max 16 packets at the same time??? */
uint8_t packet_ids[16];
int packet_pos;

static int slip_radio_cmd_handler(const uint8_t *data, int len);

#if RADIO_DEVICE_cc2420
int cmd_handler_cc2420(const uint8_t *data, int len);
#elif CONTIKI_TARGET_NOOLIBERRY
int cmd_handler_rf230(const uint8_t *data, int len);
#elif CONTIKI_TARGET_ECONOTAG
int cmd_handler_mc1322x(const uint8_t *data, int len);
#elif CONTIKI_TARGET_COOJA
int cmd_handler_cooja(const uint8_t *data, int len);
#else /* Leave CC2420 as default */
int cmd_handler_cc2420(const uint8_t *data, int len);
#endif /* CONTIKI_TARGET */

#define SLIP_END     0300

char slip_debug_frame = 0;

/*---------------------------------------------------------------------------*/
#ifdef CMD_CONF_HANDLERS
CMD_HANDLERS(CMD_CONF_HANDLERS);
#else
CMD_HANDLERS(slip_radio_cmd_handler);
#endif
/*---------------------------------------------------------------------------*/
static void
packet_sent(void *ptr, int status, int transmissions)
{
  uint8_t buf[20];
  uint8_t sid;
  int pos;
  sid = *((uint8_t *)ptr);
  PRINTF("Slip-radio: packet sent! sid: %d, status: %d, tx: %d\n",
  	 sid, status, transmissions);
  /* packet callback from lower layers */
  /*  neighbor_info_packet_sent(status, transmissions); */
  pos = 0;
  buf[pos++] = '!';
  buf[pos++] = 'R';
  buf[pos++] = sid;
  buf[pos++] = status; /* one byte ? */
  buf[pos++] = transmissions;
  cmd_send(buf, pos);
}
/*---------------------------------------------------------------------------*/
static int
slip_radio_cmd_handler(const uint8_t *data, int len)
{
#if SLIP_RADIO_IP
  linkaddr_t dest;
#endif
  int i;
  if(data[0] == '!') {
    /* should send out stuff to the radio - ignore it as IP */
    /* --- s e n d --- */
    if(data[1] == 'S') {
#if SLIP_RADIO_IP
      uint8_t sid = data[2];
      memcpy(&dest, &data[3], sizeof(uip_lladdr_t));
      memmove(&uip_buf[UIP_LLH_LEN], &data[3 + sizeof(uip_lladdr_t)], len - 3 - sizeof(uip_lladdr_t));
      uip_len = len - 3 - sizeof(uip_lladdr_t);
      if(linkaddr_cmp(&dest, &linkaddr_null)) {
        tcpip_output(NULL);
      } else {
        tcpip_output((uip_lladdr_t *)&dest);
      }
      packet_sent((void *)&sid, 0, 1);
#else /* SLIP_RADIO_IP */
      int pos = 0;
      packet_ids[packet_pos] = data[2];

      packetbuf_clear();
#if DESERIALIZE_ATTRIBUTES
      pos = packetutils_deserialize_atts(&data[3], len - 3);
      if(pos < 0) {
        PRINTF("slip-radio: illegal packet attributes\n");
        return 1;
      }
#endif
      pos += 3;
      len -= pos;
      if(len > PACKETBUF_SIZE) {
        len = PACKETBUF_SIZE;
      }
      memcpy(packetbuf_dataptr(), &data[pos], len);
      packetbuf_set_datalen(len);

      PRINTF("slip-radio: sending %u (%d bytes)\n",
             data[2], packetbuf_datalen());

      /* parse frame before sending to get addresses, etc. */
      no_framer.parse();
      NETSTACK_LLSEC.send(packet_sent, &packet_ids[packet_pos]);

      packet_pos++;
      if(packet_pos >= sizeof(packet_ids)) {
	packet_pos = 0;
      }
#endif /* SLIP_RADIO_IP */
      return 1;
    } else if(data[1] == 'R' && len == 2) {
#if !CONTIKI_TARGET_CC2538DK
      PRINTF("Rebooting\n");
      watchdog_reboot();
#endif
      return 1;
#if !(RADIO_DEVICE_cc2420 || CONTIKI_TARGET_SKY || CONTIKI_TARGET_Z1 || CONTIKI_TARGET_NOOLIBERRY || CONTIKI_TARGET_ECONOTAG || CONTIKI_TARGET_COOJA)
    } else if(data[1] == 'P' && len == 4) {
      uint16_t pan_id = data[2] + (data[3] << 8);
      PRINTF("CMD: setting pan-id: %x\n", pan_id);
      frame802154_set_pan_id(pan_id);
      no_framer_set_pan_id(pan_id);
      NETSTACK_RADIO.set_value(RADIO_PARAM_PAN_ID, pan_id);
      return 1;
    } else if(data[1] == 'C' && len == 3) {
      uint8_t channel = data[2];
      PRINTF("CMD: setting channel: %d\n", channel);
      NETSTACK_RADIO.set_value(RADIO_PARAM_CHANNEL, channel);
      return 1;
    } else if(data[1] == 'M' && len == 10) {
      printf("CMD: Set MAC\n");
      memcpy(uip_lladdr.addr, data+2, sizeof(uip_lladdr.addr));
      linkaddr_set_node_addr((linkaddr_t *) uip_lladdr.addr);
      NETSTACK_RADIO.set_object(RADIO_PARAM_64BIT_ADDR, data+2, 8);
      return 1;
#endif
    }
  } else if(uip_buf[0] == '?') {
    PRINTF("Got request message of type %c\n", uip_buf[1]);
    if(data[1] == 'M' && len == 2) {
      /* this is just a test so far... just to see if it works */
      uip_buf[0] = '!';
      uip_buf[1] = 'M';
      for(i = 0; i < 8; i++) {
        uip_buf[2 + i] = uip_lladdr.addr[i];
      }
      uip_len = 10;
      cmd_send(uip_buf, uip_len);
      return 1;
#if !(RADIO_DEVICE_cc2420 || CONTIKI_TARGET_SKY || CONTIKI_TARGET_Z1 || CONTIKI_TARGET_NOOLIBERRY || CONTIKI_TARGET_ECONOTAG || CONTIKI_TARGET_COOJA)
    } else if(data[1] == 'P' && len == 2) {
      uint16_t pan_id = no_framer_get_pan_id();
      uip_buf[0] = '!'; uip_buf[1] = 'P';
      uip_buf[2] = pan_id & 0xFF;
      uip_buf[3] = (pan_id >> 8) & 0xFF;
      uip_len = 4;
      cmd_send(uip_buf, uip_len);
      return 1;
    } else if(data[1] == 'C' && len == 2) {
      radio_value_t value = 0;
      NETSTACK_RADIO.get_value(RADIO_PARAM_CHANNEL, &value);
      uip_buf[0] = '!';
      uip_buf[1] = 'C';
      uip_buf[2] = value;
      uip_len = 3;
      cmd_send(uip_buf, uip_len);
      return 1;
#endif
    }
  }
  return 0;
}
/*---------------------------------------------------------------------------*/
void
slip_radio_cmd_output(const uint8_t *data, int data_len)
{
#if !SLIP_RADIO_CONF_NO_PUTCHAR
  if(slip_debug_frame) {
    slip_arch_writeb(SLIP_END);
    slip_debug_frame = 0;
  }
#endif

  slip_send_packet(data, data_len);
}
/*---------------------------------------------------------------------------*/
static void
slip_input_callback(void)
{
  PRINTF("SR-SIN: %u '%c%c'\n", uip_len, uip_buf[0], uip_buf[1]);
  cmd_input(uip_buf, uip_len);
  uip_clear_buf();
}
/*---------------------------------------------------------------------------*/
#if SLIP_RADIO_IP
static void
slip_output()
{
  if(uip_len > 0) {
    memmove(&uip_buf[UIP_LLH_LEN + sizeof(uip_lladdr_t) * 2], &uip_buf[UIP_LLH_LEN], uip_len);
    memcpy(&uip_buf[UIP_LLH_LEN], packetbuf_addr(PACKETBUF_ADDR_SENDER), sizeof(uip_lladdr_t));
    memcpy(&uip_buf[UIP_LLH_LEN + sizeof(uip_lladdr_t)], packetbuf_addr(PACKETBUF_ADDR_RECEIVER), sizeof(uip_lladdr_t));
    slip_send_packet(&uip_buf[UIP_LLH_LEN], uip_len + sizeof(uip_lladdr_t) * 2);
  }
}
#endif
/*---------------------------------------------------------------------------*/
static void
init(void)
{
#ifndef BAUD2UBR
#define BAUD2UBR(baud) baud
#endif
  slip_arch_init(BAUD2UBR(115200));
  process_start(&slip_process, NULL);
  slip_set_input_callback(slip_input_callback);
  packet_pos = 0;
#if SLIP_RADIO_IP
  tcpip_set_inputfunc(slip_output);
#endif
}
/*---------------------------------------------------------------------------*/
#if !SLIP_RADIO_CONF_NO_PUTCHAR
#undef putchar
int
putchar(int c)
{
  if(!slip_debug_frame) {            /* Start of debug output */
    slip_arch_writeb(SLIP_END);
    slip_arch_writeb('\r');     /* Type debug line == '\r' */
    slip_debug_frame = 1;
  }

  /* Need to also print '\n' because for example COOJA will not show
     any output before line end */
  slip_arch_writeb((char)c);

  /*
   * Line buffered output, a newline marks the end of debug output and
   * implicitly flushes debug output.
   */
  if(c == '\n') {
    slip_arch_writeb(SLIP_END);
    slip_debug_frame = 0;
  }
  return c;
}
#endif
/*---------------------------------------------------------------------------*/
PROCESS(slip_radio_process, "Slip radio process");
AUTOSTART_PROCESSES(&slip_radio_process);
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(slip_radio_process, ev, data)
{
  static struct etimer et;
  PROCESS_BEGIN();

  init();
  NETSTACK_RDC.off(1);
#ifdef SLIP_RADIO_CONF_SENSORS
  SLIP_RADIO_CONF_SENSORS.init();
#endif
  printf("Slip Radio started...\n");

#if WITH_TSCH
  tsch_set_coordinator(1);
  tsch_set_eb_period(TSCH_EB_PERIOD);
  tsch_set_join_priority(0);
  NETSTACK_MAC.on();
#endif

  etimer_set(&et, CLOCK_SECOND * 3);

  while(1) {
    PROCESS_YIELD();

    if(etimer_expired(&et)) {
      etimer_reset(&et);
#ifdef SLIP_RADIO_CONF_SENSORS
      SLIP_RADIO_CONF_SENSORS.send();
#endif
    }
  }
  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
