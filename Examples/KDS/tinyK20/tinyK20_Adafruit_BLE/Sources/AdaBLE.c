/*
 * AdaBLE.c
 *
 *  Created on: 30.12.2015
 *      Author: Erich Styger
 */

#include "AdaBLE.h"
#include "SM1.h"
#include "WAIT1.h"
#include "BLE_CS.h"
#include "BLE_IRQ.h"
#include "sdep.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "RxBuffer.h"
#include "CLS1.h"
#include "UTIL1.h"

/* IRQ will be high to indicate data from the module */
/* CS is low active */

#define BLUEFRUIT_MODE_COMMAND   1
#define BLUEFRUIT_MODE_DATA      0

#define BLE_MAX_RESPONSE_SIZE  48
#define BLE_MAX_AT_CMD_SIZE    48

/* should be in a class object */
static uint8_t  _mode;
static uint8_t  m_tx_buffer[SDEP_MAX_PACKETSIZE];
static uint8_t  m_tx_count;

#define SPI_CS_ENABLE()   BLE_CS_ClrVal()
#define SPI_CS_DISABLE()  BLE_CS_SetVal()

#define SPI_IGNORED_BYTE          0xFEu /**< SPI default character. Character clocked out in case of an ignored transaction. */
#define SPI_OVERREAD_BYTE         0xFFu /**< SPI over-read character. Character clocked out after an over-read of the transmit buffer. */
#define SPI_DEFAULT_DELAY_US      50

#define delayMicroseconds WAIT1_Waitus

static uint8_t rxDummy; /* dummy byte if we do not need the result. Needed to read from SPI register. */
#define SD1_SPI_WRITE(write)            \
   { \
     while(SM1_SendChar(write)!=ERR_OK) {} \
     while(SM1_RecvChar(&rxDummy)!=ERR_OK) {} \
   }
#define SD1_SPI_WRITE_READ(write, readP) \
   { \
     while(SM1_SendChar(write)!=ERR_OK) {} \
     while(SM1_RecvChar(readP)!=ERR_OK) {} \
   }

static uint8_t spixfer(uint8_t val) {
  uint8_t rd;
  SD1_SPI_WRITE_READ(val, &rd);
  return rd;
}

static void spixferblock(uint8_t *buf, size_t bufSize) {
  while(bufSize>0) {
    *buf = spixfer(*buf);
    buf++;
    bufSize--;
  }
}

static void flush(void) {
  SM1_ClearRxBuf();
  SM1_ClearTxBuf();
}

/******************************************************************************/
/*!
    @brief Simulate "+++" switch mode command
*/
/******************************************************************************/
bool BLE_setMode(uint8_t new_mode) {
  // invalid mode
  if ( !(new_mode == BLUEFRUIT_MODE_COMMAND || new_mode == BLUEFRUIT_MODE_DATA) ) return FALSE;

  // Already in the wanted mode
  if ( _mode == new_mode ) return TRUE;

  // SPI use different SDEP command when in DATA/COMMAND mode.
  // --> does not switch using +++ command
  _mode = new_mode;

  return TRUE;
}

/******************************************************************************/
/*!
    @brief      Perform a single SPI SDEP transaction and is used by getReponse to
                get a full response composed of multiple packets.

    @param[in]  buf
                Memory location where payload is copied to

    @return number of bytes in SDEP payload
*/
/******************************************************************************/
bool BLE_getPacket(sdepMsgResponse_t* p_response)
{
  sdepMsgHeader_t* p_header = &p_response->header;
  bool result=FALSE;
  uint16_t cmd_id;
  #define word(a, b) (((a)<<8)|b)

 // if (m_sck_pin == -1)
 //   SPI.beginTransaction(bluefruitSPI);
  SPI_CS_ENABLE();

  //TimeoutTimer tt(_timeout);

  // Bluefruit may not be ready
  while ( ( (p_header->msg_type = spixfer(0xff)) == SPI_IGNORED_BYTE ) /*&& !tt.expired()*/ )
  {
    // Disable & Re-enable CS with a bit of delay for Bluefruit to ready itself
    SPI_CS_DISABLE();
    delayMicroseconds(SPI_DEFAULT_DELAY_US);
    SPI_CS_ENABLE();
  }


  // Not a loop, just a way to avoid goto with error handling
  do
  {
    //if ( tt.expired() ) break;

    // Look for the header
    while ( p_header->msg_type != SDEP_MSGTYPE_RESPONSE && p_header->msg_type != SDEP_MSGTYPE_ERROR )
    {
      p_header->msg_type = spixfer(0xff);
    }
    memset( (&p_header->msg_type)+1, 0xff, sizeof(sdepMsgHeader_t) - 1);
    spixferblock((&p_header->msg_type)+1, sizeof(sdepMsgHeader_t) - 1);

    // Command is 16-bit at odd address, may have alignment issue with 32-bit chip
    cmd_id = word(p_header->cmd_id_high, p_header->cmd_id_low);

    // Error Message Response
    if ( p_header->msg_type == SDEP_MSGTYPE_ERROR ) break;

    // Invalid command
    if (!(cmd_id == SDEP_CMDTYPE_AT_WRAPPER ||
          cmd_id == SDEP_CMDTYPE_BLE_UARTTX ||
          cmd_id == SDEP_CMDTYPE_BLE_UARTRX) )
    {
      break;
    }

    // Invalid length
    if(p_header->length > SDEP_MAX_PACKETSIZE) break;

    // read payload
    memset(p_response->payload, 0xff, p_header->length);
    spixferblock(p_response->payload, p_header->length);

    result = TRUE;
  } while(0);

  SPI_CS_DISABLE();
  return result;
}

/******************************************************************************/
/*!
    @brief  Try to perform an full AT response transfer from Bluefruit, or execute
            as many SPI transaction as internal FIFO can hold up.

    @note   If verbose is enabled, all the received data will be print to Serial

    @return
      - true  : if succeeded
      - false : if failed
*/
/******************************************************************************/
bool BLE_getResponse(void) {
  // Blocking wait until IRQ is asserted
  while (!BLE_IRQ_GetVal()) {} /* wait until IRQ line goes high */

  // There is data from Bluefruit & enough room in the fifo
  while (BLE_IRQ_GetVal() &&
          RxBuffer_NofFreeElements() >= SDEP_MAX_PACKETSIZE )
  {
    // Get a SDEP packet
    sdepMsgResponse_t msg_response;
    memset(&msg_response, 0, sizeof(sdepMsgResponse_t));

    if ( !BLE_getPacket(&msg_response) ) {
      return FALSE;
    }

    // Write to fifo
    if ( msg_response.header.length > 0)
    {
      RxBuffer_Putn(msg_response.payload, msg_response.header.length);
    }

    // No more packet data
    if ( !msg_response.header.more_data ) break;

    // It takes a bit since all Data received to IRQ to get LOW
    // May need to delay a bit for it to be stable before the next try
    // delayMicroseconds(SPI_DEFAULT_DELAY_US);
  }
  return TRUE;
}

void BLE_simulateSwitchMode(void)
{
  _mode = 1 - _mode;

  char ch = '0' + _mode;
  RxBuffer_Put(ch);
  //m_rx_fifo.write(&ch);
  RxBuffer_Putn("\r\nOK\r\n", 6);
}


/******************************************************************************/
/*!
    @brief  Print API. Either buffer the data internally or send it to bus
            if possible. \r and \n are command terminators and will force the
            packet to be sent to the Bluefruit LE module.

    @param[in]  c
                Character to send
*/
/******************************************************************************/
size_t BLE_write(uint8_t c)
{
  if (_mode == BLUEFRUIT_MODE_DATA)
  {
    BLE_sendPacket(SDEP_CMDTYPE_BLE_UARTTX, &c, 1, 0);
    BLE_getResponse();
    return 1;
  }

  // Following code handle BLUEFRUIT_MODE_COMMAND

  // Final packet due to \r or \n terminator
  if (c == '\r' || c == '\n')
  {
    if (m_tx_count > 0)
    {
      // +++ command to switch mode
      if (RxBuffer_Compare(0, "+++", 3) == 0) {
        BLE_simulateSwitchMode();
      } else {
        BLE_sendPacket(SDEP_CMDTYPE_AT_WRAPPER, m_tx_buffer, m_tx_count, 0);
      }
      m_tx_count = 0;
    }
  }
  // More than max packet buffered --> send with more_data = 1
  else if (m_tx_count == SDEP_MAX_PACKETSIZE)
  {
    BLE_sendPacket(SDEP_CMDTYPE_AT_WRAPPER, m_tx_buffer, m_tx_count, 1);

    m_tx_buffer[0] = c;
    m_tx_count = 1;
  }
  // Not enough data, continue to buffer
  else
  {
    m_tx_buffer[m_tx_count++] = c;
  }
  return 1;
}

bool BLE_sendPacket(uint16_t command, const uint8_t *buf, uint8_t count, uint8_t more_data) {
  sdepMsgCommand_t msgCmd;
  bool result;

  // flush old response before sending the new command
  if (more_data == 0) {
    flush();
  }
  msgCmd.header.msg_type    = SDEP_MSGTYPE_COMMAND;
  msgCmd.header.cmd_id_high = command>>8;
  msgCmd.header.cmd_id_low  = command&0xff;
  msgCmd.header.length      = count;
  msgCmd.header.more_data   = (count == SDEP_MAX_PACKETSIZE) ? more_data : 0;

  /* Copy payload */
  if ( buf != NULL && count > 0) {
    memcpy(msgCmd.payload, buf, count);
  }
  SPI_CS_ENABLE();

  //TimeoutTimer tt(_timeout);

  // Bluefruit may not be ready
  while ((spixfer(msgCmd.header.msg_type)==SPI_IGNORED_BYTE) /*&& !tt.expired()*/ )
  {
    // Disable & Re-enable CS with a bit of delay for Bluefruit to ready itself
    SPI_CS_DISABLE();
    delayMicroseconds(SPI_DEFAULT_DELAY_US);
    SPI_CS_ENABLE();
  }

  //result = !tt.expired();
  result = TRUE;
  if (result) {
    /* transfer the rest of the data */
    spixferblock((void*)(((uint8_t*)&msgCmd)+1), sizeof(sdepMsgHeader_t)+count-1);
  }
  SPI_CS_DISABLE();
  return result;
}

static bool BLE_sendInitializePattern(void) {
  return BLE_sendPacket(SDEP_CMDTYPE_INITIALIZE, NULL, 0, 0);
}

uint8_t BLE_SendATCommand(uint8_t *cmd, uint8_t *rxBuf, size_t rxBufSize, uint8_t *expectedTailStr) {
  uint8_t res = ERR_OK;
  uint8_t rxRes;
  uint8_t ch;

  RxBuffer_Clear();
  BLE_sendPacket(SDEP_CMDTYPE_AT_WRAPPER, cmd, UTIL1_strlen(cmd), 0);
  if (!BLE_getResponse()) {
    return ERR_FAILED;
  } else if (rxBuf!=NULL) {
    while(rxBufSize>1 && RxBuffer_NofElements()>0) {
      rxRes = RxBuffer_Get(&ch);
      *rxBuf = ch;
      rxBuf++; rxBufSize--;
    }
    *rxBuf = '\0';
  }
  return res;
}

static uint8_t BLE_SendCommand(const unsigned char *cmd, const CLS1_StdIOType *io) {
  unsigned char cmdBuf[BLE_MAX_AT_CMD_SIZE];
  uint8_t res = ERR_OK;
  uint8_t ch;

  if (BLE_IRQ_GetVal()) { /* IRQ high already? There is some data from the module? */
    RxBuffer_Clear();
    BLE_getResponse(); /* check response */
    CLS1_SendStr((unsigned char*)"*** IRQ is high?\r\n", io->stdErr);
    res = ERR_FAILED;
    return res;
  }
  UTIL1_strcpy(cmdBuf, sizeof(cmdBuf), cmd);
  UTIL1_chcat(cmdBuf, sizeof(cmdBuf), '\n'); /* append newline */
  if (BLE_SendATCommand(cmdBuf, NULL, 0, (unsigned char*)"") != ERR_OK) {
    if (io!=NULL) {
      CLS1_SendStr((unsigned char*)"*** Failed sending AT command\r\n", io->stdErr);
    }
    res = ERR_FAILED;
  }
  if (res==ERR_OK) {
    /* print response */
    do {
      while(RxBuffer_NofElements()>0) {
        res = RxBuffer_Get(&ch);
        if (res!=ERR_OK) {
          break; /* stop if there is an error */
        }
        if (io!=NULL) {
          io->stdOut(ch);
        }
      }
      if (BLE_IRQ_GetVal()) { /* IRQ still high already? There is some data from the module? */
        BLE_getResponse(); /* check response */
      } else {
        break; /* break loop */
      }
    } while(1);
    if (io!=NULL) {
      CLS1_SendStr((unsigned char*)"\r\n", io->stdOut);
    }
  }
  return res;
}

uint8_t BLE_Echo(bool on) {
  if (on) {
    return BLE_SendCommand("ATE=1", NULL); /* enable echo support */
  } else {
    return BLE_SendCommand("ATE=0", NULL); /* disable echo support */
  }
}

int32_t BLE_ReadlineParseInt(void) {
  return 0; /* NYI */
}

bool BLE_WaitForOK(void) {
  return FALSE; /* NYI */
}

bool BLE_sendCommandWithIntReply(const uint8_t *cmd, int32_t *reply) {
  BLE_SendCommand(cmd, NULL);
  *reply = BLE_ReadlineParseInt(); /* parse integer response */
  return BLE_WaitForOK();
}

bool BLE_IsConnected(void) {
  int32_t connected = 0;

  BLE_sendCommandWithIntReply("AT+GAPGETCONN", &connected);
  return connected;
}

uint8_t BLE_PrintStatus(CLS1_ConstStdIOType *io) {
  CLS1_SendStatusStr((const unsigned char*)"ble", (unsigned char*)"\r\n", io->stdOut);
  CLS1_SendStatusStr((const unsigned char*)"  at", (unsigned char*)"ok\r\n", io->stdOut);
  return ERR_OK;
}

uint8_t BLE_ParseCommand(const unsigned char *cmd, bool *handled, const CLS1_StdIOType *io) {
  uint8_t res = ERR_OK;

  if (UTIL1_strcmp((char*)cmd, CLS1_CMD_HELP)==0 || UTIL1_strcmp((char*)cmd, "ble help")==0) {
    CLS1_SendHelpStr((unsigned char*)"ble", (const unsigned char*)"Group of Adafruit BLE commands\r\n", io->stdOut);
    CLS1_SendHelpStr((unsigned char*)"  help|status", (const unsigned char*)"Print help or status information\r\n", io->stdOut);
    CLS1_SendHelpStr((unsigned char*)"  cmd <AT command>", (unsigned char*)"Send an AT command, e.g ATI\r\n", io->stdOut);
    *handled = TRUE;
    return ERR_OK;
  } else if ((UTIL1_strcmp((char*)cmd, CLS1_CMD_STATUS)==0) || (UTIL1_strcmp((char*)cmd, "ble status")==0)) {
    *handled = TRUE;
    return BLE_PrintStatus(io);
  } else if (UTIL1_strncmp((char*)cmd, "ble cmd ", sizeof("ble cmd ") - 1) == 0) {
    *handled = TRUE;
    res = BLE_SendCommand(cmd+sizeof("ble cmd ")-1, io);
  }
  return res; /* no error */
}


void BLE_Init(void) {
  _mode = BLUEFRUIT_MODE_COMMAND;
  SM1_Disable();
  (void)SM1_SetIdleClockPolarity(0);    /* low idle clock polarity */
  (void)SM1_SetShiftClockPolarity(0);   /* shift clock polarity: falling edge */
  SM1_Enable();
  BLE_sendInitializePattern();
}
