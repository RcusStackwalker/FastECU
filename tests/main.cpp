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
    return status;
}
