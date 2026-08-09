/*
 * ies_checksum.cpp
 *
 * Intentionally empty — all implementations are static inline in ies_checksum.h.
 *
 * Background: Arduino IDE does not reliably compile .cpp files in sketch
 * subdirectories, so the CRC-8/SHT75 functions were moved to the header as
 * static inline to guarantee they are always available to any translation unit
 * that includes the header.
 */
