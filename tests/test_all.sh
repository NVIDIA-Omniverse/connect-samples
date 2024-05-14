#!/bin/bash

set -e

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
echo Test script lives in ${SCRIPT_DIR}
ROOT_DIR=${SCRIPT_DIR}/..

echo Running script in "${ROOT_DIR}"

pushd "$ROOT_DIR" > /dev/null

export PYTHON=${ROOT_DIR}/_build/target-deps/python/python
export PYTHONPATH=${ROOT_DIR}/source/pyHelloWorld

if [ ! -f "${PYTHON}" ]; then
    echo "Python, USD, and Omniverse dependencies are missing. Run \"./repo.sh build\" to configure them."
    popd
    exit
fi

"${PYTHON}" -s ./tests/test_all.py "$@"
popd > /dev/null
