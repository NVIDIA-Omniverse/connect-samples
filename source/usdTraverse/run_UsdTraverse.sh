
export CARB_APP_PATH=./connect-sdk/linux-x86_64/release
export LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:${CARB_APP_PATH}/lib

./release/UsdTraverse "$@"