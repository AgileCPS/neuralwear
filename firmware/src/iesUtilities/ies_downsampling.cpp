/*
 * ies_downsampling.cpp
 *
 * Ported from iES_v0.3-master to Arduino Nicla Voice, April 2026
 *
 * Implementation notes:
 * 
 * In iES v0.3, downsampling is controlled by:
 * - Command: 'd' (IES_BTSPP_DOWNSAMPLING) followed by 1 byte factor
 * - Default factor: 4 (transmit every 4th sample = 75% reduction)
 * - Side effect: factor < 4 disables SD card, factor >= 4 enables SD card
 * 
 * Downsampling logic (from ies_task.cpp):
 *   uint32_t downsampling_count = 0;
 *   
 *   while (streaming) {
 *       ads1299.updateChannelData();
 *       
 *       if (downsampling_count % ies2btspp_down_sampling == 0) {
 *           // Transmit over BLE/UART
 *           mq_send(ies2btspp_mqd, &sample, sizeof(sample), 0);
 *       }
 *       
 *       if (save_to_sd_card) {
 *           // Always write full-rate to SD
 *           f_write(&eeg_fil, buffer, length, &nbytes);
 *       }
 *       
 *       downsampling_count++;
 *   }
 * 
 * For Nicla Voice implementation:
 * - No separate implementation file needed (header-only)
 * - Inline functions are used for efficiency
 * - Downsampling factor stored in global config or passed to transmit function
 */

#include "ies_downsampling.h"

// This file intentionally left empty - all functions are inline in the header
// to maximize performance (no function call overhead for simple modulo check).
