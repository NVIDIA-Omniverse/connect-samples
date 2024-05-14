#!/bin/bash

set -e

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

export CARB_APP_PATH=${SCRIPT_DIR}/_build/linux-x86_64/release
export PYTHONHOME=${CARB_APP_PATH}/python-runtime
export PYTHON=${PYTHONHOME}/python

export LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:${PYTHONHOME}/lib:${CARB_APP_PATH}
export PYTHONPATH=${CARB_APP_PATH}/python:${CARB_APP_PATH}/scripts:${CARB_APP_PATH}/bindings-python

echo Running script in "${SCRIPT_DIR}"
pushd "$SCRIPT_DIR" > /dev/null

if [ ! -f "${PYTHON}" ]; then
    echo "Python, USD, and Omniverse dependencies are missing. Run \"./repo.sh build\" to configure them."
    popd
    exit
fi

"${PYTHON}" -s source/pyHelloWorld/assetValidatorBootstrap.py "$@"

popd > /dev/null
