#!/bin/bash
set -euo pipefail

echo "==> Running vkcc test suite"
./vkcc --version

echo "==> Test C99 compilation"
./vkcc -std=c99 -Wall -Wextra -O2 -c /dev/null -o /tmp/vkcc_null.o
rm -f /tmp/vkcc_null.o

echo "==> Test C23 compilation"
./vkcc -std=c23 -Wall -Wextra -c /dev/null -o /tmp/vkcc_null.o
rm -f /tmp/vkcc_null.o

echo "==> Test C++20 compilation"
./vkc++ -std=c++20 -Wall -Wextra -c /dev/null -o /tmp/vkcc_null.o
rm -f /tmp/vkcc_null.o

echo "==> Running assertion test binary"
./vkcc_check

echo "==> ALL TESTS PASSED SUCCESSFULLY"
