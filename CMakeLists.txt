#
# project: fips-gainput
#

if (NOT FIPS_IMPORT)
    cmake_minimum_required(VERSION 3.21)
    # include the fips main cmake file
    get_filename_component(FIPS_ROOT_DIR "../fips" ABSOLUTE)
    include("${FIPS_ROOT_DIR}/cmake/fips.cmake")

    fips_setup(PROJECT fips-gainput)
endif()

fips_add_subdirectory(gainput)
