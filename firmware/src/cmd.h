/**
 * @file    cmd.h
 * @brief   Command / response data types and FIFO queue type aliases.
 *
 * Defines Command and Response structs and provides type aliases:
 *
 *   ICmdQueue  - capacity-independent interface alias (IQueue<Command>).
 *   CmdFifo<N> - concrete alias (FifoQueue<Command, N>).
 *
 * Also defines the IES_CMD_* command byte constants - the shared command
 * vocabulary for all transports (single definition, no duplication).
 */

#pragma once

#include <stdint.h>
#include "config.h"
#include "fifo_queue.h"


// -----------------------------------------------------------------------------
// IES command byte constants
// -----------------------------------------------------------------------------

constexpr uint8_t IES_CMD_STREAM_START   = 'b';   ///< Begin streaming (no payload)
constexpr uint8_t IES_CMD_STREAM_STOP    = 's';   ///< Stop streaming  (no payload)
constexpr uint8_t IES_CMD_TIME_SYNC      = 't';   ///< Time sync: 4-byte Unix time + 1-byte CRC-8
constexpr uint8_t IES_CMD_HEARTBEAT      = '.';   ///< Keepalive ping  (no payload)
constexpr uint8_t IES_CMD_UART_PRINT_SEL = 'p';   ///< Log category bitmask (1-byte payload)
constexpr uint8_t IES_CMD_DOWNSAMPLING   = 'd';   ///< Downsampling ratio   (1-byte payload)
constexpr uint8_t IES_CMD_IMPEDANCE_ON   = 'Z';   ///< Enable impedance check  (no payload)
constexpr uint8_t IES_CMD_IMPEDANCE_OFF  = 'z';   ///< Disable impedance check (no payload)
constexpr uint8_t IES_CMD_SOFT_RESET     = 'v';   ///< Soft reset (0x76)


// -----------------------------------------------------------------------------
// CmdSource - which transport delivered the command
// -----------------------------------------------------------------------------

/** @brief Transport origin of a received command. */
enum class CmdSource : uint8_t {
    UART = 0,   ///< UART / USB Serial
    BLE  = 1    ///< BLE control channel
};


// -----------------------------------------------------------------------------
// CmdStatus - result codes returned in Response::status
// -----------------------------------------------------------------------------

/** @brief Execution result codes for command responses. */
enum class CmdStatus : uint8_t {
    OK              = 0x00,   ///< Success.
    ERR_UNKNOWN     = 0x01,   ///< Command byte not recognised.
    ERR_BAD_PAYLOAD = 0x02,   ///< payload_len wrong or payload malformed.
    ERR_BAD_CRC     = 0x03,   ///< CRC check failed.
    ERR_NOT_ALLOWED = 0x04    ///< Command not permitted in current state.
};


// -----------------------------------------------------------------------------
// Command  - decoded command pushed into the command queue
// -----------------------------------------------------------------------------

/**
 * @brief Decoded command to be pushed into a command queue.
 *
 * All fields are 1-byte aligned; no internal or trailing padding.
 * Memory layout:
 *   Offset  0:    cmd_id      (1 B)
 *   Offset  1:    payload[N]  (IES_CMD_PAYLOAD_MAX B)
 *   Offset  1+N:  payload_len (1 B)
 *   Offset  2+N:  source      (1 B)
 *   sizeof == 3 + IES_CMD_PAYLOAD_MAX bytes  (11 with default N = 8).
 */
struct Command {
    uint8_t   cmd_id;                        ///< IES_CMD_* constant (command byte)
    uint8_t   payload[IES_CMD_PAYLOAD_MAX];  ///< Raw payload bytes (see cmd byte docs)
    uint8_t   payload_len;                   ///< Number of valid bytes in payload[]
    CmdSource source;                        ///< Which transport sent this command
};
static_assert(sizeof(Command) == 3 + IES_CMD_PAYLOAD_MAX,
    "Command size mismatch - check IES_CMD_PAYLOAD_MAX and field types.");


// -----------------------------------------------------------------------------
// Response  - reply to a Command
// -----------------------------------------------------------------------------

/**
 * @brief Response to a Command.
 *
 * All fields are 1-byte aligned; no internal or trailing padding.
 * Memory layout:
 *   Offset  0:    cmd_id      (1 B)
 *   Offset  1:    status      (1 B)
 *   Offset  2:    payload[N]  (IES_CMD_PAYLOAD_MAX B)
 *   Offset  2+N:  payload_len (1 B)
 *   Offset  3+N:  dest        (1 B)
 *   sizeof == 4 + IES_CMD_PAYLOAD_MAX bytes  (12 with default N = 8).
 */
struct Response {
    uint8_t   cmd_id;                        ///< Echoes Command::cmd_id
    CmdStatus status;                        ///< CmdStatus::OK or error code
    uint8_t   payload[IES_CMD_PAYLOAD_MAX];  ///< Response data (command-specific)
    uint8_t   payload_len;                   ///< Valid bytes in payload[]
    CmdSource dest;                          ///< Transport for the reply
};
static_assert(sizeof(Response) == 4 + IES_CMD_PAYLOAD_MAX,
    "Response size mismatch - check IES_CMD_PAYLOAD_MAX and field types.");


// -----------------------------------------------------------------------------
// Type aliases
// -----------------------------------------------------------------------------

/** @brief Capacity-independent interface alias for Command queues. */
using ICmdQueue = IQueue<Command>;

/** @brief Concrete command FIFO parameterised by depth N. */
template<size_t N>
using CmdFifo = FifoQueue<Command, N>;


