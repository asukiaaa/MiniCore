/*
  twi.c - TWI/I2C library for Wiring & Arduino
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

  Modified 2012 by Todd Krein (todd@krein.org) to implement repeated starts
*/

#include <math.h>
#include <stdlib.h>
#include <inttypes.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <compat/twi.h>
#include "Arduino.h" // for digitalWrite

#ifndef cbi
#define cbi(sfr, bit) (_SFR_BYTE(sfr) &= ~_BV(bit))
#endif

#ifndef sbi
#define sbi(sfr, bit) (_SFR_BYTE(sfr) |= _BV(bit))
#endif

#include "pins_arduino.h"
#include "twi.h"

Twi::Twi(int bufferSize,
         volatile uint8_t* twar,
         volatile uint8_t* twbr,
         volatile uint8_t* twcr,
         volatile uint8_t* twdr,
         volatile uint8_t* twsr)
{
  this->bufferSize = bufferSize;
  masterBuffer = new uint8_t[bufferSize];
  txBuffer = new uint8_t[bufferSize];
  rxBuffer = new uint8_t[bufferSize];

  this->twar = twar;
  this->twbr = twbr;
  this->twcr = twcr;
  this->twdr = twdr;
  this->twsr = twsr;

  // initialize state
  state = TWI_READY;
  sendStop = true;  // default value
  inRepStart = false;
}

Twi::~Twi()
{
  delete masterBuffer;
  delete txBuffer;
  delete rxBuffer;
}

void Twi::begin()
{
  // activate internal pullups for twi.
  digitalWrite(SDA, 1);
  digitalWrite(SCL, 1);

  // initialize twi prescaler and bit rate
  cbi(*twsr, TWPS0);
  cbi(*twsr, TWPS1);
  *twbr = ((F_CPU / TWI_FREQ) - 16) / 2;

  /* twi bit rate formula from atmega128 manual pg 204
  SCL Frequency = CPU Clock Frequency / (16 + (2 * TWBR))
  note: TWBR should be 10 or higher for master mode
  It is 72 for a 16mhz Wiring board with 100kHz TWI */

  // enable twi module, acks, and twi interrupt
  *twcr = _BV(TWEN) | _BV(TWIE) | _BV(TWEA);
}

/* 
 * Function twi_disable
 * Desc     disables twi pins
 * Input    none
 * Output   none
 */
void Twi::disable(void)
{
  // disable twi module, acks, and twi interrupt
  *twcr &= ~(_BV(TWEN) | _BV(TWIE) | _BV(TWEA));

  // deactivate internal pullups for twi.
  digitalWrite(SDA, 0);
  digitalWrite(SCL, 0);
}

/* 
 * Function twi_slaveInit
 * Desc     sets slave address and enables interrupt
 * Input    none
 * Output   none
 */
void Twi::setAddress(uint8_t address)
{
  // set twi slave address (skip over TWGCE bit)
  *twar = address << 1;
}

/*
 * Function twi_setFrequency
 * Desc     sets twi bit rate
 * Input    Clock frequency
 * Output   none
 */
void Twi::setFrequency(uint32_t frequency)
{
  *twbr = ((F_CPU / frequency) - 16) / 2;

  /* twi bit rate formula from atmega128 manual pg 204
  SCL Frequency = CPU Clock Frequency / (16 + (2 * TWBR))
  note: TWBR should be 10 or higher for master mode
  It is 72 for a 16mhz Wiring board with 100kHz TWI */
}

/* 
 * Function twi_readFrom
 * Desc     attempts to become twi bus master and read a
 *          series of bytes from a device on the bus
 * Input    address: 7bit i2c device address
 *          data: pointer to byte array
 *          length: number of bytes to read into array
 *          sendStop: Boolean indicating whether to send a stop at the end
 * Output   number of bytes read
 */
uint8_t Twi::readFrom(uint8_t address, uint8_t* data, uint8_t length, uint8_t sendStop) {
  uint8_t i;

  // ensure data will fit into buffer
  if(bufferSize < length){
    return 0;
  }

  // wait until twi is ready, become master receiver
  while(TWI_READY != state){
    continue;
  }
  state = TWI_MRX;
  sendStop = sendStop;
  // reset error state (0xFF.. no error occured)
  error = 0xFF;

  // initialize buffer iteration vars
  masterBufferIndex = 0;
  masterBufferLength = length-1;  // This is not intuitive, read on...
  // On receive, the previously configured ACK/NACK setting is transmitted in
  // response to the received byte before the interrupt is signalled. 
  // Therefor we must actually set NACK when the _next_ to last byte is
  // received, causing that NACK to be sent in response to receiving the last
  // expected byte of data.

  // build sla+w, slave device address + w bit
  slarw = TW_READ;
  slarw |= address << 1;

  if (true == inRepStart) {
    // if we're in the repeated start state, then we've already sent the start,
    // (@@@ we hope), and the TWI statemachine is just waiting for the address byte.
    // We need to remove ourselves from the repeated start state before we enable interrupts,
    // since the ISR is ASYNC, and we could get confused if we hit the ISR before cleaning
    // up. Also, don't enable the START interrupt. There may be one pending from the 
    // repeated start that we sent ourselves, and that would really confuse things.
    inRepStart = false; // remember, we're dealing with an ASYNC ISR
    do {
      *twdr = slarw;
    } while(*twcr & _BV(TWWC));
    *twcr = _BV(TWINT) | _BV(TWEA) | _BV(TWEN) | _BV(TWIE);  // enable INTs, but not START
  }
  else
    // send start condition
    *twcr = _BV(TWEN) | _BV(TWIE) | _BV(TWEA) | _BV(TWINT) | _BV(TWSTA);

  // wait for read operation to complete
  while(TWI_MRX == state){
    continue;
  }

  if (masterBufferIndex < length)
    length = masterBufferIndex;

  // copy twi buffer to data
  for(i = 0; i < length; ++i){
    data[i] = masterBuffer[i];
  }

  return length;
}

/* 
 * Function twi_writeTo
 * Desc     attempts to become twi bus master and write a
 *          series of bytes to a device on the bus
 * Input    address: 7bit i2c device address
 *          data: pointer to byte array
 *          length: number of bytes in array
 *          wait: boolean indicating to wait for write or not
 *          sendStop: boolean indicating whether or not to send a stop at the end
 * Output   0 .. success
 *          1 .. length to long for buffer
 *          2 .. address send, NACK received
 *          3 .. data send, NACK received
 *          4 .. other twi error (lost bus arbitration, bus error, ..)
 */
uint8_t Twi::writeTo(uint8_t address, uint8_t* data, uint8_t length, uint8_t wait, uint8_t sendStop)
{
  uint8_t i;

  // ensure data will fit into buffer
  if(bufferSize < length){
    return 1;
  }

  // wait until twi is ready, become master transmitter
  while(TWI_READY != state){
    continue;
  }
  state = TWI_MTX;
  sendStop = sendStop;
  // reset error state (0xFF.. no error occured)
  error = 0xFF;

  // initialize buffer iteration vars
  masterBufferIndex = 0;
  masterBufferLength = length;
  
  // copy data to twi buffer
  for(i = 0; i < length; ++i){
    masterBuffer[i] = data[i];
  }
  
  // build sla+w, slave device address + w bit
  slarw = TW_WRITE;
  slarw |= address << 1;
  
  // if we're in a repeated start, then we've already sent the START
  // in the ISR. Don't do it again.
  //
  if (true == inRepStart) {
    // if we're in the repeated start state, then we've already sent the start,
    // (@@@ we hope), and the TWI statemachine is just waiting for the address byte.
    // We need to remove ourselves from the repeated start state before we enable interrupts,
    // since the ISR is ASYNC, and we could get confused if we hit the ISR before cleaning
    // up. Also, don't enable the START interrupt. There may be one pending from the 
    // repeated start that we sent outselves, and that would really confuse things.
    inRepStart = false; // remember, we're dealing with an ASYNC ISR
    do {
      *twdr = slarw;
    } while(*twcr & _BV(TWWC));
    *twcr = _BV(TWINT) | _BV(TWEA) | _BV(TWEN) | _BV(TWIE);  // enable INTs, but not START
  }
  else
    // send start condition
    *twcr = _BV(TWINT) | _BV(TWEA) | _BV(TWEN) | _BV(TWIE) | _BV(TWSTA); // enable INTs

  // wait for write operation to complete
  while(wait && (TWI_MTX == state)){
    continue;
  }
  
  if (error == 0xFF)
    return 0; // success
  else if (error == TW_MT_SLA_NACK)
    return 2; // error: address send, nack received
  else if (error == TW_MT_DATA_NACK)
    return 3; // error: data send, nack received
  else
    return 4; // other twi error
}

/* 
 * Function twi_transmit
 * Desc     fills slave tx buffer with data
 *          must be called in slave tx event callback
 * Input    data: pointer to byte array
 *          length: number of bytes in array
 * Output   1 length too long for buffer
 *          2 not slave transmitter
 *          0 ok
 */
uint8_t Twi::transmit(const uint8_t* data, uint8_t length)
{
  uint8_t i;

  // ensure data will fit into buffer
  if(bufferSize < (txBufferLength+length)){
    return 1;
  }
  
  // ensure we are currently a slave transmitter
  if(TWI_STX != state){
    return 2;
  }
  
  // set length and copy data into tx buffer
  for(i = 0; i < length; ++i){
    txBuffer[txBufferLength+i] = data[i];
  }
  txBufferLength += length;

  return 0;
}

/* 
 * Function twi_reply
 * Desc     sends byte or readys receive line
 * Input    ack: byte indicating to ack or to nack
 * Output   none
 */
void Twi::reply(uint8_t ack)
{
  // transmit master read ready signal, with or without ack
  if(ack){
    *twcr = _BV(TWEN) | _BV(TWIE) | _BV(TWINT) | _BV(TWEA);
  }else{
    *twcr = _BV(TWEN) | _BV(TWIE) | _BV(TWINT);
  }
}

/* 
 * Function twi_stop
 * Desc     relinquishes bus master status
 * Input    none
 * Output   none
 */
void Twi::stop(void)
{
  // send stop condition
  *twcr = _BV(TWEN) | _BV(TWIE) | _BV(TWEA) | _BV(TWINT) | _BV(TWSTO);

  // wait for stop condition to be exectued on bus
  // TWINT is not set after a stop condition!
  while(*twcr & _BV(TWSTO)){
    continue;
  }

  // update twi state
  state = TWI_READY;
}

/* 
 * Function twi_releaseBus
 * Desc     releases bus control
 * Input    none
 * Output   none
 */
void Twi::releaseBus(void)
{
  // release bus
  *twcr = _BV(TWEN) | _BV(TWIE) | _BV(TWEA) | _BV(TWINT);

  // update twi state
  state = TWI_READY;
}

void Twi::onInterrupt() {
  switch (TW_STATUS) {
    // All Master
  case TW_START:     // sent start condition
  case TW_REP_START: // sent repeated start condition
    // copy device address and r/w bit to output register and ack
    *twdr = slarw;
    reply(1);
    break;

    // Master Transmitter
  case TW_MT_SLA_ACK:  // slave receiver acked address
  case TW_MT_DATA_ACK: // slave receiver acked data
    // if there is data to send, send it, otherwise stop 
    if(masterBufferIndex < masterBufferLength){
      // copy data to output register and ack
      *twdr = masterBuffer[masterBufferIndex++];
      reply(1);
    } else {
      if (sendStop)
        stop();
      else {
        inRepStart = true;  // we're gonna send the START
        // don't enable the interrupt. We'll generate the start, but we 
        // avoid handling the interrupt until we're in the next transaction,
        // at the point where we would normally issue the start.
        *twcr = _BV(TWINT) | _BV(TWSTA)| _BV(TWEN) ;
        state = TWI_READY;
      }
    }
    break;
  case TW_MT_SLA_NACK:  // address sent, nack received
    error = TW_MT_SLA_NACK;
    stop();
    break;
  case TW_MT_DATA_NACK: // data sent, nack received
    error = TW_MT_DATA_NACK;
    stop();
    break;
  case TW_MT_ARB_LOST: // lost bus arbitration
    error = TW_MT_ARB_LOST;
    releaseBus();
    break;

    // Master Receiver
  case TW_MR_DATA_ACK: // data received, ack sent
    // put byte into buffer
    masterBuffer[masterBufferIndex++] = *twdr;
    /* fall through */
  case TW_MR_SLA_ACK:  // address sent, ack received
    // ack if more bytes are expected, otherwise nack
    if(masterBufferIndex < masterBufferLength){
      reply(1);
    } else {
      reply(0);
    }
    break;
  case TW_MR_DATA_NACK: // data received, nack sent
    // put final byte into buffer
    masterBuffer[masterBufferIndex++] = *twdr;
    if (sendStop)
      stop();
    else {
      inRepStart = true;  // we're gonna send the START
      // don't enable the interrupt. We'll generate the start, but we 
      // avoid handling the interrupt until we're in the next transaction,
      // at the point where we would normally issue the start.
      *twcr = _BV(TWINT) | _BV(TWSTA)| _BV(TWEN) ;
      state = TWI_READY;
    }    
  break;
    case TW_MR_SLA_NACK: // address sent, nack received
      stop();
      break;
    // TW_MR_ARB_LOST handled by TW_MT_ARB_LOST case

    // Slave Receiver
    case TW_SR_SLA_ACK:   // addressed, returned ack
    case TW_SR_GCALL_ACK: // addressed generally, returned ack
    case TW_SR_ARB_LOST_SLA_ACK:   // lost arbitration, returned ack
    case TW_SR_ARB_LOST_GCALL_ACK: // lost arbitration, returned ack
      // enter slave receiver mode
      state = TWI_SRX;
      // indicate that rx buffer can be overwritten and ack
      rxBufferIndex = 0;
      reply(1);
      break;
    case TW_SR_DATA_ACK:       // data received, returned ack
    case TW_SR_GCALL_DATA_ACK: // data received generally, returned ack
      // if there is still room in the rx buffer
      if(rxBufferIndex < bufferSize){
        // put byte in buffer and ack
        rxBuffer[rxBufferIndex++] = *twdr;
        reply(1);
      }else{
        // otherwise nack
        reply(0);
      }
      break;
    case TW_SR_STOP: // stop or repeated start condition received
      // ack future responses and leave slave receiver state
      releaseBus();
      // put a null char after data if there's room
      if(rxBufferIndex < bufferSize){
        rxBuffer[rxBufferIndex] = '\0';
      }
      // callback to user defined callback
      twi0_onSlaveReceive(rxBuffer, rxBufferIndex);
      // since we submit rx buffer to "wire" library, we can reset it
      rxBufferIndex = 0;
      break;
    case TW_SR_DATA_NACK:       // data received, returned nack
    case TW_SR_GCALL_DATA_NACK: // data received generally, returned nack
      // nack back at master
      reply(0);
      break;
    
    // Slave Transmitter
    case TW_ST_SLA_ACK:          // addressed, returned ack
    case TW_ST_ARB_LOST_SLA_ACK: // arbitration lost, returned ack
      // enter slave transmitter mode
      state = TWI_STX;
      // ready the tx buffer index for iteration
      txBufferIndex = 0;
      // set tx buffer length to be zero, to verify if user changes it
      txBufferLength = 0;
      // request for txBuffer to be filled and length to be set
      // note: user must call twi_transmit(bytes, length) to do this
      twi0_onSlaveTransmit();
      // if they didn't change buffer & length, initialize it
      if(0 == txBufferLength){
        txBufferLength = 1;
        txBuffer[0] = 0x00;
      }
      // transmit first byte from buffer, fall
      /* fall through */
    case TW_ST_DATA_ACK: // byte sent, ack returned
      // copy data to output register
      *twdr = txBuffer[txBufferIndex++];
      // if there is more to send, ack, otherwise nack
      if(txBufferIndex < txBufferLength){
        reply(1);
      }else{
        reply(0);
      }
      break;
    case TW_ST_DATA_NACK: // received nack, we are done 
    case TW_ST_LAST_DATA: // received ack, but we are done already!
      // ack future responses
      reply(1);
      // leave slave receiver state
      state = TWI_READY;
      break;

    // All
    case TW_NO_INFO:   // no state information
      break;
    case TW_BUS_ERROR: // bus error, illegal stop/start
      error = TW_BUS_ERROR;
      stop();
      break;
  }
}

Twi twi0(TWI_BUFFER_SIZE,
         &TWAR,
         &TWBR,
         &TWCR,
         &TWDR,
         &TWSR);

ISR(TWI_vect)
{
  twi0.onInterrupt();
}
