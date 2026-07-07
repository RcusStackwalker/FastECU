#include "test_codec.h"
#include "test_freeform.h"
#include "test_memory.h"
#include "test_transport.h"
#include "test_init.h"
#include "test_driver.h"
#include "test_mitsu_colt_can_protocol.h"
#include "test_mitsu_colt_can_vendor_ext_protocol.h"
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
#include "test_flash_utils.h"
#include "test_ssm_protocol.h"
#include "test_expression_evaluator.h"
#include "test_menu_command.h"
#include "test_diagnostic_parsers.h"
#include "test_model_validation.h"
#include "test_checksum_results.h"

int main(int argc, char **argv)
{
    int status = 0;
    status |= run_test_codec(argc, argv);
    status |= run_test_freeform(argc, argv);
    status |= run_test_memory(argc, argv);
    status |= run_test_transport(argc, argv);
    status |= run_test_init(argc, argv);
    status |= run_test_driver(argc, argv);
    status |= run_test_mitsu_colt_can_protocol(argc, argv);
    status |= run_test_mitsu_colt_can_vendor_ext_protocol(argc, argv);
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
    status |= run_test_flash_utils(argc, argv);
    status |= run_test_ssm_protocol(argc, argv);
    status |= run_test_expression_evaluator(argc, argv);
    status |= run_test_menu_command(argc, argv);
    status |= run_test_diagnostic_parsers(argc, argv);
    status |= run_test_model_validation(argc, argv);
    status |= run_test_checksum_results(argc, argv);
    return status;
}
