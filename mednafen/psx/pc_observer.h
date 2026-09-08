#ifndef PSX_PC_OBSERVER_H
#define PSX_PC_OBSERVER_H

#include <libretro.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  PSX_OBSERVER_ABI = 1,
  PSX_OBSERVER_TARGETS = 4,
  PSX_OBSERVER_RANGES = 8,
  PSX_OBSERVER_BYTES = 512,
  PSX_OBSERVER_RECORDS = 128
};

typedef struct {
  uint32_t pc, follow_return;
} PSX_ObserverTarget;
typedef struct {
  uint32_t address, bytes;
} PSX_ObserverRange;
typedef struct {
  uint32_t abi, target_count, range_count, capacity;
  PSX_ObserverTarget targets[PSX_OBSERVER_TARGETS];
  PSX_ObserverRange ranges[PSX_OBSERVER_RANGES];
} PSX_ObserverConfig;

typedef struct {
  uint64_t scanned, matched, retained, dropped, pairing_errors;
  uint64_t entries[PSX_OBSERVER_TARGETS], returns[PSX_OBSERVER_TARGETS];
  uint32_t enabled, queued, pending, complete;
} PSX_ObserverStatus;

typedef struct {
  uint64_t ordinal, field;
  uint32_t target, kind; /* 0 = entry, 1 = arrival at its saved return PC/SP */
  uint32_t pc, next_pc, instruction, branch_delay, load_register, load_value;
  int32_t timestamp;
  uint32_t gpr[34]; /* r0..r31, LO, HI; pending load has not been forced to commit */
  uint32_t cp0_status, cp0_cause, cp0_epc, ram_bytes;
  uint8_t ram[PSX_OBSERVER_BYTES]; /* configured ranges concatenated in order */
} PSX_ObserverRecord;

/* Optional libretro extension, not part of the standard ABI. Configure/drain
 * only between retro_run calls. Invalid configuration preserves prior state. */
RETRO_API uint32_t retro_psx_observer_abi(void);
RETRO_API uint32_t retro_psx_observer_size(uint32_t kind);
RETRO_API int retro_psx_observer_configure(const PSX_ObserverConfig *config);
RETRO_API void retro_psx_observer_disable(void);
RETRO_API void retro_psx_observer_field(uint64_t field);
RETRO_API void retro_psx_observer_status(PSX_ObserverStatus *status);
RETRO_API uint32_t retro_psx_observer_drain(PSX_ObserverRecord *records, uint32_t capacity);

/* CPU-owned pre-instruction boundary: after fetch/cycle bookkeeping, before
 * opcode semantics and DO_LDS. Interrupt/halt dispatch does not call this.
 * Active locals are supplied directly; the backed PC/load fields are stale. */
extern int psx_observer_enabled;
void CPU_ObservePC(uint32_t pc,
                   uint32_t next_pc,
                   uint32_t instruction,
                   uint32_t branch_delay,
                   uint32_t load_register,
                   uint32_t load_value,
                   int32_t timestamp,
                   const uint32_t *gpr,
                   const uint32_t *cp0);

#ifdef __cplusplus
}
#endif
#endif
