// The one main() of the harness tests; run_main (test_harness.hpp) says what it runs.

#include "test_harness.hpp"

int main(int argc, char** argv) {
  return mdbus_test::run_main(argc, argv);
}
