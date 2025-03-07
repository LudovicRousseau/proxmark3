//-----------------------------------------------------------------------------
// Merlok - June 2011, 2012
// Gerhard de Koning Gans - May 2008
// Hagen Fritsch - June 2010
//
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// Routines to support ISO 14443 type A.
//-----------------------------------------------------------------------------
#include "iso14443a.h"

#include "string.h"
#include "proxmark3_arm.h"
#include "cmd.h"
#include "appmain.h"
#include "BigBuf.h"
#include "fpgaloader.h"
#include "ticks.h"
#include "dbprint.h"
#include "util.h"
#include "parity.h"
#include "mifareutil.h"
#include "commonutil.h"
#include "crc16.h"
#include "protocols.h"

#define MAX_ISO14A_TIMEOUT 524288
static uint32_t iso14a_timeout;
// if iso14443a not active - transmit/receive dont try to execute
static bool hf_field_active = false;

uint8_t colpos = 0;
int rsamples = 0;
uint8_t trigger = 0;
// the block number for the ISO14443-4 PCB
static uint8_t iso14_pcb_blocknum = 0;

//
// ISO14443 timing:
//
// minimum time between the start bits of consecutive transfers from reader to tag: 7000 carrier (13.56MHz) cycles
#define REQUEST_GUARD_TIME (7000/16 + 1)
// minimum time between last modulation of tag and next start bit from reader to tag: 1172 carrier cycles
#define FRAME_DELAY_TIME_PICC_TO_PCD (1172/16 + 1)
// bool LastCommandWasRequest = false;

//
// Total delays including SSC-Transfers between ARM and FPGA. These are in carrier clock cycles (1/13,56MHz)
//
// When the PM acts as reader and is receiving tag data, it takes
// 3 ticks delay in the AD converter
// 16 ticks until the modulation detector completes and sets curbit
// 8 ticks until bit_to_arm is assigned from curbit
// 8*16 ticks for the transfer from FPGA to ARM
// 4*16 ticks until we measure the time
// - 8*16 ticks because we measure the time of the previous transfer
#define DELAY_AIR2ARM_AS_READER (3 + 16 + 8 + 8*16 + 4*16 - 8*16)

// When the PM acts as a reader and is sending, it takes
// 4*16 ticks until we can write data to the sending hold register
// 8*16 ticks until the SHR is transferred to the Sending Shift Register
// 8 ticks until the first transfer starts
// 8 ticks later the FPGA samples the data
// 1 tick to assign mod_sig_coil
#define DELAY_ARM2AIR_AS_READER (4*16 + 8*16 + 8 + 8 + 1)

// The FPGA will report its internal sending delay in
uint16_t FpgaSendQueueDelay;
// the 5 first bits are the number of bits buffered in mod_sig_buf
// the last three bits are the remaining ticks/2 after the mod_sig_buf shift
#define DELAY_FPGA_QUEUE (FpgaSendQueueDelay<<1)

// When the PM acts as tag and is sending, it takes
// 4*16 + 8 ticks until we can write data to the sending hold register
// 8*16 ticks until the SHR is transferred to the Sending Shift Register
// 8 ticks later the FPGA samples the first data
// + 16 ticks until assigned to mod_sig
// + 1 tick to assign mod_sig_coil
// + a varying number of ticks in the FPGA Delay Queue (mod_sig_buf)
#define DELAY_ARM2AIR_AS_TAG (4*16 + 8 + 8*16 + 8 + 16 + 1 + DELAY_FPGA_QUEUE)

// When the PM acts as sniffer and is receiving tag data, it takes
// 3 ticks A/D conversion
// 14 ticks to complete the modulation detection
// 8 ticks (on average) until the result is stored in to_arm
// + the delays in transferring data - which is the same for
// sniffing reader and tag data and therefore not relevant
#define DELAY_TAG_AIR2ARM_AS_SNIFFER (3 + 14 + 8)

// When the PM acts as sniffer and is receiving reader data, it takes
// 2 ticks delay in analogue RF receiver (for the falling edge of the
// start bit, which marks the start of the communication)
// 3 ticks A/D conversion
// 8 ticks on average until the data is stored in to_arm.
// + the delays in transferring data - which is the same for
// sniffing reader and tag data and therefore not relevant
#define DELAY_READER_AIR2ARM_AS_SNIFFER (2 + 3 + 8)

//variables used for timing purposes:
//these are in ssp_clk cycles:
static uint32_t NextTransferTime;
static uint32_t LastTimeProxToAirStart;
static uint32_t LastProxToAirDuration;

// CARD TO READER - manchester
// Sequence D: 11110000 modulation with subcarrier during first half
// Sequence E: 00001111 modulation with subcarrier during second half
// Sequence F: 00000000 no modulation with subcarrier
// Sequence COLL: 11111111 load modulation over the full bitlength.
//                         Tricks the reader to think that multiple cards answer (at least one card with 1 and at least one card with 0).
// READER TO CARD - miller
// Sequence X: 00001100 drop after half a period
// Sequence Y: 00000000 no drop
// Sequence Z: 11000000 drop at start
#define SEC_D 0xf0
#define SEC_E 0x0f
#define SEC_F 0x00
#define SEC_COLL 0xff
#define SEC_X 0x0c
#define SEC_Y 0x00
#define SEC_Z 0xc0

void iso14a_set_trigger(bool enable) {
    trigger = enable;
}

void iso14a_set_timeout(uint32_t timeout) {
    iso14a_timeout = timeout + (DELAY_AIR2ARM_AS_READER + DELAY_ARM2AIR_AS_READER) / (16 * 8) + 2;
}

uint32_t iso14a_get_timeout(void) {
    return iso14a_timeout - (DELAY_AIR2ARM_AS_READER + DELAY_ARM2AIR_AS_READER) / (16 * 8) - 2;
}

//-----------------------------------------------------------------------------
// Generate the parity value for a byte sequence
//-----------------------------------------------------------------------------
void GetParity(const uint8_t *pbtCmd, uint16_t len, uint8_t *par) {
    uint16_t paritybit_cnt = 0;
    uint16_t paritybyte_cnt = 0;
    uint8_t parityBits = 0;

    for (uint16_t i = 0; i < len; i++) {
        // Generate the parity bits
        parityBits |= ((oddparity8(pbtCmd[i])) << (7 - paritybit_cnt));
        if (paritybit_cnt == 7) {
            par[paritybyte_cnt] = parityBits; // save 8 Bits parity
            parityBits = 0;                   // and advance to next Parity Byte
            paritybyte_cnt++;
            paritybit_cnt = 0;
        } else {
            paritybit_cnt++;
        }
    }

    // save remaining parity bits
    par[paritybyte_cnt] = parityBits;
}


//=============================================================================
// ISO 14443 Type A - Miller decoder
//=============================================================================
// Basics:
// This decoder is used when the PM3 acts as a tag.
// The reader will generate "pauses" by temporarily switching of the field.
// At the PM3 antenna we will therefore measure a modulated antenna voltage.
// The FPGA does a comparison with a threshold and would deliver e.g.:
// ........  1 1 1 1 1 1 0 0 1 1 1 1 1 1 1 1 1 1 0 0 1 1 1 1 1 1 1 1 1 1  .......
// The Miller decoder needs to identify the following sequences:
// 2 (or 3) ticks pause followed by 6 (or 5) ticks unmodulated: pause at beginning - Sequence Z ("start of communication" or a "0")
// 8 ticks without a modulation:                                no pause - Sequence Y (a "0" or "end of communication" or "no information")
// 4 ticks unmodulated followed by 2 (or 3) ticks pause:        pause in second half - Sequence X (a "1")
// Note 1: the bitstream may start at any time. We therefore need to sync.
// Note 2: the interpretation of Sequence Y and Z depends on the preceding sequence.
//-----------------------------------------------------------------------------
static tUart14a Uart;

// Lookup-Table to decide if 4 raw bits are a modulation.
// We accept the following:
// 0001  -   a 3 tick wide pause
// 0011  -   a 2 tick wide pause, or a three tick wide pause shifted left
// 0111  -   a 2 tick wide pause shifted left
// 1001  -   a 2 tick wide pause shifted right
const bool Mod_Miller_LUT[] = {
    false,  true, false, true,  false, false, false, true,
    false,  true, false, false, false, false, false, false
};
#define IsMillerModulationNibble1(b) (Mod_Miller_LUT[(b & 0x000000F0) >> 4])
#define IsMillerModulationNibble2(b) (Mod_Miller_LUT[(b & 0x0000000F)])

tUart14a *GetUart14a() {
    return &Uart;
}

void Uart14aReset(void) {
    Uart.state = STATE_14A_UNSYNCD;
    Uart.bitCount = 0;
    Uart.len = 0;                       // number of decoded data bytes
    Uart.parityLen = 0;                 // number of decoded parity bytes
    Uart.shiftReg = 0;                  // shiftreg to hold decoded data bits
    Uart.parityBits = 0;                // holds 8 parity bits
    Uart.startTime = 0;
    Uart.endTime = 0;
    Uart.fourBits = 0x00000000;         // clear the buffer for 4 Bits
    Uart.posCnt = 0;
    Uart.syncBit = 9999;
}

void Uart14aInit(uint8_t *data, uint8_t *par) {
    Uart.output = data;
    Uart.parity = par;
    Uart14aReset();
}

// use parameter non_real_time to provide a timestamp. Set to 0 if the decoder should measure real time
RAMFUNC bool MillerDecoding(uint8_t bit, uint32_t non_real_time) {
    Uart.fourBits = (Uart.fourBits << 8) | bit;

    if (Uart.state == STATE_14A_UNSYNCD) {                                           // not yet synced
        Uart.syncBit = 9999;                                                 // not set

        // 00x11111 2|3 ticks pause followed by 6|5 ticks unmodulated         Sequence Z (a "0" or "start of communication")
        // 11111111 8 ticks unmodulation                                      Sequence Y (a "0" or "end of communication" or "no information")
        // 111100x1 4 ticks unmodulated followed by 2|3 ticks pause           Sequence X (a "1")

        // The start bit is one ore more Sequence Y followed by a Sequence Z (... 11111111 00x11111). We need to distinguish from
        // Sequence X followed by Sequence Y followed by Sequence Z     (111100x1 11111111 00x11111)
        // we therefore look for a ...xx1111 11111111 00x11111xxxxxx... pattern
        // (12 '1's followed by 2 '0's, eventually followed by another '0', followed by 5 '1's)
#define ISO14443A_STARTBIT_MASK       0x07FFEF80                            // mask is    00000111 11111111 11101111 10000000
#define ISO14443A_STARTBIT_PATTERN    0x07FF8F80                            // pattern is 00000111 11111111 10001111 10000000
        if ((Uart.fourBits & (ISO14443A_STARTBIT_MASK >> 0)) == ISO14443A_STARTBIT_PATTERN >> 0) Uart.syncBit = 7;
        else if ((Uart.fourBits & (ISO14443A_STARTBIT_MASK >> 1)) == ISO14443A_STARTBIT_PATTERN >> 1) Uart.syncBit = 6;
        else if ((Uart.fourBits & (ISO14443A_STARTBIT_MASK >> 2)) == ISO14443A_STARTBIT_PATTERN >> 2) Uart.syncBit = 5;
        else if ((Uart.fourBits & (ISO14443A_STARTBIT_MASK >> 3)) == ISO14443A_STARTBIT_PATTERN >> 3) Uart.syncBit = 4;
        else if ((Uart.fourBits & (ISO14443A_STARTBIT_MASK >> 4)) == ISO14443A_STARTBIT_PATTERN >> 4) Uart.syncBit = 3;
        else if ((Uart.fourBits & (ISO14443A_STARTBIT_MASK >> 5)) == ISO14443A_STARTBIT_PATTERN >> 5) Uart.syncBit = 2;
        else if ((Uart.fourBits & (ISO14443A_STARTBIT_MASK >> 6)) == ISO14443A_STARTBIT_PATTERN >> 6) Uart.syncBit = 1;
        else if ((Uart.fourBits & (ISO14443A_STARTBIT_MASK >> 7)) == ISO14443A_STARTBIT_PATTERN >> 7) Uart.syncBit = 0;

        if (Uart.syncBit != 9999) {                                              // found a sync bit
            Uart.startTime = non_real_time ? non_real_time : (GetCountSspClk() & 0xfffffff8);
            Uart.startTime -= Uart.syncBit;
            Uart.endTime = Uart.startTime;
            Uart.state = STATE_14A_START_OF_COMMUNICATION;
        }
    } else {

        if (IsMillerModulationNibble1(Uart.fourBits >> Uart.syncBit)) {
            if (IsMillerModulationNibble2(Uart.fourBits >> Uart.syncBit)) {      // Modulation in both halves - error
                Uart14aReset();
            } else {                                                             // Modulation in first half = Sequence Z = logic "0"
                if (Uart.state == STATE_14A_MILLER_X) {                              // error - must not follow after X
                    Uart14aReset();
                } else {
                    Uart.bitCount++;
                    Uart.shiftReg = (Uart.shiftReg >> 1);                        // add a 0 to the shiftreg
                    Uart.state = STATE_14A_MILLER_Z;
                    Uart.endTime = Uart.startTime + 8 * (9 * Uart.len + Uart.bitCount + 1) - 6;
                    if (Uart.bitCount >= 9) {                                    // if we decoded a full byte (including parity)
                        Uart.output[Uart.len++] = (Uart.shiftReg & 0xff);
                        Uart.parityBits <<= 1;                                   // make room for the parity bit
                        Uart.parityBits |= ((Uart.shiftReg >> 8) & 0x01);        // store parity bit
                        Uart.bitCount = 0;
                        Uart.shiftReg = 0;
                        if ((Uart.len & 0x0007) == 0) {                          // every 8 data bytes
                            Uart.parity[Uart.parityLen++] = Uart.parityBits;     // store 8 parity bits
                            Uart.parityBits = 0;
                        }
                    }
                }
            }
        } else {
            if (IsMillerModulationNibble2(Uart.fourBits >> Uart.syncBit)) {      // Modulation second half = Sequence X = logic "1"
                Uart.bitCount++;
                Uart.shiftReg = (Uart.shiftReg >> 1) | 0x100;                    // add a 1 to the shiftreg
                Uart.state = STATE_14A_MILLER_X;
                Uart.endTime = Uart.startTime + 8 * (9 * Uart.len + Uart.bitCount + 1) - 2;
                if (Uart.bitCount >= 9) {                                        // if we decoded a full byte (including parity)
                    Uart.output[Uart.len++] = (Uart.shiftReg & 0xff);
                    Uart.parityBits <<= 1;                                       // make room for the new parity bit
                    Uart.parityBits |= ((Uart.shiftReg >> 8) & 0x01);            // store parity bit
                    Uart.bitCount = 0;
                    Uart.shiftReg = 0;
                    if ((Uart.len & 0x0007) == 0) {                              // every 8 data bytes
                        Uart.parity[Uart.parityLen++] = Uart.parityBits;         // store 8 parity bits
                        Uart.parityBits = 0;
                    }
                }
            } else {                                                             // no modulation in both halves - Sequence Y
                if (Uart.state == STATE_14A_MILLER_Z || Uart.state == STATE_14A_MILLER_Y) {    // Y after logic "0" - End of Communication
                    Uart.state = STATE_14A_UNSYNCD;
                    Uart.bitCount--;                                             // last "0" was part of EOC sequence
                    Uart.shiftReg <<= 1;                                         // drop it
                    if (Uart.bitCount > 0) {                                     // if we decoded some bits
                        Uart.shiftReg >>= (9 - Uart.bitCount);                   // right align them
                        Uart.output[Uart.len++] = (Uart.shiftReg & 0xff);        // add last byte to the output
                        Uart.parityBits <<= 1;                                   // add a (void) parity bit
                        Uart.parityBits <<= (8 - (Uart.len & 0x0007));           // left align parity bits
                        Uart.parity[Uart.parityLen++] = Uart.parityBits;         // and store it
                        return true;
                    } else if (Uart.len & 0x0007) {                              // there are some parity bits to store
                        Uart.parityBits <<= (8 - (Uart.len & 0x0007));           // left align remaining parity bits
                        Uart.parity[Uart.parityLen++] = Uart.parityBits;         // and store them
                    }
                    if (Uart.len) {
                        return true;                                             // we are finished with decoding the raw data sequence
                    } else {
                        Uart14aReset();                                             // Nothing received - start over
                    }
                }
                if (Uart.state == STATE_14A_START_OF_COMMUNICATION) {                // error - must not follow directly after SOC
                    Uart14aReset();
                } else {                                                         // a logic "0"
                    Uart.bitCount++;
                    Uart.shiftReg = (Uart.shiftReg >> 1);                        // add a 0 to the shiftreg
                    Uart.state = STATE_14A_MILLER_Y;
                    if (Uart.bitCount >= 9) {                                    // if we decoded a full byte (including parity)
                        Uart.output[Uart.len++] = (Uart.shiftReg & 0xff);
                        Uart.parityBits <<= 1;                                   // make room for the parity bit
                        Uart.parityBits |= ((Uart.shiftReg >> 8) & 0x01);        // store parity bit
                        Uart.bitCount = 0;
                        Uart.shiftReg = 0;
                        if ((Uart.len & 0x0007) == 0) {                          // every 8 data bytes
                            Uart.parity[Uart.parityLen++] = Uart.parityBits;     // store 8 parity bits
                            Uart.parityBits = 0;
                        }
                    }
                }
            }
        }
    }
    return false;    // not finished yet, need more data
}

//=============================================================================
// ISO 14443 Type A - Manchester decoder
//=============================================================================
// Basics:
// This decoder is used when the PM3 acts as a reader.
// The tag will modulate the reader field by asserting different loads to it. As a consequence, the voltage
// at the reader antenna will be modulated as well. The FPGA detects the modulation for us and would deliver e.g. the following:
// ........ 0 0 1 1 1 1 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 .......
// The Manchester decoder needs to identify the following sequences:
// 4 ticks modulated followed by 4 ticks unmodulated:     Sequence D = 1 (also used as "start of communication")
// 4 ticks unmodulated followed by 4 ticks modulated:     Sequence E = 0
// 8 ticks unmodulated:                                   Sequence F = end of communication
// 8 ticks modulated:                                     A collision. Save the collision position and treat as Sequence D
// Note 1: the bitstream may start at any time. We therefore need to sync.
// Note 2: parameter offset is used to determine the position of the parity bits (required for the anticollision command only)
tDemod14a Demod;

// Lookup-Table to decide if 4 raw bits are a modulation.
// We accept three or four "1" in any position
const bool Mod_Manchester_LUT[] = {
    false, false, false, false, false, false, false, true,
    false, false, false, true,  false, true,  true,  true
};

#define IsManchesterModulationNibble1(b) (Mod_Manchester_LUT[(b & 0x00F0) >> 4])
#define IsManchesterModulationNibble2(b) (Mod_Manchester_LUT[(b & 0x000F)])

tDemod14a *GetDemod14a() {
    return &Demod;
}
void Demod14aReset(void) {
    Demod.state = DEMOD_14A_UNSYNCD;
    Demod.len = 0;                       // number of decoded data bytes
    Demod.parityLen = 0;
    Demod.shiftReg = 0;                  // shiftreg to hold decoded data bits
    Demod.parityBits = 0;                //
    Demod.collisionPos = 0;              // Position of collision bit
    Demod.twoBits = 0xFFFF;              // buffer for 2 Bits
    Demod.highCnt = 0;
    Demod.startTime = 0;
    Demod.endTime = 0;
    Demod.bitCount = 0;
    Demod.syncBit = 0xFFFF;
    Demod.samples = 0;
}

void Demod14aInit(uint8_t *data, uint8_t *par) {
    Demod.output = data;
    Demod.parity = par;
    Demod14aReset();
}

// use parameter non_real_time to provide a timestamp. Set to 0 if the decoder should measure real time
RAMFUNC int ManchesterDecoding(uint8_t bit, uint16_t offset, uint32_t non_real_time) {
    Demod.twoBits = (Demod.twoBits << 8) | bit;

    if (Demod.state == DEMOD_14A_UNSYNCD) {

        if (Demod.highCnt < 2) {                                            // wait for a stable unmodulated signal
            if (Demod.twoBits == 0x0000) {
                Demod.highCnt++;
            } else {
                Demod.highCnt = 0;
            }
        } else {
            Demod.syncBit = 0xFFFF;            // not set
            if ((Demod.twoBits & 0x7700) == 0x7000) Demod.syncBit = 7;
            else if ((Demod.twoBits & 0x3B80) == 0x3800) Demod.syncBit = 6;
            else if ((Demod.twoBits & 0x1DC0) == 0x1C00) Demod.syncBit = 5;
            else if ((Demod.twoBits & 0x0EE0) == 0x0E00) Demod.syncBit = 4;
            else if ((Demod.twoBits & 0x0770) == 0x0700) Demod.syncBit = 3;
            else if ((Demod.twoBits & 0x03B8) == 0x0380) Demod.syncBit = 2;
            else if ((Demod.twoBits & 0x01DC) == 0x01C0) Demod.syncBit = 1;
            else if ((Demod.twoBits & 0x00EE) == 0x00E0) Demod.syncBit = 0;
            if (Demod.syncBit != 0xFFFF) {
                Demod.startTime = non_real_time ? non_real_time : (GetCountSspClk() & 0xfffffff8);
                Demod.startTime -= Demod.syncBit;
                Demod.bitCount = offset;            // number of decoded data bits
                Demod.state = DEMOD_14A_MANCHESTER_DATA;
            }
        }
    } else {

        if (IsManchesterModulationNibble1(Demod.twoBits >> Demod.syncBit)) {      // modulation in first half
            if (IsManchesterModulationNibble2(Demod.twoBits >> Demod.syncBit)) {  // ... and in second half = collision
                if (!Demod.collisionPos) {
                    Demod.collisionPos = (Demod.len << 3) + Demod.bitCount;
                }
            }                                                           // modulation in first half only - Sequence D = 1
            Demod.bitCount++;
            Demod.shiftReg = (Demod.shiftReg >> 1) | 0x100;             // in both cases, add a 1 to the shiftreg
            if (Demod.bitCount == 9) {                                  // if we decoded a full byte (including parity)
                Demod.output[Demod.len++] = (Demod.shiftReg & 0xff);
                Demod.parityBits <<= 1;                                 // make room for the parity bit
                Demod.parityBits |= ((Demod.shiftReg >> 8) & 0x01);     // store parity bit
                Demod.bitCount = 0;
                Demod.shiftReg = 0;
                if ((Demod.len & 0x0007) == 0) {                        // every 8 data bytes
                    Demod.parity[Demod.parityLen++] = Demod.parityBits; // store 8 parity bits
                    Demod.parityBits = 0;
                }
            }
            Demod.endTime = Demod.startTime + 8 * (9 * Demod.len + Demod.bitCount + 1) - 4;
        } else {                                                        // no modulation in first half
            if (IsManchesterModulationNibble2(Demod.twoBits >> Demod.syncBit)) {    // and modulation in second half = Sequence E = 0
                Demod.bitCount++;
                Demod.shiftReg = (Demod.shiftReg >> 1);                 // add a 0 to the shiftreg
                if (Demod.bitCount >= 9) {                              // if we decoded a full byte (including parity)
                    Demod.output[Demod.len++] = (Demod.shiftReg & 0xff);
                    Demod.parityBits <<= 1;                             // make room for the new parity bit
                    Demod.parityBits |= ((Demod.shiftReg >> 8) & 0x01); // store parity bit
                    Demod.bitCount = 0;
                    Demod.shiftReg = 0;
                    if ((Demod.len & 0x0007) == 0) {                    // every 8 data bytes
                        Demod.parity[Demod.parityLen++] = Demod.parityBits;    // store 8 parity bits1
                        Demod.parityBits = 0;
                    }
                }
                Demod.endTime = Demod.startTime + 8 * (9 * Demod.len + Demod.bitCount + 1);
            } else {                                                    // no modulation in both halves - End of communication
                if (Demod.bitCount > 0) {                               // there are some remaining data bits
                    Demod.shiftReg >>= (9 - Demod.bitCount);            // right align the decoded bits
                    Demod.output[Demod.len++] = Demod.shiftReg & 0xff;  // and add them to the output
                    Demod.parityBits <<= 1;                             // add a (void) parity bit
                    Demod.parityBits <<= (8 - (Demod.len & 0x0007));    // left align remaining parity bits
                    Demod.parity[Demod.parityLen++] = Demod.parityBits; // and store them
                    return true;
                } else if (Demod.len & 0x0007) {                        // there are some parity bits to store
                    Demod.parityBits <<= (8 - (Demod.len & 0x0007));    // left align remaining parity bits
                    Demod.parity[Demod.parityLen++] = Demod.parityBits; // and store them
                }
                if (Demod.len) {
                    return true;                                        // we are finished with decoding the raw data sequence
                } else {                                                // nothing received. Start over
                    Demod14aReset();
                }
            }
        }
    }
    return false;    // not finished yet, need more data
}


// Thinfilm, Kovio mangels ISO14443A in the way that they don't use start bit nor parity bits.
RAMFUNC int ManchesterDecoding_Thinfilm(uint8_t bit) {
    Demod.twoBits = (Demod.twoBits << 8) | bit;

    if (Demod.state == DEMOD_14A_UNSYNCD) {

        if (Demod.highCnt < 2) {                                            // wait for a stable unmodulated signal
            if (Demod.twoBits == 0x0000) {
                Demod.highCnt++;
            } else {
                Demod.highCnt = 0;
            }
        } else {
            Demod.syncBit = 0xFFFF;            // not set
            if ((Demod.twoBits & 0x7700) == 0x7000) Demod.syncBit = 7;
            else if ((Demod.twoBits & 0x3B80) == 0x3800) Demod.syncBit = 6;
            else if ((Demod.twoBits & 0x1DC0) == 0x1C00) Demod.syncBit = 5;
            else if ((Demod.twoBits & 0x0EE0) == 0x0E00) Demod.syncBit = 4;
            else if ((Demod.twoBits & 0x0770) == 0x0700) Demod.syncBit = 3;
            else if ((Demod.twoBits & 0x03B8) == 0x0380) Demod.syncBit = 2;
            else if ((Demod.twoBits & 0x01DC) == 0x01C0) Demod.syncBit = 1;
            else if ((Demod.twoBits & 0x00EE) == 0x00E0) Demod.syncBit = 0;
            if (Demod.syncBit != 0xFFFF) {
                Demod.startTime = (GetCountSspClk() & 0xfffffff8);
                Demod.startTime -= Demod.syncBit;
                Demod.bitCount = 1;            // number of decoded data bits
                Demod.shiftReg = 1;
                Demod.state = DEMOD_14A_MANCHESTER_DATA;
            }
        }
    } else {

        if (IsManchesterModulationNibble1(Demod.twoBits >> Demod.syncBit)) {      // modulation in first half
            if (IsManchesterModulationNibble2(Demod.twoBits >> Demod.syncBit)) {  // ... and in second half = collision
                if (!Demod.collisionPos) {
                    Demod.collisionPos = (Demod.len << 3) + Demod.bitCount;
                }
            }                                                           // modulation in first half only - Sequence D = 1
            Demod.bitCount++;
            Demod.shiftReg = (Demod.shiftReg << 1) | 0x1;             // in both cases, add a 1 to the shiftreg
            if (Demod.bitCount == 8) {                                  // if we decoded a full byte
                Demod.output[Demod.len++] = (Demod.shiftReg & 0xff);
                Demod.bitCount = 0;
                Demod.shiftReg = 0;
            }
            Demod.endTime = Demod.startTime + 8 * (8 * Demod.len + Demod.bitCount + 1) - 4;
        } else {                                                        // no modulation in first half
            if (IsManchesterModulationNibble2(Demod.twoBits >> Demod.syncBit)) {    // and modulation in second half = Sequence E = 0
                Demod.bitCount++;
                Demod.shiftReg = (Demod.shiftReg << 1);                 // add a 0 to the shiftreg
                if (Demod.bitCount >= 8) {                              // if we decoded a full byte
                    Demod.output[Demod.len++] = (Demod.shiftReg & 0xff);
                    Demod.bitCount = 0;
                    Demod.shiftReg = 0;
                }
                Demod.endTime = Demod.startTime + 8 * (8 * Demod.len + Demod.bitCount + 1);
            } else {                                                    // no modulation in both halves - End of communication
                if (Demod.bitCount > 0) {                               // there are some remaining data bits
                    Demod.shiftReg <<= (8 - Demod.bitCount);            // left align the decoded bits
                    Demod.output[Demod.len++] = Demod.shiftReg & 0xff;  // and add them to the output
                    return true;
                }
                if (Demod.len) {
                    return true;                                        // we are finished with decoding the raw data sequence
                } else {                                                // nothing received. Start over
                    Demod14aReset();
                }
            }
        }
    }
    return false;    // not finished yet, need more data
}



//=============================================================================
// Finally, a `sniffer' for ISO 14443 Type A
// Both sides of communication!
//=============================================================================

//-----------------------------------------------------------------------------
// Record the sequence of commands sent by the reader to the tag, with
// triggering so that we start recording at the point that the tag is moved
// near the reader.
// "hf 14a sniff"
//-----------------------------------------------------------------------------
void RAMFUNC SniffIso14443a(uint8_t param) {
    LEDsoff();
    // param:
    // bit 0 - trigger from first card answer
    // bit 1 - trigger from first reader 7-bit request
    iso14443a_setup(FPGA_HF_ISO14443A_SNIFFER);

    // Allocate memory from BigBuf for some buffers
    // free all previous allocations first
    BigBuf_free();
    BigBuf_Clear_ext(false);
    clear_trace();
    set_tracing(true);

    // The command (reader -> tag) that we're receiving.
    uint8_t *receivedCmd = BigBuf_malloc(MAX_FRAME_SIZE);
    uint8_t *receivedCmdPar = BigBuf_malloc(MAX_PARITY_SIZE);

    // The response (tag -> reader) that we're receiving.
    uint8_t *receivedResp = BigBuf_malloc(MAX_FRAME_SIZE);
    uint8_t *receivedRespPar = BigBuf_malloc(MAX_PARITY_SIZE);

    // The DMA buffer, used to stream samples from the FPGA
    uint8_t *dmaBuf = BigBuf_malloc(DMA_BUFFER_SIZE);
    uint8_t *data = dmaBuf;

    uint8_t previous_data = 0;
    int maxDataLen = 0, dataLen;
    bool TagIsActive = false;
    bool ReaderIsActive = false;

    // Set up the demodulator for tag -> reader responses.
    Demod14aInit(receivedResp, receivedRespPar);

    // Set up the demodulator for the reader -> tag commands
    Uart14aInit(receivedCmd, receivedCmdPar);

    DbpString("Starting to sniff");

    // Setup and start DMA.
    if (!FpgaSetupSscDma((uint8_t *) dmaBuf, DMA_BUFFER_SIZE)) {
        if (DBGLEVEL > 1) Dbprintf("FpgaSetupSscDma failed. Exiting");
        return;
    }

    // We won't start recording the frames that we acquire until we trigger;
    // a good trigger condition to get started is probably when we see a
    // response from the tag.
    // triggered == false -- to wait first for card
    bool triggered = !(param & 0x03);

    uint32_t rx_samples = 0;

    // loop and listen
    while (!BUTTON_PRESS()) {
        WDT_HIT();
        LED_A_ON();

        int register readBufDataP = data - dmaBuf;
        int register dmaBufDataP = DMA_BUFFER_SIZE - AT91C_BASE_PDC_SSC->PDC_RCR;
        if (readBufDataP <= dmaBufDataP)
            dataLen = dmaBufDataP - readBufDataP;
        else
            dataLen = DMA_BUFFER_SIZE - readBufDataP + dmaBufDataP;

        // test for length of buffer
        if (dataLen > maxDataLen) {
            maxDataLen = dataLen;
            if (dataLen > (9 * DMA_BUFFER_SIZE / 10)) {
                Dbprintf("[!] blew circular buffer! | datalen %u", dataLen);
                break;
            }
        }
        if (dataLen < 1) continue;

        // primary buffer was stopped( <-- we lost data!
        if (!AT91C_BASE_PDC_SSC->PDC_RCR) {
            AT91C_BASE_PDC_SSC->PDC_RPR = (uint32_t) dmaBuf;
            AT91C_BASE_PDC_SSC->PDC_RCR = DMA_BUFFER_SIZE;
            Dbprintf("[-] RxEmpty ERROR | data length %d", dataLen); // temporary
        }
        // secondary buffer sets as primary, secondary buffer was stopped
        if (!AT91C_BASE_PDC_SSC->PDC_RNCR) {
            AT91C_BASE_PDC_SSC->PDC_RNPR = (uint32_t) dmaBuf;
            AT91C_BASE_PDC_SSC->PDC_RNCR = DMA_BUFFER_SIZE;
        }

        LED_A_OFF();

        // Need two samples to feed Miller and Manchester-Decoder
        if (rx_samples & 0x01) {

            if (!TagIsActive) {        // no need to try decoding reader data if the tag is sending
                uint8_t readerdata = (previous_data & 0xF0) | (*data >> 4);
                if (MillerDecoding(readerdata, (rx_samples - 1) * 4)) {
                    LED_C_ON();

                    // check - if there is a short 7bit request from reader
                    if ((!triggered) && (param & 0x02) && (Uart.len == 1) && (Uart.bitCount == 7)) triggered = true;

                    if (triggered) {
                        if (!LogTrace(receivedCmd,
                                      Uart.len,
                                      Uart.startTime * 16 - DELAY_READER_AIR2ARM_AS_SNIFFER,
                                      Uart.endTime * 16 - DELAY_READER_AIR2ARM_AS_SNIFFER,
                                      Uart.parity,
                                      true)) break;
                    }
                    /* ready to receive another command. */
                    Uart14aReset();
                    /* reset the demod code, which might have been */
                    /* false-triggered by the commands from the reader. */
                    Demod14aReset();
                    LED_B_OFF();
                }
                ReaderIsActive = (Uart.state != STATE_14A_UNSYNCD);
            }

            // no need to try decoding tag data if the reader is sending - and we cannot afford the time
            if (!ReaderIsActive) {
                uint8_t tagdata = (previous_data << 4) | (*data & 0x0F);
                if (ManchesterDecoding(tagdata, 0, (rx_samples - 1) * 4)) {
                    LED_B_ON();

                    if (!LogTrace(receivedResp,
                                  Demod.len,
                                  Demod.startTime * 16 - DELAY_TAG_AIR2ARM_AS_SNIFFER,
                                  Demod.endTime * 16 - DELAY_TAG_AIR2ARM_AS_SNIFFER,
                                  Demod.parity,
                                  false)) break;

                    if ((!triggered) && (param & 0x01)) triggered = true;

                    // ready to receive another response.
                    Demod14aReset();
                    // reset the Miller decoder including its (now outdated) input buffer
                    Uart14aReset();
                    //Uart14aInit(receivedCmd, receivedCmdPar);
                    LED_C_OFF();
                }
                TagIsActive = (Demod.state != DEMOD_14A_UNSYNCD);
            }
        }

        previous_data = *data;
        rx_samples++;
        data++;
        if (data == dmaBuf + DMA_BUFFER_SIZE) {
            data = dmaBuf;
        }
    } // end main loop

    if (DBGLEVEL >= DBG_ERROR) {
        Dbprintf("maxDataLen=%d, Uart.state=%x, Uart.len=%d", maxDataLen, Uart.state, Uart.len);
        Dbprintf("traceLen=" _YELLOW_("%d")", Uart.output[0]="_YELLOW_("%08x"), BigBuf_get_traceLen(), (uint32_t)Uart.output[0]);
    }
    switch_off();
}

//-----------------------------------------------------------------------------
// Prepare tag messages
//-----------------------------------------------------------------------------
static void CodeIso14443aAsTagPar(const uint8_t *cmd, uint16_t len, uint8_t *par, bool collision) {

    //uint8_t localCol = 0;
    ToSendReset();

    // Correction bit, might be removed when not needed
    ToSendStuffBit(0);
    ToSendStuffBit(0);
    ToSendStuffBit(0);
    ToSendStuffBit(0);
    ToSendStuffBit(1);  // <-----
    ToSendStuffBit(0);
    ToSendStuffBit(0);
    ToSendStuffBit(0);

    // Send startbit
    ToSend[++ToSendMax] = SEC_D;
    LastProxToAirDuration = 8 * ToSendMax - 4;

    for (uint16_t i = 0; i < len; i++) {
        uint8_t b = cmd[i];

        // Data bits
        for (uint16_t j = 0; j < 8; j++) {
            //if (collision && (localCol >= colpos)){
            if (collision) {
                ToSend[++ToSendMax] = SEC_COLL;
                //localCol++;
            } else {
                if (b & 1) {
                    ToSend[++ToSendMax] = SEC_D;
                } else {
                    ToSend[++ToSendMax] = SEC_E;
                }
                b >>= 1;
            }
        }

        if (collision) {
            ToSend[++ToSendMax] = SEC_COLL;
            LastProxToAirDuration = 8 * ToSendMax;
        } else {
            // Get the parity bit
            if (par[i >> 3] & (0x80 >> (i & 0x0007))) {
                ToSend[++ToSendMax] = SEC_D;
                LastProxToAirDuration = 8 * ToSendMax - 4;
            } else {
                ToSend[++ToSendMax] = SEC_E;
                LastProxToAirDuration = 8 * ToSendMax;
            }
        }
    }

    // Send stopbit
    ToSend[++ToSendMax] = SEC_F;

    // Convert from last byte pos to length
    ToSendMax++;
}

static void CodeIso14443aAsTagEx(const uint8_t *cmd, uint16_t len, bool collision) {
    uint8_t par[MAX_PARITY_SIZE] = {0};
    GetParity(cmd, len, par);
    CodeIso14443aAsTagPar(cmd, len, par, collision);
}
static void CodeIso14443aAsTag(const uint8_t *cmd, uint16_t len) {
    CodeIso14443aAsTagEx(cmd, len, false);
}

static void Code4bitAnswerAsTag(uint8_t cmd) {
    uint8_t b = cmd;

    ToSendReset();

    // Correction bit, might be removed when not needed
    ToSendStuffBit(0);
    ToSendStuffBit(0);
    ToSendStuffBit(0);
    ToSendStuffBit(0);
    ToSendStuffBit(1);  // 1
    ToSendStuffBit(0);
    ToSendStuffBit(0);
    ToSendStuffBit(0);

    // Send startbit
    ToSend[++ToSendMax] = SEC_D;

    for (uint8_t i = 0; i < 4; i++) {
        if (b & 1) {
            ToSend[++ToSendMax] = SEC_D;
            LastProxToAirDuration = 8 * ToSendMax - 4;
        } else {
            ToSend[++ToSendMax] = SEC_E;
            LastProxToAirDuration = 8 * ToSendMax;
        }
        b >>= 1;
    }

    // Send stopbit
    ToSend[++ToSendMax] = SEC_F;

    // Convert from last byte pos to length
    ToSendMax++;
}

//-----------------------------------------------------------------------------
// Wait for commands from reader
// stop when button is pressed or client usb connection resets
// or return TRUE when command is captured
//-----------------------------------------------------------------------------
static bool GetIso14443aCommandFromReader(uint8_t *received, uint8_t *par, int *len) {
    // Set FPGA mode to "simulated ISO 14443 tag", no modulation (listen
    // only, since we are receiving, not transmitting).
    // Signal field is off with the appropriate LED
    LED_D_OFF();
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_ISO14443A | FPGA_HF_ISO14443A_TAGSIM_LISTEN);

    // Now run a `software UART` on the stream of incoming samples.
    Uart14aInit(received, par);

    // clear RXRDY:
    uint8_t b = (uint8_t)AT91C_BASE_SSC->SSC_RHR;
    (void)b;

    uint16_t check = 0;

    for (;;) {
        if (check == 1000) {
            if (BUTTON_PRESS() || data_available())
                return false;
            check = 0;
        }
        ++check;
        WDT_HIT();

        if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_RXRDY)) {
            b = (uint8_t)AT91C_BASE_SSC->SSC_RHR;
            if (MillerDecoding(b, 0)) {
                *len = Uart.len;
                return true;
            }
        }
    }
    return false;
}

static bool prepare_tag_modulation(tag_response_info_t *response_info, size_t max_buffer_size)  {
    // Example response, answer to MIFARE Classic read block will be 16 bytes + 2 CRC = 18 bytes
    // This will need the following byte array for a modulation sequence
    //    144        data bits (18 * 8)
    //     18        parity bits
    //      2        Start and stop
    //      1        Correction bit (Answer in 1172 or 1236 periods, see FPGA)
    //      1        just for the case
    // ----------- +
    //    166 bytes, since every bit that needs to be send costs us a byte
    //
    // Prepare the tag modulation bits from the message
    CodeIso14443aAsTag(response_info->response, response_info->response_n);

    // Make sure we do not exceed the free buffer space
    if (ToSendMax > max_buffer_size) {
        Dbprintf("Out of memory, when modulating bits for tag answer:");
        Dbhexdump(response_info->response_n, response_info->response, false);
        return false;
    }

    // Copy the byte array, used for this modulation to the buffer position
    memcpy(response_info->modulation, ToSend, ToSendMax);

    // Store the number of bytes that were used for encoding/modulation and the time needed to transfer them
    response_info->modulation_n = ToSendMax;
    response_info->ProxToAirDuration = LastProxToAirDuration;
    return true;
}

bool prepare_allocated_tag_modulation(tag_response_info_t *response_info, uint8_t **buffer, size_t *max_buffer_size) {

    // Retrieve and store the current buffer index
    response_info->modulation = *buffer;

    // Forward the prepare tag modulation function to the inner function
    if (prepare_tag_modulation(response_info, *max_buffer_size)) {
        // Update the free buffer offset and the remaining buffer size
        *buffer += ToSendMax;
        *max_buffer_size -= ToSendMax;
        return true;
    } else {
        return false;
    }
}

static bool SimulateIso14443aInit(int tagType, int flags, uint8_t *data, tag_response_info_t **responses, uint32_t *cuid, uint32_t counters[3], uint8_t tearings[3], uint8_t *pages) {
    uint8_t sak = 0;
    // The first response contains the ATQA (note: bytes are transmitted in reverse order).
    static uint8_t rATQA[2] = { 0x00 };
    // The second response contains the (mandatory) first 24 bits of the UID
    static uint8_t rUIDc1[5] = { 0x00 };
    // For UID size 7,
    static uint8_t rUIDc2[5] = { 0x00 };
    // Prepare the mandatory SAK (for 4 and 7 byte UID)
    static uint8_t rSAKc1[3]  = { 0x00 };
    // Prepare the optional second SAK (for 7 byte UID), drop the cascade bit
    static uint8_t rSAKc2[3]  = { 0x00 };
    // dummy ATS (pseudo-ATR), answer to RATS
    static uint8_t rRATS[] = { 0x04, 0x58, 0x80, 0x02, 0x00, 0x00 };
    // GET_VERSION response for EV1/NTAG
    static uint8_t rVERSION[10] = { 0x00 };
    // READ_SIG response for EV1/NTAG
    static uint8_t rSIGN[34] = { 0x00 };

    switch (tagType) {
        case 1: { // MIFARE Classic 1k
            rATQA[0] = 0x04;
            sak = 0x08;
        }
        break;
        case 2: { // MIFARE Ultralight
            rATQA[0] = 0x44;
            sak = 0x00;
            // some first pages of UL/NTAG dump is special data
            mfu_dump_t *mfu_header = (mfu_dump_t *) BigBuf_get_EM_addr();
            *pages = MAX(mfu_header->pages, 15);
        }
        break;
        case 3: { // MIFARE DESFire
            rATQA[0] = 0x04;
            rATQA[1] = 0x03;
            sak = 0x20;
        }
        break;
        case 4: { // ISO/IEC 14443-4 - javacard (JCOP)
            rATQA[0] = 0x04;
            sak = 0x28;
        }
        break;
        case 5: { // MIFARE TNP3XXX
            rATQA[0] = 0x01;
            rATQA[1] = 0x0f;
            sak = 0x01;
        }
        break;
        case 6: { // MIFARE Mini 320b
            rATQA[0] = 0x44;
            sak = 0x09;
        }
        break;
        case 7: { // NTAG
            rATQA[0] = 0x44;
            sak = 0x00;
            // some first pages of UL/NTAG dump is special data
            mfu_dump_t *mfu_header = (mfu_dump_t *) BigBuf_get_EM_addr();
            *pages = MAX(mfu_header->pages, 19);
            // counters and tearing flags
            for (int i = 0; i < 3; i++) {
                counters[i] = le24toh(mfu_header->counter_tearing[i]);
                tearings[i] = mfu_header->counter_tearing[i][3];
            }
            // GET_VERSION
            memcpy(rVERSION, mfu_header->version, 8);
            AddCrc14A(rVERSION, sizeof(rVERSION) - 2);
            // READ_SIG
            memcpy(rSIGN, mfu_header->signature, 32);
            AddCrc14A(rSIGN, sizeof(rSIGN) - 2);
        }
        break;
        case 8: { // MIFARE Classic 4k
            rATQA[0] = 0x02;
            sak = 0x18;
        }
        break;
        case 9 : { // FM11RF005SH (Shanghai Metro)
            rATQA[0] = 0x03;
            rATQA[1] = 0x00;
            sak = 0x0A;
        }
        break;
        default: {
            if (DBGLEVEL >= DBG_ERROR) Dbprintf("Error: unknown tagtype (%d)", tagType);
            return false;
        }
        break;
    }

    // if uid not supplied then get from emulator memory
    if (data[0] == 0 || (flags & FLAG_UID_IN_EMUL) == FLAG_UID_IN_EMUL) {
        if (tagType == 2 || tagType == 7) {
            uint16_t start = MFU_DUMP_PREFIX_LENGTH;
            uint8_t emdata[8];
            emlGetMemBt(emdata, start, sizeof(emdata));
            memcpy(data, emdata, 3); // uid bytes 0-2
            memcpy(data + 3, emdata + 4, 4); // uid bytes 3-7
            flags |= FLAG_7B_UID_IN_DATA;
        } else {
            emlGetMemBt(data, 0, 4);
            flags |= FLAG_4B_UID_IN_DATA;
        }
    }

    if ((flags & FLAG_7B_UID_IN_DATA) == FLAG_7B_UID_IN_DATA) {
        rUIDc1[0] = 0x88;  // Cascade Tag marker
        rUIDc1[1] = data[0];
        rUIDc1[2] = data[1];
        rUIDc1[3] = data[2];

        rUIDc2[0] = data[3];
        rUIDc2[1] = data[4];
        rUIDc2[2] = data[5];
        rUIDc2[3] = data[6];
        rUIDc2[4] = rUIDc2[0] ^ rUIDc2[1] ^ rUIDc2[2] ^ rUIDc2[3];

        // Configure the ATQA and SAK accordingly
        rATQA[0] |= 0x40;
        sak |= 0x04;

        *cuid = bytes_to_num(data + 3, 4);
    } else if ((flags & FLAG_4B_UID_IN_DATA) == FLAG_4B_UID_IN_DATA) {
        memcpy(rUIDc1, data, 4);
        // Configure the ATQA and SAK accordingly
        rATQA[0] &= 0xBF;
        sak &= 0xFB;
        *cuid = bytes_to_num(data, 4);
    } else {
        if (DBGLEVEL >= DBG_ERROR) Dbprintf("[-] ERROR: UID size not defined");
        return false;
    }

    // Calculate the BitCountCheck (BCC) for the first 4 bytes of the UID.
    rUIDc1[4] = rUIDc1[0] ^ rUIDc1[1] ^ rUIDc1[2] ^ rUIDc1[3];

    rSAKc1[0] = sak;
    AddCrc14A(rSAKc1, sizeof(rSAKc1) - 2);

    rSAKc2[0] = sak & 0xFB;
    AddCrc14A(rSAKc2, sizeof(rSAKc2) - 2);

    // Format byte = 0x58: FSCI=0x08 (FSC=256), TA(1) and TC(1) present,
    // TA(1) = 0x80: different divisors not supported, DR = 1, DS = 1
    // TB(1) = not present. Defaults: FWI = 4 (FWT = 256 * 16 * 2^4 * 1/fc = 4833us), SFGI = 0 (SFG = 256 * 16 * 2^0 * 1/fc = 302us)
    // TC(1) = 0x02: CID supported, NAD not supported
    AddCrc14A(rRATS, sizeof(rRATS) - 2);

#define TAG_RESPONSE_COUNT 8
    static tag_response_info_t responses_init[TAG_RESPONSE_COUNT] = {
        { .response = rATQA,      .response_n = sizeof(rATQA)      },  // Answer to request - respond with card type
        { .response = rUIDc1,     .response_n = sizeof(rUIDc1)     },  // Anticollision cascade1 - respond with uid
        { .response = rUIDc2,     .response_n = sizeof(rUIDc2)     },  // Anticollision cascade2 - respond with 2nd half of uid if asked
        { .response = rSAKc1,     .response_n = sizeof(rSAKc1)     },  // Acknowledge select - cascade 1
        { .response = rSAKc2,     .response_n = sizeof(rSAKc2)     },  // Acknowledge select - cascade 2
        { .response = rRATS,      .response_n = sizeof(rRATS)      },  // dummy ATS (pseudo-ATR), answer to RATS
        { .response = rVERSION,   .response_n = sizeof(rVERSION)   },  // EV1/NTAG GET_VERSION response
        { .response = rSIGN,      .response_n = sizeof(rSIGN)      }   // EV1/NTAG READ_SIG response
    };

    // "precompile" responses. There are 8 predefined responses with a total of 68 bytes data to transmit.
    // Coded responses need one byte per bit to transfer (data, parity, start, stop, correction)
    // 68 * 8 data bits, 68 * 1 parity bits, 8 start bits, 8 stop bits, 8 correction bits -- 636 bytes buffer
#define ALLOCATED_TAG_MODULATION_BUFFER_SIZE 636

    uint8_t *free_buffer = BigBuf_malloc(ALLOCATED_TAG_MODULATION_BUFFER_SIZE);
    // modulation buffer pointer and current buffer free space size
    uint8_t *free_buffer_pointer = free_buffer;
    size_t free_buffer_size = ALLOCATED_TAG_MODULATION_BUFFER_SIZE;

    // Prepare the responses of the anticollision phase
    // there will be not enough time to do this at the moment the reader sends it REQA
    for (size_t i = 0; i < TAG_RESPONSE_COUNT; i++) {
        if (prepare_allocated_tag_modulation(&responses_init[i], &free_buffer_pointer, &free_buffer_size) == false) {
            BigBuf_free_keep_EM();
            if (DBGLEVEL >= DBG_ERROR)    Dbprintf("Not enough modulation buffer size, exit after %d elements", i);
            return false;
        }
    }

    *responses = responses_init;

    // indices into responses array:
#define ATQA      0
#define UIDC1     1
#define UIDC2     2
#define SAKC1     3
#define SAKC2     4
#define RATS      5
#define VERSION   6
#define SIGNATURE 7

    return true;
}

//-----------------------------------------------------------------------------
// Main loop of simulated tag: receive commands from reader, decide what
// response to send, and send it.
// 'hf 14a sim'
//-----------------------------------------------------------------------------
void SimulateIso14443aTag(uint8_t tagType, uint8_t flags, uint8_t *data) {

#define ATTACK_KEY_COUNT 8 // keep same as define in cmdhfmf.c -> readerAttack()

    tag_response_info_t *responses;
    uint32_t cuid = 0;
    uint32_t nonce = 0;
    uint32_t counters[3] = { 0x00, 0x00, 0x00 };
    uint8_t tearings[3] = { 0xbd, 0xbd, 0xbd };
    uint8_t pages = 0;

    // Here, we collect CUID, block1, keytype1, NT1, NR1, AR1, CUID, block2, keytyp2, NT2, NR2, AR2
    // it should also collect block, keytype.
    uint8_t cardAUTHSC = 0;
    uint8_t cardAUTHKEY = 0xff;  // no authentication
    // allow collecting up to 8 sets of nonces to allow recovery of up to 8 keys

    nonces_t ar_nr_nonces[ATTACK_KEY_COUNT]; // for attack types moebius
    memset(ar_nr_nonces, 0x00, sizeof(ar_nr_nonces));
    uint8_t moebius_count = 0;

    // command buffers
    uint8_t receivedCmd[MAX_FRAME_SIZE] = { 0x00 };
    uint8_t receivedCmdPar[MAX_PARITY_SIZE] = { 0x00 };

    // Allocate 512 bytes for the dynamic modulation, created when the reader queries for it
    // Such a response is less time critical, so we can prepare them on the fly
#define DYNAMIC_RESPONSE_BUFFER_SIZE 64
#define DYNAMIC_MODULATION_BUFFER_SIZE 512
    uint8_t dynamic_response_buffer[DYNAMIC_RESPONSE_BUFFER_SIZE];
    uint8_t dynamic_modulation_buffer[DYNAMIC_MODULATION_BUFFER_SIZE];
    tag_response_info_t dynamic_response_info = {
        .response = dynamic_response_buffer,
        .response_n = 0,
        .modulation = dynamic_modulation_buffer,
        .modulation_n = 0
    };

    // free eventually allocated BigBuf memory but keep Emulator Memory
    BigBuf_free_keep_EM();

    if (SimulateIso14443aInit(tagType, flags, data, &responses, &cuid, counters, tearings, &pages) == false) {
        BigBuf_free_keep_EM();
        reply_ng(CMD_HF_MIFARE_SIMULATE, PM3_EINIT, NULL, 0);
        return;
    }

    // We need to listen to the high-frequency, peak-detected path.
    iso14443a_setup(FPGA_HF_ISO14443A_TAGSIM_LISTEN);

    int len = 0;

    // To control where we are in the protocol
#define ORDER_NONE           0
#define ORDER_REQA           1
#define ORDER_SELECT_ALL_CL1 2
#define ORDER_SELECT_CL1     3
#define ORDER_HALTED         5
#define ORDER_WUPA           6
#define ORDER_AUTH           7
#define ORDER_SELECT_ALL_CL2 20
#define ORDER_SELECT_CL2     30
#define ORDER_EV1_COMP_WRITE 40
#define ORDER_RATS           70
    uint8_t order = ORDER_NONE;

    int retval = PM3_SUCCESS;

    // Just to allow some checks
    int happened = 0;
    int happened2 = 0;
    int cmdsRecvd = 0;

    // compatible write block number
    uint8_t wrblock = 0;

    clear_trace();
    set_tracing(true);
    LED_A_ON();
    for (;;) {
        WDT_HIT();

        // Clean receive command buffer
        if (!GetIso14443aCommandFromReader(receivedCmd, receivedCmdPar, &len)) {
            Dbprintf("Emulator stopped.  Trace length: %d ", BigBuf_get_traceLen());
            retval = PM3_EOPABORTED;
            break;
        }
        tag_response_info_t *p_response = NULL;

        // Okay, look at the command now.
        int lastorder = order;

        //
        // we need to check "ordered" states before, because received data may be same to any command - is wrong!!!
        //

        if (order == ORDER_EV1_COMP_WRITE && len == 18) {
            // MIFARE_ULC_COMP_WRITE part 2
            // 16 bytes data + 2 bytes crc, only least significant 4 bytes are written
            bool isCrcCorrect = CheckCrc14A(receivedCmd, len);
            if (isCrcCorrect) {
                // first blocks of emu are header
                emlSetMem_xt(receivedCmd, wrblock + MFU_DUMP_PREFIX_LENGTH / 4, 1, 4);
                // send ACK
                EmSend4bit(CARD_ACK);
            } else {
                // send NACK 0x1 == crc/parity error
                EmSend4bit(CARD_NACK_PA);
            }
            order = ORDER_NONE; // back to work state
            p_response = NULL;

        } else if (order == ORDER_AUTH && len == 8) {
            // Received {nr] and {ar} (part of authentication)
            LogTrace(receivedCmd, Uart.len, Uart.startTime * 16 - DELAY_AIR2ARM_AS_TAG, Uart.endTime * 16 - DELAY_AIR2ARM_AS_TAG, Uart.parity, true);
            uint32_t nr = bytes_to_num(receivedCmd, 4);
            uint32_t ar = bytes_to_num(receivedCmd + 4, 4);

            // Collect AR/NR per keytype & sector
            if ((flags & FLAG_NR_AR_ATTACK) == FLAG_NR_AR_ATTACK) {

                int8_t index = -1;
                int8_t empty = -1;
                for (uint8_t i = 0; i < ATTACK_KEY_COUNT; i++) {
                    // find which index to use
                    if ((cardAUTHSC == ar_nr_nonces[i].sector) && (cardAUTHKEY == ar_nr_nonces[i].keytype))
                        index = i;

                    // keep track of empty slots.
                    if (ar_nr_nonces[i].state == EMPTY)
                        empty = i;
                }
                // if no empty slots.  Choose first and overwrite.
                if (index == -1) {
                    if (empty == -1) {
                        index = 0;
                        ar_nr_nonces[index].state = EMPTY;
                    } else {
                        index = empty;
                    }
                }

                switch (ar_nr_nonces[index].state) {
                    case EMPTY: {
                        // first nonce collect
                        ar_nr_nonces[index].cuid = cuid;
                        ar_nr_nonces[index].sector = cardAUTHSC;
                        ar_nr_nonces[index].keytype = cardAUTHKEY;
                        ar_nr_nonces[index].nonce = nonce;
                        ar_nr_nonces[index].nr = nr;
                        ar_nr_nonces[index].ar = ar;
                        ar_nr_nonces[index].state = FIRST;
                        break;
                    }
                    case FIRST : {
                        // second nonce collect
                        ar_nr_nonces[index].nonce2 = nonce;
                        ar_nr_nonces[index].nr2 = nr;
                        ar_nr_nonces[index].ar2 = ar;
                        ar_nr_nonces[index].state = SECOND;

                        // send to client  (one struct nonces_t)
                        reply_ng(CMD_HF_MIFARE_SIMULATE, PM3_SUCCESS, (uint8_t *)&ar_nr_nonces[index], sizeof(nonces_t));

                        ar_nr_nonces[index].state = EMPTY;
                        ar_nr_nonces[index].sector = 0;
                        ar_nr_nonces[index].keytype = 0;

                        moebius_count++;
                        break;
                    }
                    default:
                        break;
                }
            }
            order = ORDER_NONE; // back to work state
            p_response = NULL;

            //
            // now check commands in received buffer
            //

        } else if (receivedCmd[0] == ISO14443A_CMD_REQA && len == 1) { // Received a REQUEST
            p_response = &responses[ATQA];
            order = ORDER_REQA;
        } else if (receivedCmd[0] == ISO14443A_CMD_WUPA && len == 1) { // Received a WAKEUP
            p_response = &responses[ATQA];
            order = ORDER_WUPA;
        } else if (receivedCmd[1] == 0x20 && receivedCmd[0] == ISO14443A_CMD_ANTICOLL_OR_SELECT && len == 2) {    // Received request for UID (cascade 1)
            p_response = &responses[UIDC1];
            order = ORDER_SELECT_ALL_CL1;
        } else if (receivedCmd[1] == 0x20 && receivedCmd[0] == ISO14443A_CMD_ANTICOLL_OR_SELECT_2 && len == 2) {  // Received request for UID (cascade 2)
            p_response = &responses[UIDC2];
            order = ORDER_SELECT_ALL_CL2;
        } else if (receivedCmd[1] == 0x70 && receivedCmd[0] == ISO14443A_CMD_ANTICOLL_OR_SELECT && len == 9) {    // Received a SELECT (cascade 1)
            p_response = &responses[SAKC1];
            order = ORDER_SELECT_CL1;
        } else if (receivedCmd[1] == 0x70 && receivedCmd[0] == ISO14443A_CMD_ANTICOLL_OR_SELECT_2 && len == 9) {  // Received a SELECT (cascade 2)
            p_response = &responses[SAKC2];
            order = ORDER_SELECT_CL2;
        } else if (receivedCmd[0] == ISO14443A_CMD_READBLOCK && len == 4) {    // Received a (plain) READ
            uint8_t block = receivedCmd[1];
            // if Ultralight or NTAG (4 byte blocks)
            if (tagType == 7 || tagType == 2) {
                if (block > pages) {
                    // send NACK 0x0 == invalid argument
                    EmSend4bit(CARD_NACK_IV);
                } else {
                    // first blocks of emu are header
                    uint16_t start = block * 4 + MFU_DUMP_PREFIX_LENGTH;
                    uint8_t emdata[MAX_MIFARE_FRAME_SIZE];
                    emlGetMemBt(emdata, start, 16);
                    AddCrc14A(emdata, 16);
                    EmSendCmd(emdata, sizeof(emdata));
                }
                // We already responded, do not send anything with the EmSendCmd14443aRaw() that is called below
                p_response = NULL;
            } else if (tagType == 9 && block == 1) {
                // FM11005SH.   16blocks,  4bytes / block.
                //   block0 = 2byte Customer ID (CID), 2byte Manufacture ID (MID)
                //   block1 = 4byte UID.
                p_response = &responses[UIDC1];
            } else { // all other tags (16 byte block tags)
                uint8_t emdata[MAX_MIFARE_FRAME_SIZE];
                emlGetMemBt(emdata, block, 16);
                AddCrc14A(emdata, 16);
                EmSendCmd(emdata, sizeof(emdata));
                // EmSendCmd(data+(4*receivedCmd[1]),16);
                // Dbprintf("Read request from reader: %x %x",receivedCmd[0],receivedCmd[1]);
                // We already responded, do not send anything with the EmSendCmd14443aRaw() that is called below
                p_response = NULL;
            }
        } else if (receivedCmd[0] == MIFARE_ULEV1_FASTREAD && len == 5) {    // Received a FAST READ (ranged read)
            uint8_t block1 = receivedCmd[1];
            uint8_t block2 = receivedCmd[2];
            if (block1 > pages) {
                // send NACK 0x0 == invalid argument
                EmSend4bit(CARD_NACK_IV);
            } else {
                uint8_t emdata[MAX_FRAME_SIZE];
                // first blocks of emu are header
                int start = block1 * 4 + MFU_DUMP_PREFIX_LENGTH;
                len   = (block2 - block1 + 1) * 4;
                emlGetMemBt(emdata, start, len);
                AddCrc14A(emdata, len);
                EmSendCmd(emdata, len + 2);
            }
            p_response = NULL;
        } else if (receivedCmd[0] == MIFARE_ULC_WRITE && len == 8 && (tagType == 2 || tagType == 7)) {        // Received a WRITE
            // cmd + block + 4 bytes data + 2 bytes crc
            bool isCrcCorrect = CheckCrc14A(receivedCmd, len);
            if (isCrcCorrect) {
                uint8_t block = receivedCmd[1];
                if (block > pages) {
                    // send NACK 0x0 == invalid argument
                    EmSend4bit(CARD_NACK_IV);
                } else {
                    // first blocks of emu are header
                    emlSetMem_xt(&receivedCmd[2], block + MFU_DUMP_PREFIX_LENGTH / 4, 1, 4);
                    // send ACK
                    EmSend4bit(CARD_ACK);
                }
            } else {
                // send NACK 0x1 == crc/parity error
                EmSend4bit(CARD_NACK_PA);
            }
            p_response = NULL;
        } else if (receivedCmd[0] == MIFARE_ULC_COMP_WRITE && len == 4 && (tagType == 2 || tagType == 7)) {
            // cmd + block + 2 bytes crc
            bool isCrcCorrect = CheckCrc14A(receivedCmd, len);
            if (isCrcCorrect) {
                wrblock = receivedCmd[1];
                if (wrblock > pages) {
                    // send NACK 0x0 == invalid argument
                    EmSend4bit(CARD_NACK_IV);
                } else {
                    // send ACK
                    EmSend4bit(CARD_ACK);
                    // go to part 2
                    order = ORDER_EV1_COMP_WRITE;
                }
            } else {
                // send NACK 0x1 == crc/parity error
                EmSend4bit(CARD_NACK_PA);
            }
            p_response = NULL;
        } else if (receivedCmd[0] == MIFARE_ULEV1_READSIG && len == 4 && tagType == 7) {    // Received a READ SIGNATURE --
            p_response = &responses[SIGNATURE];
        } else if (receivedCmd[0] == MIFARE_ULEV1_READ_CNT && len == 4 && tagType == 7) {    // Received a READ COUNTER --
            uint8_t index = receivedCmd[1];
            if (index > 2) {
                // send NACK 0x0 == invalid argument
                EmSend4bit(CARD_NACK_IV);
            } else {
                uint8_t cmd[] =  {0x00, 0x00, 0x00, 0x14, 0xa5};
                htole24(counters[index], cmd);
                AddCrc14A(cmd, sizeof(cmd) - 2);
                EmSendCmd(cmd, sizeof(cmd));
            }
            p_response = NULL;
        } else if (receivedCmd[0] == MIFARE_ULEV1_INCR_CNT && len == 8 && tagType == 7) {    // Received a INC COUNTER --
            uint8_t index = receivedCmd[1];
            if (index > 2) {
                // send NACK 0x0 == invalid argument
                EmSend4bit(CARD_NACK_IV);
            } else {
                uint32_t val = le24toh(receivedCmd + 2) + counters[index];
                // if new value + old value is bigger 24bits,  fail
                if (val > 0xFFFFFF) {
                    // send NACK 0x4 == counter overflow
                    EmSend4bit(CARD_NACK_NA);
                } else {
                    counters[index] = val;
                    // send ACK
                    EmSend4bit(CARD_ACK);
                }
            }
            p_response = NULL;
        } else if (receivedCmd[0] == MIFARE_ULEV1_CHECKTEAR && len == 4 && tagType == 7) {    // Received a CHECK_TEARING_EVENT --
            // first 12 blocks of emu are [getversion answer - check tearing - pack - 0x00 - signature]
            uint8_t index = receivedCmd[1];
            if (index > 2) {
                // send NACK 0x0 == invalid argument
                EmSend4bit(CARD_NACK_IV);
            } else {
                uint8_t cmd[3];
                cmd[0] = tearings[index];
                AddCrc14A(cmd, sizeof(cmd) - 2);
                EmSendCmd(cmd, sizeof(cmd));
            }
            p_response = NULL;
        } else if (receivedCmd[0] == ISO14443A_CMD_HALT && len == 4) {    // Received a HALT
            LogTrace(receivedCmd, Uart.len, Uart.startTime * 16 - DELAY_AIR2ARM_AS_TAG, Uart.endTime * 16 - DELAY_AIR2ARM_AS_TAG, Uart.parity, true);
            p_response = NULL;
            order = ORDER_HALTED;
        } else if (receivedCmd[0] == MIFARE_ULEV1_VERSION && len == 3 && (tagType == 2 || tagType == 7)) {
            p_response = &responses[VERSION];
        } else if ((receivedCmd[0] == MIFARE_AUTH_KEYA || receivedCmd[0] == MIFARE_AUTH_KEYB) && len == 4 && tagType != 2 && tagType != 7) {    // Received an authentication request
            cardAUTHKEY = receivedCmd[0] - 0x60;
            cardAUTHSC = receivedCmd[1] / 4; // received block num

            // incease nonce at AUTH requests. this is time consuming.
            nonce = prng_successor(GetTickCount(), 32);
            num_to_bytes(nonce, 4, dynamic_response_info.response);
            dynamic_response_info.response_n = 4;

            prepare_tag_modulation(&dynamic_response_info, DYNAMIC_MODULATION_BUFFER_SIZE);
            p_response = &dynamic_response_info;
            order = ORDER_AUTH;
        } else if (receivedCmd[0] == ISO14443A_CMD_RATS && len == 4) {    // Received a RATS request
            if (tagType == 1 || tagType == 2) {    // RATS not supported
                EmSend4bit(CARD_NACK_NA);
                p_response = NULL;
            } else {
                p_response = &responses[RATS];
                order = ORDER_RATS;
            }
        } else if (receivedCmd[0] == MIFARE_ULC_AUTH_1) {  // ULC authentication, or Desfire Authentication
            LogTrace(receivedCmd, Uart.len, Uart.startTime * 16 - DELAY_AIR2ARM_AS_TAG, Uart.endTime * 16 - DELAY_AIR2ARM_AS_TAG, Uart.parity, true);
            p_response = NULL;
        } else if (receivedCmd[0] == MIFARE_ULEV1_AUTH && len == 7 && tagType == 7) { // NTAG / EV-1 authentication
            // PWD stored in dump now
            uint8_t pwd[4];
            emlGetMemBt(pwd, (pages - 1) * 4 + MFU_DUMP_PREFIX_LENGTH, sizeof(pwd));
            if (memcmp(receivedCmd + 1, pwd, 4) == 0) {
                uint8_t cmd[4];
                emlGetMemBt(cmd, pages * 4 + MFU_DUMP_PREFIX_LENGTH, 2);
                AddCrc14A(cmd, sizeof(cmd) - 2);
                EmSendCmd(cmd, sizeof(cmd));
            } else {
                EmSend4bit(CARD_NACK_NA);
                if (DBGLEVEL >= DBG_DEBUG) Dbprintf("Auth attempt: %08x", bytes_to_num(receivedCmd + 1, 4));
            }
            p_response = NULL;
        } else if (receivedCmd[0] == MIFARE_ULEV1_VCSL && len == 23 && tagType == 7) {
            uint8_t cmd[3];
            emlGetMemBt(cmd, (pages - 2) * 4 + 1 + MFU_DUMP_PREFIX_LENGTH, 1);
            AddCrc14A(cmd, sizeof(cmd) - 2);
            EmSendCmd(cmd, sizeof(cmd));
            p_response = NULL;
        } else {
            // Check for ISO 14443A-4 compliant commands, look at left nibble
            switch (receivedCmd[0]) {
                case 0x02:
                case 0x03: {  // IBlock (command no CID)
                    dynamic_response_info.response[0] = receivedCmd[0];
                    dynamic_response_info.response[1] = 0x90;
                    dynamic_response_info.response[2] = 0x00;
                    dynamic_response_info.response_n = 3;
                }
                break;
                case 0x0B:
                case 0x0A: { // IBlock (command CID)
                    dynamic_response_info.response[0] = receivedCmd[0];
                    dynamic_response_info.response[1] = 0x00;
                    dynamic_response_info.response[2] = 0x90;
                    dynamic_response_info.response[3] = 0x00;
                    dynamic_response_info.response_n = 4;
                }
                break;

                case 0x1A:
                case 0x1B: { // Chaining command
                    dynamic_response_info.response[0] = 0xaa | ((receivedCmd[0]) & 1);
                    dynamic_response_info.response_n = 2;
                }
                break;

                case 0xAA:
                case 0xBB: {
                    dynamic_response_info.response[0] = receivedCmd[0] ^ 0x11;
                    dynamic_response_info.response_n = 2;
                }
                break;

                case 0xBA: { // ping / pong
                    dynamic_response_info.response[0] = 0xAB;
                    dynamic_response_info.response[1] = 0x00;
                    dynamic_response_info.response_n = 2;
                }
                break;

                case 0xCA:
                case 0xC2: { // Readers sends deselect command
                    dynamic_response_info.response[0] = 0xCA;
                    dynamic_response_info.response[1] = 0x00;
                    dynamic_response_info.response_n = 2;
                }
                break;

                default: {
                    // Never seen this command before
                    LogTrace(receivedCmd, Uart.len, Uart.startTime * 16 - DELAY_AIR2ARM_AS_TAG, Uart.endTime * 16 - DELAY_AIR2ARM_AS_TAG, Uart.parity, true);
                    if (DBGLEVEL >= DBG_DEBUG) {
                        Dbprintf("Received unknown command (len=%d):", len);
                        Dbhexdump(len, receivedCmd, false);
                    }
                    // Do not respond
                    dynamic_response_info.response_n = 0;
                    order = ORDER_NONE; // back to work state
                }
                break;
            }

            if (dynamic_response_info.response_n > 0) {
                // Copy the CID from the reader query
                dynamic_response_info.response[1] = receivedCmd[1];

                // Add CRC bytes, always used in ISO 14443A-4 compliant cards
                AddCrc14A(dynamic_response_info.response, dynamic_response_info.response_n);
                dynamic_response_info.response_n += 2;

                if (prepare_tag_modulation(&dynamic_response_info, DYNAMIC_MODULATION_BUFFER_SIZE) == false) {
                    if (DBGLEVEL >= DBG_DEBUG) DbpString("Error preparing tag response");
                    LogTrace(receivedCmd, Uart.len, Uart.startTime * 16 - DELAY_AIR2ARM_AS_TAG, Uart.endTime * 16 - DELAY_AIR2ARM_AS_TAG, Uart.parity, true);
                    break;
                }
                p_response = &dynamic_response_info;
            }
        }

        // Count number of wakeups received after a halt
        if (order == ORDER_WUPA && lastorder == ORDER_HALTED) { happened++; }

        // Count number of other messages after a halt
        if (order != ORDER_WUPA && lastorder == ORDER_HALTED) { happened2++; }

        cmdsRecvd++;

        if (p_response != NULL) {
            EmSendPrecompiledCmd(p_response);
        }
    }

    switch_off();

    set_tracing(false);
    BigBuf_free_keep_EM();

    if (DBGLEVEL >= DBG_EXTENDED) {
        Dbprintf("-[ Wake ups after halt  [%d]", happened);
        Dbprintf("-[ Messages after halt  [%d]", happened2);
        Dbprintf("-[ Num of received cmd  [%d]", cmdsRecvd);
        Dbprintf("-[ Num of moebius tries [%d]", moebius_count);
    }

    reply_ng(CMD_HF_MIFARE_SIMULATE, retval, NULL, 0);
}

// prepare a delayed transfer. This simply shifts ToSend[] by a number
// of bits specified in the delay parameter.
void PrepareDelayedTransfer(uint16_t delay) {
    delay &= 0x07;
    if (!delay) return;

    uint8_t bitmask = 0;
    uint8_t bits_shifted = 0;

    for (uint16_t i = 0; i < delay; i++)
        bitmask |= (0x01 << i);

    ToSend[ToSendMax++] = 0x00;

    for (uint16_t i = 0; i < ToSendMax; i++) {
        uint8_t bits_to_shift = ToSend[i] & bitmask;
        ToSend[i] = ToSend[i] >> delay;
        ToSend[i] = ToSend[i] | (bits_shifted << (8 - delay));
        bits_shifted = bits_to_shift;
    }
}


//-------------------------------------------------------------------------------------
// Transmit the command (to the tag) that was placed in ToSend[].
// Parameter timing:
// if NULL: transfer at next possible time, taking into account
//             request guard time and frame delay time
// if == 0:    transfer immediately and return time of transfer
// if != 0: delay transfer until time specified
//-------------------------------------------------------------------------------------
static void TransmitFor14443a(const uint8_t *cmd, uint16_t len, uint32_t *timing) {

    if (!hf_field_active)
        return;

    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_ISO14443A | FPGA_HF_ISO14443A_READER_MOD);

    if (timing) {
        if (*timing == 0)                                        // Measure time
            *timing = (GetCountSspClk() + 8) & 0xfffffff8;
        else
            PrepareDelayedTransfer(*timing & 0x00000007);        // Delay transfer (fine tuning - up to 7 MF clock ticks)

        if (DBGLEVEL >= DBG_EXTENDED && GetCountSspClk() >= (*timing & 0xfffffff8))
            Dbprintf("TransmitFor14443a: Missed timing");
        while (GetCountSspClk() < (*timing & 0xfffffff8)) {};    // Delay transfer (multiple of 8 MF clock ticks)
        LastTimeProxToAirStart = *timing;
    } else {

        uint32_t ThisTransferTime = 0;
        ThisTransferTime = ((MAX(NextTransferTime, GetCountSspClk()) & 0xfffffff8) + 8);

        while (GetCountSspClk() < ThisTransferTime) {};

        LastTimeProxToAirStart = ThisTransferTime;
    }

    // clear TXRDY
    AT91C_BASE_SSC->SSC_THR = SEC_Y;

    uint16_t c = 0;
    while (c < len) {
        if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_TXRDY)) {
            AT91C_BASE_SSC->SSC_THR = cmd[c];
            c++;
        }
    }

    NextTransferTime = MAX(NextTransferTime, LastTimeProxToAirStart + REQUEST_GUARD_TIME);
}

//-----------------------------------------------------------------------------
// Prepare reader command (in bits, support short frames) to send to FPGA
//-----------------------------------------------------------------------------
void CodeIso14443aBitsAsReaderPar(const uint8_t *cmd, uint16_t bits, const uint8_t *par) {
    int last = 0;

    ToSendReset();

    // Start of Communication (Seq. Z)
    ToSend[++ToSendMax] = SEC_Z;
    LastProxToAirDuration = 8 * (ToSendMax + 1) - 6;

    size_t bytecount = nbytes(bits);
    // Generate send structure for the data bits
    for (int i = 0; i < bytecount; i++) {
        // Get the current byte to send
        uint8_t b = cmd[i];
        size_t bitsleft = MIN((bits - (i * 8)), 8);
        int j;
        for (j = 0; j < bitsleft; j++) {
            if (b & 1) {
                // Sequence X
                ToSend[++ToSendMax] = SEC_X;
                LastProxToAirDuration = 8 * (ToSendMax + 1) - 2;
                last = 1;
            } else {
                if (last == 0) {
                    // Sequence Z
                    ToSend[++ToSendMax] = SEC_Z;
                    LastProxToAirDuration = 8 * (ToSendMax + 1) - 6;
                } else {
                    // Sequence Y
                    ToSend[++ToSendMax] = SEC_Y;
                    last = 0;
                }
            }
            b >>= 1;
        }

        // Only transmit parity bit if we transmitted a complete byte
        if (j == 8 && par != NULL) {
            // Get the parity bit
            if (par[i >> 3] & (0x80 >> (i & 0x0007))) {
                // Sequence X
                ToSend[++ToSendMax] = SEC_X;
                LastProxToAirDuration = 8 * (ToSendMax + 1) - 2;
                last = 1;
            } else {
                if (last == 0) {
                    // Sequence Z
                    ToSend[++ToSendMax] = SEC_Z;
                    LastProxToAirDuration = 8 * (ToSendMax + 1) - 6;
                } else {
                    // Sequence Y
                    ToSend[++ToSendMax] = SEC_Y;
                    last = 0;
                }
            }
        }
    }

    // End of Communication: Logic 0 followed by Sequence Y
    if (last == 0) {
        // Sequence Z
        ToSend[++ToSendMax] = SEC_Z;
        LastProxToAirDuration = 8 * (ToSendMax + 1) - 6;
    } else {
        // Sequence Y
        ToSend[++ToSendMax] = SEC_Y;
    }
    ToSend[++ToSendMax] = SEC_Y;

    // Convert to length of command:
    ToSendMax++;
}

//-----------------------------------------------------------------------------
// Prepare reader command to send to FPGA
//-----------------------------------------------------------------------------
void CodeIso14443aAsReaderPar(const uint8_t *cmd, uint16_t len, const uint8_t *par) {
    CodeIso14443aBitsAsReaderPar(cmd, len * 8, par);
}

//-----------------------------------------------------------------------------
// Wait for commands from reader
// Stop when button is pressed (return 1) or field was gone (return 2)
// Or return 0 when command is captured
//-----------------------------------------------------------------------------
int EmGetCmd(uint8_t *received, uint16_t *len, uint8_t *par) {
    *len = 0;

    uint32_t timer = 0;
    int analogCnt = 0;
    int analogAVG = 0;

    // Set FPGA mode to "simulated ISO 14443 tag", no modulation (listen
    // only, since we are receiving, not transmitting).
    // Signal field is off with the appropriate LED
    LED_D_OFF();
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_ISO14443A | FPGA_HF_ISO14443A_TAGSIM_LISTEN);

    // Set ADC to read field strength
    AT91C_BASE_ADC->ADC_CR = AT91C_ADC_SWRST;
    AT91C_BASE_ADC->ADC_MR =
        ADC_MODE_PRESCALE(63) |
        ADC_MODE_STARTUP_TIME(1) |
        ADC_MODE_SAMPLE_HOLD_TIME(15);
    AT91C_BASE_ADC->ADC_CHER = ADC_CHANNEL(ADC_CHAN_HF);
    // start ADC
    AT91C_BASE_ADC->ADC_CR = AT91C_ADC_START;

    // Now run a 'software UART' on the stream of incoming samples.
    Uart14aInit(received, par);

    // Clear RXRDY:
    uint8_t b = (uint8_t)AT91C_BASE_SSC->SSC_RHR;
    (void)b;

    uint16_t check = 0;

    for (;;) {
        WDT_HIT();

        if (check == 1000) {
            if (BUTTON_PRESS() || data_available())
                return 1;
            check = 0;
        }
        ++check;

        // test if the field exists
        if (AT91C_BASE_ADC->ADC_SR & ADC_END_OF_CONVERSION(ADC_CHAN_HF)) {
            analogCnt++;
            analogAVG += AT91C_BASE_ADC->ADC_CDR[ADC_CHAN_HF];
            AT91C_BASE_ADC->ADC_CR = AT91C_ADC_START;
            if (analogCnt >= 32) {
                if ((MAX_ADC_HF_VOLTAGE_RDV40 * (analogAVG / analogCnt) >> 10) < MF_MINFIELDV) {
                    if (timer == 0) {
                        timer = GetTickCount();
                    } else {
                        // 50ms no field --> card to idle state
                        if (GetTickCountDelta(timer) > 50) {
                            return 2;
                        }
                    }
                } else {
                    timer = 0;
                }
                analogCnt = 0;
                analogAVG = 0;
            }
        }

        // receive and test the miller decoding
        if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_RXRDY)) {
            b = (uint8_t)AT91C_BASE_SSC->SSC_RHR;
            if (MillerDecoding(b, 0)) {
                *len = Uart.len;
                return 0;
            }
        }
    }
}

int EmSendCmd14443aRaw(uint8_t *resp, uint16_t respLen) {
    volatile uint8_t b;
    uint16_t i = 0;
    uint32_t ThisTransferTime;
    bool correctionNeeded;

    // Modulate Manchester
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_ISO14443A | FPGA_HF_ISO14443A_TAGSIM_MOD);

    // Include correction bit if necessary
    if (Uart.bitCount == 7) {
        // Short tags (7 bits) don't have parity, determine the correct value from MSB
        correctionNeeded = Uart.output[0] & 0x40;
    } else {
        // The parity bits are left-aligned
        correctionNeeded = Uart.parity[(Uart.len - 1) / 8] & (0x80 >> ((Uart.len - 1) & 7));
    }
    // 1236, so correction bit needed
    i = (correctionNeeded) ? 0 : 1;

    // clear receiving shift register and holding register
    while (!(AT91C_BASE_SSC->SSC_SR & AT91C_SSC_RXRDY));
    b = AT91C_BASE_SSC->SSC_RHR;
    (void) b;
    while (!(AT91C_BASE_SSC->SSC_SR & AT91C_SSC_RXRDY));
    b = AT91C_BASE_SSC->SSC_RHR;
    (void) b;

    // wait for the FPGA to signal fdt_indicator == 1 (the FPGA is ready to queue new data in its delay line)
    for (uint8_t j = 0; j < 5; j++) {    // allow timeout - better late than never
        while (!(AT91C_BASE_SSC->SSC_SR & AT91C_SSC_RXRDY));
        if (AT91C_BASE_SSC->SSC_RHR) break;
    }

    while ((ThisTransferTime = GetCountSspClk()) & 0x00000007);

    // Clear TXRDY:
    AT91C_BASE_SSC->SSC_THR = SEC_F;

    // send cycle
    for (; i < respLen;) {
        if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_TXRDY)) {
            AT91C_BASE_SSC->SSC_THR = resp[i++];
            FpgaSendQueueDelay = (uint8_t)AT91C_BASE_SSC->SSC_RHR;
        }

        if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_RXRDY)) {
            b = (uint16_t)(AT91C_BASE_SSC->SSC_RHR);
            (void)b;
        }
        if (BUTTON_PRESS()) break;
    }

    // Ensure that the FPGA Delay Queue is empty before we switch to TAGSIM_LISTEN again:
    uint8_t fpga_queued_bits = FpgaSendQueueDelay >> 3;
    for (i = 0; i <= fpga_queued_bits / 8 + 1;) {
        if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_TXRDY)) {
            AT91C_BASE_SSC->SSC_THR = SEC_F;
            FpgaSendQueueDelay = (uint8_t)AT91C_BASE_SSC->SSC_RHR;
            i++;
        }
    }
    LastTimeProxToAirStart = ThisTransferTime + (correctionNeeded ? 8 : 0);
    return 0;
}

int EmSend4bit(uint8_t resp) {
    Code4bitAnswerAsTag(resp);
    int res = EmSendCmd14443aRaw(ToSend, ToSendMax);
    // do the tracing for the previous reader request and this tag answer:
    uint8_t par[1] = {0x00};
    GetParity(&resp, 1, par);
    EmLogTrace(Uart.output,
               Uart.len,
               Uart.startTime * 16 - DELAY_AIR2ARM_AS_TAG,
               Uart.endTime * 16 - DELAY_AIR2ARM_AS_TAG,
               Uart.parity,
               &resp,
               1,
               LastTimeProxToAirStart * 16 + DELAY_ARM2AIR_AS_TAG,
               (LastTimeProxToAirStart + LastProxToAirDuration) * 16 + DELAY_ARM2AIR_AS_TAG,
               par);
    return res;
}
int EmSendCmdPar(uint8_t *resp, uint16_t respLen, uint8_t *par) {
    return EmSendCmdParEx(resp, respLen, par, false);
}
int EmSendCmdParEx(uint8_t *resp, uint16_t respLen, uint8_t *par, bool collision) {
    CodeIso14443aAsTagPar(resp, respLen, par, collision);
    int res = EmSendCmd14443aRaw(ToSend, ToSendMax);
    // do the tracing for the previous reader request and this tag answer:
    EmLogTrace(Uart.output,
               Uart.len,
               Uart.startTime * 16 - DELAY_AIR2ARM_AS_TAG,
               Uart.endTime * 16 - DELAY_AIR2ARM_AS_TAG,
               Uart.parity,
               resp,
               respLen,
               LastTimeProxToAirStart * 16 + DELAY_ARM2AIR_AS_TAG,
               (LastTimeProxToAirStart + LastProxToAirDuration) * 16 + DELAY_ARM2AIR_AS_TAG,
               par);
    return res;
}
int EmSendCmd(uint8_t *resp, uint16_t respLen) {
    return EmSendCmdEx(resp, respLen, false);
}
int EmSendCmdEx(uint8_t *resp, uint16_t respLen, bool collision) {
    uint8_t par[MAX_PARITY_SIZE] = {0x00};
    GetParity(resp, respLen, par);
    return EmSendCmdParEx(resp, respLen, par, collision);
}

int EmSendPrecompiledCmd(tag_response_info_t *p_response) {
    int ret = EmSendCmd14443aRaw(p_response->modulation, p_response->modulation_n);
    // do the tracing for the previous reader request and this tag answer:
    uint8_t par[MAX_PARITY_SIZE] = {0x00};
    GetParity(p_response->response, p_response->response_n, par);

    EmLogTrace(Uart.output,
               Uart.len,
               Uart.startTime * 16 - DELAY_AIR2ARM_AS_TAG,
               Uart.endTime * 16 - DELAY_AIR2ARM_AS_TAG,
               Uart.parity,
               p_response->response,
               p_response->response_n,
               LastTimeProxToAirStart * 16 + DELAY_ARM2AIR_AS_TAG,
               (LastTimeProxToAirStart + p_response->ProxToAirDuration) * 16 + DELAY_ARM2AIR_AS_TAG,
               par);

    if (DBGLEVEL >= DBG_EXTENDED) {
        Dbprintf("response_info->response %02X", p_response->response);
        Dbprintf("response_info->response_n %02X", p_response->response_n);
        Dbprintf("response_info->par %02X", &(p_response->par));
    }

    return ret;
}

bool EmLogTrace(uint8_t *reader_data, uint16_t reader_len, uint32_t reader_StartTime,
                uint32_t reader_EndTime, uint8_t *reader_Parity, uint8_t *tag_data,
                uint16_t tag_len, uint32_t tag_StartTime, uint32_t tag_EndTime, uint8_t *tag_Parity) {

    // we cannot exactly measure the end and start of a received command from reader. However we know that the delay from
    // end of the received command to start of the tag's (simulated by us) answer is n*128+20 or n*128+84 resp.
    // with n >= 9. The start of the tags answer can be measured and therefore the end of the received command be calculated:

    uint16_t reader_modlen = reader_EndTime - reader_StartTime;
    uint16_t approx_fdt = tag_StartTime - reader_EndTime;
    uint16_t exact_fdt = (approx_fdt - 20 + 32) / 64 * 64 + 20;
    reader_EndTime = tag_StartTime - exact_fdt;
    reader_StartTime = reader_EndTime - reader_modlen;

    if (!LogTrace(reader_data, reader_len, reader_StartTime, reader_EndTime, reader_Parity, true))
        return false;
    else
        return (!LogTrace(tag_data, tag_len, tag_StartTime, tag_EndTime, tag_Parity, false));

}

//-----------------------------------------------------------------------------
// Kovio - Thinfilm barcode.  TAG-TALK-FIRST -
// Wait a certain time for tag response
//  If a response is captured return TRUE
//  If it takes too long return FALSE
//-----------------------------------------------------------------------------
bool GetIso14443aAnswerFromTag_Thinfilm(uint8_t *receivedResponse,  uint8_t *received_len) {

    if (!hf_field_active)
        return false;

    // Set FPGA mode to "reader listen mode", no modulation (listen
    // only, since we are receiving, not transmitting).
    // Signal field is on with the appropriate LED
    LED_D_ON();
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_ISO14443A | FPGA_HF_ISO14443A_READER_LISTEN);

    // Now get the answer from the card
    Demod14aInit(receivedResponse, NULL);

    // clear RXRDY:
    uint8_t b = (uint8_t)AT91C_BASE_SSC->SSC_RHR;
    (void)b;

    uint32_t receive_timer = GetTickCount();
    for (;;) {
        WDT_HIT();

        if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_RXRDY)) {
            b = (uint8_t)AT91C_BASE_SSC->SSC_RHR;
            if (ManchesterDecoding_Thinfilm(b)) {
                *received_len = Demod.len;
                // log
                LogTrace(receivedResponse, Demod.len, Demod.startTime * 16 - DELAY_AIR2ARM_AS_READER, Demod.endTime * 16 - DELAY_AIR2ARM_AS_READER, NULL, false);
                return true;
            }
        }

        // timeout already in ms + 10ms guard time
        if (GetTickCount() - receive_timer >  1160)
            break;
    }
    *received_len = Demod.len;
    // log
    LogTrace(receivedResponse, Demod.len, Demod.startTime * 16 - DELAY_AIR2ARM_AS_READER, Demod.endTime * 16 - DELAY_AIR2ARM_AS_READER, NULL, false);
    return false;
}


//-----------------------------------------------------------------------------
// Wait a certain time for tag response
//  If a response is captured return TRUE
//  If it takes too long return FALSE
//-----------------------------------------------------------------------------
static int GetIso14443aAnswerFromTag(uint8_t *receivedResponse, uint8_t *receivedResponsePar, uint16_t offset) {
    uint32_t c = 0;

    if (!hf_field_active)
        return false;

    // Set FPGA mode to "reader listen mode", no modulation (listen
    // only, since we are receiving, not transmitting).
    // Signal field is on with the appropriate LED
    LED_D_ON();
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_ISO14443A | FPGA_HF_ISO14443A_READER_LISTEN);

    // Now get the answer from the card
    Demod14aInit(receivedResponse, receivedResponsePar);

    // clear RXRDY:
    uint8_t b = (uint8_t)AT91C_BASE_SSC->SSC_RHR;
    (void)b;

    uint32_t timeout = iso14a_get_timeout();
    uint32_t receive_timer = GetTickCount();
    for (;;) {
        WDT_HIT();

        if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_RXRDY)) {
            b = (uint8_t)AT91C_BASE_SSC->SSC_RHR;
            if (ManchesterDecoding(b, offset, 0)) {
                NextTransferTime = MAX(NextTransferTime, Demod.endTime - (DELAY_AIR2ARM_AS_READER + DELAY_ARM2AIR_AS_READER) / 16 + FRAME_DELAY_TIME_PICC_TO_PCD);
                return true;
            } else if (c++ > timeout && Demod.state == DEMOD_14A_UNSYNCD) {
                return false;
            }
        }

        // timeout already in ms + 100ms guard time
        if (GetTickCount() - receive_timer > timeout + 100)
            break;
    }
    return false;
}

void ReaderTransmitBitsPar(uint8_t *frame, uint16_t bits, uint8_t *par, uint32_t *timing) {

    CodeIso14443aBitsAsReaderPar(frame, bits, par);
    // Send command to tag
    TransmitFor14443a(ToSend, ToSendMax, timing);
    if (trigger) LED_A_ON();

    LogTrace(frame, nbytes(bits), (LastTimeProxToAirStart << 4) + DELAY_ARM2AIR_AS_READER, ((LastTimeProxToAirStart + LastProxToAirDuration) << 4) + DELAY_ARM2AIR_AS_READER, par, true);
}

void ReaderTransmitPar(uint8_t *frame, uint16_t len, uint8_t *par, uint32_t *timing) {
    ReaderTransmitBitsPar(frame, len * 8, par, timing);
}

void ReaderTransmitBits(uint8_t *frame, uint16_t len, uint32_t *timing) {
    // Generate parity and redirect
    uint8_t par[MAX_PARITY_SIZE] = {0x00};
    GetParity(frame, len / 8, par);
    ReaderTransmitBitsPar(frame, len, par, timing);
}

void ReaderTransmit(uint8_t *frame, uint16_t len, uint32_t *timing) {
    // Generate parity and redirect
    uint8_t par[MAX_PARITY_SIZE] = {0x00};
    GetParity(frame, len, par);
    ReaderTransmitBitsPar(frame, len * 8, par, timing);
}

int ReaderReceiveOffset(uint8_t *receivedAnswer, uint16_t offset, uint8_t *par) {
    if (!GetIso14443aAnswerFromTag(receivedAnswer, par, offset))
        return false;
    LogTrace(receivedAnswer, Demod.len, Demod.startTime * 16 - DELAY_AIR2ARM_AS_READER, Demod.endTime * 16 - DELAY_AIR2ARM_AS_READER, par, false);
    return Demod.len;
}

int ReaderReceive(uint8_t *receivedAnswer, uint8_t *par) {
    if (!GetIso14443aAnswerFromTag(receivedAnswer, par, 0))
        return false;
    LogTrace(receivedAnswer, Demod.len, Demod.startTime * 16 - DELAY_AIR2ARM_AS_READER, Demod.endTime * 16 - DELAY_AIR2ARM_AS_READER, par, false);
    return Demod.len;
}


// This function misstreats the ISO 14443a anticollision procedure.
// by fooling the reader there is a collision and forceing the reader to
// increase the uid bytes.   The might be an overflow, DoS will occure.
void iso14443a_antifuzz(uint32_t flags) {

    // We need to listen to the high-frequency, peak-detected path.
    iso14443a_setup(FPGA_HF_ISO14443A_TAGSIM_LISTEN);

    BigBuf_free_keep_EM();
    clear_trace();
    set_tracing(true);

    int len = 0;

    // allocate buffers:
    uint8_t *received = BigBuf_malloc(MAX_FRAME_SIZE);
    uint8_t *receivedPar = BigBuf_malloc(MAX_PARITY_SIZE);
    uint8_t *resp = BigBuf_malloc(20);

    memset(resp, 0xFF, 20);

    LED_A_ON();
    for (;;) {
        WDT_HIT();

        // Clean receive command buffer
        if (!GetIso14443aCommandFromReader(received, receivedPar, &len)) {
            Dbprintf("Anti-fuzz stopped. Trace length: %d ", BigBuf_get_traceLen());
            break;
        }
        if (received[0] == ISO14443A_CMD_WUPA || received[0] == ISO14443A_CMD_REQA) {
            resp[0] = 0x04;
            resp[1] = 0x00;

            if ((flags & FLAG_7B_UID_IN_DATA) == FLAG_7B_UID_IN_DATA) {
                resp[0] = 0x44;
            }

            EmSendCmd(resp, 2);
            continue;
        }

        // Received request for UID (cascade 1)
        //if (received[1] >= 0x20 && received[1] <= 0x57 && received[0] == ISO14443A_CMD_ANTICOLL_OR_SELECT) {
        if (received[1] >= 0x20 && received[0] == ISO14443A_CMD_ANTICOLL_OR_SELECT) {
            resp[0] = 0xFF;
            resp[1] = 0xFF;
            resp[2] = 0xFF;
            resp[3] = 0xFF;
            resp[4] =  resp[0] ^ resp[1] ^ resp[2] ^ resp[3];
            colpos = 0;

            if ((flags & FLAG_7B_UID_IN_DATA) == FLAG_7B_UID_IN_DATA) {
                resp[0] = 0x88;
                colpos = 8;
            }

            EmSendCmdEx(resp, 5, true);
            if (DBGLEVEL >= DBG_EXTENDED) Dbprintf("ANTICOLL or SELECT %x", received[1]);
            LED_D_INV();

            continue;
        } else if (received[1] == 0x20 && received[0] == ISO14443A_CMD_ANTICOLL_OR_SELECT_2) {  // Received request for UID (cascade 2)
            if (DBGLEVEL >= DBG_EXTENDED) Dbprintf("ANTICOLL or SELECT_2");
        } else if (received[1] == 0x70 && received[0] == ISO14443A_CMD_ANTICOLL_OR_SELECT) {    // Received a SELECT (cascade 1)
        } else if (received[1] == 0x70 && received[0] == ISO14443A_CMD_ANTICOLL_OR_SELECT_2) {  // Received a SELECT (cascade 2)
        } else {
            Dbprintf("unknown command %x", received[0]);
        }
    }

    reply_old(CMD_ACK, 1, 0, 0, 0, 0);
    switch_off();
    BigBuf_free_keep_EM();
}

static void iso14a_set_ATS_times(uint8_t *ats) {

    if (ats[0] > 1) {                           // there is a format byte T0
        if ((ats[1] & 0x20) == 0x20) {          // there is an interface byte TB(1)
            uint8_t tb1;
            if ((ats[1] & 0x10) == 0x10) {      // there is an interface byte TA(1) preceding TB(1)
                tb1 = ats[3];
            } else {
                tb1 = ats[2];
            }
            uint8_t fwi = (tb1 & 0xf0) >> 4;            // frame waiting time integer (FWI)
            if (fwi != 15) {
                uint32_t fwt = 256 * 16 * (1 << fwi);    // frame waiting time (FWT) in 1/fc
                iso14a_set_timeout(fwt / (8 * 16));
            }
            uint8_t sfgi = tb1 & 0x0f;                  // startup frame guard time integer (SFGI)
            if (sfgi != 0 && sfgi != 15) {
                uint32_t sfgt = 256 * 16 * (1 << sfgi);  // startup frame guard time (SFGT) in 1/fc
                NextTransferTime = MAX(NextTransferTime, Demod.endTime + (sfgt - DELAY_AIR2ARM_AS_READER - DELAY_ARM2AIR_AS_READER) / 16);
            }
        }
    }
}

static int GetATQA(uint8_t *resp, uint8_t *resp_par) {

#define WUPA_RETRY_TIMEOUT 10    // 10ms
    uint8_t wupa[] = { ISO14443A_CMD_WUPA };  // 0x26 - REQA  0x52 - WAKE-UP

    uint32_t save_iso14a_timeout = iso14a_get_timeout();
    iso14a_set_timeout(1236 / (16 * 8) + 1);  // response to WUPA is expected at exactly 1236/fc. No need to wait longer.

    uint32_t start_time = GetTickCount();
    int len;

    // we may need several tries if we did send an unknown command or a wrong authentication before...
    do {
        // Broadcast for a card, WUPA (0x52) will force response from all cards in the field
        ReaderTransmitBitsPar(wupa, 7, NULL, NULL);
        // Receive the ATQA
        len = ReaderReceive(resp, resp_par);
    } while (len == 0 && GetTickCountDelta(start_time) <= WUPA_RETRY_TIMEOUT);

    iso14a_set_timeout(save_iso14a_timeout);
    return len;
}

// performs iso14443a anticollision (optional) and card select procedure
// fills the uid and cuid pointer unless NULL
// fills the card info record unless NULL
// if anticollision is false, then the UID must be provided in uid_ptr[]
// and num_cascades must be set (1: 4 Byte UID, 2: 7 Byte UID, 3: 10 Byte UID)
// requests ATS unless no_rats is true
int iso14443a_select_card(uint8_t *uid_ptr, iso14a_card_select_t *p_card, uint32_t *cuid_ptr, bool anticollision, uint8_t num_cascades, bool no_rats) {

    uint8_t resp[MAX_FRAME_SIZE] = {0}; // theoretically. A usual RATS will be much smaller
    uint8_t resp_par[MAX_PARITY_SIZE] = {0};

    uint8_t sak = 0x04; // cascade uid
    int cascade_level = 0;

    if (p_card) {
        p_card->uidlen = 0;
        memset(p_card->uid, 0, 10);
        p_card->ats_len = 0;
    }

    if (!GetATQA(resp, resp_par)) {
        return 0;
    }

    if (p_card) {
        p_card->atqa[0] = resp[0];
        p_card->atqa[1] = resp[1];
    }

    if (anticollision) {
        // clear uid
        if (uid_ptr)
            memset(uid_ptr, 0, 10);
    }

    // check for proprietary anticollision:
    if ((resp[0] & 0x1F) == 0) return 3;

    // OK we will select at least at cascade 1, lets see if first byte of UID was 0x88 in
    // which case we need to make a cascade 2 request and select - this is a long UID
    // While the UID is not complete, the 3nd bit (from the right) is set in the SAK.
    for (; sak & 0x04; cascade_level++) {
        // SELECT_* (L1: 0x93, L2: 0x95, L3: 0x97)
        uint8_t sel_all[]    = { ISO14443A_CMD_ANTICOLL_OR_SELECT, 0x20 };
        uint8_t sel_uid[]    = { ISO14443A_CMD_ANTICOLL_OR_SELECT, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        uint8_t uid_resp[4] = {0};
        sel_uid[0] = sel_all[0] = 0x93 + cascade_level * 2;

        if (anticollision) {
            // SELECT_ALL
            ReaderTransmit(sel_all, sizeof(sel_all), NULL);
            if (!ReaderReceive(resp, resp_par)) return 0;

            if (Demod.collisionPos) {            // we had a collision and need to construct the UID bit by bit
                memset(uid_resp, 0, 4);
                uint16_t uid_resp_bits = 0;
                uint16_t collision_answer_offset = 0;
                // anti-collision-loop:
                while (Demod.collisionPos) {
                    Dbprintf("Multiple tags detected. Collision after Bit %d", Demod.collisionPos);
                    for (uint16_t i = collision_answer_offset; i < Demod.collisionPos; i++, uid_resp_bits++) {    // add valid UID bits before collision point
                        uint16_t UIDbit = (resp[i / 8] >> (i % 8)) & 0x01;
                        uid_resp[uid_resp_bits / 8] |= UIDbit << (uid_resp_bits % 8);
                    }
                    uid_resp[uid_resp_bits / 8] |= 1 << (uid_resp_bits % 8);                  // next time select the card(s) with a 1 in the collision position
                    uid_resp_bits++;
                    // construct anticollosion command:
                    sel_uid[1] = ((2 + uid_resp_bits / 8) << 4) | (uid_resp_bits & 0x07);     // length of data in bytes and bits
                    for (uint16_t i = 0; i <= uid_resp_bits / 8; i++) {
                        sel_uid[2 + i] = uid_resp[i];
                    }
                    collision_answer_offset = uid_resp_bits % 8;
                    ReaderTransmitBits(sel_uid, 16 + uid_resp_bits, NULL);
                    if (!ReaderReceiveOffset(resp, collision_answer_offset, resp_par)) return 0;
                }
                // finally, add the last bits and BCC of the UID
                for (uint16_t i = collision_answer_offset; i < (Demod.len - 1) * 8; i++, uid_resp_bits++) {
                    uint16_t UIDbit = (resp[i / 8] >> (i % 8)) & 0x01;
                    uid_resp[uid_resp_bits / 8] |= UIDbit << (uid_resp_bits % 8);
                }

            } else {        // no collision, use the response to SELECT_ALL as current uid
                memcpy(uid_resp, resp, 4);
            }

        } else {
            if (cascade_level < num_cascades - 1) {
                uid_resp[0] = 0x88;
                memcpy(uid_resp + 1, uid_ptr + cascade_level * 3, 3);
            } else {
                memcpy(uid_resp, uid_ptr + cascade_level * 3, 4);
            }
        }
        size_t uid_resp_len = 4;

        // calculate crypto UID. Always use last 4 Bytes.
        if (cuid_ptr)
            *cuid_ptr = bytes_to_num(uid_resp, 4);

        // Construct SELECT UID command
        sel_uid[1] = 0x70;                                              // transmitting a full UID (1 Byte cmd, 1 Byte NVB, 4 Byte UID, 1 Byte BCC, 2 Bytes CRC)
        memcpy(sel_uid + 2, uid_resp, 4);                               // the UID received during anticollision, or the provided UID
        sel_uid[6] = sel_uid[2] ^ sel_uid[3] ^ sel_uid[4] ^ sel_uid[5]; // calculate and add BCC
        AddCrc14A(sel_uid, 7);                                          // calculate and add CRC
        ReaderTransmit(sel_uid, sizeof(sel_uid), NULL);

        // Receive the SAK
        if (!ReaderReceive(resp, resp_par)) return 0;

        sak = resp[0];

        // Test if more parts of the uid are coming
        if ((sak & 0x04) /* && uid_resp[0] == 0x88 */) {
            // Remove first byte, 0x88 is not an UID byte, it CT, see page 3 of:
            // http://www.nxp.com/documents/application_note/AN10927.pdf
            uid_resp[0] = uid_resp[1];
            uid_resp[1] = uid_resp[2];
            uid_resp[2] = uid_resp[3];
            uid_resp_len = 3;
        }

        if (uid_ptr && anticollision)
            memcpy(uid_ptr + (cascade_level * 3), uid_resp, uid_resp_len);

        if (p_card) {
            memcpy(p_card->uid + (cascade_level * 3), uid_resp, uid_resp_len);
            p_card->uidlen += uid_resp_len;
        }
    }

    if (p_card) {
        p_card->sak = sak;
    }

    // PICC compilant with iso14443a-4 ---> (SAK & 0x20 != 0)
    if ((sak & 0x20) == 0) return 2;

    // RATS, Request for answer to select
    if (!no_rats) {
        uint8_t rats[]       = { ISO14443A_CMD_RATS, 0x80, 0x00, 0x00 }; // FSD=256, FSDI=8, CID=0
        AddCrc14A(rats, 2);
        ReaderTransmit(rats, sizeof(rats), NULL);
        int len = ReaderReceive(resp, resp_par);

        if (!len) return 0;

        if (p_card) {
            memcpy(p_card->ats, resp, sizeof(p_card->ats));
            p_card->ats_len = len;
        }

        // reset the PCB block number
        iso14_pcb_blocknum = 0;

        // set default timeout and delay next transfer based on ATS
        iso14a_set_ATS_times(resp);
    }
    return 1;
}

int iso14443a_fast_select_card(uint8_t *uid_ptr, uint8_t num_cascades) {
    uint8_t resp[5] = {0}; // theoretically. A usual RATS will be much smaller
    uint8_t resp_par[1] = {0};
    uint8_t uid_resp[4] = {0};

    uint8_t sak = 0x04; // cascade uid
    int cascade_level = 0;

    if (!GetATQA(resp, resp_par)) {
        return 0;
    }

    // OK we will select at least at cascade 1, lets see if first byte of UID was 0x88 in
    // which case we need to make a cascade 2 request and select - this is a long UID
    // While the UID is not complete, the 3nd bit (from the right) is set in the SAK.
    for (; sak & 0x04; cascade_level++) {
        uint8_t sel_all[]    = { ISO14443A_CMD_ANTICOLL_OR_SELECT, 0x20 };
        uint8_t sel_uid[]    = { ISO14443A_CMD_ANTICOLL_OR_SELECT, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        // SELECT_* (L1: 0x93, L2: 0x95, L3: 0x97)
        sel_uid[0] = sel_all[0] = 0x93 + cascade_level * 2;

        if (cascade_level < num_cascades - 1) {
            uid_resp[0] = 0x88;
            memcpy(uid_resp + 1, uid_ptr + cascade_level * 3, 3);
        } else {
            memcpy(uid_resp, uid_ptr + cascade_level * 3, 4);
        }

        // Construct SELECT UID command
        //sel_uid[1] = 0x70;                                            // transmitting a full UID (1 Byte cmd, 1 Byte NVB, 4 Byte UID, 1 Byte BCC, 2 Bytes CRC)
        memcpy(sel_uid + 2, uid_resp, 4);                               // the UID received during anticollision, or the provided UID
        sel_uid[6] = sel_uid[2] ^ sel_uid[3] ^ sel_uid[4] ^ sel_uid[5]; // calculate and add BCC
        AddCrc14A(sel_uid, 7);                                          // calculate and add CRC
        ReaderTransmit(sel_uid, sizeof(sel_uid), NULL);

        // Receive the SAK
        if (!ReaderReceive(resp, resp_par)) return 0;

        sak = resp[0];

        // Test if more parts of the uid are coming
        if ((sak & 0x04) /* && uid_resp[0] == 0x88 */) {
            // Remove first byte, 0x88 is not an UID byte, it CT, see page 3 of:
            // http://www.nxp.com/documents/application_note/AN10927.pdf
            uid_resp[0] = uid_resp[1];
            uid_resp[1] = uid_resp[2];
            uid_resp[2] = uid_resp[3];
        }
    }
    return 1;
}

void iso14443a_setup(uint8_t fpga_minor_mode) {

    FpgaDownloadAndGo(FPGA_BITSTREAM_HF);
    // Set up the synchronous serial port
    FpgaSetupSsc();
    // connect Demodulated Signal to ADC:
    SetAdcMuxFor(GPIO_MUXSEL_HIPKD);

    LED_D_OFF();
    // Signal field is on with the appropriate LED
    if (fpga_minor_mode == FPGA_HF_ISO14443A_READER_MOD || fpga_minor_mode == FPGA_HF_ISO14443A_READER_LISTEN)
        LED_D_ON();

    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_ISO14443A | fpga_minor_mode);
    SpinDelay(100);

    // Start the timer
    StartCountSspClk();

    // Prepare the demodulation functions
    Demod14aReset();
    Uart14aReset();
    NextTransferTime = 2 * DELAY_ARM2AIR_AS_READER;
    iso14a_set_timeout(1060); // 106 * 10ms default

    hf_field_active = true;
}


void hf_field_off(void) {
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    hf_field_active = false;
}

/* Peter Fillmore 2015
Added card id field to the function
 info from ISO14443A standard
b1 = Block Number
b2 = RFU (always 1)
b3 = depends on block
b4 = Card ID following if set to 1
b5 = depends on block type
b6 = depends on block type
b7,b8 = block type.
Coding of I-BLOCK:
b8 b7 b6 b5 b4 b3 b2 b1
0  0  0  x  x  x  1  x
b5 = chaining bit
Coding of R-block:
b8 b7 b6 b5 b4 b3 b2 b1
1  0  1  x  x  0  1  x
b5 = ACK/NACK
Coding of S-block:
b8 b7 b6 b5 b4 b3 b2 b1
1  1  x  x  x  0  1  0
b5,b6 = 00 - DESELECT
        11 - WTX
*/
int iso14_apdu(uint8_t *cmd, uint16_t cmd_len, bool send_chaining, void *data, uint8_t *res) {
    uint8_t parity[MAX_PARITY_SIZE] = {0x00};
    uint8_t real_cmd[cmd_len + 4];

    if (cmd_len) {
        // ISO 14443 APDU frame: PCB [CID] [NAD] APDU CRC PCB=0x02
        real_cmd[0] = 0x02; // bnr,nad,cid,chn=0; i-block(0x00)
        if (send_chaining) {
            real_cmd[0] |= 0x10;
        }
        // put block number into the PCB
        real_cmd[0] |= iso14_pcb_blocknum;
        memcpy(real_cmd + 1, cmd, cmd_len);
    } else {
        // R-block. ACK
        real_cmd[0] = 0xA2; // r-block + ACK
        real_cmd[0] |= iso14_pcb_blocknum;
    }
    AddCrc14A(real_cmd, cmd_len + 1);

    ReaderTransmit(real_cmd, cmd_len + 3, NULL);

    size_t len = ReaderReceive(data, parity);
    uint8_t *data_bytes = (uint8_t *) data;

    if (!len) {
        return 0; //DATA LINK ERROR
    } else {
        // S-Block WTX
        while (len && ((data_bytes[0] & 0xF2) == 0xF2)) {
            uint32_t save_iso14a_timeout = iso14a_get_timeout();
            // temporarily increase timeout
            iso14a_set_timeout(MAX((data_bytes[1] & 0x3f) * save_iso14a_timeout, MAX_ISO14A_TIMEOUT));
            // Transmit WTX back
            // byte1 - WTXM [1..59]. command FWT=FWT*WTXM
            data_bytes[1] = data_bytes[1] & 0x3f; // 2 high bits mandatory set to 0b
            // now need to fix CRC.
            AddCrc14A(data_bytes, len - 2);
            // transmit S-Block
            ReaderTransmit(data_bytes, len, NULL);
            // retrieve the result again (with increased timeout)
            len = ReaderReceive(data, parity);
            data_bytes = data;
            // restore timeout
            iso14a_set_timeout(save_iso14a_timeout);
        }

        // if we received an I- or R(ACK)-Block with a block number equal to the
        // current block number, toggle the current block number
        if (len >= 3 // PCB+CRC = 3 bytes
                && ((data_bytes[0] & 0xC0) == 0 // I-Block
                    || (data_bytes[0] & 0xD0) == 0x80) // R-Block with ACK bit set to 0
                && (data_bytes[0] & 0x01) == iso14_pcb_blocknum) { // equal block numbers
            iso14_pcb_blocknum ^= 1;
        }

        // if we received I-block with chaining we need to send ACK and receive another block of data
        if (res)
            *res = data_bytes[0];

        // crc check
        if (len >= 3 && !CheckCrc14A(data_bytes, len)) {
            return -1;
        }

    }

    if (len) {
        // cut frame byte
        len -= 1;
        // memmove(data_bytes, data_bytes + 1, len);
        for (int i = 0; i < len; i++)
            data_bytes[i] = data_bytes[i + 1];
    }

    return len;
}

//-----------------------------------------------------------------------------
// Read an ISO 14443a tag. Send out commands and store answers.
//-----------------------------------------------------------------------------
// arg0         iso_14a flags
// arg1         high ::  number of bits, if you want to send 7bits etc
//             low  ::  len of commandbytes
// arg2         timeout
// d.asBytes command bytes to send
void ReaderIso14443a(PacketCommandNG *c) {
    iso14a_command_t param = c->oldarg[0];
    size_t len = c->oldarg[1] & 0xffff;
    size_t lenbits = c->oldarg[1] >> 16;
    uint32_t timeout = c->oldarg[2];
    uint8_t *cmd = c->data.asBytes;
    uint32_t arg0;
    uint8_t buf[PM3_CMD_DATA_SIZE] = {0x00};
    uint8_t par[MAX_PARITY_SIZE] = {0x00};

    if ((param & ISO14A_CONNECT))
        clear_trace();

    set_tracing(true);

    if ((param & ISO14A_REQUEST_TRIGGER))
        iso14a_set_trigger(true);

    if ((param & ISO14A_CONNECT)) {
        iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

        // notify client selecting status.
        // if failed selecting, turn off antenna and quite.
        if (!(param & ISO14A_NO_SELECT)) {
            iso14a_card_select_t *card = (iso14a_card_select_t *)buf;
            arg0 = iso14443a_select_card(NULL, card, NULL, true, 0, param & ISO14A_NO_RATS);
            reply_mix(CMD_ACK, arg0, card->uidlen, 0, buf, sizeof(iso14a_card_select_t));
            if (arg0 == 0)
                goto OUT;
        }
    }

    if ((param & ISO14A_SET_TIMEOUT))
        iso14a_set_timeout(timeout);

    if ((param & ISO14A_APDU)) {
        uint8_t res;
        arg0 = iso14_apdu(cmd, len, (param & ISO14A_SEND_CHAINING), buf, &res);
        reply_old(CMD_ACK, arg0, res, 0, buf, sizeof(buf));
    }

    if ((param & ISO14A_RAW)) {

        if ((param & ISO14A_APPEND_CRC)) {
            // Don't append crc on empty bytearray...
            if (len > 0) {
                if ((param & ISO14A_TOPAZMODE))
                    AddCrc14B(cmd, len);
                else
                    AddCrc14A(cmd, len);

                len += 2;
                if (lenbits) lenbits += 16;
            }
        }

        if (lenbits > 0) {                // want to send a specific number of bits (e.g. short commands)
            if ((param & ISO14A_TOPAZMODE)) {
                int bits_to_send = lenbits;
                uint16_t i = 0;
                ReaderTransmitBitsPar(&cmd[i++], MIN(bits_to_send, 7), NULL, NULL);     // first byte is always short (7bits) and no parity
                bits_to_send -= 7;
                while (bits_to_send > 0) {
                    ReaderTransmitBitsPar(&cmd[i++], MIN(bits_to_send, 8), NULL, NULL); // following bytes are 8 bit and no parity
                    bits_to_send -= 8;
                }
            } else {
                GetParity(cmd, lenbits / 8, par);
                ReaderTransmitBitsPar(cmd, lenbits, par, NULL);                         // bytes are 8 bit with odd parity
            }
        } else {                    // want to send complete bytes only
            if ((param & ISO14A_TOPAZMODE)) {
                uint16_t i = 0;
                ReaderTransmitBitsPar(&cmd[i++], 7, NULL, NULL);                        // first byte: 7 bits, no paritiy
                while (i < len) {
                    ReaderTransmitBitsPar(&cmd[i++], 8, NULL, NULL);                    // following bytes: 8 bits, no paritiy
                }
            } else {
                ReaderTransmit(cmd, len, NULL);                                         // 8 bits, odd parity
            }
        }
        arg0 = ReaderReceive(buf, par);
        reply_old(CMD_ACK, arg0, 0, 0, buf, sizeof(buf));
    }

    if ((param & ISO14A_REQUEST_TRIGGER))
        iso14a_set_trigger(false);

    if ((param & ISO14A_NO_DISCONNECT))
        return;

OUT:
    hf_field_off();
    set_tracing(false);
}

// Determine the distance between two nonces.
// Assume that the difference is small, but we don't know which is first.
// Therefore try in alternating directions.
int32_t dist_nt(uint32_t nt1, uint32_t nt2) {

    if (nt1 == nt2) return 0;

    uint32_t nttmp1 = nt1;
    uint32_t nttmp2 = nt2;

    for (uint16_t i = 1; i < 32768; i++) {
        nttmp1 = prng_successor(nttmp1, 1);
        if (nttmp1 == nt2) return i;

        nttmp2 = prng_successor(nttmp2, 1);
        if (nttmp2 == nt1) return -i;
    }

    return (-99999); // either nt1 or nt2 are invalid nonces
}


#define PRNG_SEQUENCE_LENGTH    (1 << 16)
#define MAX_UNEXPECTED_RANDOM    4        // maximum number of unexpected (i.e. real) random numbers when trying to sync. Then give up.
#define MAX_SYNC_TRIES        32

//-----------------------------------------------------------------------------
// Recover several bits of the cypher stream. This implements (first stages of)
// the algorithm described in "The Dark Side of Security by Obscurity and
// Cloning MiFare Classic Rail and Building Passes, Anywhere, Anytime"
// (article by Nicolas T. Courtois, 2009)
//-----------------------------------------------------------------------------
void ReaderMifare(bool first_try, uint8_t block, uint8_t keytype) {

    iso14443a_setup(FPGA_HF_ISO14443A_READER_MOD);

    BigBuf_free();
    BigBuf_Clear_ext(false);
    clear_trace();
    set_tracing(true);

    uint8_t mf_auth[]     = { keytype, block, 0x00, 0x00 };
    uint8_t mf_nr_ar[]    = {0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t uid[10]       = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t par_list[8]   = {0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t ks_list[8]    = {0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t receivedAnswer[MAX_MIFARE_FRAME_SIZE] = {0x00};
    uint8_t receivedAnswerPar[MAX_MIFARE_PARITY_SIZE] = {0x00};
    uint8_t par[1] = {0};    // maximum 8 Bytes to be sent here, 1 byte parity is therefore enough
    uint8_t nt_diff = 0;

    uint32_t nt = 0, previous_nt = 0, cuid = 0;
    uint32_t sync_time = GetCountSspClk() & 0xfffffff8;

    int32_t catch_up_cycles = 0;
    int32_t last_catch_up = 0;
    int32_t isOK = 0;

    uint16_t elapsed_prng_sequences = 1;
    uint16_t consecutive_resyncs = 0;
    uint16_t unexpected_random = 0;
    uint16_t sync_tries = 0;

    bool have_uid = false;
    uint8_t cascade_levels = 0;

    // static variables here, is re-used in the next call
    static uint32_t nt_attacked = 0;
    static int32_t sync_cycles = 0;
    static uint8_t par_low = 0;
    static uint8_t mf_nr_ar3 = 0;

    AddCrc14A(mf_auth, 2);

    if (first_try) {
        sync_cycles = PRNG_SEQUENCE_LENGTH; // Mifare Classic's random generator repeats every 2^16 cycles (and so do the nonces).
        nt_attacked = 0;
        mf_nr_ar3 = 0;
        par_low = 0;
    } else {
        // we were unsuccessful on a previous call.
        // Try another READER nonce (first 3 parity bits remain the same)
        mf_nr_ar3++;
        mf_nr_ar[3] = mf_nr_ar3;
        par[0] = par_low;
    }

    LED_C_ON();
    uint16_t i;
    for (i = 0; true; ++i) {

        bool received_nack = false;

        WDT_HIT();

        // Test if the action was cancelled
        if (BUTTON_PRESS()) {
            isOK = -1;
            break;
        }

        // this part is from Piwi's faster nonce collecting part in Hardnested.
        if (!have_uid) { // need a full select cycle to get the uid first
            iso14a_card_select_t card_info;
            if (!iso14443a_select_card(uid, &card_info, &cuid, true, 0, true)) {
                if (DBGLEVEL >= DBG_ERROR)    Dbprintf("Mifare: Can't select card (ALL)");
                continue;
            }
            switch (card_info.uidlen) {
                case 4 :
                    cascade_levels = 1;
                    break;
                case 7 :
                    cascade_levels = 2;
                    break;
                case 10:
                    cascade_levels = 3;
                    break;
                default:
                    break;
            }
            have_uid = true;
        } else { // no need for anticollision. We can directly select the card
            if (!iso14443a_fast_select_card(uid, cascade_levels)) {
                if (DBGLEVEL >= DBG_ERROR)    Dbprintf("Mifare: Can't select card (UID)");
                continue;
            }
        }

        elapsed_prng_sequences = 1;

        // Sending timeslot of ISO14443a frame
        sync_time = (sync_time & 0xfffffff8) + sync_cycles + catch_up_cycles;
        catch_up_cycles = 0;

#define SYNC_TIME_BUFFER        16        // if there is only SYNC_TIME_BUFFER left before next planned sync, wait for next PRNG cycle

        // if we missed the sync time already or are about to miss it, advance to the next nonce repeat
        while (sync_time < GetCountSspClk() + SYNC_TIME_BUFFER) {
            ++elapsed_prng_sequences;
            sync_time = (sync_time & 0xfffffff8) + sync_cycles;
        }

        // Transmit MIFARE_CLASSIC_AUTH at synctime. Should result in returning the same tag nonce (== nt_attacked)
        ReaderTransmit(mf_auth, sizeof(mf_auth), &sync_time);

        // Receive the (4 Byte) "random" TAG nonce
        if (!ReaderReceive(receivedAnswer, receivedAnswerPar))
            continue;

        previous_nt = nt;
        nt = bytes_to_num(receivedAnswer, 4);

        // Transmit reader nonce with fake par
        ReaderTransmitPar(mf_nr_ar, sizeof(mf_nr_ar), par, NULL);

        // Receive answer. This will be a 4 Bit NACK when the 8 parity bits are OK after decoding
        if (ReaderReceive(receivedAnswer, receivedAnswerPar))
            received_nack = true;

        // we didn't calibrate our clock yet,
        // iceman: has to be calibrated every time.
        if (previous_nt && !nt_attacked) {

            int nt_distance = dist_nt(previous_nt, nt);

            // if no distance between,  then we are in sync.
            if (nt_distance == 0) {
                nt_attacked = nt;
            } else {
                if (nt_distance == -99999) { // invalid nonce received
                    unexpected_random++;
                    if (unexpected_random > MAX_UNEXPECTED_RANDOM) {
                        isOK = -3;        // Card has an unpredictable PRNG. Give up
                        break;
                    } else {
                        continue;        // continue trying...
                    }
                }

                if (++sync_tries > MAX_SYNC_TRIES) {
                    isOK = -4;             // Card's PRNG runs at an unexpected frequency or resets unexpectedly
                    break;
                }

                sync_cycles = (sync_cycles - nt_distance) / elapsed_prng_sequences;

                // no negative sync_cycles
                if (sync_cycles <= 0) sync_cycles += PRNG_SEQUENCE_LENGTH;

                // reset sync_cycles
                if (sync_cycles > PRNG_SEQUENCE_LENGTH * 2) {
                    sync_cycles = PRNG_SEQUENCE_LENGTH;
                    sync_time = GetCountSspClk() & 0xfffffff8;
                }

                if (DBGLEVEL >= DBG_EXTENDED)
                    Dbprintf("calibrating in cycle %d. nt_distance=%d, elapsed_prng_sequences=%d, new sync_cycles: %d\n", i, nt_distance, elapsed_prng_sequences, sync_cycles);

                LED_B_OFF();
                continue;
            }
        }
        LED_B_OFF();

        if ((nt != nt_attacked) && nt_attacked) {      // we somehow lost sync. Try to catch up again...

            catch_up_cycles = -dist_nt(nt_attacked, nt);
            if (catch_up_cycles == 99999) {            // invalid nonce received. Don't resync on that one.
                catch_up_cycles = 0;
                continue;
            }
            // average?
            catch_up_cycles /= elapsed_prng_sequences;

            if (catch_up_cycles == last_catch_up) {
                consecutive_resyncs++;
            } else {
                last_catch_up = catch_up_cycles;
                consecutive_resyncs = 0;
            }

            if (consecutive_resyncs < 3) {
                if (DBGLEVEL >= DBG_EXTENDED) {
                    Dbprintf("Lost sync in cycle %d. nt_distance=%d. Consecutive Resyncs = %d. Trying one time catch up...\n", i, catch_up_cycles, consecutive_resyncs);
                }
            } else {
                sync_cycles += catch_up_cycles;

                if (DBGLEVEL >= DBG_EXTENDED)
                    Dbprintf("Lost sync in cycle %d for the fourth time consecutively (nt_distance = %d). Adjusting sync_cycles to %d.\n", i, catch_up_cycles, sync_cycles);

                last_catch_up = 0;
                catch_up_cycles = 0;
                consecutive_resyncs = 0;
            }
            continue;
        }

        // Receive answer. This will be a 4 Bit NACK when the 8 parity bits are OK after decoding
        if (received_nack) {
            catch_up_cycles = 8;     // the PRNG is delayed by 8 cycles due to the NAC (4Bits = 0x05 encrypted) transfer

            if (nt_diff == 0)
                par_low = par[0] & 0xE0; // there is no need to check all parities for other nt_diff. Parity Bits for mf_nr_ar[0..2] won't change

            par_list[nt_diff] = reflect8(par[0]);
            ks_list[nt_diff] = receivedAnswer[0] ^ 0x05;  // xor with NACK value to get keystream

            // Test if the information is complete
            if (nt_diff == 0x07) {
                isOK = 1;
                break;
            }

            nt_diff = (nt_diff + 1) & 0x07;
            mf_nr_ar[3] = (mf_nr_ar[3] & 0x1F) | (nt_diff << 5);
            par[0] = par_low;

        } else {
            // No NACK.
            if (nt_diff == 0 && first_try) {
                par[0]++;
                if (par[0] == 0) {    // tried all 256 possible parities without success. Card doesn't send NACK.
                    isOK = -2;
                    break;
                }
            } else {
                // Why this?
                par[0] = ((par[0] & 0x1F) + 1) | par_low;
            }
        }

        // reset the resyncs since we got a complete transaction on right time.
        consecutive_resyncs = 0;
    } // end for loop

    mf_nr_ar[3] &= 0x1F;

    if (DBGLEVEL >= DBG_EXTENDED) Dbprintf("Number of sent auth requests: %u", i);

    uint8_t buf[32] = {0x00};
    memset(buf, 0x00, sizeof(buf));
    num_to_bytes(cuid, 4, buf);
    num_to_bytes(nt, 4, buf + 4);
    memcpy(buf + 8,  par_list, 8);
    memcpy(buf + 16, ks_list, 8);
    memcpy(buf + 24, mf_nr_ar, 8);

    reply_mix(CMD_ACK, isOK, 0, 0, buf, sizeof(buf));

    hf_field_off();
    set_tracing(false);
}

/*
*  Mifare Classic NACK-bug detection
*  Thanks to @doegox for the feedback and new approaches.
*/
void DetectNACKbug(void) {
    uint8_t mf_auth[]     = {0x60, 0x00, 0xF5, 0x7B};
    uint8_t mf_nr_ar[]    = {0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t uid[10]       = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t receivedAnswer[MAX_MIFARE_FRAME_SIZE] = {0x00};
    uint8_t receivedAnswerPar[MAX_MIFARE_PARITY_SIZE] = {0x00};
    uint8_t par[1] = {0};    // maximum 8 Bytes to be sent here, 1 byte parity is therefore enough

    uint32_t nt = 0, previous_nt = 0, nt_attacked = 0, cuid = 0;
    int32_t catch_up_cycles = 0, last_catch_up = 0;
    uint8_t cascade_levels = 0, num_nacks = 0, isOK = 0;
    uint16_t elapsed_prng_sequences = 1;
    uint16_t consecutive_resyncs = 0;
    uint16_t unexpected_random = 0;
    uint16_t sync_tries = 0;
    uint32_t sync_time = 0;
    bool have_uid = false;

    int32_t status = PM3_SUCCESS;

    // Mifare Classic's random generator repeats every 2^16 cycles (and so do the nonces).
    int32_t sync_cycles = PRNG_SEQUENCE_LENGTH;

    BigBuf_free();
    BigBuf_Clear_ext(false);
    clear_trace();
    set_tracing(true);
    iso14443a_setup(FPGA_HF_ISO14443A_READER_MOD);

    sync_time = GetCountSspClk() & 0xfffffff8;

    LED_C_ON();
    uint16_t i;
    for (i = 1; true; ++i) {

        bool received_nack = false;

        // Cards always leaks a NACK, no matter the parity
        if ((i == 10) && (num_nacks == i - 1)) {
            isOK = 2;
            break;
        }

        WDT_HIT();

        // Test if the action was cancelled
        if (BUTTON_PRESS() || data_available()) {
            status = PM3_EOPABORTED;
            break;
        }

        // this part is from Piwi's faster nonce collecting part in Hardnested.
        if (!have_uid) { // need a full select cycle to get the uid first
            iso14a_card_select_t card_info;
            if (!iso14443a_select_card(uid, &card_info, &cuid, true, 0, true)) {
                if (DBGLEVEL >= DBG_ERROR) Dbprintf("Mifare: Can't select card (ALL)");
                i = 0;
                continue;
            }
            switch (card_info.uidlen) {
                case 4 :
                    cascade_levels = 1;
                    break;
                case 7 :
                    cascade_levels = 2;
                    break;
                case 10:
                    cascade_levels = 3;
                    break;
                default:
                    i = 0;
                    have_uid = false;
                    continue;
            }
            have_uid = true;
        } else { // no need for anticollision. We can directly select the card
            if (!iso14443a_fast_select_card(uid, cascade_levels)) {
                if (DBGLEVEL >= DBG_ERROR)    Dbprintf("Mifare: Can't select card (UID)");
                i = 0;
                have_uid = false;
                continue;
            }
        }

        elapsed_prng_sequences = 1;

        // Sending timeslot of ISO14443a frame
        sync_time = (sync_time & 0xfffffff8) + sync_cycles + catch_up_cycles;
        catch_up_cycles = 0;

        // if we missed the sync time already, advance to the next nonce repeat
        while (GetCountSspClk() > sync_time) {
            ++elapsed_prng_sequences;
            sync_time = (sync_time & 0xfffffff8) + sync_cycles;
        }

        // Transmit MIFARE_CLASSIC_AUTH at synctime. Should result in returning the same tag nonce (== nt_attacked)
        ReaderTransmit(mf_auth, sizeof(mf_auth), &sync_time);

        // Receive the (4 Byte) "random" TAG nonce
        if (!ReaderReceive(receivedAnswer, receivedAnswerPar))
            continue;

        previous_nt = nt;
        nt = bytes_to_num(receivedAnswer, 4);

        // Transmit reader nonce with fake par
        ReaderTransmitPar(mf_nr_ar, sizeof(mf_nr_ar), par, NULL);

        if (ReaderReceive(receivedAnswer, receivedAnswerPar)) {
            received_nack = true;
            num_nacks++;
            // ALWAYS leak Detection.
            if (i == num_nacks) {
                continue;
            }
        }

        // we didn't calibrate our clock yet,
        // iceman: has to be calibrated every time.
        if (previous_nt && !nt_attacked) {

            int nt_distance = dist_nt(previous_nt, nt);

            // if no distance between,  then we are in sync.
            if (nt_distance == 0) {
                nt_attacked = nt;
            } else {
                if (nt_distance == -99999) { // invalid nonce received
                    unexpected_random++;
                    if (unexpected_random > MAX_UNEXPECTED_RANDOM) {
                        // Card has an unpredictable PRNG. Give up
                        isOK = 98;
                        break;
                    } else {
                        if (sync_cycles <= 0) {
                            sync_cycles += PRNG_SEQUENCE_LENGTH;
                        }
                        continue;
                    }
                }

                if (++sync_tries > MAX_SYNC_TRIES) {
                    isOK = 97;             // Card's PRNG runs at an unexpected frequency or resets unexpectedly
                    break;
                }

                sync_cycles = (sync_cycles - nt_distance) / elapsed_prng_sequences;

                if (sync_cycles <= 0)
                    sync_cycles += PRNG_SEQUENCE_LENGTH;

                if (sync_cycles > PRNG_SEQUENCE_LENGTH * 2) {
                    isOK = 96;             // Card's PRNG runs at an unexpected frequency or resets unexpectedly
                    break;
                }

                if (DBGLEVEL >= DBG_EXTENDED)
                    Dbprintf("calibrating in cycle %d. nt_distance=%d, elapsed_prng_sequences=%d, new sync_cycles: %d\n", i, nt_distance, elapsed_prng_sequences, sync_cycles);

                continue;
            }
        }

        if ((nt != nt_attacked) && nt_attacked) {
            // we somehow lost sync. Try to catch up again...
            catch_up_cycles = -dist_nt(nt_attacked, nt);

            if (catch_up_cycles == 99999) {
                // invalid nonce received. Don't resync on that one.
                catch_up_cycles = 0;
                continue;
            }
            // average?
            catch_up_cycles /= elapsed_prng_sequences;

            if (catch_up_cycles == last_catch_up) {
                consecutive_resyncs++;
            } else {
                last_catch_up = catch_up_cycles;
                consecutive_resyncs = 0;
            }

            if (consecutive_resyncs < 3) {
                if (DBGLEVEL >= DBG_EXTENDED) {
                    Dbprintf("Lost sync in cycle %d. nt_distance=%d. Consecutive Resyncs = %d. Trying one time catch up...\n", i, catch_up_cycles, consecutive_resyncs);
                }
            } else {
                sync_cycles += catch_up_cycles;

                if (DBGLEVEL >= DBG_EXTENDED) {
                    Dbprintf("Lost sync in cycle %d for the fourth time consecutively (nt_distance = %d). Adjusting sync_cycles to %d.\n", i, catch_up_cycles, sync_cycles);
                    Dbprintf("nt [%08x] attacted [%08x]", nt, nt_attacked);
                }
                last_catch_up = 0;
                catch_up_cycles = 0;
                consecutive_resyncs = 0;
            }
            continue;
        }

        // Receive answer. This will be a 4 Bit NACK when the 8 parity bits are OK after decoding
        if (received_nack)
            catch_up_cycles = 8;     // the PRNG is delayed by 8 cycles due to the NAC (4Bits = 0x05 encrypted) transfer

        // we are testing all 256 possibilities.
        par[0]++;

        // tried all 256 possible parities without success.
        if (par[0] == 0) {
            // did we get one NACK?
            if (num_nacks == 1)
                isOK = 1;
            break;
        }

        // reset the resyncs since we got a complete transaction on right time.
        consecutive_resyncs = 0;
    } // end for loop

    // num_nacks = number of nacks recieved. should be only 1. if not its a clone card which always sends NACK (parity == 0) ?
    // i  =  number of authentications sent.  Not always 256, since we are trying to sync but close to it.

    uint8_t *data = BigBuf_malloc(4);
    data[0] = isOK;
    data[1] = num_nacks;
    num_to_bytes(i, 2, data + 2);
    reply_ng(CMD_HF_MIFARE_NACK_DETECT, status, data, 4);

    //reply_mix(CMD_ACK, isOK, num_nacks, i, 0, 0);
    BigBuf_free();
    hf_field_off();
    set_tracing(false);
}
