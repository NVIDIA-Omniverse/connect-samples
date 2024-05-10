#!/bin/bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

echo Running script in ${SCRIPT_DIR}

export CARB_APP_PATH=${SCRIPT_DIR}/_build/linux-x86_64/release
export PYTHONHOME=${CARB_APP_PATH}/python-runtime

export LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:${PYTHONHOME}/lib:${CARB_APP_PATH}

echo Running script in ${SCRIPT_DIR}
pushd "$SCRIPT_DIR" > /dev/null
./_build/linux-x86_64/release/omniSimpleSensor "$@"

if [ $? -eq 0 ]
then
  #for i in 0{0..$2}
  i=0
  while [ $i -lt $2 ]
  do
    ./_build/linux-x86_64/release/omniSensorThread "$1" $i $3 &
    ((i++))
  done
fi

popd > /dev/null
