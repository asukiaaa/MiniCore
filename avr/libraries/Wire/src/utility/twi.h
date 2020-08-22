/*
  twi.h - TWI/I2C library for Wiring & Arduino
  Copyright (c) 2006 Nicholas Zambetti.  All right reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef twi_h
#define twi_h

  #include <inttypes.h>

  #ifndef TWI_FREQ
  #define TWI_FREQ 100000L
  #endif

  #ifndef TWI_BUFFER_SIZE
  #define TWI_BUFFER_SIZE 32
  #endif

  #define TWI_READY 0
  #define TWI_MRX   1
  #define TWI_MTX   2
  #define TWI_SRX   3
  #define TWI_STX   4

class Twi {
 public:
  Twi(int bufferSize);
  ~Twi();
  void begin();
  void disable();
  void setAddress(uint8_t address);
  void setFrequency(uint32_t frequency);
  uint8_t readFrom(uint8_t, uint8_t*, uint8_t, uint8_t);
  uint8_t writeTo(uint8_t, uint8_t*, uint8_t, uint8_t, uint8_t);
  uint8_t ransmit(const uint8_t*, uint8_t);
  void attachSlaveRxEvent( void (*)(uint8_t*, int) );
  void attachSlaveTxEvent( void (*)(void) );
  void reply(uint8_t);
  void stop(void);
  void releaseBus(void);
  uint8_t transmit(const uint8_t* data, uint8_t length);
  uint8_t state;
  uint8_t slarw;
  uint8_t sendStop; // should the transaction end with a stop
  uint8_t inRepStart; // in the middle of a repeated start
  uint8_t *masterBuffer;
  uint8_t masterBufferIndex;
  uint8_t masterBufferLength;
  uint8_t *txBuffer;
  uint8_t txBufferIndex;
  uint8_t txBufferLength;
  uint8_t *rxBuffer;
  uint8_t rxBufferIndex;
  uint8_t error;
  int bufferSize;
};

extern void (*twi0_onSlaveTransmit)(void);
extern void (*twi0_onSlaveReceive)(uint8_t*, int);

extern Twi twi0;

#endif

