#include "pc_observer.h"

#include "psx.h"
#include <string.h>

int psx_observer_enabled;
static PSX_ObserverConfig configuration;
static PSX_ObserverStatus census;
static PSX_ObserverRecord ring[PSX_OBSERVER_RECORDS];
static uint32_t return_pc[PSX_OBSERVER_TARGETS], return_sp[PSX_OBSERVER_TARGETS];
static uint8_t pending[PSX_OBSERVER_TARGETS];
static uint64_t current_field;

uint32_t retro_psx_observer_abi(void) {
  return PSX_OBSERVER_ABI;
}
uint32_t retro_psx_observer_size(uint32_t kind) {
  switch (kind) {
  case 0:
    return sizeof(PSX_ObserverConfig);
  case 1:
    return sizeof(PSX_ObserverStatus);
  case 2:
    return sizeof(PSX_ObserverRecord);
  default:
    return 0;
  }
}

int retro_psx_observer_configure(const PSX_ObserverConfig *config) {
  uint32_t bytes = 0, i, j;
  if (!config || config->abi != PSX_OBSERVER_ABI || !config->target_count ||
      config->target_count > PSX_OBSERVER_TARGETS || config->range_count > PSX_OBSERVER_RANGES || !config->capacity ||
      config->capacity > PSX_OBSERVER_RECORDS || !MainRAM || !MainRAM->data8 || MainRAM->size != 0x200000u) {
    return 0;
  }
  for (i = 0; i < config->target_count; ++i) {
    if ((config->targets[i].pc & 3u) || config->targets[i].follow_return > 1u) {
      return 0;
    }
    for (j = 0; j < i; ++j) {
      if (config->targets[i].pc == config->targets[j].pc) {
        return 0;
      }
    }
  }
  for (i = 0; i < config->range_count; ++i) {
    const uint32_t segment = config->ranges[i].address & 0xe0000000u;
    const uint32_t address = config->ranges[i].address & 0x1fffffffu;
    /* Admit physical main RAM and its KSEG0/KSEG1 aliases, never other segments. */
    if (segment != 0 && segment != 0x80000000u && segment != 0xa0000000u) {
      return 0;
    }
    const uint32_t size = config->ranges[i].bytes;
    if (address >= 0x200000u || !size || size > 0x200000u - address || size > PSX_OBSERVER_BYTES - bytes) {
      return 0;
    }
    bytes += size;
  }
  configuration = *config;
  memset(&census, 0, sizeof(census));
  memset(pending, 0, sizeof(pending));
  current_field = 0;
  psx_observer_enabled = 1;
  return 1;
}

void retro_psx_observer_disable(void) {
  psx_observer_enabled = 0;
}
void retro_psx_observer_field(uint64_t field) {
  current_field = field;
}

void retro_psx_observer_status(PSX_ObserverStatus *status) {
  uint32_t i;
  if (!status) {
    return;
  }
  *status = census;
  status->enabled = psx_observer_enabled != 0;
  status->complete = census.scanned != 0 && !census.dropped && !census.pairing_errors;
  for (i = 0; i < configuration.target_count; ++i) {
    status->pending += pending[i] != 0;
    if (!census.entries[i] || pending[i] ||
        (configuration.targets[i].follow_return && census.entries[i] != census.returns[i])) {
      status->complete = 0;
    }
  }
}

uint32_t retro_psx_observer_drain(PSX_ObserverRecord *records, uint32_t capacity) {
  uint32_t count;
  if (!records || !capacity) {
    return 0;
  }
  count = census.queued < capacity ? census.queued : capacity;
  memcpy(records, ring, count * sizeof(*records));
  census.queued -= count;
  memmove(ring, ring + count, census.queued * sizeof(*ring));
  return count;
}

static void retain(uint32_t target,
                   uint32_t kind,
                   uint32_t pc,
                   uint32_t next_pc,
                   uint32_t instruction,
                   uint32_t branch_delay,
                   uint32_t load_register,
                   uint32_t load_value,
                   int32_t timestamp,
                   const uint32_t *gpr,
                   const uint32_t *cp0) {
  uint32_t i;
  PSX_ObserverRecord *record;
  ++census.matched;
  if (census.queued == configuration.capacity) {
    ++census.dropped;
    return;
  }
  record = &ring[census.queued++];
  memset(record, 0, sizeof(*record));
  record->ordinal = census.scanned;
  record->field = current_field;
  record->target = target;
  record->kind = kind;
  record->pc = pc;
  record->next_pc = next_pc;
  record->instruction = instruction;
  record->branch_delay = branch_delay;
  record->load_register = load_register;
  record->load_value = load_value;
  record->timestamp = timestamp;
  memcpy(record->gpr, gpr, sizeof(record->gpr));
  record->cp0_status = cp0[12];
  record->cp0_cause = cp0[13];
  record->cp0_epc = cp0[14];
  for (i = 0; i < configuration.range_count; ++i) {
    const PSX_ObserverRange *range = &configuration.ranges[i];
    memcpy(record->ram + record->ram_bytes, MainRAM->data8 + (range->address & 0x1fffffffu), range->bytes);
    record->ram_bytes += range->bytes;
  }
  ++census.retained;
}

void CPU_ObservePC(uint32_t pc,
                   uint32_t next_pc,
                   uint32_t instruction,
                   uint32_t branch_delay,
                   uint32_t load_register,
                   uint32_t load_value,
                   int32_t timestamp,
                   const uint32_t *gpr,
                   const uint32_t *cp0) {
  uint32_t i;
  ++census.scanned;
  for (i = 0; i < configuration.target_count; ++i) {
    if (pending[i] && pc == return_pc[i] && gpr[29] == return_sp[i] && !branch_delay) {
      pending[i] = 0;
      ++census.returns[i];
      retain(i, 1, pc, next_pc, instruction, branch_delay, load_register, load_value, timestamp, gpr, cp0);
    }
    if (pc != configuration.targets[i].pc) {
      continue;
    }
    ++census.entries[i];
    retain(i, 0, pc, next_pc, instruction, branch_delay, load_register, load_value, timestamp, gpr, cp0);
    if (configuration.targets[i].follow_return) {
      if (pending[i] || (gpr[31] & 3u)) {
        ++census.pairing_errors; /* Nested/reentrant observation is explicitly incomplete. */
      } else {
        return_pc[i] = gpr[31];
        return_sp[i] = gpr[29];
        pending[i] = 1;
      }
    }
  }
}
