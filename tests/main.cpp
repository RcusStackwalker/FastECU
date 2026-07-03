#include "test_codec.h"
#include "test_freeform.h"
#include "test_memory.h"
#include "test_transport.h"
#include "test_init.h"
#include "test_driver.h"
#include "test_mitsu_colt_can_protocol.h"
#include "test_mitsu_colt_can_cdbg_protocol.h"
#include "test_cdbg_driver.h"
#include "test_logging_worker.h"
#include "test_logging_engine.h"
#include "test_romraider_conversion.h"
#include "test_ssm_logging_protocol.h"
#include "test_mut_dma_logging_protocol.h"
#include "test_cdbg_logging_protocol.h"
#include "test_flash_operation_worker.h"
#include "test_flash_ecu_mitsu_m32r_can_operation.h"

int main(int argc, char** argv) {
    int status = 0;
    status |= run_test_codec(argc, argv);
    status |= run_test_freeform(argc, argv);
    status |= run_test_memory(argc, argv);
    status |= run_test_transport(argc, argv);
    status |= run_test_init(argc, argv);
    status |= run_test_driver(argc, argv);
    status |= run_test_mitsu_colt_can_protocol(argc, argv);
    status |= run_test_mitsu_colt_can_cdbg_protocol(argc, argv);
    status |= run_test_cdbg_driver(argc, argv);
    status |= run_test_logging_worker(argc, argv);
    status |= run_test_logging_engine(argc, argv);
    status |= run_test_romraider_conversion(argc, argv);
    status |= run_test_ssm_logging_protocol(argc, argv);
    status |= run_test_mut_dma_logging_protocol(argc, argv);
    status |= run_test_cdbg_logging_protocol(argc, argv);
    status |= run_test_flash_operation_worker(argc, argv);
    status |= run_test_flash_ecu_mitsu_m32r_can_operation(argc, argv);
    return status;
}
