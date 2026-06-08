#include "test_codec.h"

int main(int argc, char** argv) {
    int status = 0;
    status |= run_test_codec(argc, argv);
    return status;
}
