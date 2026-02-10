#!/usr/bin/env bash
#
# Copyright (c) 2025-2026, Arm Limited. All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_INSTALL_DIR="${SCRIPT_DIR}"
TEST_INSTALL_BUILD_DIR="$(mktemp -d)"

LIBTPM_DIR="$(realpath "${SCRIPT_DIR}/../..")"
LIBTPM_BUILD_DIR="$(mktemp -d)"
INSTALL_DIR="$(mktemp -d)"

echo INSTALL_DIR=${INSTALL_DIR}
echo "==> Building main project at ${LIBTPM_DIR}"
cmake -S "${LIBTPM_DIR}" -B "${LIBTPM_BUILD_DIR}" -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}"
cmake --build "${LIBTPM_BUILD_DIR}" --parallel
cmake --install "${LIBTPM_BUILD_DIR}"

echo "==> Building subproject at ${TEST_INSTALL_DIR}"
cmake -S "${TEST_INSTALL_DIR}" -B "${TEST_INSTALL_BUILD_DIR}" -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}"
cmake --build "${TEST_INSTALL_BUILD_DIR}" --parallel
