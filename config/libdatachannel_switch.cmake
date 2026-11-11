get_filename_component(LUNANX_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

add_compile_definitions(
    MBEDTLS_USER_CONFIG_FILE="${LUNANX_ROOT}/config/mbedtls_switch.h"
)

set(MbedTLS_INCLUDE_DIR "${LUNANX_ROOT}/external/mbedtls/include" CACHE PATH "")
set(MbedTLS_LIBRARY "${LUNANX_ROOT}/build-mbedtls-switch/library/libmbedtls.a" CACHE FILEPATH "")
set(MbedX509_LIBRARY "${LUNANX_ROOT}/build-mbedtls-switch/library/libmbedx509.a" CACHE FILEPATH "")
set(MbedCrypto_LIBRARY "${LUNANX_ROOT}/build-mbedtls-switch/library/libmbedcrypto.a" CACHE FILEPATH "")

set(MBEDTLS_INCLUDE_DIRS "${MbedTLS_INCLUDE_DIR}" CACHE PATH "")
set(MBEDTLS_LIBRARY "${MbedTLS_LIBRARY}" CACHE FILEPATH "")
set(MBEDX509_LIBRARY "${MbedX509_LIBRARY}" CACHE FILEPATH "")
set(MBEDCRYPTO_LIBRARY "${MbedCrypto_LIBRARY}" CACHE FILEPATH "")