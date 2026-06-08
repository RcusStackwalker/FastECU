#include "test_codec.h"
#include "test_freeform.h"
#include "test_memory.h"

int main(int argc, char** argv) {
    int status = 0;
    status |= run_test_codec(argc, argv);
    status |= run_test_freeform(argc, argv);
    status |= run_test_memory(argc, argv);
    return status;
}
