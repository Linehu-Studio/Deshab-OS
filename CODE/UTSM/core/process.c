#include <utsm/process.h>
#include <utsm/drr.h>

void utsm_process_create_test(utsm_process_context *process, u64 seed) {
    process->process_uuid = uuid_make_test(seed);
    process->crypto_epoch = 1;
    process->recovery_generation = drr_recovery_generation();
    process->capability_table_slot = 0;
    process->capability_count = 0;
    process->flags = 0;
}
